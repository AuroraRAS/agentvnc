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
#include <cctype>
#include <cerrno>
#include <cmath>
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
#include <rfb/giiTypes.h>
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
static const uint64_t kPointerIdGiiBase = UINT64_C(0x4749490000000000);
static const size_t kMaxTotalGIIDevices = 256;
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

std::string toLowerAscii(const std::string& s)
{
  std::string out(s);
  for (size_t i = 0; i < out.size(); ++i)
    out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
  return out;
}

bool containsSubstr(const std::string& haystack, const char* needle)
{
  return haystack.find(needle) != std::string::npos;
}

bool isXValuatorName(const std::string& lowerLong, const std::string& lowerShort)
{
  return lowerLong == "x" || lowerLong == "abs x" || lowerLong == "rel x" ||
         lowerLong == "abs_x" || lowerLong == "rel_x" ||
         lowerLong == "abs mt position x" || lowerLong == "mt position x" ||
         lowerShort == "x";
}

bool isYValuatorName(const std::string& lowerLong, const std::string& lowerShort)
{
  return lowerLong == "y" || lowerLong == "abs y" || lowerLong == "rel y" ||
         lowerLong == "abs_y" || lowerLong == "rel_y" ||
         lowerLong == "abs mt position y" || lowerLong == "mt position y" ||
         lowerShort == "y";
}

bool isZValuatorName(const std::string& lowerLong, const std::string& lowerShort)
{
  return lowerLong == "z" || lowerLong == "abs z" || lowerLong == "rel z" ||
         lowerLong == "abs_z" || lowerLong == "rel_z" ||
         lowerShort == "z";
}

int scaleValuatorToScreen(int32_t value, int32_t rangeMin, int32_t rangeMax,
                          int screenExtent)
{
  if (screenExtent <= 1)
    return 0;

  if (rangeMax > rangeMin) {
    int32_t clamped = std::max(rangeMin, std::min(value, rangeMax));
    double frac = (static_cast<double>(clamped) -
                   static_cast<double>(rangeMin)) /
                  (static_cast<double>(rangeMax) -
                   static_cast<double>(rangeMin));
    long long scaled = std::llround(frac * static_cast<double>(screenExtent - 1));
    if (scaled < 0)
      return 0;
    if (scaled > screenExtent - 1)
      return screenExtent - 1;
    return static_cast<int>(scaled);
  }

  return std::max(0, std::min(static_cast<int>(value), screenExtent - 1));
}

float scaleValuatorToUnit(int32_t value, int32_t rangeMin, int32_t rangeMax)
{
  if (rangeMax > rangeMin) {
    int32_t clamped = std::max(rangeMin, std::min(value, rangeMax));
    double frac = (static_cast<double>(clamped) -
                   static_cast<double>(rangeMin)) /
                  (static_cast<double>(rangeMax) -
                   static_cast<double>(rangeMin));
    if (frac < 0.0)
      frac = 0.0;
    if (frac > 1.0)
      frac = 1.0;
    return static_cast<float>(frac);
  }
  return value > 0 ? 1.0f : 0.0f;
}

int clampScreenCoord(double coord, int screenExtent)
{
  if (screenExtent <= 1)
    return 0;
  long long rounded = std::llround(coord);
  if (rounded < 0)
    return 0;
  if (rounded > screenExtent - 1)
    return screenExtent - 1;
  return static_cast<int>(rounded);
}

uint32_t mapGiiModifiersToAndroidMeta(uint32_t modifiers)
{
  uint32_t meta = 0;
  if (modifiers & 0x01u)
    meta |= kMetaShiftOn | kMetaShiftLeftOn;
  if (modifiers & 0x02u)
    meta |= kMetaCtrlOn | kMetaCtrlLeftOn;
  if (modifiers & 0x04u)
    meta |= kMetaAltOn | kMetaAltLeftOn;
  if (modifiers & (0x08u | 0x10u))
    meta |= kMetaMetaOn | kMetaMetaLeftOn;
  return meta;
}

uint32_t mapGiiSymbolToKeysym(uint32_t symbol, uint32_t label)
{
  uint32_t sym = symbol ? symbol : label;
  if (sym == 0)
    return 0;

  switch (sym) {
  case 0x08: return XK_BackSpace;
  case 0x09: return XK_Tab;
  case 0x0A:
  case 0x0D: return XK_Return;
  case 0x1B: return XK_Escape;
  case 0x7F: return XK_Delete;
  default:   break;
  }

  if ((sym >= 0x0020 && sym <= 0x00ff) ||
      mapKeysymToAndroidKeycode(sym, nullptr) != 0)
    return sym;

  if (sym <= 0x10ffffu)
    return 0x01000000u | sym;

  return 0;
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
    scrollRange_(16.0f),
    nextGiiOrigin_(1)
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
  disconnect();
  giiDevices_.clear();
}

void ScrcpyControl::disconnect()
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
    // The caller may still be updating a device's input state.
    disconnect();
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
      disconnect();
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

uint32_t ScrcpyControl::giiMouseButtons() const
{
  uint32_t btns = 0;
  for (const auto& entry : giiDevices_)
    btns |= entry.second.mouseButtons;
  return btns;
}

uint32_t ScrcpyControl::combinedMouseButtons() const
{
  return androidButtons_ | giiMouseButtons();
}

bool ScrcpyControl::anyGiiTouchDown() const
{
  for (const auto& entry : giiDevices_) {
    if (entry.second.touchDown)
      return true;
  }
  return false;
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
  uint32_t giiBtns = giiMouseButtons();
  uint32_t effectiveButtons = newButtons | giiBtns;
  uint32_t pressed = newButtons & ~androidButtons_;
  uint32_t released = androidButtons_ & ~newButtons;

  if (pressed == 0 && released == 0) {
    if (clampedX != lastX_ || clampedY != lastY_) {
      if (effectiveButtons != 0) {
        sendTouchEvent(kMotionActionMove, kPointerIdMouse, clampedX, clampedY, w, h,
                       1.0f, 0, effectiveButtons);
      } else if (!anyGiiTouchDown()) {
        sendTouchEvent(kMotionActionHoverMove, kPointerIdMouse, clampedX, clampedY, w, h,
                       0.0f, 0, 0);
      }
    }
  } else {
    static const uint32_t kButtonOrder[] = {
      kAndroidButtonPrimary,
      kAndroidButtonSecondary,
      kAndroidButtonTertiary,
      kAndroidButtonBack,
      kAndroidButtonForward,
    };
    uint32_t cur = androidButtons_ | giiBtns;
    for (uint32_t bit : kButtonOrder) {
      if (pressed & bit) {
        bool wasHeldByGii = (cur & bit) != 0;
        cur |= bit;
        if (!wasHeldByGii) {
          sendTouchEvent(kMotionActionDown, kPointerIdMouse,
                         clampedX, clampedY, w, h, 1.0f, bit, cur);
        }
      }
      if (released & bit) {
        if (!(giiBtns & bit)) {
          cur &= ~bit;
          sendTouchEvent(kMotionActionUp, kPointerIdMouse,
                         clampedX, clampedY, w, h, 0.0f, bit, cur);
        }
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
    sendScrollEvent(clampedX, clampedY, w, h, hscroll, vscroll, effectiveButtons);

  lastVncMask_ = buttonMask;
  lastX_ = clampedX;
  lastY_ = clampedY;
}

void ScrcpyControl::releasePointer(int screenW, int screenH)
{
  handleVncPointer(lastX_, lastY_, 0, screenW, screenH);
}

void ScrcpyControl::injectKey(uint32_t keysym, bool down, uint32_t repeat,
                              uint32_t extraMeta)
{
  bool shiftOverride = false;
  uint32_t androidKey = mapKeysymToAndroidKeycode(keysym, &shiftOverride);
  if (androidKey != 0) {
    uint32_t meta = normalizeCombinedMeta(metaState_ | extraMeta);
    if (shiftOverride)
      meta |= kMetaShiftOn | kMetaShiftLeftOn;
    sendKeycode(down ? kKeyActionDown : kKeyActionUp, androidKey, repeat, meta);
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

  injectKey(keysym, down, 0, 0);
}

uint32_t ScrcpyControl::createGIIDevice(const rfb::GIIDevice& device)
{
  if (fd_ < 0 || giiDevices_.size() >= kMaxTotalGIIDevices)
    return 0;

  while (nextGiiOrigin_ == 0 || giiDevices_.count(nextGiiOrigin_) != 0)
    ++nextGiiOrigin_;
  uint32_t origin = nextGiiOrigin_++;

  GIIDeviceState state;
  state.pointerId = kPointerIdGiiBase | static_cast<uint64_t>(origin);
  state.canGenerate = device.canGenerate;
  state.numButtons = device.numButtons;
  state.valuators.resize(device.valuators.size());

  for (size_t i = 0; i < device.valuators.size(); ++i) {
    const rfb::GIIValuator& val = device.valuators[i];
    GIIValuatorState& vs = state.valuators[i];
    vs.rangeMin = val.rangeMin;
    vs.rangeMax = val.rangeMax;

    std::string lowerLong = toLowerAscii(val.longName);
    std::string lowerShort = toLowerAscii(val.shortName);

    if (isXValuatorName(lowerLong, lowerShort)) {
      vs.role = GIIValuatorRole::X;
      if (state.xValIdx == -1)
        state.xValIdx = static_cast<int>(i);
    } else if (isYValuatorName(lowerLong, lowerShort)) {
      vs.role = GIIValuatorRole::Y;
      if (state.yValIdx == -1)
        state.yValIdx = static_cast<int>(i);
    } else if (val.siUnit == rfb::giiUnitPressure ||
               val.siUnit == rfb::giiUnitForce ||
               containsSubstr(lowerLong, "pressure") ||
               lowerShort == "prs") {
      vs.role = GIIValuatorRole::Pressure;
      if (state.pressureValIdx == -1)
        state.pressureValIdx = static_cast<int>(i);
    } else if (containsSubstr(lowerLong, "tilt x") ||
               containsSubstr(lowerLong, "tilt_x") ||
               lowerShort == "tltx") {
      vs.role = GIIValuatorRole::TiltX;
    } else if (containsSubstr(lowerLong, "tilt y") ||
               containsSubstr(lowerLong, "tilt_y") ||
               lowerShort == "tlty") {
      vs.role = GIIValuatorRole::TiltY;
    } else if ((containsSubstr(lowerLong, "horiz") &&
                (containsSubstr(lowerLong, "wheel") ||
                 containsSubstr(lowerLong, "scroll"))) ||
               containsSubstr(lowerLong, "hwheel") ||
               containsSubstr(lowerLong, "hscroll")) {
      vs.role = GIIValuatorRole::WheelH;
    } else if (containsSubstr(lowerLong, "wheel") ||
               containsSubstr(lowerLong, "scroll") ||
               lowerShort == "whl") {
      vs.role = GIIValuatorRole::WheelV;
    } else if (isZValuatorName(lowerLong, lowerShort)) {
      vs.role = GIIValuatorRole::Z;
    }
  }

  if (state.xValIdx == -1 && state.yValIdx == -1 &&
      device.valuators.size() >= 2 &&
      state.valuators[0].role == GIIValuatorRole::Unknown &&
      state.valuators[1].role == GIIValuatorRole::Unknown &&
      device.valuators[0].longName.empty() &&
      device.valuators[0].shortName.empty() &&
      device.valuators[1].longName.empty() &&
      device.valuators[1].shortName.empty()) {
    state.valuators[0].role = GIIValuatorRole::X;
    state.valuators[1].role = GIIValuatorRole::Y;
    state.xValIdx = 0;
    state.yValIdx = 1;
  }

  giiDevices_.emplace(origin, std::move(state));
  return origin;
}

void ScrcpyControl::destroyGIIDevice(uint32_t origin, int screenW, int screenH)
{
  auto it = giiDevices_.find(origin);
  if (it == giiDevices_.end())
    return;

  GIIDeviceState dev = std::move(it->second);
  giiDevices_.erase(it);

  if (fd_ < 0)
    return;

  if (screenW > 0 && screenH > 0) {
    int x = dev.hasPosition ? clampScreenCoord(dev.posX, screenW)
                            : std::max(0, std::min(screenW / 2, screenW - 1));
    int y = dev.hasPosition ? clampScreenCoord(dev.posY, screenH)
                            : std::max(0, std::min(screenH / 2, screenH - 1));
    uint16_t w = static_cast<uint16_t>(screenW);
    uint16_t h = static_cast<uint16_t>(screenH);

    if (dev.touchDown) {
      sendTouchEvent(kMotionActionUp, dev.pointerId, x, y, w, h,
                     0.0f, kAndroidButtonPrimary, 0);
    }

    if (dev.mouseButtons != 0) {
      static const uint32_t kMouseButtonOrder[] = {
        kAndroidButtonSecondary,
        kAndroidButtonTertiary,
        kAndroidButtonBack,
        kAndroidButtonForward,
      };
      for (uint32_t bit : kMouseButtonOrder) {
        if (dev.mouseButtons & bit) {
          dev.mouseButtons &= ~bit;
          uint32_t rem = dev.mouseButtons | combinedMouseButtons();
          if (!(rem & bit)) {
            sendTouchEvent(kMotionActionUp, kPointerIdMouse, x, y, w, h,
                           0.0f, bit, rem);
            lastX_ = x;
            lastY_ = y;
          }
        }
      }
    }
  }

  for (const auto& key : dev.pressedKeys)
    injectKey(key.second, false, 0, 0);
}

void ScrcpyControl::emitGIIMotion(GIIDeviceState& dev, int x, int y,
                                  uint16_t w, uint16_t h,
                                  bool posChanged, bool /*pressureChanged*/)
{
  if (dev.touchDown) {
    float p = (dev.pressureValIdx >= 0 && dev.hasPressureSample)
              ? dev.pressure : 1.0f;
    if (x != dev.lastSentX || y != dev.lastSentY ||
        floatToU16Fp(p) != floatToU16Fp(dev.lastSentPressure)) {
      sendTouchEvent(kMotionActionMove, dev.pointerId, x, y, w, h,
                     p, 0, kAndroidButtonPrimary | dev.mouseButtons);
      dev.lastSentX = x;
      dev.lastSentY = y;
      dev.lastSentPressure = p;
    }
  } else if (dev.mouseButtons != 0) {
    if (posChanged && (x != lastX_ || y != lastY_)) {
      sendTouchEvent(kMotionActionMove, kPointerIdMouse, x, y, w, h,
                     1.0f, 0, combinedMouseButtons());
      lastX_ = x;
      lastY_ = y;
    }
  } else if (posChanged && combinedMouseButtons() == 0 && !anyGiiTouchDown()) {
    if (!dev.hasHoverSent || x != lastX_ || y != lastY_) {
      sendTouchEvent(kMotionActionHoverMove, kPointerIdMouse, x, y, w, h,
                     0.0f, 0, 0);
      dev.lastSentX = x;
      dev.lastSentY = y;
      dev.hasHoverSent = true;
      lastX_ = x;
      lastY_ = y;
    }
  }
}

void ScrcpyControl::handleGIIKeyEvent(GIIDeviceState& dev,
                                      const rfb::GIIEvent& event)
{
  uint32_t keysym = mapGiiSymbolToKeysym(event.symbol, event.label);
  uint32_t extraMeta = mapGiiModifiersToAndroidMeta(event.modifiers);
  auto lookup = std::make_pair(event.button != 0,
                               event.button ? event.button : keysym);

  if (event.type == rfb::giiEventKeyPress) {
    if (keysym == 0 || dev.pressedKeys.count(lookup) != 0 ||
        dev.pressedKeys.size() >= 256)
      return;
    dev.pressedKeys.emplace(lookup, keysym);
    injectKey(keysym, true, 0, extraMeta);
  } else if (event.type == rfb::giiEventKeyRepeat) {
    auto it = dev.pressedKeys.find(lookup);
    if (it != dev.pressedKeys.end())
      injectKey(it->second, true, 1, extraMeta);
  } else if (event.type == rfb::giiEventKeyRelease) {
    auto it = dev.pressedKeys.find(lookup);
    if (it == dev.pressedKeys.end())
      return;
    uint32_t useSym = it->second;
    dev.pressedKeys.erase(it);
    injectKey(useSym, false, 0, extraMeta);
  }
}

void ScrcpyControl::handleGIIButtonEvent(GIIDeviceState& dev,
                                         const rfb::GIIEvent& event,
                                         int screenW, int screenH)
{
  if (!dev.hasPosition) {
    dev.posX = std::max(0, std::min(screenW / 2, screenW - 1));
    dev.posY = std::max(0, std::min(screenH / 2, screenH - 1));
    dev.hasPosition = true;
  }

  int x = clampScreenCoord(dev.posX, screenW);
  int y = clampScreenCoord(dev.posY, screenH);
  uint16_t w = static_cast<uint16_t>(screenW);
  uint16_t h = static_cast<uint16_t>(screenH);
  bool press = (event.type == rfb::giiEventPtrButtonPress);

  if (event.button == 1) {
    if (press) {
      if (!dev.touchDown) {
        dev.touchDown = true;
        float p = (dev.pressureValIdx >= 0 && dev.hasPressureSample &&
                   dev.pressure > 0.0f)
                  ? dev.pressure : 1.0f;
        sendTouchEvent(kMotionActionDown, dev.pointerId, x, y, w, h, p,
                       kAndroidButtonPrimary,
                       kAndroidButtonPrimary | dev.mouseButtons);
        dev.lastSentX = x;
        dev.lastSentY = y;
        dev.lastSentPressure = p;
      }
    } else {
      if (dev.touchDown) {
        dev.touchDown = false;
        sendTouchEvent(kMotionActionUp, dev.pointerId, x, y, w, h, 0.0f,
                       kAndroidButtonPrimary, dev.mouseButtons);
        dev.lastSentX = x;
        dev.lastSentY = y;
        dev.lastSentPressure = 0.0f;
      }
    }
    return;
  }

  uint32_t bit = 0;
  switch (event.button) {
  case 2: bit = kAndroidButtonSecondary; break;
  case 3: bit = kAndroidButtonTertiary; break;
  case 8: bit = kAndroidButtonBack; break;
  case 9: bit = kAndroidButtonForward; break;
  default: break;
  }

  if (bit != 0) {
    if (press) {
      if (!(dev.mouseButtons & bit)) {
        bool wasHeld = (combinedMouseButtons() & bit) != 0;
        dev.mouseButtons |= bit;
        if (!wasHeld) {
          sendTouchEvent(kMotionActionDown, kPointerIdMouse, x, y, w, h,
                         1.0f, bit, combinedMouseButtons());
          lastX_ = x;
          lastY_ = y;
        }
      }
    } else {
      if (dev.mouseButtons & bit) {
        dev.mouseButtons &= ~bit;
        uint32_t rem = combinedMouseButtons();
        if (!(rem & bit)) {
          sendTouchEvent(kMotionActionUp, kPointerIdMouse, x, y, w, h,
                         0.0f, bit, rem);
          lastX_ = x;
          lastY_ = y;
        }
      }
    }
    return;
  }

  if (press && event.button >= 4 && event.button <= 7) {
    float vscroll = 0.0f;
    float hscroll = 0.0f;
    if (event.button == 4)
      vscroll = 1.0f;
    else if (event.button == 5)
      vscroll = -1.0f;
    else if (event.button == 6)
      hscroll = -1.0f;
    else if (event.button == 7)
      hscroll = 1.0f;
    sendScrollEvent(x, y, w, h, hscroll, vscroll, combinedMouseButtons());
  }
}

void ScrcpyControl::handleGIIValuatorEvent(GIIDeviceState& dev,
                                           const rfb::GIIEvent& event,
                                           int screenW, int screenH)
{
  if (event.values.empty() || event.first >= dev.valuators.size() ||
      event.values.size() > dev.valuators.size() - event.first)
    return;

  bool isAbsolute = (event.type == rfb::giiEventValuatorAbsolute);
  if (!dev.hasPosition) {
    dev.posX = std::max(0, std::min(screenW / 2, screenW - 1));
    dev.posY = std::max(0, std::min(screenH / 2, screenH - 1));
    dev.hasPosition = true;
  }

  bool posChanged = false;
  bool pressureChanged = false;
  float hscroll = 0.0f;
  float vscroll = 0.0f;

  for (size_t i = 0; i < event.values.size(); ++i) {
    GIIValuatorState& vs = dev.valuators[static_cast<size_t>(event.first) + i];
    int32_t val = event.values[i];

    switch (vs.role) {
    case GIIValuatorRole::X:
      if (isAbsolute) {
        dev.posX = scaleValuatorToScreen(val, vs.rangeMin, vs.rangeMax, screenW);
        dev.hasPosition = true;
      } else {
        dev.posX = std::max(0.0,
                            std::min(dev.posX + static_cast<double>(val),
                                     static_cast<double>(screenW - 1)));
      }
      posChanged = true;
      break;

    case GIIValuatorRole::Y:
      if (isAbsolute) {
        dev.posY = scaleValuatorToScreen(val, vs.rangeMin, vs.rangeMax, screenH);
        dev.hasPosition = true;
      } else {
        dev.posY = std::max(0.0,
                            std::min(dev.posY + static_cast<double>(val),
                                     static_cast<double>(screenH - 1)));
      }
      posChanged = true;
      break;

    case GIIValuatorRole::Pressure:
      if (isAbsolute) {
        dev.pressure = scaleValuatorToUnit(val, vs.rangeMin, vs.rangeMax);
      } else if (vs.rangeMax > vs.rangeMin) {
        double delta = static_cast<double>(val) /
                       (static_cast<double>(vs.rangeMax) -
                        static_cast<double>(vs.rangeMin));
        dev.pressure = std::max(0.0f,
                                std::min(1.0f,
                                         dev.pressure + static_cast<float>(delta)));
      }
      dev.hasPressureSample = true;
      pressureChanged = true;
      break;

    case GIIValuatorRole::WheelV:
      if (isAbsolute) {
        if (vs.hasPrev)
          vscroll += static_cast<float>(static_cast<int64_t>(val) -
                                        static_cast<int64_t>(vs.prevValue));
      } else {
        vscroll += static_cast<float>(val);
      }
      break;

    case GIIValuatorRole::WheelH:
      if (isAbsolute) {
        if (vs.hasPrev)
          hscroll += static_cast<float>(static_cast<int64_t>(val) -
                                        static_cast<int64_t>(vs.prevValue));
      } else {
        hscroll += static_cast<float>(val);
      }
      break;

    default:
      break;
    }

    vs.prevValue = val;
    vs.hasPrev = true;
  }

  int x = clampScreenCoord(dev.posX, screenW);
  int y = clampScreenCoord(dev.posY, screenH);
  uint16_t w = static_cast<uint16_t>(screenW);
  uint16_t h = static_cast<uint16_t>(screenH);

  bool buttonlessPressure =
    (dev.pressureValIdx >= 0) &&
    ((dev.numButtons == 0) ||
     !(dev.canGenerate & rfb::giiEventMaskPtrButtonPress));

  if (buttonlessPressure && pressureChanged) {
    if (!dev.touchDown && dev.pressure > 0.0f) {
      dev.touchDown = true;
      sendTouchEvent(kMotionActionDown, dev.pointerId, x, y, w, h,
                     dev.pressure, kAndroidButtonPrimary,
                     kAndroidButtonPrimary | dev.mouseButtons);
      dev.lastSentX = x;
      dev.lastSentY = y;
      dev.lastSentPressure = dev.pressure;
    } else if (dev.touchDown && dev.pressure <= 0.0f) {
      dev.touchDown = false;
      sendTouchEvent(kMotionActionUp, dev.pointerId, x, y, w, h, 0.0f,
                     kAndroidButtonPrimary, dev.mouseButtons);
      dev.lastSentX = x;
      dev.lastSentY = y;
      dev.lastSentPressure = 0.0f;
    } else {
      emitGIIMotion(dev, x, y, w, h, posChanged, pressureChanged);
    }
  } else {
    emitGIIMotion(dev, x, y, w, h, posChanged, pressureChanged);
  }

  if (hscroll != 0.0f || vscroll != 0.0f)
    sendScrollEvent(x, y, w, h, hscroll, vscroll, combinedMouseButtons());
}

void ScrcpyControl::handleGIIPtrMoveEvent(GIIDeviceState& dev,
                                          const rfb::GIIEvent& event,
                                          int screenW, int screenH)
{
  if (event.type == rfb::giiEventPtrAbsolute) {
    dev.posX = std::max(0.0,
                        std::min(static_cast<double>(event.x),
                                 static_cast<double>(screenW - 1)));
    dev.posY = std::max(0.0,
                        std::min(static_cast<double>(event.y),
                                 static_cast<double>(screenH - 1)));
    dev.hasPosition = true;
  } else {
    if (!dev.hasPosition) {
      dev.posX = std::max(0, std::min(screenW / 2, screenW - 1));
      dev.posY = std::max(0, std::min(screenH / 2, screenH - 1));
      dev.hasPosition = true;
    }
    dev.posX = std::max(0.0,
                        std::min(dev.posX + static_cast<double>(event.x),
                                 static_cast<double>(screenW - 1)));
    dev.posY = std::max(0.0,
                        std::min(dev.posY + static_cast<double>(event.y),
                                 static_cast<double>(screenH - 1)));
  }

  int x = clampScreenCoord(dev.posX, screenW);
  int y = clampScreenCoord(dev.posY, screenH);
  uint16_t w = static_cast<uint16_t>(screenW);
  uint16_t h = static_cast<uint16_t>(screenH);

  emitGIIMotion(dev, x, y, w, h, true, false);

  if (event.type == rfb::giiEventPtrRelative) {
    if (event.wheel != 0)
      sendScrollEvent(x, y, w, h, 0.0f, static_cast<float>(event.wheel),
                      combinedMouseButtons());
  } else {
    if (dev.hasPtrWheel && event.wheel != dev.lastPtrWheel) {
      float delta = static_cast<float>(static_cast<int64_t>(event.wheel) -
                                       static_cast<int64_t>(dev.lastPtrWheel));
      sendScrollEvent(x, y, w, h, 0.0f, delta, combinedMouseButtons());
    }
    dev.lastPtrWheel = event.wheel;
    dev.hasPtrWheel = true;
  }
}

void ScrcpyControl::handleGIIEvent(const rfb::GIIEvent& event,
                                   int screenW, int screenH)
{
  if (fd_ < 0)
    return;

  auto it = giiDevices_.find(event.origin);
  if (it == giiDevices_.end())
    return;

  GIIDeviceState& dev = it->second;

  if (rfb::giiIsKeyEvent(event.type)) {
    handleGIIKeyEvent(dev, event);
    return;
  }

  if (screenW <= 0 || screenH <= 0)
    return;

  switch (event.type) {
  case rfb::giiEventPtrButtonPress:
  case rfb::giiEventPtrButtonRelease:
    handleGIIButtonEvent(dev, event, screenW, screenH);
    break;
  case rfb::giiEventValuatorRelative:
  case rfb::giiEventValuatorAbsolute:
    handleGIIValuatorEvent(dev, event, screenW, screenH);
    break;
  case rfb::giiEventPtrRelative:
  case rfb::giiEventPtrAbsolute:
    handleGIIPtrMoveEvent(dev, event, screenW, screenH);
    break;
  default:
    break;
  }
}
