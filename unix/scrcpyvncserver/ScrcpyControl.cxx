/* Copyright 2026 TigerVNC Team
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "ScrcpyControl.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

#include <core/Exception.h>
#include <core/LogWriter.h>
#include <core/string.h>

#define XK_LATIN1
#define XK_MISCELLANY
#define XK_XKB_KEYS
#include <rfb/keysymdef.h>
#include <rfb/XF86keysym.h>

static core::LogWriter vlog("ScrcpyControl");

namespace {

// Control message types
enum ControlMsgType : uint8_t {
  kMsgInjectKeycode = 0,
  kMsgInjectText = 1,
  kMsgInjectTouchEvent = 2,
  kMsgInjectScrollEvent = 3,
  kMsgSetClipboard = 9,
  kMsgResetVideo = 17,
};

// Device message types
enum DeviceMsgType : uint8_t {
  kDeviceMsgClipboard = 0,
  kDeviceMsgAckClipboard = 1,
  kDeviceMsgUhidOutput = 2,
};

// Android KeyEvent actions
enum AndroidKeyAction : uint8_t {
  kKeyActionDown = 0,
  kKeyActionUp = 1,
};

// Android MotionEvent actions
enum AndroidMotionAction : uint8_t {
  kMotionActionDown = 0,
  kMotionActionUp = 1,
  kMotionActionMove = 2,
  kMotionActionHoverMove = 7,
};

// Android MotionEvent buttons
enum AndroidMotionButton : uint32_t {
  kAndroidButtonPrimary = 1u << 0,
  kAndroidButtonSecondary = 1u << 1,
  kAndroidButtonTertiary = 1u << 2,
  kAndroidButtonBack = 1u << 3,
  kAndroidButtonForward = 1u << 4,
};

// Android MetaState flags
enum AndroidMetaState : uint32_t {
  kMetaShiftOn = 0x01,
  kMetaAltOn = 0x02,
  kMetaAltLeftOn = 0x10,
  kMetaAltRightOn = 0x20,
  kMetaShiftLeftOn = 0x40,
  kMetaShiftRightOn = 0x80,
  kMetaCtrlOn = 0x1000,
  kMetaCtrlLeftOn = 0x2000,
  kMetaCtrlRightOn = 0x4000,
  kMetaMetaOn = 0x10000,
  kMetaMetaLeftOn = 0x20000,
  kMetaMetaRightOn = 0x40000,
};

static const uint64_t kPointerIdMouse = UINT64_C(-1);
static const size_t kMaxTextLength = 300;
static const size_t kMaxClipboardLength = (1u << 18) - 14u;
static const size_t kMaxDeviceMsgSize = (1u << 18);
static const size_t kMaxControlQueue = 4 * kMaxDeviceMsgSize;

void writeBe16(uint8_t* p, uint16_t v)
{
  p[0] = static_cast<uint8_t>((v >> 8) & 0xff);
  p[1] = static_cast<uint8_t>(v & 0xff);
}

void writeBe32(uint8_t* p, uint32_t v)
{
  p[0] = static_cast<uint8_t>((v >> 24) & 0xff);
  p[1] = static_cast<uint8_t>((v >> 16) & 0xff);
  p[2] = static_cast<uint8_t>((v >> 8) & 0xff);
  p[3] = static_cast<uint8_t>(v & 0xff);
}

void writeBe64(uint8_t* p, uint64_t v)
{
  writeBe32(p, static_cast<uint32_t>(v >> 32));
  writeBe32(p + 4, static_cast<uint32_t>(v & 0xffffffffu));
}

uint16_t readBe16(const uint8_t* p)
{
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t readBe32(const uint8_t* p)
{
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) |
         static_cast<uint32_t>(p[3]);
}

uint16_t floatToU16Fp(float f)
{
  if (f <= 0.0f)
    return 0;
  if (f >= 1.0f)
    return 0xffffu;
  uint32_t u = static_cast<uint32_t>(f * 65536.0f);
  if (u > 0xffffu)
    u = 0xffffu;
  return static_cast<uint16_t>(u);
}

int16_t floatToI16Fp(float f)
{
  if (f <= -1.0f)
    return -0x8000;
  if (f >= 1.0f)
    return 0x7fff;
  int32_t i = static_cast<int32_t>(f * 32768.0f);
  if (i < -0x8000)
    i = -0x8000;
  if (i > 0x7fff)
    i = 0x7fff;
  return static_cast<int16_t>(i);
}

size_t utf8TruncationLen(const std::string& s, size_t maxBytes)
{
  if (s.size() <= maxBytes)
    return s.size();
  size_t n = maxBytes;
  while (n > 0 && (static_cast<uint8_t>(s[n]) & 0xc0u) == 0x80u)
    n--;
  return n;
}

uint32_t mapKeysymToAndroidKeycode(uint32_t keysym, bool* shiftOverride)
{
  if (shiftOverride)
    *shiftOverride = false;

  if (keysym >= XK_a && keysym <= XK_z)
    return 29u + (keysym - XK_a); // AKEYCODE_A .. AKEYCODE_Z
  if (keysym >= XK_A && keysym <= XK_Z) {
    if (shiftOverride)
      *shiftOverride = true;
    return 29u + (keysym - XK_A);
  }
  if (keysym >= XK_0 && keysym <= XK_9)
    return 7u + (keysym - XK_0); // AKEYCODE_0 .. AKEYCODE_9

  switch (keysym) {
  case XK_space:         return 62;  // AKEYCODE_SPACE
  case XK_Return:
  case XK_KP_Enter:      return 66;  // AKEYCODE_ENTER
  case XK_BackSpace:     return 67;  // AKEYCODE_DEL
  case XK_Tab:
  case XK_ISO_Left_Tab:  return 61;  // AKEYCODE_TAB
  case XK_Escape:        return 111; // AKEYCODE_ESCAPE
  case XK_Delete:
  case XK_KP_Delete:     return 112; // AKEYCODE_FORWARD_DEL
  case XK_Home:
  case XK_KP_Home:       return 122; // AKEYCODE_MOVE_HOME
  case XK_End:
  case XK_KP_End:        return 123; // AKEYCODE_MOVE_END
  case XK_Insert:
  case XK_KP_Insert:     return 124; // AKEYCODE_INSERT
  case XK_Page_Up:
  case XK_KP_Page_Up:    return 92;  // AKEYCODE_PAGE_UP
  case XK_Page_Down:
  case XK_KP_Page_Down:  return 93;  // AKEYCODE_PAGE_DOWN
  case XK_Up:
  case XK_KP_Up:         return 19;  // AKEYCODE_DPAD_UP
  case XK_Down:
  case XK_KP_Down:       return 20;  // AKEYCODE_DPAD_DOWN
  case XK_Left:
  case XK_KP_Left:       return 21;  // AKEYCODE_DPAD_LEFT
  case XK_Right:
  case XK_KP_Right:      return 22;  // AKEYCODE_DPAD_RIGHT
  case XK_Menu:          return 82;  // AKEYCODE_MENU
  case XK_Shift_L:       return 59;  // AKEYCODE_SHIFT_LEFT
  case XK_Shift_R:       return 60;  // AKEYCODE_SHIFT_RIGHT
  case XK_Control_L:     return 113; // AKEYCODE_CTRL_LEFT
  case XK_Control_R:     return 114; // AKEYCODE_CTRL_RIGHT
  case XK_Alt_L:         return 57;  // AKEYCODE_ALT_LEFT
  case XK_Alt_R:         return 58;  // AKEYCODE_ALT_RIGHT
  case XK_Super_L:       return 117; // AKEYCODE_META_LEFT
  case XK_Super_R:       return 118; // AKEYCODE_META_RIGHT
  case XK_Caps_Lock:     return 115; // AKEYCODE_CAPS_LOCK
  case XK_Num_Lock:      return 143; // AKEYCODE_NUM_LOCK
  case XK_Scroll_Lock:   return 116; // AKEYCODE_SCROLL_LOCK
  case XK_comma:         return 55;  // AKEYCODE_COMMA
  case XK_period:        return 56;  // AKEYCODE_PERIOD
  case XK_grave:         return 68;  // AKEYCODE_GRAVE
  case XK_minus:         return 69;  // AKEYCODE_MINUS
  case XK_equal:         return 70;  // AKEYCODE_EQUALS
  case XK_bracketleft:   return 71;  // AKEYCODE_LEFT_BRACKET
  case XK_bracketright:  return 72;  // AKEYCODE_RIGHT_BRACKET
  case XK_backslash:     return 73;  // AKEYCODE_BACKSLASH
  case XK_semicolon:     return 74;  // AKEYCODE_SEMICOLON
  case XK_apostrophe:    return 75;  // AKEYCODE_APOSTROPHE
  case XK_slash:         return 76;  // AKEYCODE_SLASH
  case XK_at:            return 77;  // AKEYCODE_AT
  case XK_plus:          return 81;  // AKEYCODE_PLUS
  case XK_asterisk:      return 17;  // AKEYCODE_STAR
  case XK_numbersign:    return 18;  // AKEYCODE_POUND
  case XK_F1:            return 131;
  case XK_F2:            return 132;
  case XK_F3:            return 133;
  case XK_F4:            return 134;
  case XK_F5:            return 135;
  case XK_F6:            return 136;
  case XK_F7:            return 137;
  case XK_F8:            return 138;
  case XK_F9:            return 139;
  case XK_F10:           return 140;
  case XK_F11:           return 141;
  case XK_F12:           return 142;
  case XK_KP_0:          return 144;
  case XK_KP_1:          return 145;
  case XK_KP_2:          return 146;
  case XK_KP_3:          return 147;
  case XK_KP_4:          return 148;
  case XK_KP_5:          return 149;
  case XK_KP_6:          return 150;
  case XK_KP_7:          return 151;
  case XK_KP_8:          return 152;
  case XK_KP_9:          return 153;
  case XK_KP_Divide:     return 154;
  case XK_KP_Multiply:   return 155;
  case XK_KP_Subtract:   return 156;
  case XK_KP_Add:        return 157;
  case XK_KP_Decimal:    return 158;
  case XF86XK_Back:             return 4;   // AKEYCODE_BACK
  case XF86XK_HomePage:         return 3;   // AKEYCODE_HOME
  case XF86XK_AudioLowerVolume: return 25;  // AKEYCODE_VOLUME_DOWN
  case XF86XK_AudioRaiseVolume: return 24;  // AKEYCODE_VOLUME_UP
  case XF86XK_AudioMute:        return 164; // AKEYCODE_VOLUME_MUTE
  case XF86XK_AudioPlay:        return 85;  // AKEYCODE_MEDIA_PLAY_PAUSE
  case XF86XK_AudioStop:        return 86;  // AKEYCODE_MEDIA_STOP
  case XF86XK_AudioNext:        return 87;  // AKEYCODE_MEDIA_NEXT
  case XF86XK_AudioPrev:        return 88;  // AKEYCODE_MEDIA_PREVIOUS
  case XF86XK_PowerOff:         return 26;  // AKEYCODE_POWER
  case XF86XK_Search:           return 84;  // AKEYCODE_SEARCH
  default:                      return 0;
  }
}

uint32_t normalizeCombinedMeta(uint32_t m)
{
  m &= ~(kMetaShiftOn | kMetaAltOn | kMetaCtrlOn | kMetaMetaOn);
  if (m & (kMetaShiftLeftOn | kMetaShiftRightOn))
    m |= kMetaShiftOn;
  if (m & (kMetaAltLeftOn | kMetaAltRightOn))
    m |= kMetaAltOn;
  if (m & (kMetaCtrlLeftOn | kMetaCtrlRightOn))
    m |= kMetaCtrlOn;
  if (m & (kMetaMetaLeftOn | kMetaMetaRightOn))
    m |= kMetaMetaOn;
  return m;
}

} // namespace

ScrcpyControl::ScrcpyControl()
  : fd_(-1),
    sendOffset_(0),
    recvOffset_(0),
    androidButtons_(0),
    lastVncMask_(0),
    lastX_(0),
    lastY_(0),
    metaState_(0),
    supportsVideoReset_(true),
    scrollRange_(16.0f)
{
}

ScrcpyControl::~ScrcpyControl()
{
  close();
}

void ScrcpyControl::attach(int fd)
{
  close();
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0)
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  fd_ = fd;
}

void ScrcpyControl::setServerVersion(const std::string& version)
{
  unsigned major = 0, minor = 0, patch = 0;
  if (std::sscanf(version.c_str(), "%9u.%9u.%9u", &major, &minor, &patch) < 2)
    return;

  supportsVideoReset_ = major >= 3;
  // scrcpy 3.3.1 expanded the range encoded by the scroll fields.
  bool extendedScroll = major > 3 ||
                        (major == 3 && (minor > 3 || (minor == 3 && patch >= 1)));
  scrollRange_ = extendedScroll ? 16.0f : 1.0f;
}

void ScrcpyControl::close()
{
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  sendBuf_.clear();
  sendOffset_ = 0;
  recvBuf_.clear();
  recvOffset_ = 0;
  androidButtons_ = 0;
  lastVncMask_ = 0;
  metaState_ = 0;
}

void ScrcpyControl::enqueue(const uint8_t* data, size_t len)
{
  if (fd_ < 0 || len == 0)
    return;

  if (len > kMaxControlQueue - (sendBuf_.size() - sendOffset_)) {
    vlog.error("Control socket output queue is full");
    close();
    return;
  }

  if (sendOffset_ == sendBuf_.size()) {
    size_t sent = 0;
    while (sent < len) {
      ssize_t n = ::send(fd_, data + sent, len - sent,
                         MSG_DONTWAIT | MSG_NOSIGNAL);
      if (n > 0) {
        sent += static_cast<size_t>(n);
        continue;
      }
      if (n < 0 && errno == EINTR)
        continue;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        break;
      vlog.error("Control socket write error: %s", std::strerror(errno));
      close();
      return;
    }
    if (sent == len)
      return;
    sendBuf_.insert(sendBuf_.end(), data + sent, data + len);
    return;
  }

  if (sendBuf_.size() + len > kMaxControlQueue) {
    sendBuf_.erase(sendBuf_.begin(), sendBuf_.begin() + sendOffset_);
    sendOffset_ = 0;
  }
  sendBuf_.insert(sendBuf_.end(), data, data + len);
}

void ScrcpyControl::processWritable()
{
  if (fd_ < 0)
    return;

  while (sendOffset_ < sendBuf_.size()) {
    ssize_t n = ::send(fd_, sendBuf_.data() + sendOffset_,
                       sendBuf_.size() - sendOffset_,
                       MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n > 0) {
      sendOffset_ += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      break;
    vlog.error("Control socket flush error: %s", std::strerror(errno));
    close();
    return;
  }

  if (sendOffset_ == sendBuf_.size()) {
    sendBuf_.clear();
    sendOffset_ = 0;
  }
}

bool ScrcpyControl::processReadable(Callbacks& cb)
{
  if (fd_ < 0)
    return false;

  uint8_t tmp[4096];
  for (int i = 0; i < 16; ++i) {
    ssize_t n = ::recv(fd_, tmp, sizeof(tmp), MSG_DONTWAIT);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;
      vlog.error("Control socket read error: %s", std::strerror(errno));
      close();
      return false;
    }
    if (n == 0) {
      close();
      return false;
    }
    recvBuf_.insert(recvBuf_.end(), tmp, tmp + n);
    if (!processDeviceMessages(cb))
      return false;
  }
  return true;
}

bool ScrcpyControl::processDeviceMessages(Callbacks& cb)
{
  while (recvOffset_ < recvBuf_.size()) {
    const uint8_t* p = recvBuf_.data() + recvOffset_;
    size_t avail = recvBuf_.size() - recvOffset_;
    uint8_t msgType = p[0];

    if (msgType == kDeviceMsgClipboard) {
      if (avail < 5)
        break;
      uint32_t textLen = readBe32(p + 1);
      if (textLen > kMaxDeviceMsgSize - 5u) {
        vlog.error("Invalid device clipboard length: %u", textLen);
        close();
        return false;
      }
      if (avail < 5u + textLen)
        break;
      std::string text(reinterpret_cast<const char*>(p + 5), textLen);
      recvOffset_ += 5u + textLen;
      cb.onDeviceClipboard(text);
    } else if (msgType == kDeviceMsgAckClipboard) {
      if (avail < 9)
        break;
      recvOffset_ += 9;
    } else if (msgType == kDeviceMsgUhidOutput) {
      if (avail < 5)
        break;
      uint16_t size = readBe16(p + 3);
      if (avail < 5u + size)
        break;
      recvOffset_ += 5u + size;
    } else {
      vlog.error("Unknown scrcpy device message type: %u", msgType);
      close();
      return false;
    }
  }

  if (recvOffset_ == recvBuf_.size()) {
    recvBuf_.clear();
    recvOffset_ = 0;
  } else if (recvOffset_ > 4096) {
    recvBuf_.erase(recvBuf_.begin(),
                   recvBuf_.begin() + static_cast<ptrdiff_t>(recvOffset_));
    recvOffset_ = 0;
  }

  return true;
}

void ScrcpyControl::sendTouchEvent(uint8_t action, uint64_t pointerId,
                                   int32_t x, int32_t y,
                                   uint16_t screenW, uint16_t screenH,
                                   float pressure,
                                   uint32_t actionButton, uint32_t buttons)
{
  uint8_t buf[32];
  buf[0] = kMsgInjectTouchEvent;
  buf[1] = action;
  writeBe64(buf + 2, pointerId);
  writeBe32(buf + 10, static_cast<uint32_t>(x));
  writeBe32(buf + 14, static_cast<uint32_t>(y));
  writeBe16(buf + 18, screenW);
  writeBe16(buf + 20, screenH);
  writeBe16(buf + 22, floatToU16Fp(pressure));
  writeBe32(buf + 24, actionButton);
  writeBe32(buf + 28, buttons);
  enqueue(buf, sizeof(buf));
}

void ScrcpyControl::sendScrollEvent(int32_t x, int32_t y,
                                    uint16_t screenW, uint16_t screenH,
                                    float hscroll, float vscroll,
                                    uint32_t buttons)
{
  float hNorm = std::max(-1.0f, std::min(1.0f, hscroll / scrollRange_));
  float vNorm = std::max(-1.0f, std::min(1.0f, vscroll / scrollRange_));

  uint8_t buf[21];
  buf[0] = kMsgInjectScrollEvent;
  writeBe32(buf + 1, static_cast<uint32_t>(x));
  writeBe32(buf + 5, static_cast<uint32_t>(y));
  writeBe16(buf + 9, screenW);
  writeBe16(buf + 11, screenH);
  writeBe16(buf + 13, static_cast<uint16_t>(floatToI16Fp(hNorm)));
  writeBe16(buf + 15, static_cast<uint16_t>(floatToI16Fp(vNorm)));
  writeBe32(buf + 17, buttons);
  enqueue(buf, sizeof(buf));
}

void ScrcpyControl::sendKeycode(uint8_t action, uint32_t androidKeycode,
                                uint32_t repeat, uint32_t metastate)
{
  uint8_t buf[14];
  buf[0] = kMsgInjectKeycode;
  buf[1] = action;
  writeBe32(buf + 2, androidKeycode);
  writeBe32(buf + 6, repeat);
  writeBe32(buf + 10, metastate);
  enqueue(buf, sizeof(buf));
}

void ScrcpyControl::sendText(const std::string& utf8)
{
  size_t len = utf8TruncationLen(utf8, kMaxTextLength);
  if (len == 0)
    return;

  std::vector<uint8_t> buf(5 + len);
  buf[0] = kMsgInjectText;
  writeBe32(buf.data() + 1, static_cast<uint32_t>(len));
  std::memcpy(buf.data() + 5, utf8.data(), len);
  enqueue(buf.data(), buf.size());
}

void ScrcpyControl::sendSetClipboard(uint64_t sequence,
                                     const std::string& text,
                                     bool paste)
{
  size_t len = utf8TruncationLen(text, kMaxClipboardLength);
  std::vector<uint8_t> buf(14 + len);
  buf[0] = kMsgSetClipboard;
  writeBe64(buf.data() + 1, sequence);
  buf[9] = paste ? 1 : 0;
  writeBe32(buf.data() + 10, static_cast<uint32_t>(len));
  if (len > 0)
    std::memcpy(buf.data() + 14, text.data(), len);
  enqueue(buf.data(), buf.size());
}

void ScrcpyControl::sendResetVideo()
{
  if (!supportsVideoReset_)
    return;

  uint8_t msg = kMsgResetVideo;
  enqueue(&msg, 1);
}

uint32_t ScrcpyControl::vncButtonsToAndroid(uint16_t mask) const
{
  uint32_t btns = 0;
  if (mask & 0x0001u)
    btns |= kAndroidButtonPrimary;
  if (mask & 0x0002u)
    btns |= kAndroidButtonTertiary;
  if (mask & 0x0004u)
    btns |= kAndroidButtonSecondary;
  if (mask & 0x0080u)
    btns |= kAndroidButtonBack;
  if (mask & 0x0100u)
    btns |= kAndroidButtonForward;
  return btns;
}

void ScrcpyControl::handleVncPointer(int x, int y, uint16_t buttonMask,
                                     int screenW, int screenH)
{
  if (fd_ < 0 || screenW <= 0 || screenH <= 0)
    return;

  int clampedX = std::max(0, std::min(x, screenW - 1));
  int clampedY = std::max(0, std::min(y, screenH - 1));
  uint16_t w = static_cast<uint16_t>(screenW);
  uint16_t h = static_cast<uint16_t>(screenH);

  uint32_t newButtons = vncButtonsToAndroid(buttonMask);
  uint32_t pressed = newButtons & ~androidButtons_;
  uint32_t released = androidButtons_ & ~newButtons;

  if (pressed == 0 && released == 0) {
    if (clampedX != lastX_ || clampedY != lastY_) {
      uint8_t action = newButtons ? kMotionActionMove : kMotionActionHoverMove;
      sendTouchEvent(action, kPointerIdMouse, clampedX, clampedY, w, h,
                     newButtons ? 1.0f : 0.0f, 0, newButtons);
    }
  } else {
    static const uint32_t kButtonOrder[] = {
      kAndroidButtonPrimary,
      kAndroidButtonSecondary,
      kAndroidButtonTertiary,
      kAndroidButtonBack,
      kAndroidButtonForward,
    };
    uint32_t cur = androidButtons_;
    for (uint32_t bit : kButtonOrder) {
      if (pressed & bit) {
        cur |= bit;
        sendTouchEvent(kMotionActionDown, kPointerIdMouse,
                       clampedX, clampedY, w, h, 1.0f, bit, cur);
      }
      if (released & bit) {
        cur &= ~bit;
        sendTouchEvent(kMotionActionUp, kPointerIdMouse,
                       clampedX, clampedY, w, h, 0.0f, bit, cur);
      }
    }
    androidButtons_ = newButtons;
  }

  // Wheel bits (3: up, 4: down, 5: left, 6: right) on rising edge
  uint16_t wheelRising = buttonMask & ~lastVncMask_;
  float vscroll = 0.0f;
  float hscroll = 0.0f;
  if (wheelRising & 0x0008u)
    vscroll += 1.0f;
  if (wheelRising & 0x0010u)
    vscroll -= 1.0f;
  if (wheelRising & 0x0020u)
    hscroll -= 1.0f;
  if (wheelRising & 0x0040u)
    hscroll += 1.0f;
  if (vscroll != 0.0f || hscroll != 0.0f)
    sendScrollEvent(clampedX, clampedY, w, h, hscroll, vscroll, newButtons);

  lastVncMask_ = buttonMask;
  lastX_ = clampedX;
  lastY_ = clampedY;
}

void ScrcpyControl::releasePointer(int screenW, int screenH)
{
  handleVncPointer(lastX_, lastY_, 0, screenW, screenH);
}

void ScrcpyControl::handleVncKey(uint32_t keysym, uint32_t /*keycode*/,
                                 bool down)
{
  if (fd_ < 0)
    return;

  // Track modifier state
  uint32_t modBit = 0;
  switch (keysym) {
  case XK_Shift_L:   modBit = kMetaShiftLeftOn; break;
  case XK_Shift_R:   modBit = kMetaShiftRightOn; break;
  case XK_Control_L: modBit = kMetaCtrlLeftOn; break;
  case XK_Control_R: modBit = kMetaCtrlRightOn; break;
  case XK_Alt_L:     modBit = kMetaAltLeftOn; break;
  case XK_Alt_R:     modBit = kMetaAltRightOn; break;
  case XK_Super_L:   modBit = kMetaMetaLeftOn; break;
  case XK_Super_R:   modBit = kMetaMetaRightOn; break;
  default:           break;
  }
  if (modBit != 0) {
    if (down)
      metaState_ |= modBit;
    else
      metaState_ &= ~modBit;
    metaState_ = normalizeCombinedMeta(metaState_);
  }

  bool shiftOverride = false;
  uint32_t androidKey = mapKeysymToAndroidKeycode(keysym, &shiftOverride);
  if (androidKey != 0) {
    uint32_t meta = metaState_;
    if (shiftOverride)
      meta |= kMetaShiftOn | kMetaShiftLeftOn;
    sendKeycode(down ? kKeyActionDown : kKeyActionUp, androidKey, 0, meta);
    return;
  }

  if (!down)
    return;

  // Fallback to text injection for printable Latin-1 / Unicode keysyms
  unsigned ucs4 = 0;
  if (keysym >= 0x0020 && keysym <= 0x00ff) {
    ucs4 = keysym;
  } else if (keysym >= 0x01000000 && keysym <= 0x0110ffff) {
    ucs4 = keysym - 0x01000000;
  }

  if (ucs4 != 0) {
    char utf8[5];
    size_t len = core::ucs4ToUTF8(ucs4, utf8);
    if (len > 0)
      sendText(std::string(utf8, len));
  }
}
