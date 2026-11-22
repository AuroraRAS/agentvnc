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

#include "ScrcpyStream.h"
#include "H264SpsParser.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

#include <core/Exception.h>
#include <core/LogWriter.h>
#include <core/string.h>

static core::LogWriter vlog("ScrcpyStream");

namespace {

static const uint32_t kCodecIdH264 = 0x68323634u; // "h264"
static const uint32_t kMaxPacketSize = 16u * 1024u * 1024u;

uint32_t readBe32(const uint8_t* p)
{
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) |
         static_cast<uint32_t>(p[3]);
}

uint64_t readBe64(const uint8_t* p)
{
  return (static_cast<uint64_t>(readBe32(p)) << 32) |
         static_cast<uint64_t>(readBe32(p + 4));
}

// Returns bytes read (0 only if EOF before any byte read), throws on timeout/error/partial EOF.
size_t readExactTimeout(int fd, uint8_t* buf, size_t len, int timeoutMs,
                        bool allowInitialEof)
{
  size_t off = 0;
  while (off < len) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int pr;
    do {
      pr = ::poll(&pfd, 1, timeoutMs);
    } while (pr < 0 && errno == EINTR);

    if (pr < 0)
      throw core::socket_error("poll failed during scrcpy handshake", errno);
    if (pr == 0)
      throw std::runtime_error("Timed out waiting for scrcpy video handshake");

    ssize_t n;
    do {
      n = ::recv(fd, buf + off, len - off, 0);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        continue;
      throw core::socket_error("recv failed during scrcpy handshake", errno);
    }
    if (n == 0) {
      if (off == 0 && allowInitialEof)
        return 0;
      throw std::runtime_error("scrcpy closed video socket during handshake");
    }
    off += static_cast<size_t>(n);
  }
  return off;
}

void setNonBlockingAndNoDelay(int fd)
{
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0)
    throw core::posix_error("fcntl(F_GETFL) failed", errno);
  if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    throw core::posix_error("fcntl(F_SETFL) failed", errno);
}

} // namespace

ScrcpyStream::ScrcpyStream()
  : fd_(-1),
    protocol_(ScrcpyProtocol::Unknown),
    width_(0),
    height_(0),
    state_(ReadState::Header),
    hdrRead_(0),
    packetSize_(0),
    payloadRead_(0),
    isConfig_(false),
    isKeyFrame_(false)
{
  std::memset(hdrBuf_, 0, sizeof(hdrBuf_));
}

ScrcpyStream::~ScrcpyStream()
{
  close();
}

void ScrcpyStream::close()
{
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

bool ScrcpyStream::readDummyByte(int fd, int timeoutMs)
{
  uint8_t dummy = 0;
  size_t r = readExactTimeout(fd, &dummy, 1, timeoutMs, true);
  return r == 1;
}

bool ScrcpyStream::readStreamHeader(int fd, bool expectDeviceMeta, int timeoutMs)
{
  if (expectDeviceMeta) {
    uint8_t nameBuf[64];
    size_t r = readExactTimeout(fd, nameBuf, sizeof(nameBuf), timeoutMs, true);
    if (r == 0)
      return false;
    size_t nameLen = 0;
    while (nameLen < sizeof(nameBuf) && nameBuf[nameLen] != '\0')
      nameLen++;
    deviceName_.assign(reinterpret_cast<const char*>(nameBuf), nameLen);
  } else {
    deviceName_ = "Android";
  }

  uint8_t codecBuf[4];
  size_t r = readExactTimeout(fd, codecBuf, sizeof(codecBuf), timeoutMs,
                              !expectDeviceMeta);
  if (r == 0)
    return false;

  uint32_t codecId = readBe32(codecBuf);
  if (codecId == 0u || codecId == 1u) {
    throw std::runtime_error(
      "scrcpy-server failed or disabled the video stream");
  }
  if (codecId != kCodecIdH264) {
    throw std::runtime_error(
      core::format("Unsupported scrcpy video codec 0x%08x (expected h264)",
                   codecId));
  }

  // Distinguish scrcpy v2/v3 (8-byte width+height) from v4/v5 (12-byte session
  // packet whose MSB is 1).
  uint8_t probe[4];
  readExactTimeout(fd, probe, sizeof(probe), timeoutMs, false);

  if ((probe[0] & 0x80u) != 0) {
    uint8_t rest[8];
    readExactTimeout(fd, rest, sizeof(rest), timeoutMs, false);
    protocol_ = ScrcpyProtocol::V4;
    width_ = static_cast<int>(readBe32(rest));
    height_ = static_cast<int>(readBe32(rest + 4));
  } else {
    uint8_t rest[4];
    readExactTimeout(fd, rest, sizeof(rest), timeoutMs, false);
    protocol_ = ScrcpyProtocol::V2;
    width_ = static_cast<int>(readBe32(probe));
    height_ = static_cast<int>(readBe32(rest));
  }

  if (width_ <= 0 || height_ <= 0 || width_ > 16384 || height_ > 16384) {
    throw std::runtime_error(
      core::format("Invalid video dimensions from scrcpy: %dx%d",
                   width_, height_));
  }

  setNonBlockingAndNoDelay(fd);
  fd_ = fd;
  state_ = ReadState::Header;
  hdrRead_ = 0;
  packetSize_ = 0;
  payloadRead_ = 0;
  spsPps_.clear();

  vlog.info("Connected to scrcpy video stream (%s): '%s' %dx%d",
            protocol_ == ScrcpyProtocol::V4 ? "v4+" : "v2/v3",
            deviceName_.c_str(), width_, height_);
  return true;
}

bool ScrcpyStream::handshake(int fd, bool expectDummyByte,
                             bool expectDeviceMeta, int timeoutMs)
{
  if (expectDummyByte && !readDummyByte(fd, timeoutMs))
    return false;
  return readStreamHeader(fd, expectDeviceMeta, timeoutMs);
}

void ScrcpyStream::attachForTest(int fd, ScrcpyProtocol protocol,
                                 int width, int height)
{
  setNonBlockingAndNoDelay(fd);
  fd_ = fd;
  protocol_ = protocol;
  width_ = width;
  height_ = height;
  deviceName_ = "TestDevice";
  state_ = ReadState::Header;
  hdrRead_ = 0;
  packetSize_ = 0;
  payloadRead_ = 0;
  spsPps_.clear();
}

bool ScrcpyStream::processReadable(Callbacks& cb)
{
  if (fd_ < 0)
    return false;

  while (true) {
    if (state_ == ReadState::Header) {
      while (hdrRead_ < sizeof(hdrBuf_)) {
        ssize_t n = ::recv(fd_, hdrBuf_ + hdrRead_,
                           sizeof(hdrBuf_) - hdrRead_, 0);
        if (n < 0) {
          if (errno == EINTR)
            continue;
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            return true;
          throw core::socket_error("scrcpy video recv header failed", errno);
        }
        if (n == 0) {
          if (hdrRead_ == 0)
            return false;
          throw std::runtime_error("scrcpy video EOF mid-header");
        }
        hdrRead_ += static_cast<size_t>(n);
      }

      if (protocol_ == ScrcpyProtocol::V4 && (hdrBuf_[0] & 0x80u) != 0) {
        int newW = static_cast<int>(readBe32(hdrBuf_ + 4));
        int newH = static_cast<int>(readBe32(hdrBuf_ + 8));
        hdrRead_ = 0;

        if (newW <= 0 || newH <= 0 || newW > 16384 || newH > 16384) {
          throw std::runtime_error(
            core::format("Invalid session resize from scrcpy: %dx%d",
                         newW, newH));
        }

        if (newW != width_ || newH != height_) {
          width_ = newW;
          height_ = newH;
          cb.onResize(width_, height_);
        }
        continue;
      }

      uint64_t ptsFlags = readBe64(hdrBuf_);
      uint32_t pktSize = readBe32(hdrBuf_ + 8);
      if (pktSize == 0 || pktSize > kMaxPacketSize) {
        throw std::runtime_error(
          core::format("Invalid scrcpy packet size: %u", pktSize));
      }

      if (protocol_ == ScrcpyProtocol::V4) {
        isConfig_ = (ptsFlags & (UINT64_C(1) << 62)) != 0;
        isKeyFrame_ = (ptsFlags & (UINT64_C(1) << 61)) != 0;
      } else {
        isConfig_ = (ptsFlags & (UINT64_C(1) << 63)) != 0;
        isKeyFrame_ = (ptsFlags & (UINT64_C(1) << 62)) != 0;
      }

      packetSize_ = pktSize;
      payloadRead_ = 0;
      if (packetBuf_.size() < packetSize_)
        packetBuf_.resize(packetSize_);
      state_ = ReadState::Payload;
    }

    while (payloadRead_ < packetSize_) {
      ssize_t n = ::recv(fd_, packetBuf_.data() + payloadRead_,
                         packetSize_ - payloadRead_, 0);
      if (n < 0) {
        if (errno == EINTR)
          continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          return true;
        throw core::socket_error("scrcpy video recv payload failed", errno);
      }
      if (n == 0)
        throw std::runtime_error("scrcpy video EOF mid-packet");
      payloadRead_ += static_cast<size_t>(n);
    }

    state_ = ReadState::Header;
    hdrRead_ = 0;

    if (isConfig_) {
      spsPps_.assign(packetBuf_.data(), packetBuf_.data() + packetSize_);
      if (protocol_ == ScrcpyProtocol::V2) {
        int spsW = 0;
        int spsH = 0;
        if (parseSpsDimensions(spsPps_.data(), spsPps_.size(), &spsW, &spsH)) {
          if (spsW != width_ || spsH != height_) {
            width_ = spsW;
            height_ = spsH;
            cb.onResize(width_, height_);
          }
        }
      }
      cb.onConfig(spsPps_);
    } else {
      H264PacketInfo info = inspectAnnexB(packetBuf_.data(), packetSize_);
      bool effectiveKeyFrame = isKeyFrame_ || info.hasIdr;
      bool hasInlineSpsPps = info.hasSps && info.hasPps;

      if (hasInlineSpsPps) {
        spsPps_ = std::move(info.spsPps);
        if (protocol_ == ScrcpyProtocol::V2) {
          int spsW = 0;
          int spsH = 0;
          if (parseSpsDimensions(spsPps_.data(), spsPps_.size(),
                                 &spsW, &spsH)) {
            if (spsW != width_ || spsH != height_) {
              width_ = spsW;
              height_ = spsH;
              cb.onResize(width_, height_);
            }
          }
        }
      }

      cb.onFrame(packetBuf_.data(), packetSize_,
                 effectiveKeyFrame, hasInlineSpsPps);
    }
  }
}
