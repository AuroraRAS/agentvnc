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

#ifndef __SCRCPYVNCSERVER_SCRCPYSTREAM_H__
#define __SCRCPYVNCSERVER_SCRCPYSTREAM_H__

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

enum class ScrcpyProtocol {
  Unknown,
  V2, // scrcpy 2.x / 3.x: 8-byte width+height header, bit 63=config, bit 62=keyframe
  V4, // scrcpy 4.x / 5.x: 12-byte session packet (bit 63), bit 62=config, bit 61=keyframe
};

class ScrcpyStream {
public:
  class Callbacks {
  public:
    virtual ~Callbacks() {}
    virtual void onResize(int width, int height) = 0;
    virtual void onConfig(const std::vector<uint8_t>& spsPps) = 0;
    virtual void onFrame(const uint8_t* data, size_t len,
                         bool isKeyFrame, bool hasInlineSpsPps) = 0;
  };

  ScrcpyStream();
  ~ScrcpyStream();

  bool readDummyByte(int fd, int timeoutMs = 5000);
  bool readStreamHeader(int fd, bool expectDeviceMeta, int timeoutMs = 5000);
  bool handshake(int fd, bool expectDummyByte, bool expectDeviceMeta,
                 int timeoutMs = 5000);

  void attachForTest(int fd, ScrcpyProtocol protocol, int width, int height);

  // Returns false on clean EOF, true when EAGAIN/drained.
  bool processReadable(Callbacks& cb);

  int fd() const { return fd_; }
  void close();

  ScrcpyProtocol protocol() const { return protocol_; }
  const std::string& deviceName() const { return deviceName_; }
  int width() const { return width_; }
  int height() const { return height_; }
  const std::vector<uint8_t>& spsPps() const { return spsPps_; }

private:
  enum class ReadState {
    Header,
    Payload,
  };

  ScrcpyStream(const ScrcpyStream&) = delete;
  ScrcpyStream& operator=(const ScrcpyStream&) = delete;

  int fd_;
  ScrcpyProtocol protocol_;
  std::string deviceName_;
  int width_;
  int height_;
  std::vector<uint8_t> spsPps_;

  ReadState state_;
  uint8_t hdrBuf_[12];
  size_t hdrRead_;
  uint32_t packetSize_;
  size_t payloadRead_;
  bool isConfig_;
  bool isKeyFrame_;
  std::vector<uint8_t> packetBuf_;
};

#endif // __SCRCPYVNCSERVER_SCRCPYSTREAM_H__
