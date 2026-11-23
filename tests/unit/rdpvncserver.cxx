/* Copyright (C) 2026 TigerVNC Team
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

#include <gtest/gtest.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <freerdp/input.h>
#include <core/Configuration.h>
#include <core/Timer.h>
#include <network/UnixSocket.h>
#include <rdr/FdOutStream.h>
#include <rfb/SDesktop.h>
#include <rfb/VNCServerST.h>
#include <rfb/encodings.h>
#include <rfb/screenTypes.h>

#include "KeyMap.h"
#include "RDPDesktop.h"
#include "RdpH264Passthrough.h"
#include "RdpPixelBuffer.h"

using namespace rdpvnc;

namespace {

uint32_t readU32BE(const uint8_t* p)
{
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) |
         static_cast<uint32_t>(p[3]);
}

int32_t readS32BE(const uint8_t* p)
{
  return static_cast<int32_t>(readU32BE(p));
}

uint16_t readU16BE(const uint8_t* p)
{
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

void writeU16BE(std::vector<uint8_t>& buf, uint16_t v)
{
  buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

void writeU32BE(std::vector<uint8_t>& buf, uint32_t v)
{
  buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

void writeS32BE(std::vector<uint8_t>& buf, int32_t v)
{
  writeU32BE(buf, static_cast<uint32_t>(v));
}

// Helper client driving one RFB socket against an in-process VNCServerST.
class TestRfbClient {
public:
  explicit TestRfbClient(rfb::VNCServerST& server)
    : server_(server), clientFd_(-1), serverSock_(nullptr)
  {
    int fds[2]{-1, -1};
    EXPECT_EQ(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    fcntl(fds[1], F_SETFL, O_NONBLOCK);
    clientFd_ = fds[1];
    serverSock_ = new network::UnixSocket(fds[0]);
    server_.addSocket(serverSock_);
    EXPECT_TRUE(handshake());
  }

  ~TestRfbClient()
  {
    closeConnection();
  }

  void closeConnection()
  {
    if (serverSock_) {
      server_.removeSocket(serverSock_);
      delete serverSock_;
      serverSock_ = nullptr;
    }
    if (clientFd_ >= 0) {
      ::close(clientFd_);
      clientFd_ = -1;
    }
  }

  bool isClosedByServer()
  {
    pump();
    return serverSock_ == nullptr || serverSock_->isShutdownWrite() ||
           serverSock_->isShutdownRead() || eof_;
  }

  void pump()
  {
    core::Timer::checkTimeouts();
    for (int iter = 0; iter < 16; ++iter) {
      bool didWork = false;
      core::Timer::checkTimeouts();
      if (serverSock_ && !serverSock_->isShutdownRead()) {
        struct pollfd pfd{};
        pfd.fd = serverSock_->getFd();
        pfd.events = POLLIN;
        if (::poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
          server_.processSocketReadEvent(serverSock_);
          didWork = true;
        }
        if (!serverSock_->isShutdownRead() &&
            serverSock_->outStream().hasBufferedData()) {
          server_.processSocketWriteEvent(serverSock_);
          didWork = true;
        }
      }
      if (clientFd_ >= 0) {
        uint8_t tmp[4096];
        ssize_t n = ::read(clientFd_, tmp, sizeof(tmp));
        if (n > 0) {
          rx_.insert(rx_.end(), tmp, tmp + n);
          didWork = true;
        } else if (n == 0) {
          eof_ = true;
        }
      }
      if (!didWork)
        break;
    }
  }

  bool waitBytes(size_t needed, int maxIters = 100)
  {
    for (int i = 0; i < maxIters && rx_.size() < needed; ++i) {
      pump();
      if (rx_.size() < needed)
        usleep(1000);
    }
    return rx_.size() >= needed;
  }

  std::vector<uint8_t> readBytes(size_t count)
  {
    if (!waitBytes(count))
      throw std::runtime_error("Timed out reading RFB test response");
    std::vector<uint8_t> out(rx_.begin(), rx_.begin() + count);
    rx_.erase(rx_.begin(), rx_.begin() + count);
    return out;
  }

  void sendBytes(const uint8_t* data, size_t len)
  {
    size_t off = 0;
    while (off < len) {
      ssize_t n = ::write(clientFd_, data + off, len - off);
      if (n > 0) {
        off += static_cast<size_t>(n);
      } else {
        pump();
      }
    }
    pump();
  }

  void sendSetEncodings(const std::vector<int32_t>& encodings)
  {
    std::vector<uint8_t> msg;
    msg.push_back(2); // SetEncodings
    msg.push_back(0);
    writeU16BE(msg, static_cast<uint16_t>(encodings.size()));
    for (int32_t enc : encodings)
      writeS32BE(msg, enc);
    sendBytes(msg.data(), msg.size());
  }

  void sendSetPixelFormat32Bgrx()
  {
    std::vector<uint8_t> msg(20, 0);
    msg[0] = 0; // SetPixelFormat
    msg[4] = 32; // bpp
    msg[5] = 24; // depth
    msg[6] = 0;  // big-endian = false
    msg[7] = 1;  // true-colour = true
    msg[8] = 0; msg[9] = 255;   // redMax
    msg[10] = 0; msg[11] = 255; // greenMax
    msg[12] = 0; msg[13] = 255; // blueMax
    msg[14] = 16; // redShift
    msg[15] = 8;  // greenShift
    msg[16] = 0;  // blueShift
    sendBytes(msg.data(), msg.size());
  }

  void sendSetPixelFormat16Rgb565()
  {
    std::vector<uint8_t> msg(20, 0);
    msg[0] = 0;
    msg[4] = 16;
    msg[5] = 16;
    msg[6] = 0;
    msg[7] = 1;
    msg[8] = 0; msg[9] = 31;
    msg[10] = 0; msg[11] = 63;
    msg[12] = 0; msg[13] = 31;
    msg[14] = 11;
    msg[15] = 5;
    msg[16] = 0;
    sendBytes(msg.data(), msg.size());
  }

  void sendFbUpdateRequest(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool incremental)
  {
    std::vector<uint8_t> msg;
    msg.push_back(3);
    msg.push_back(incremental ? 1 : 0);
    writeU16BE(msg, x);
    writeU16BE(msg, y);
    writeU16BE(msg, w);
    writeU16BE(msg, h);
    sendBytes(msg.data(), msg.size());
  }

  void sendSetDesktopSize(uint16_t w, uint16_t h)
  {
    std::vector<uint8_t> msg;
    msg.push_back(251); // SetDesktopSize
    msg.push_back(0);
    writeU16BE(msg, w);
    writeU16BE(msg, h);
    msg.push_back(1); // 1 screen
    msg.push_back(0);
    writeU32BE(msg, 1);
    writeU16BE(msg, 0);
    writeU16BE(msg, 0);
    writeU16BE(msg, w);
    writeU16BE(msg, h);
    writeU32BE(msg, 0);
    sendBytes(msg.data(), msg.size());
  }

  struct WireRect {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
    int32_t encoding;
    std::vector<uint8_t> data;
  };

  std::vector<WireRect> readNextFramebufferUpdate(int bpp = 32)
  {
    std::vector<WireRect> rects;
    while (true) {
      auto typeBuf = readBytes(1);
      if (typeBuf[0] == 4) { // Fence
        auto hdr = readBytes(7);
        (void)hdr;
        continue;
      }
      if (typeBuf[0] == 150) { // EndOfContinuousUpdates
        continue;
      }
      EXPECT_EQ(0, typeBuf[0]); // FramebufferUpdate
      auto hdr = readBytes(3);
      uint16_t nRects = readU16BE(&hdr[1]);
      for (uint32_t i = 0; nRects == 0xFFFF || i < nRects; ++i) {
        auto rh = readBytes(12);
        WireRect r;
        r.x = readU16BE(&rh[0]);
        r.y = readU16BE(&rh[2]);
        r.w = readU16BE(&rh[4]);
        r.h = readU16BE(&rh[6]);
        r.encoding = readS32BE(&rh[8]);
        if (r.encoding == rfb::pseudoEncodingLastRect)
          break;
        if (r.encoding == rfb::encodingCopyRect) {
          r.data = readBytes(4); // srcX, srcY
        } else if (r.encoding == rfb::encodingRaw) {
          r.data = readBytes(static_cast<size_t>(r.w) * r.h * (bpp / 8));
        } else if (r.encoding == rfb::encodingH264) {
          auto hHdr = readBytes(8);
          uint32_t len = readU32BE(&hHdr[0]);
          r.data = hHdr;
          auto payload = readBytes(len);
          r.data.insert(r.data.end(), payload.begin(), payload.end());
        } else if (r.encoding == rfb::encodingTight) {
          r.data = readTightPayload(r.w, r.h, bpp);
        } else if (r.encoding == rfb::encodingZRLE) {
          auto lHdr = readBytes(4);
          uint32_t len = readU32BE(&lHdr[0]);
          r.data = lHdr;
          auto payload = readBytes(len);
          r.data.insert(r.data.end(), payload.begin(), payload.end());
        } else if (r.encoding == rfb::pseudoEncodingExtendedDesktopSize) {
          auto edsHdr = readBytes(4);
          uint8_t numScreens = edsHdr[0];
          r.data = edsHdr;
          auto screens = readBytes(static_cast<size_t>(numScreens) * 16);
          r.data.insert(r.data.end(), screens.begin(), screens.end());
        }
        rects.push_back(std::move(r));
      }
      break;
    }
    return rects;
  }

  size_t bufferedRxSize()
  {
    pump();
    return rx_.size();
  }

private:
  uint32_t readTightCompactLength(std::vector<uint8_t>& rawOut)
  {
    uint32_t len = 0;
    auto b0 = readBytes(1);
    rawOut.push_back(b0[0]);
    len = b0[0] & 0x7FU;
    if (b0[0] & 0x80U) {
      auto b1 = readBytes(1);
      rawOut.push_back(b1[0]);
      len |= static_cast<uint32_t>(b1[0] & 0x7FU) << 7;
      if (b1[0] & 0x80U) {
        auto b2 = readBytes(1);
        rawOut.push_back(b2[0]);
        len |= static_cast<uint32_t>(b2[0]) << 14;
      }
    }
    return len;
  }

  std::vector<uint8_t> readTightPayload(uint16_t w, uint16_t h, int bpp)
  {
    std::vector<uint8_t> out = readBytes(1);
    uint8_t ctrl = out[0];
    uint8_t compType = (ctrl >> 4) & 0x0FU;
    if (compType == 0x08) { // FillCompression
      int bytes = (bpp == 32) ? 3 : (bpp / 8); // Tight TPIXEL uses 3 bytes for 24-bit depth
      auto pix = readBytes(bytes);
      out.insert(out.end(), pix.begin(), pix.end());
      return out;
    }
    if (compType == 0x09) { // JPEGCompression
      uint32_t len = readTightCompactLength(out);
      auto jpeg = readBytes(len);
      out.insert(out.end(), jpeg.begin(), jpeg.end());
      return out;
    }
    // BasicCompression
    size_t pixelBytes = 3; // TPIXEL for 24-bit depth
    uint8_t streamFlags = ctrl & 0x0F;
    (void)streamFlags;
    if (ctrl & 0x40) { // read filter id
      auto f = readBytes(1);
      out.push_back(f[0]);
      if (f[0] == 1) { // PaletteFilter
        auto numCols = readBytes(1);
        out.push_back(numCols[0]);
        int nColors = static_cast<int>(numCols[0]) + 1;
        auto pal = readBytes(static_cast<size_t>(nColors) * pixelBytes);
        out.insert(out.end(), pal.begin(), pal.end());
        size_t rowBytes = (nColors <= 2) ? ((w + 7) / 8) : w;
        size_t uncomp = rowBytes * h;
        if (uncomp >= 12) {
          uint32_t len = readTightCompactLength(out);
          auto z = readBytes(len);
          out.insert(out.end(), z.begin(), z.end());
        } else {
          auto raw = readBytes(uncomp);
          out.insert(out.end(), raw.begin(), raw.end());
        }
        return out;
      }
    }
    size_t uncomp = static_cast<size_t>(w) * h * pixelBytes;
    if (uncomp >= 12) {
      uint32_t len = readTightCompactLength(out);
      auto z = readBytes(len);
      out.insert(out.end(), z.begin(), z.end());
    } else {
      auto raw = readBytes(uncomp);
      out.insert(out.end(), raw.begin(), raw.end());
    }
    return out;
  }

  bool handshake()
  {
    auto ver = readBytes(12);
    if (ver.size() != 12)
      return false;
    sendBytes(reinterpret_cast<const uint8_t*>("RFB 003.008\n"), 12);

    auto nSec = readBytes(1);
    if (nSec.empty() || nSec[0] == 0)
      return false;
    auto secTypes = readBytes(nSec[0]);
    bool hasNone = false;
    for (uint8_t s : secTypes) {
      if (s == 1)
        hasNone = true;
    }
    if (!hasNone)
      return false;
    uint8_t chosen = 1; // secTypeNone
    sendBytes(&chosen, 1);

    auto secResult = readBytes(4);
    if (readU32BE(secResult.data()) != 0)
      return false;

    uint8_t shared = 1;
    sendBytes(&shared, 1);

    auto serverInit = readBytes(24);
    uint32_t nameLen = readU32BE(&serverInit[20]);
    (void)readBytes(nameLen);
    return true;
  }

  rfb::VNCServerST& server_;
  int clientFd_;
  network::UnixSocket* serverSock_;
  std::vector<uint8_t> rx_;
  bool eof_ = false;
};

class MockDesktop : public rfb::SDesktop {
public:
  MockDesktop(int w, int h)
    : pb_(w, h), server_(nullptr)
  {
  }

  void init(rfb::VNCServer* vs) override
  {
    server_ = vs;
    server_->setPixelBuffer(&pb_);
  }

  void start() override { started_ = true; }
  void stop() override { started_ = false; }
  void queryConnection(rfb::SConnection* conn) override
  {
    if (server_)
      server_->approveConnection(conn, true, nullptr);
  }
  void terminate() override {}

  unsigned int setScreenLayout(int fb_width, int fb_height,
                               const rfb::ScreenSet& layout) override
  {
    lastRequestedW_ = fb_width;
    lastRequestedH_ = fb_height;
    lastRequestedLayout_ = layout;
    ++setScreenLayoutCalls_;
    return nextLayoutResult_;
  }

  void clientEncodingsChanged() override { ++encodingsChangedCalls_; }

  RdpPixelBuffer pb_;
  rfb::VNCServer* server_;
  bool started_ = false;
  unsigned int nextLayoutResult_ = rfb::resultSuccess;
  int lastRequestedW_ = 0;
  int lastRequestedH_ = 0;
  rfb::ScreenSet lastRequestedLayout_;
  int setScreenLayoutCalls_ = 0;
  int encodingsChangedCalls_ = 0;
};

std::vector<uint8_t> makeSampleJfif(int w, int h)
{
  std::vector<uint8_t> jfif = {
    0xFF, 0xD8, // SOI
    0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, // APP0 JFIF
    0xFF, 0xC0, 0x00, 0x0B, 0x08, static_cast<uint8_t>(h >> 8), static_cast<uint8_t>(h & 0xFF),
    static_cast<uint8_t>(w >> 8), static_cast<uint8_t>(w & 0xFF), 0x01, 0x01, 0x11, 0x00, // SOF0
    0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00, // SOS
    0x00, // dummy entropy byte
    0xFF, 0xD9 // EOI
  };
  return jfif;
}

} // namespace

TEST(KeyMapTest, QemuExtendedScancodes)
{
  KeyAction aDown = KeyMap::translateKey(0, 0x1E, true);
  EXPECT_EQ(KeyAction::Kind::Scancode, aDown.kind);
  EXPECT_EQ(0x1E, aDown.code);
  EXPECT_EQ(KBD_FLAGS_DOWN, aDown.flags);

  KeyAction aUp = KeyMap::translateKey(0, 0x1E, false);
  EXPECT_EQ(KeyAction::Kind::Scancode, aUp.kind);
  EXPECT_EQ(KBD_FLAGS_RELEASE, aUp.flags);

  // KP_Enter (0x1c | 0x80 == 0x9c)
  KeyAction kpEnter = KeyMap::translateKey(0, 0x9C, true);
  EXPECT_EQ(KeyAction::Kind::Scancode, kpEnter.kind);
  EXPECT_EQ(0x1C, kpEnter.code);
  EXPECT_EQ(static_cast<uint16_t>(KBD_FLAGS_DOWN | KBD_FLAGS_EXTENDED),
            kpEnter.flags);

  // PrintScreen (0x37 | 0x80 == 0xb7)
  KeyAction print = KeyMap::translateKey(0, 0xB7, true);
  EXPECT_EQ(KeyAction::Kind::Scancode, print.kind);
  EXPECT_EQ(0x37, print.code);
  EXPECT_TRUE((print.flags & KBD_FLAGS_EXTENDED) != 0);

  // Pause (0xc6)
  KeyAction pauseDown = KeyMap::translateKey(0, 0xC6, true);
  EXPECT_EQ(KeyAction::Kind::Pause, pauseDown.kind);
  KeyAction pauseUp = KeyMap::translateKey(0, 0xC6, false);
  EXPECT_EQ(KeyAction::Kind::None, pauseUp.kind);
}

TEST(KeyMapTest, LegacyKeysymFallbackAndUnicode)
{
  KeyAction left = KeyMap::translateKey(0xFF51, 0, true);
  EXPECT_EQ(KeyAction::Kind::Scancode, left.kind);
  EXPECT_EQ(0x4B, left.code);
  EXPECT_EQ(static_cast<uint16_t>(KBD_FLAGS_DOWN | KBD_FLAGS_EXTENDED),
            left.flags);

  KeyAction ctrlR = KeyMap::translateKey(0xFFE4, 0, false);
  EXPECT_EQ(KeyAction::Kind::Scancode, ctrlR.kind);
  EXPECT_EQ(0x1D, ctrlR.code);
  EXPECT_EQ(static_cast<uint16_t>(KBD_FLAGS_RELEASE | KBD_FLAGS_EXTENDED),
            ctrlR.flags);

  KeyAction f12 = KeyMap::translateKey(0xFFC9, 0, true);
  EXPECT_EQ(KeyAction::Kind::Scancode, f12.kind);
  EXPECT_EQ(0x58, f12.code);
  EXPECT_EQ(KBD_FLAGS_DOWN, f12.flags);

  // Printable Latin-1 via Unicode
  KeyAction eAcute = KeyMap::translateKey(0x00E9, 0, true);
  EXPECT_EQ(KeyAction::Kind::Unicode, eAcute.kind);
  EXPECT_EQ(0x00E9, eAcute.code);
  EXPECT_EQ(0, eAcute.code2);

  // Non-BMP emoji (U+1F600 -> keysym 0x0101F600) -> UTF-16 surrogates
  KeyAction rocket = KeyMap::translateKey(0x0101F600, 0, true);
  EXPECT_EQ(KeyAction::Kind::Unicode, rocket.kind);
  EXPECT_EQ(0xD83DU, rocket.code);
  EXPECT_EQ(0xDE00U, rocket.code2);
}

TEST(PointerMapTest, ButtonsWheelExtendedAndClamping)
{
  // Middle (bit 1 -> PTR_FLAGS_BUTTON3) + Right (bit 2 -> PTR_FLAGS_BUTTON2)
  auto a1 = KeyMap::translatePointer(100, 200, 0x06, 0x00, 1920, 1080);
  ASSERT_EQ(2U, a1.size());
  EXPECT_EQ(static_cast<uint16_t>(PTR_FLAGS_DOWN | PTR_FLAGS_BUTTON3),
            a1[0].flags);
  EXPECT_EQ(static_cast<uint16_t>(PTR_FLAGS_DOWN | PTR_FLAGS_BUTTON2),
            a1[1].flags);

  // Scroll wheel up/down + negative horizontal scroll left/right
  auto wUp = KeyMap::translatePointer(10, 20, 0x08, 0x00, 800, 600);
  ASSERT_EQ(1U, wUp.size());
  EXPECT_EQ(static_cast<uint16_t>(PTR_FLAGS_WHEEL | 0x0078U), wUp[0].flags);

  // Releasing wheel bit does NOT repeat scroll; emits pure move
  auto wUpRel = KeyMap::translatePointer(10, 20, 0x00, 0x08, 800, 600);
  ASSERT_EQ(1U, wUpRel.size());
  EXPECT_EQ(static_cast<uint16_t>(PTR_FLAGS_MOVE), wUpRel[0].flags);

  auto wDown = KeyMap::translatePointer(10, 20, 0x10, 0x00, 800, 600);
  ASSERT_EQ(1U, wDown.size());
  EXPECT_EQ(static_cast<uint16_t>(PTR_FLAGS_WHEEL | PTR_FLAGS_WHEEL_NEGATIVE |
                                  0x0088U),
            wDown[0].flags);

  auto hwLeft = KeyMap::translatePointer(10, 20, 0x20, 0x00, 800, 600);
  ASSERT_EQ(1U, hwLeft.size());
  EXPECT_EQ(static_cast<uint16_t>(PTR_FLAGS_HWHEEL | PTR_FLAGS_WHEEL_NEGATIVE |
                                  0x0088U),
            hwLeft[0].flags);

  auto hwRight = KeyMap::translatePointer(10, 20, 0x40, 0x00, 800, 600);
  ASSERT_EQ(1U, hwRight.size());
  EXPECT_EQ(static_cast<uint16_t>(PTR_FLAGS_HWHEEL | 0x0078U),
            hwRight[0].flags);

  // Extended X1/X2 buttons + bounds clamping
  auto x1 = KeyMap::translatePointer(-50, 5000, 0x80, 0x00, 800, 600);
  ASSERT_EQ(1U, x1.size());
  EXPECT_TRUE(x1[0].isExtended);
  EXPECT_EQ(0, x1[0].x);
  EXPECT_EQ(599, x1[0].y);
  EXPECT_EQ(static_cast<uint16_t>(PTR_XFLAGS_DOWN | PTR_XFLAGS_BUTTON1),
            x1[0].flags);
}

TEST(ClipboardCodecTest, Utf8Utf16RoundTripAndLineEndings)
{
  std::string utf8 = "Hello\nWorld\r\n日本語 🚀!";
  std::vector<uint8_t> utf16 = KeyMap::utf8ToUtf16LeCrlf(utf8);

  // Must be NUL-terminated UTF-16LE
  ASSERT_GE(utf16.size(), 4U);
  EXPECT_EQ(0x00, utf16[utf16.size() - 2]);
  EXPECT_EQ(0x00, utf16[utf16.size() - 1]);

  std::string roundTrip = KeyMap::utf16LeToUtf8Lf(utf16.data(), utf16.size());
  EXPECT_EQ("Hello\nWorld\n日本語 🚀!", roundTrip);

  // Stops at embedded NUL and tolerates odd trailing byte
  std::vector<uint8_t> withNull = {'A', 0, '\r', 0, '\n', 0, 0, 0, 'B', 0, 0xFF};
  EXPECT_EQ("A\n", KeyMap::utf16LeToUtf8Lf(withNull.data(), withNull.size()));
}

TEST(MonitorLayoutTest, ClampsMonitorsAndEvenWidths)
{
  rfb::ScreenSet set;
  set.add_screen(rfb::Screen(1, 0, 0, 100, 9000, 0));
  set.add_screen(rfb::Screen(2, 100, 0, 1367, 768, 0));

  auto monitors = KeyMap::buildMonitorLayout(1920, 1080, set);
  ASSERT_EQ(2U, monitors.size());
  EXPECT_EQ(DISPLAY_CONTROL_MONITOR_PRIMARY, monitors[0].Flags);
  EXPECT_EQ(200U, monitors[0].Width);
  EXPECT_EQ(8192U, monitors[0].Height);

  EXPECT_EQ(0U, monitors[1].Flags);
  EXPECT_EQ(1366U, monitors[1].Width); // even alignment
  EXPECT_EQ(768U, monitors[1].Height);
}

TEST(MonitorLayoutTest, UsesPrimaryMonitorAsOrigin)
{
  rfb::ScreenSet set;
  set.add_screen(rfb::Screen(1, 1280, 200, 1920, 1080, 0));
  set.add_screen(rfb::Screen(2, 0, 0, 1280, 1024, 0));

  auto monitors = KeyMap::buildMonitorLayout(3200, 1280, set);
  ASSERT_EQ(2U, monitors.size());
  EXPECT_EQ(0, monitors[0].Left);
  EXPECT_EQ(0, monitors[0].Top);
  EXPECT_EQ(-1280, monitors[1].Left);
  EXPECT_EQ(-200, monitors[1].Top);
}

TEST(RdpPixelBufferTest, AlignmentAndFormat)
{
  auto buf = AlignedBuffer::create(1366, 768, 7);
  EXPECT_EQ(0U, reinterpret_cast<uintptr_t>(buf->data()) % 64U);
  EXPECT_EQ(1376, buf->stridePixels());
  EXPECT_EQ(1376 * 4, buf->strideBytes());
  EXPECT_EQ(7U, buf->generation());

  RdpPixelBuffer pb;
  pb.rebind(buf);
  EXPECT_TRUE(pb.hasValidBuffer());
  EXPECT_EQ(1366, pb.width());
  EXPECT_EQ(768, pb.height());
  EXPECT_EQ(7U, pb.generation());

  // Verify [B, G, R, X] match with TigerVNC PixelFormat
  rfb::Pixel px = pb.getPF().pixelFromRGB(uint8_t(0xAA), uint8_t(0xBB), uint8_t(0xCC));
  std::memcpy(buf->data(), &px, 4);
  EXPECT_EQ(0xCC, buf->data()[0]); // B
  EXPECT_EQ(0xBB, buf->data()[1]); // G
  EXPECT_EQ(0xAA, buf->data()[2]); // R

  pb.detach();
  EXPECT_FALSE(pb.hasValidBuffer());
}

TEST(RdpH264PassthroughTest, SpsPpsCachingFlagsAndJfif)
{
  RdpH264StreamState state;

  // Frame 1: 4-byte SPS (7), PPS (8), IDR (5)
  std::vector<uint8_t> au1 = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x1E,
    0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80,
    0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21
  };
  ProcessedH264Frame f1;
  ASSERT_TRUE(state.processAccessUnit(au1.data(), au1.size(), f1));
  EXPECT_TRUE(f1.hasIdr);
  EXPECT_TRUE(f1.hasSps);
  EXPECT_TRUE(f1.hasPps);
  EXPECT_EQ(kH264FlagResetAllContexts, f1.flags);
  EXPECT_TRUE(state.hasCachedParameters());

  // Frame 2: 3-byte start code non-IDR P-frame (type 1)
  std::vector<uint8_t> au2 = { 0x00, 0x00, 0x01, 0x41, 0x9A, 0x22 };
  ProcessedH264Frame f2;
  ASSERT_TRUE(state.processAccessUnit(au2.data(), au2.size(), f2));
  EXPECT_FALSE(f2.hasIdr);
  EXPECT_EQ(0U, f2.flags);
  EXPECT_EQ(au2, f2.annexB); // No spurious SPS/PPS prepended to P-frame

  // Frame 3: Surface resize + IDR without SPS/PPS -> cached SPS/PPS prepended!
  state.markContextReset();
  std::vector<uint8_t> au3 = { 0x00, 0x00, 0x01, 0x65, 0x88, 0x99 };
  ProcessedH264Frame f3;
  ASSERT_TRUE(state.processAccessUnit(au3.data(), au3.size(), f3));
  EXPECT_TRUE(f3.hasIdr);
  EXPECT_TRUE(f3.hasSps);
  EXPECT_TRUE(f3.hasPps);
  EXPECT_EQ(kH264FlagResetContext, f3.flags);
  EXPECT_GT(f3.annexB.size(), au3.size());

  std::vector<AnnexBNalUnit> parsed;
  ASSERT_TRUE(RdpH264StreamState::parseAnnexBNalUnits(
      f3.annexB.data(), f3.annexB.size(), parsed));
  ASSERT_EQ(3U, parsed.size());
  EXPECT_EQ(7U, parsed[0].nalType);
  EXPECT_EQ(8U, parsed[1].nalType);
  EXPECT_EQ(5U, parsed[2].nalType);

  auto pkt = RdpH264StreamState::packetizeRfbRect(
      f3.annexB.data(), f3.annexB.size(), f3.flags);
  ASSERT_EQ(8U + f3.annexB.size(), pkt.size());
  EXPECT_EQ(f3.annexB.size(), readU32BE(pkt.data()));
  EXPECT_EQ(kH264FlagResetContext, readU32BE(pkt.data() + 4));

  auto jfif = makeSampleJfif(32, 32);
  EXPECT_TRUE(RdpH264StreamState::isValidJfif(jfif.data(), jfif.size()));
  jfif.back() = 0x00; // corrupt EOI
  EXPECT_FALSE(RdpH264StreamState::isValidJfif(jfif.data(), jfif.size()));
}

TEST(EventQueueOrderTest, FifoOrderingAndEventFdWakeup)
{
  RdpEventQueue q;

  RdpEvent f1;
  f1.type = RdpEvent::Type::FrameUpdate;
  f1.fbGeneration = 1;
  f1.dirty = core::Region(core::Rect(0, 0, 10, 10));
  q.push(std::move(f1));

  // Adjacent frame updates retain their own pixel data.
  RdpEvent f2;
  f2.type = RdpEvent::Type::FrameUpdate;
  f2.fbGeneration = 1;
  f2.dirty = core::Region(core::Rect(20, 20, 30, 30));
  f2.pixels.assign(10 * 10 * 4, 42);
  q.push(std::move(f2));

  // CopyRect acts as barrier
  RdpEvent cp;
  cp.type = RdpEvent::Type::CopyRect;
  cp.fbGeneration = 1;
  cp.rect = core::Rect(5, 5, 15, 15);
  q.push(std::move(cp));

  RdpEvent f3;
  f3.type = RdpEvent::Type::FrameUpdate;
  f3.fbGeneration = 1;
  f3.dirty = core::Region(core::Rect(0, 0, 10, 10));
  q.push(std::move(f3));

  struct pollfd pfd{};
  pfd.fd = q.fd();
  pfd.events = POLLIN;
  EXPECT_EQ(1, ::poll(&pfd, 1, 0));

  std::vector<RdpEvent> out;
  q.drain(out);
  ASSERT_EQ(4U, out.size());
  EXPECT_EQ(RdpEvent::Type::FrameUpdate, out[0].type);
  EXPECT_EQ(RdpEvent::Type::FrameUpdate, out[1].type);
  EXPECT_EQ(std::vector<uint8_t>(10 * 10 * 4, 42), out[1].pixels);
  EXPECT_EQ(RdpEvent::Type::CopyRect, out[2].type);
  EXPECT_EQ(RdpEvent::Type::FrameUpdate, out[3].type);

  // Drain clears eventfd
  pfd.revents = 0;
  EXPECT_EQ(0, ::poll(&pfd, 1, 0));
}

TEST(EventQueueOrderTest, CancelWakesProducerWaitingForSpace)
{
  RdpEventQueue queue;
  for (int i = 0; i < 1024; ++i)
    queue.push(RdpEvent());

  auto producer = std::async(std::launch::async, [&] {
    queue.push(RdpEvent());
  });
  EXPECT_EQ(std::future_status::timeout,
            producer.wait_for(std::chrono::milliseconds(20)));
  queue.cancel();
  producer.get();
  EXPECT_EQ(1024U, queue.approximateSize());

  queue.clear();
  queue.push(RdpEvent());
  EXPECT_EQ(1U, queue.approximateSize());
}

TEST(RfbPassthroughPipelineTest, PathACopyRectSolidFillRawJpegAndH264Mixed)
{
  core::Configuration::setParam("SecurityTypes", "None");
  core::Configuration::setParam("ZlibLevel", "0");
  core::Configuration::setParam("FrameRate", "1000");

  MockDesktop desktop(64, 64);
  rfb::VNCServerST server("test", &desktop);

  // Paint initial solid green background
  auto fillRect = [&](const core::Rect& r, uint8_t red, uint8_t green,
                      uint8_t blue) {
    uint8_t* base = desktop.pb_.backing()->data();
    int stride = desktop.pb_.backing()->strideBytes();
    for (int y = r.tl.y; y < r.br.y; ++y) {
      uint8_t* row = base + static_cast<size_t>(y) * stride;
      for (int x = r.tl.x; x < r.br.x; ++x) {
        row[x * 4 + 0] = blue;
        row[x * 4 + 1] = green;
        row[x * 4 + 2] = red;
        row[x * 4 + 3] = 0;
      }
    }
  };
  fillRect(core::Rect(0, 0, 64, 64), 0, 200, 0);

  // Connect Client 1 (H.264 + Tight + CopyRect + Raw + ExtendedDesktopSize)
  TestRfbClient c1(server);
  c1.sendSetPixelFormat32Bgrx();
  c1.sendSetEncodings({rfb::encodingH264, rfb::encodingCopyRect,
                       rfb::encodingTight, rfb::encodingRaw,
                       rfb::pseudoEncodingExtendedDesktopSize});
  c1.sendFbUpdateRequest(0, 0, 64, 64, false);
  auto initRects1 = c1.readNextFramebufferUpdate(32);
  ASSERT_FALSE(initRects1.empty());
  if (initRects1.size() == 1 &&
      initRects1[0].encoding == rfb::pseudoEncodingExtendedDesktopSize) {
    // ExtendedDesktopSize pseudo-update was sent first; now read the initial frame
    // using incremental=true so writeDesktopSize(reasonServer) does not re-trigger.
    c1.sendFbUpdateRequest(0, 0, 64, 64, true);
    auto initFrame1 = c1.readNextFramebufferUpdate(32);
    ASSERT_FALSE(initFrame1.empty());
    EXPECT_NE(rfb::pseudoEncodingExtendedDesktopSize, initFrame1[0].encoding);
  }
  EXPECT_GE(desktop.encodingsChangedCalls_, 1);

  // Connect Client 2 (legacy ZRLE + CopyRect + Tight ONLY - no H.264!)
  TestRfbClient c2(server);
  c2.sendSetPixelFormat32Bgrx();
  c2.sendSetEncodings({rfb::encodingCopyRect, rfb::encodingZRLE,
                       rfb::encodingRaw});
  c2.sendFbUpdateRequest(0, 0, 64, 64, false);
  auto initRects2 = c2.readNextFramebufferUpdate(32);
  ASSERT_FALSE(initRects2.empty());
  c1.pump();
  c2.pump();

  int totalClients = 0;
  int h264Clients = 0;
  int h264Synced = 0;
  int jpegClients = 0;
  server.getClientCapabilities(&totalClients, &h264Clients, &h264Synced,
                               &jpegClients);
  EXPECT_EQ(2, totalClients);
  EXPECT_EQ(1, h264Clients);
  EXPECT_EQ(0, h264Synced);

  // 1) Unsynced H.264 client must skip non-IDR frame, then accept IDR
  std::vector<uint8_t> nonIdr = {0x00, 0x00, 0x01, 0x41, 0x9A, 0x01};
  std::vector<uint8_t> idr = {
    0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x1E,
    0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80,
    0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21
  };
  c1.sendFbUpdateRequest(0, 0, 64, 64, true);
  c1.pump();
  server.add_h264(core::Rect(0, 0, 32, 32), nonIdr.data(), nonIdr.size(),
                  0, false, core::Region());
  c1.pump();
  EXPECT_EQ(0U, c1.bufferedRxSize());

  fillRect(core::Rect(0, 0, 32, 32), 20, 40, 60);
  c2.sendFbUpdateRequest(0, 0, 64, 64, true);
  c2.pump();
  server.add_h264(core::Rect(0, 0, 32, 32), idr.data(), idr.size(),
                  0, true, core::Region(core::Rect(0, 0, 32, 32)));
  auto h264Rects = c1.readNextFramebufferUpdate(32);
  ASSERT_EQ(1U, h264Rects.size());
  EXPECT_EQ(rfb::encodingH264, h264Rects[0].encoding);
  EXPECT_EQ(idr.size(), readU32BE(h264Rects[0].data.data()));
  EXPECT_NE(0U, readU32BE(h264Rects[0].data.data() + 4) & kH264FlagResetContext);

  // Legacy c2 falls back to Path A (ZRLE), never receives encodingH264
  auto legacyFallback = c2.readNextFramebufferUpdate(32);
  ASSERT_EQ(1U, legacyFallback.size());
  EXPECT_EQ(rfb::encodingZRLE, legacyFallback[0].encoding);

  server.getClientCapabilities(&totalClients, &h264Clients, &h264Synced,
                               &jpegClients);
  EXPECT_EQ(1, h264Synced);

  // 2) P1 CopyRect test
  c1.sendFbUpdateRequest(0, 0, 64, 64, true);
  c2.sendFbUpdateRequest(0, 0, 64, 64, true);
  server.add_copied(core::Region(core::Rect(16, 16, 48, 48)),
                    core::Point(8, 8)); // src = (8, 8)
  auto cp1 = c1.readNextFramebufferUpdate(32);
  ASSERT_EQ(1U, cp1.size());
  EXPECT_EQ(rfb::encodingCopyRect, cp1[0].encoding);
  EXPECT_EQ(16, cp1[0].x);
  EXPECT_EQ(16, cp1[0].y);
  EXPECT_EQ(8, readU16BE(&cp1[0].data[0]));
  EXPECT_EQ(8, readU16BE(&cp1[0].data[2]));

  auto cp2 = c2.readNextFramebufferUpdate(32);
  ASSERT_EQ(1U, cp2.size());
  EXPECT_EQ(rfb::encodingCopyRect, cp2[0].encoding);

  // 3) P2 SolidFill test (uses fast-path tight FillCompression on c1)
  c1.sendFbUpdateRequest(0, 0, 64, 64, true);
  c2.sendFbUpdateRequest(0, 0, 64, 64, true);
  fillRect(core::Rect(0, 0, 16, 16), 10, 20, 30);
  const uint8_t bgrxColor[4] = { 64, 128, 255, 0 }; // B=64, G=128, R=255
  server.add_solid_fill(core::Rect(0, 0, 16, 16), bgrxColor);
  auto sf1 = c1.readNextFramebufferUpdate(32);
  ASSERT_EQ(1U, sf1.size());
  EXPECT_EQ(rfb::encodingTight, sf1[0].encoding);
  EXPECT_EQ(0x80, sf1[0].data[0] & 0xF0); // Tight FillCompression

  // Legacy ZRLE c2 falls back cleanly
  auto sf2 = c2.readNextFramebufferUpdate(32);
  ASSERT_EQ(1U, sf2.size());
  EXPECT_EQ(rfb::encodingZRLE, sf2[0].encoding);

  // 4) P4 JFIF JPEG passthrough on c1 vs ZRLE fallback on c2
  c1.sendSetEncodings({rfb::encodingCopyRect, rfb::encodingTight,
                       rfb::pseudoEncodingQualityLevel0 + 6,
                       rfb::pseudoEncodingLastRect});
  c1.sendFbUpdateRequest(0, 0, 64, 64, true);
  c2.sendFbUpdateRequest(0, 0, 64, 64, true);
  fillRect(core::Rect(32, 32, 48, 48), 90, 91, 92);
  auto jfif = makeSampleJfif(16, 16);
  server.add_jpeg_passthrough(core::Rect(32, 32, 48, 48), jfif.data(),
                              jfif.size());
  auto jp1 = c1.readNextFramebufferUpdate(32);
  ASSERT_EQ(1U, jp1.size());
  EXPECT_EQ(rfb::encodingTight, jp1[0].encoding);
  EXPECT_EQ(0x90, jp1[0].data[0] & 0xF0); // Tight JPEGCompression
  // Verify raw JFIF SOI/EOI preserved zero-decode inside Tight payload
  EXPECT_NE(std::search(jp1[0].data.begin(), jp1[0].data.end(),
                        jfif.begin(), jfif.end()),
            jp1[0].data.end());

  auto jp2 = c2.readNextFramebufferUpdate(32);
  ASSERT_EQ(1U, jp2.size());
  EXPECT_EQ(rfb::encodingZRLE, jp2[0].encoding);

  // 5) P3 Raw passthrough bit-for-bit match vs 16bpp fallback
  c1.sendFbUpdateRequest(0, 0, 64, 64, true);
  c2.sendSetPixelFormat16Rgb565();
  c2.sendFbUpdateRequest(0, 0, 64, 64, true);
  fillRect(core::Rect(48, 0, 64, 16), 12, 34, 56);
  server.add_raw_passthrough(core::Rect(48, 0, 64, 16));
  auto raw1 = c1.readNextFramebufferUpdate(32);
  ASSERT_EQ(1U, raw1.size());
  EXPECT_EQ(rfb::encodingRaw, raw1[0].encoding);
  EXPECT_EQ(56, raw1[0].data[0]);
  EXPECT_EQ(34, raw1[0].data[1]);
  EXPECT_EQ(12, raw1[0].data[2]);

  auto fb16 = c2.readNextFramebufferUpdate(16);
  ASSERT_EQ(1U, fb16.size());

  // 6) Async SetDesktopSize with resultPending suppresses immediate ExtendedDesktopSize
  c2.closeConnection();
  desktop.nextLayoutResult_ = rfb::resultPending;
  c1.sendSetEncodings({rfb::encodingTight, rfb::pseudoEncodingExtendedDesktopSize});
  c1.sendFbUpdateRequest(0, 0, 64, 64, true);
  // Adding ExtendedDesktopSize back may trigger an initial EDS update; consume it if sent
  c1.pump();
  if (c1.bufferedRxSize() > 0) {
    (void)c1.readNextFramebufferUpdate(32);
    c1.sendFbUpdateRequest(0, 0, 64, 64, true);
    c1.pump();
  }
  c1.sendSetDesktopSize(2500, 768);
  c1.pump();
  EXPECT_EQ(1, desktop.setScreenLayoutCalls_);
  EXPECT_EQ(0U, c1.bufferedRxSize());

  // Apply 2500x768 framebuffer -> emits ExtendedDesktopSize
  auto newBuf = AlignedBuffer::create(2500, 768, 2);
  desktop.pb_.rebind(newBuf);
  rfb::ScreenSet newLayout;
  newLayout.add_screen(rfb::Screen(1, 0, 0, 2500, 768, 0));
  server.setPixelBuffer(&desktop.pb_, newLayout, true);
  auto resizeRects = c1.readNextFramebufferUpdate(32);
  ASSERT_FALSE(resizeRects.empty());
  bool sawEds = false;
  for (const auto& r : resizeRects) {
    if (r.encoding == rfb::pseudoEncodingExtendedDesktopSize) {
      EXPECT_EQ(2500, r.w);
      EXPECT_EQ(768, r.h);
      EXPECT_EQ(rfb::reasonClient, r.x);
      EXPECT_EQ(rfb::resultSuccess, r.y);
      sawEds = true;
    }
  }
  EXPECT_TRUE(sawEds);

  // Verify SolidFill > SubRectMaxWidth (2048) splits cleanly without assertion
  // Note: setPixelBuffer already queues an update for the new framebuffer; consume it
  // if needed, then test a wide SolidFill rect.
  c1.sendFbUpdateRequest(0, 0, 2500, 768, true);
  c1.pump();
  if (c1.bufferedRxSize() > 0) {
    (void)c1.readNextFramebufferUpdate(32);
    c1.sendFbUpdateRequest(0, 0, 2500, 768, true);
    c1.pump();
  }
  fillRect(core::Rect(0, 0, 2500, 10), 42, 43, 44);
  const uint8_t splitColor[4] = { 10, 20, 30, 0 };
  server.add_solid_fill(core::Rect(0, 0, 2500, 10), splitColor);
  auto splitRects = c1.readNextFramebufferUpdate(32);
  ASSERT_GE(splitRects.size(), 2U);
  for (const auto& r : splitRects) {
    EXPECT_LE(r.w, 2048);
    EXPECT_EQ(rfb::encodingTight, r.encoding);
  }
}

TEST(RfbPassthroughPipelineTest, H264OutsideRequestedAreaUsesPixelFallback)
{
  core::Configuration::setParam("SecurityTypes", "None");
  core::Configuration::setParam("FrameRate", "1000");
  MockDesktop desktop(64, 64);
  rfb::VNCServerST server("test", &desktop);
  TestRfbClient client(server);
  client.sendSetEncodings({rfb::encodingH264, rfb::encodingRaw});
  client.sendFbUpdateRequest(0, 0, 64, 64, false);
  ASSERT_FALSE(client.readNextFramebufferUpdate().empty());

  core::Rect fullFrame(0, 0, 64, 64);
  const uint8_t frame[] = {0, 0, 1, 0x65, 0x88};
  desktop.pb_.backing()->data()[0] = 123;
  server.add_h264(fullFrame, frame, sizeof(frame), 0, true,
                  core::Region(fullFrame));
  client.sendFbUpdateRequest(0, 0, 16, 16, true);
  auto rects = client.readNextFramebufferUpdate();
  ASSERT_EQ(1U, rects.size());
  EXPECT_EQ(rfb::encodingRaw, rects[0].encoding);
  EXPECT_EQ(16, rects[0].w);
  EXPECT_EQ(16, rects[0].h);
  ASSERT_FALSE(rects[0].data.empty());
  EXPECT_EQ(123, rects[0].data[0]);

  int synced = -1;
  server.getClientCapabilities(nullptr, nullptr, &synced, nullptr);
  EXPECT_EQ(0, synced);
}

TEST(RfbPassthroughPipelineTest, ResizeFailureRepliesWithoutOutstandingUpdate)
{
  core::Configuration::setParam("SecurityTypes", "None");
  MockDesktop desktop(64, 64);
  desktop.nextLayoutResult_ = rfb::resultPending;
  rfb::VNCServerST server("test", &desktop);
  TestRfbClient client(server);
  client.sendSetEncodings({rfb::encodingRaw,
                           rfb::pseudoEncodingExtendedDesktopSize});
  client.sendSetDesktopSize(100, 100);
  client.pump();
  EXPECT_EQ(0U, client.bufferedRxSize());

  server.finishDesktopSize(rfb::resultProhibited);
  auto rects = client.readNextFramebufferUpdate();
  ASSERT_EQ(1U, rects.size());
  EXPECT_EQ(rfb::pseudoEncodingExtendedDesktopSize, rects[0].encoding);
  EXPECT_EQ(rfb::reasonClient, rects[0].x);
  EXPECT_EQ(rfb::resultProhibited, rects[0].y);
}

TEST(RDPDesktopPolicyTest, DebounceClampingAndZeroSilentHangOnDisconnect)
{
  core::Configuration::setParam("SecurityTypes", "None");

  RdpConfig cfg;
  cfg.host = "127.0.0.1";
  cfg.port = 1; // guaranteed fast ECONNREFUSED on closed port
  cfg.connectTimeoutMs = 1000;

  DesktopPolicyConfig pol;
  pol.resizeDebounceMs = 50; // clamped up to mandatory 300ms minimum
  pol.maxReconnectAttempts = 0;

  RDPDesktop desktop(cfg, pol);
  rfb::VNCServerST server("rdp-test", &desktop);
  TestRfbClient client(server);
  EXPECT_EQ(RDPDesktop::State::Connecting, desktop.state());

  // Dynamic resize prohibited while not Connected
  rfb::ScreenSet set;
  set.add_screen(rfb::Screen(1, 0, 0, 1920, 1080, 0));
  EXPECT_EQ(rfb::resultProhibited, desktop.setScreenLayout(1920, 1080, set));

  // Zero Silent-Hang Invariant: worker notifies eventfd and main thread closes client
  struct pollfd pfd{};
  pfd.fd = desktop.eventFd();
  pfd.events = POLLIN;
  ASSERT_GT(::poll(&pfd, 1, 3000), 0);
  desktop.processEvents();
  EXPECT_EQ(RDPDesktop::State::Idle, desktop.state());
  EXPECT_TRUE(client.isClosedByServer());

  desktop.terminate();
  EXPECT_EQ(RDPDesktop::State::Idle, desktop.state());
}
