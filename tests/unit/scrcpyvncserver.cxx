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

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

#include <core/Configuration.h>
#include <core/Rect.h>
#include <network/TcpSocket.h>
#include <rfb/PixelBuffer.h>
#include <rfb/PixelFormat.h>
#include <rfb/SecurityServer.h>
#include <rfb/ServerParams.h>
#include <rfb/encodings.h>
#include <rfb/msgTypes.h>
#include <rfb/screenTypes.h>

#ifdef HAVE_H264
#include <rfb/H264Decoder.h>
#endif

#include "AdbClient.h"
#include "H264SpsParser.h"
#include "ScrcpyControl.h"
#include "ScrcpyServer.h"
#include "ScrcpyStream.h"
#include "ZeroCopyOutStream.h"

namespace {

// Same grey IDR frames from tests/unit/h264decoder.cxx:
// frame1080 = SPS (0..25) + PPS (26..34) + IDR (35..end)
static const uint8_t kFrame1080[] = {
  0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x10, 0x33, 0xac, 0xb8, 0x0f, 0x00, 0x44, 0xfc, 0xb8, 0x08,
  0x80, 0x00, 0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x1e, 0x02, 0x00, 0x00, 0x00, 0x01, 0x68, 0xee,
  0x0f, 0x2c, 0x8b, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x04, 0xbf, 0xfe, 0xf7, 0xad, 0xdf, 0x81,
  0x36, 0x81, 0x5a, 0xee, 0x95, 0xce, 0x2d, 0x32, 0xea, 0xe5, 0x0d, 0x0f, 0x54, 0x7d, 0xa5, 0x29,
  0x9e, 0x2d, 0x92, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
  0x00, 0x0c, 0xba, 0x6b, 0x43, 0x9b, 0x51, 0x1c, 0x38, 0x7f, 0xa0, 0xf0, 0x00, 0x00, 0x03, 0x00,
  0x00, 0x03, 0x00, 0x07, 0xe0, 0x00, 0x00, 0x03, 0x01, 0x97, 0x00, 0x00, 0x03, 0x00, 0x8a, 0x80,
  0x00, 0x00, 0x42, 0xc0, 0x00, 0x00, 0x35, 0x00, 0x00, 0x03, 0x00, 0x32, 0x40, 0x00, 0x00, 0x38,
  0x80, 0x00, 0x00, 0x3f, 0x80, 0x00, 0x00, 0x4c, 0x80, 0x00, 0x00, 0x86, 0x80, 0x00, 0x01, 0x01,
  0x00, 0x00, 0x03, 0x01, 0x8a, 0x00, 0x00, 0x03, 0x03, 0xb0, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
  0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
  0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
  0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
  0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
  0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
  0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
  0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
  0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
  0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
  0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
  0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
  0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
  0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x10, 0x11,
};

static const size_t kFrame1080SpsPpsLen = 35;

static const uint8_t kFrame1440[] = {
  0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x10, 0x33, 0xac, 0xb8, 0x05, 0x00, 0x16, 0xb6, 0x02, 0x20,
  0x00, 0x00, 0x03, 0x00, 0x20, 0x00, 0x00, 0x07, 0x80, 0x80, 0x00, 0x00, 0x00, 0x01, 0x68, 0xee,
  0x0f, 0x2c, 0x8b, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x04, 0xbf, 0xfe, 0xf7, 0xad, 0xdf, 0x81,
  0x36, 0x81, 0x5a, 0xee, 0x95, 0xce, 0x2d, 0x32, 0xea, 0xe5, 0x0d, 0x0f, 0x54, 0x7d, 0xa5, 0x29,
  0x9e, 0x2d, 0x92, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
  0x00, 0x03, 0x00, 0x03, 0x2e, 0x9a, 0xd0, 0xe6, 0xd4, 0x47, 0x0e, 0x1f, 0xe8, 0x3c, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x7e, 0x00, 0x00, 0x03, 0x00, 0x00, 0x06, 0x5c,
  0x00, 0x00, 0x03, 0x00, 0x00, 0x8a, 0x80, 0x00, 0x00, 0x03, 0x00, 0x10, 0xb0, 0x00, 0x00, 0x03,
  0x00, 0x03, 0x50, 0x00, 0x00, 0x03, 0x00, 0x00, 0xc9, 0x00, 0x00, 0x03, 0x00, 0x00, 0x38, 0x80,
  0x00, 0x00, 0x03, 0x00, 0x0f, 0xe0, 0x00, 0x00, 0x03, 0x00, 0x04, 0xc8, 0x00, 0x00, 0x03, 0x00,
  0x02, 0x1a, 0x00, 0x00, 0x03, 0x00, 0x01, 0x01, 0x00, 0x00, 0x03, 0x00, 0x00, 0x62, 0x80, 0x00,
  0x00, 0x03, 0x00, 0x3b, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
  0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
  0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
  0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
  0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
  0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
  0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
  0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
  0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
  0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
  0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
  0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
  0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
  0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
  0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
  0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
  0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
  0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
  0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
  0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
  0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
  0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
  0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
  0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
  0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x13, 0x51,
};

static const size_t kFrame1440SpsPpsLen = 35;

class BitWriter {
public:
  void writeBit(uint32_t b)
  {
    if (bitCount_ == 0)
      bytes_.push_back(0);
    if (b & 1u)
      bytes_.back() |= static_cast<uint8_t>(1u << (7 - bitCount_));
    bitCount_ = (bitCount_ + 1) & 7;
  }

  void writeBits(uint32_t v, int count)
  {
    for (int i = count - 1; i >= 0; --i)
      writeBit((v >> i) & 1u);
  }

  void writeUE(uint32_t v)
  {
    uint32_t code = v + 1;
    int bits = 0;
    for (uint32_t t = code; t > 0; t >>= 1)
      bits++;
    for (int i = 0; i < bits - 1; ++i)
      writeBit(0);
    writeBits(code, bits);
  }

  std::vector<uint8_t> finish()
  {
    writeBit(1); // RBSP trailing stop bit
    return bytes_;
  }

private:
  std::vector<uint8_t> bytes_;
  int bitCount_ = 0;
};

std::vector<uint8_t> makeBaselineSps(int width, int height)
{
  int mbW = (width + 15) / 16;
  int mbH = (height + 15) / 16;
  int cropR = (mbW * 16 - width) / 2;
  int cropB = (mbH * 16 - height) / 2;

  BitWriter bw;
  bw.writeBits(66, 8); // Baseline profile
  bw.writeBits(0, 8);  // constraints
  bw.writeBits(41, 8); // level 4.1
  bw.writeUE(0);       // sps_id
  bw.writeUE(0);       // log2_max_frame_num_minus4
  bw.writeUE(0);       // pic_order_cnt_type
  bw.writeUE(0);       // log2_max_pic_order_cnt_lsb_minus4
  bw.writeUE(1);       // max_num_ref_frames
  bw.writeBit(0);      // gaps_in_frame_num_value_allowed_flag
  bw.writeUE(static_cast<uint32_t>(mbW - 1));
  bw.writeUE(static_cast<uint32_t>(mbH - 1));
  bw.writeBit(1);      // frame_mbs_only_flag
  bw.writeBit(1);      // direct_8x8_inference_flag
  if (cropR > 0 || cropB > 0) {
    bw.writeBit(1);
    bw.writeUE(0);
    bw.writeUE(static_cast<uint32_t>(cropR));
    bw.writeUE(0);
    bw.writeUE(static_cast<uint32_t>(cropB));
  } else {
    bw.writeBit(0);
  }
  bw.writeBit(0);      // vui_parameters_present_flag

  std::vector<uint8_t> rbsp = bw.finish();
  std::vector<uint8_t> nal = { 0x00, 0x00, 0x00, 0x01, 0x67 };
  nal.insert(nal.end(), rbsp.begin(), rbsp.end());
  return nal;
}

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

int16_t readBeS16(const uint8_t* p)
{
  return static_cast<int16_t>(readBe16(p));
}

uint32_t readBe32(const uint8_t* p)
{
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) |
         static_cast<uint32_t>(p[3]);
}

int32_t readBeS32(const uint8_t* p)
{
  return static_cast<int32_t>(readBe32(p));
}

void writeAll(int fd, const void* buf, size_t len)
{
  const uint8_t* p = static_cast<const uint8_t*>(buf);
  size_t off = 0;
  while (off < len) {
    ssize_t n = ::send(fd, p + off, len - off, MSG_NOSIGNAL);
    ASSERT_GT(n, 0);
    off += static_cast<size_t>(n);
  }
}

void readAllTimeout(int fd, void* buf, size_t len, int timeoutMs = 3000)
{
  uint8_t* p = static_cast<uint8_t*>(buf);
  size_t off = 0;
  while (off < len) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int pr = ::poll(&pfd, 1, timeoutMs);
    ASSERT_GT(pr, 0) << "Timed out waiting to read " << len << " bytes";
    ssize_t n = ::recv(fd, p + off, len - off, 0);
    ASSERT_GT(n, 0);
    off += static_cast<size_t>(n);
  }
}

void sendScrcpyPacket(int fd, uint64_t ptsFlags,
                      const uint8_t* payload, size_t len)
{
  uint8_t hdr[12];
  writeBe64(hdr, ptsFlags);
  writeBe32(hdr + 8, static_cast<uint32_t>(len));
  writeAll(fd, hdr, sizeof(hdr));
  writeAll(fd, payload, len);
}

struct RecordingCallbacks : public ScrcpyStream::Callbacks {
  std::vector<std::pair<int, int>> resizes;
  std::vector<std::vector<uint8_t>> configs;
  struct Frame {
    std::vector<uint8_t> data;
    bool isKey;
    bool hasInline;
  };
  std::vector<Frame> frames;

  void onResize(int width, int height) override
  {
    resizes.emplace_back(width, height);
  }
  void onConfig(const std::vector<uint8_t>& spsPps) override
  {
    configs.push_back(spsPps);
  }
  void onFrame(const uint8_t* data, size_t len,
               bool isKeyFrame, bool hasInlineSpsPps) override
  {
    Frame f;
    f.data.assign(data, data + len);
    f.isKey = isKeyFrame;
    f.hasInline = hasInlineSpsPps;
    frames.push_back(std::move(f));
  }
};

class LoopbackListener : public network::SocketListener {
public:
  explicit LoopbackListener(int port)
  {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(s, 0);
    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    EXPECT_EQ(::bind(s, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)), 0);
    listen(s);
  }

  int getMyPort() override { return network::getSockPort(getFd()); }

protected:
  network::Socket* createSocket(int fd_) override
  {
    return new network::TcpSocket(fd_);
  }
};

} // namespace

TEST(H264SpsParserTest, Parses1080p1440pAndCropped2880x1620)
{
  int w = 0, h = 0;
  ASSERT_TRUE(parseSpsDimensions(kFrame1080, kFrame1080SpsPpsLen, &w, &h));
  EXPECT_EQ(w, 1920);
  EXPECT_EQ(h, 1080);

  ASSERT_TRUE(parseSpsDimensions(kFrame1440, kFrame1440SpsPpsLen, &w, &h));
  EXPECT_EQ(w, 2560);
  EXPECT_EQ(h, 1440);

  std::vector<uint8_t> sps2880 = makeBaselineSps(2880, 1620);
  ASSERT_TRUE(parseSpsDimensions(sps2880.data(), sps2880.size(), &w, &h));
  EXPECT_EQ(w, 2880);
  EXPECT_EQ(h, 1620);

  H264PacketInfo fullInfo = inspectAnnexB(kFrame1080, sizeof(kFrame1080));
  EXPECT_TRUE(fullInfo.hasSps);
  EXPECT_TRUE(fullInfo.hasPps);
  EXPECT_TRUE(fullInfo.hasIdr);
  EXPECT_EQ(fullInfo.spsPps.size(), kFrame1080SpsPpsLen);

  H264PacketInfo sliceInfo = inspectAnnexB(kFrame1080 + kFrame1080SpsPpsLen,
                                           sizeof(kFrame1080) - kFrame1080SpsPpsLen);
  EXPECT_FALSE(sliceInfo.hasSps);
  EXPECT_FALSE(sliceInfo.hasPps);
  EXPECT_TRUE(sliceInfo.hasIdr);
  EXPECT_TRUE(sliceInfo.spsPps.empty());

  const uint8_t junk[] = { 0x00, 0x00, 0x01, 0x67, 0x42 };
  EXPECT_FALSE(parseSpsDimensions(junk, sizeof(junk), &w, &h));
}

TEST(ScrcpyStreamTest, ParsesV2AndV4FragmentedStreamsAndResizes)
{
  // Subtest A: scrcpy v2/v3 protocol (1-byte fragmented delivery)
  {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    std::thread writer([&]() {
      uint8_t dummy = 0;
      writeAll(fds[1], &dummy, 1);

      uint8_t name[64];
      std::memset(name, 0, sizeof(name));
      std::memcpy(name, "WayDroid", 8);
      writeAll(fds[1], name, sizeof(name));

      uint8_t meta[12];
      std::memcpy(meta, "h264", 4);
      writeBe32(meta + 4, 1920);
      writeBe32(meta + 8, 1080);
      writeAll(fds[1], meta, sizeof(meta));

      // Config packet (bit 63)
      sendScrcpyPacket(fds[1], UINT64_C(1) << 63, kFrame1080, kFrame1080SpsPpsLen);

      // Fragmented IDR packet (bit 62) sent 1 byte at a time
      uint8_t idrHdr[12];
      writeBe64(idrHdr, (UINT64_C(1) << 62) | 12345u);
      writeBe32(idrHdr + 8, static_cast<uint32_t>(sizeof(kFrame1080) - kFrame1080SpsPpsLen));
      for (size_t i = 0; i < sizeof(idrHdr); ++i)
        writeAll(fds[1], &idrHdr[i], 1);
      for (size_t i = kFrame1080SpsPpsLen; i < sizeof(kFrame1080); ++i)
        writeAll(fds[1], &kFrame1080[i], 1);

      // Rotation config packet -> 2560x1440
      sendScrcpyPacket(fds[1], UINT64_C(1) << 63, kFrame1440, kFrame1440SpsPpsLen);
      sendScrcpyPacket(fds[1], UINT64_C(1) << 62,
                       kFrame1440 + kFrame1440SpsPpsLen,
                       sizeof(kFrame1440) - kFrame1440SpsPpsLen);
      ::close(fds[1]);
    });

    ScrcpyStream stream;
    ASSERT_TRUE(stream.handshake(fds[0], true, true, 3000));
    EXPECT_EQ(stream.protocol(), ScrcpyProtocol::V2);
    EXPECT_EQ(stream.deviceName(), "WayDroid");
    EXPECT_EQ(stream.width(), 1920);
    EXPECT_EQ(stream.height(), 1080);

    RecordingCallbacks cb;
    while (true) {
      struct pollfd pfd;
      pfd.fd = stream.fd();
      pfd.events = POLLIN;
      ASSERT_GT(::poll(&pfd, 1, 3000), 0);
      if (!stream.processReadable(cb))
        break;
    }

    writer.join();
    ASSERT_EQ(cb.configs.size(), 2u);
    ASSERT_EQ(cb.resizes.size(), 1u);
    EXPECT_EQ(cb.resizes[0].first, 2560);
    EXPECT_EQ(cb.resizes[0].second, 1440);
    ASSERT_EQ(cb.frames.size(), 2u);
    EXPECT_TRUE(cb.frames[0].isKey);
    EXPECT_FALSE(cb.frames[0].hasInline);
    EXPECT_EQ(cb.frames[0].data.size(), sizeof(kFrame1080) - kFrame1080SpsPpsLen);
  }

  // Subtest B: scrcpy v4/v5 protocol with session packets
  {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    std::thread writer([&]() {
      uint8_t dummy = 0;
      writeAll(fds[1], &dummy, 1);

      uint8_t name[64];
      std::memset(name, 0, sizeof(name));
      std::memcpy(name, "PhoneV5", 7);
      writeAll(fds[1], name, sizeof(name));

      uint8_t codec[4] = {'h', '2', '6', '4'};
      writeAll(fds[1], codec, 4);

      uint8_t session1[12];
      writeBe32(session1, 0x80000000u);
      writeBe32(session1 + 4, 1920);
      writeBe32(session1 + 8, 1080);
      writeAll(fds[1], session1, sizeof(session1));

      // Config = bit 62, Keyframe = bit 61
      sendScrcpyPacket(fds[1], UINT64_C(1) << 62, kFrame1080, kFrame1080SpsPpsLen);
      sendScrcpyPacket(fds[1], UINT64_C(1) << 61,
                       kFrame1080 + kFrame1080SpsPpsLen,
                       sizeof(kFrame1080) - kFrame1080SpsPpsLen);

      // Session resize packet
      uint8_t session2[12];
      writeBe32(session2, 0x80000001u);
      writeBe32(session2 + 4, 1080);
      writeBe32(session2 + 8, 1920);
      writeAll(fds[1], session2, sizeof(session2));
      ::close(fds[1]);
    });

    ScrcpyStream stream;
    ASSERT_TRUE(stream.handshake(fds[0], true, true, 3000));
    EXPECT_EQ(stream.protocol(), ScrcpyProtocol::V4);
    EXPECT_EQ(stream.width(), 1920);
    EXPECT_EQ(stream.height(), 1080);

    RecordingCallbacks cb;
    while (true) {
      struct pollfd pfd;
      pfd.fd = stream.fd();
      pfd.events = POLLIN;
      ASSERT_GT(::poll(&pfd, 1, 3000), 0);
      if (!stream.processReadable(cb))
        break;
    }

    writer.join();
    ASSERT_EQ(cb.resizes.size(), 1u);
    EXPECT_EQ(cb.resizes[0].first, 1080);
    EXPECT_EQ(cb.resizes[0].second, 1920);
    ASSERT_EQ(cb.frames.size(), 1u);
    EXPECT_TRUE(cb.frames[0].isKey);
  }
}

TEST(ZeroCopyOutStreamTest, WritevAndPartialBackpressureOrdering)
{
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

  int sndbuf = 4096;
  ::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  int flags = ::fcntl(fds[0], F_GETFL, 0);
  ::fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);

  ZeroCopyOutStream zos(fds[0]);

  const uint8_t hdr[] = { 1, 2, 3, 4 };
  const uint8_t sps[] = { 5, 6, 7 };
  std::vector<uint8_t> payload(64, 0xab);

  struct iovec iov[3];
  iov[0].iov_base = const_cast<uint8_t*>(hdr);
  iov[0].iov_len = sizeof(hdr);
  iov[1].iov_base = const_cast<uint8_t*>(sps);
  iov[1].iov_len = sizeof(sps);
  iov[2].iov_base = payload.data();
  iov[2].iov_len = payload.size();

  ASSERT_TRUE(zos.writevFrame(iov, 3));
  EXPECT_FALSE(zos.hasBufferedData());

  std::vector<uint8_t> rx(sizeof(hdr) + sizeof(sps) + payload.size());
  readAllTimeout(fds[1], rx.data(), rx.size());
  EXPECT_EQ(std::memcmp(rx.data(), hdr, sizeof(hdr)), 0);
  EXPECT_EQ(std::memcmp(rx.data() + sizeof(hdr), sps, sizeof(sps)), 0);
  EXPECT_EQ(std::memcmp(rx.data() + sizeof(hdr) + sizeof(sps),
                        payload.data(), payload.size()), 0);

  // Saturate send buffer with a large frame so tail stays in pending_
  std::vector<uint8_t> bigPayload(256 * 1024, 0x5a);
  struct iovec bigIov[2];
  bigIov[0].iov_base = const_cast<uint8_t*>(hdr);
  bigIov[0].iov_len = sizeof(hdr);
  bigIov[1].iov_base = bigPayload.data();
  bigIov[1].iov_len = bigPayload.size();

  ASSERT_TRUE(zos.writevFrame(bigIov, 2));
  EXPECT_TRUE(zos.hasBufferedData());

  // Subsequent frame while backpressured must be rejected without corrupting stream
  EXPECT_FALSE(zos.writevFrame(iov, 3));

  // Drain reader while flushing ZeroCopyOutStream
  std::vector<uint8_t> drained;
  size_t expected = sizeof(hdr) + bigPayload.size();
  while (drained.size() < expected) {
    uint8_t buf[8192];
    ssize_t n = ::recv(fds[1], buf, sizeof(buf), MSG_DONTWAIT);
    if (n > 0)
      drained.insert(drained.end(), buf, buf + n);
    zos.flush();
    if (drained.size() < expected && n <= 0)
      ::poll(nullptr, 0, 2);
  }
  EXPECT_FALSE(zos.hasBufferedData());
  EXPECT_EQ(std::memcmp(drained.data(), hdr, sizeof(hdr)), 0);
  EXPECT_EQ(std::memcmp(drained.data() + sizeof(hdr),
                        bigPayload.data(), bigPayload.size()), 0);

  ::close(fds[0]);
  ::close(fds[1]);
}

TEST(AdbClientTest, DirectTcpProtocolAndTcpNodelay)
{
  int srv = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(srv, 0);
  int one = 1;
  ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ASSERT_EQ(::bind(srv, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)), 0);
  ASSERT_EQ(::listen(srv, 8), 0);
  int port = network::getSockPort(srv);

  std::vector<uint8_t> pushedBytes;
  std::string pushedSpec;
  bool stopMock = false;

  auto readAdbCmd = [](int fd) -> std::string {
    char hex[4];
    readAllTimeout(fd, hex, 4);
    unsigned len = 0;
    std::sscanf(hex, "%4x", &len);
    std::string cmd(len, '\0');
    if (len > 0)
      readAllTimeout(fd, &cmd[0], len);
    return cmd;
  };

  std::thread mockAdb([&]() {
    while (!stopMock) {
      struct pollfd pfd;
      pfd.fd = srv;
      pfd.events = POLLIN;
      if (::poll(&pfd, 1, 50) <= 0)
        continue;
      int c = ::accept(srv, nullptr, nullptr);
      if (c < 0)
        break;

      std::string cmd = readAdbCmd(c);
      if (cmd == "host:devices") {
        std::string payload = "192.168.240.112:5555\tdevice\n";
        char resp[128];
        int n = std::snprintf(resp, sizeof(resp), "OKAY%04x%s",
                              static_cast<unsigned>(payload.size()),
                              payload.c_str());
        writeAll(c, resp, static_cast<size_t>(n));
        ::close(c);
        continue;
      }

      if (cmd == "host:transport:192.168.240.112:5555") {
        writeAll(c, "OKAY", 4);
        std::string sub = readAdbCmd(c);
        if (sub == "localabstract:scrcpy") {
          writeAll(c, "OKAY", 4);
          writeAll(c, "PING", 4);
          ::close(c);
          continue;
        } else if (sub == "sync:") {
          writeAll(c, "OKAY", 4);
          while (true) {
            uint8_t hdr[8];
            readAllTimeout(c, hdr, 8);
            uint32_t len = static_cast<uint32_t>(hdr[4]) |
                           (static_cast<uint32_t>(hdr[5]) << 8) |
                           (static_cast<uint32_t>(hdr[6]) << 16) |
                           (static_cast<uint32_t>(hdr[7]) << 24);
            if (std::memcmp(hdr, "SEND", 4) == 0) {
              pushedSpec.resize(len);
              readAllTimeout(c, &pushedSpec[0], len);
            } else if (std::memcmp(hdr, "DATA", 4) == 0) {
              size_t old = pushedBytes.size();
              pushedBytes.resize(old + len);
              readAllTimeout(c, pushedBytes.data() + old, len);
            } else if (std::memcmp(hdr, "DONE", 4) == 0) {
              uint8_t ok[8] = {'O', 'K', 'A', 'Y', 0, 0, 0, 0};
              writeAll(c, ok, 8);
            } else if (std::memcmp(hdr, "QUIT", 4) == 0) {
              break;
            }
          }
          ::close(c);
          continue;
        }
      }

      const char* failMsg = "unknown cmd";
      char failBuf[64];
      int n = std::snprintf(failBuf, sizeof(failBuf), "FAIL%04x%s",
                            static_cast<unsigned>(std::strlen(failMsg)), failMsg);
      writeAll(c, failBuf, static_cast<size_t>(n));
      ::close(c);
    }
  });

  AdbClient client("127.0.0.1", port);
  EXPECT_EQ(client.resolveSerial(""), "192.168.240.112:5555");

  int streamFd = client.connectLocalAbstract("192.168.240.112:5555", "scrcpy");
  ASSERT_GE(streamFd, 0);

  int nodelay = 0;
  socklen_t optlen = sizeof(nodelay);
  ASSERT_EQ(::getsockopt(streamFd, IPPROTO_TCP, TCP_NODELAY, &nodelay, &optlen), 0);
  EXPECT_EQ(nodelay, 1);

  char ping[4];
  readAllTimeout(streamFd, ping, 4);
  EXPECT_EQ(std::memcmp(ping, "PING", 4), 0);
  ::close(streamFd);

  // Push temp file via sync:
  char tmpl[] = "/tmp/scrcpy-adb-test-XXXXXX";
  int tmpFd = ::mkstemp(tmpl);
  ASSERT_GE(tmpFd, 0);
  const char sampleData[] = "scrcpy-server-binary-content";
  ASSERT_EQ(::write(tmpFd, sampleData, sizeof(sampleData) - 1),
            static_cast<ssize_t>(sizeof(sampleData) - 1));
  ::close(tmpFd);

  client.pushFile("192.168.240.112:5555", tmpl,
                  "/data/local/tmp/scrcpy-server.jar");
  ::unlink(tmpl);

  EXPECT_EQ(pushedSpec, "/data/local/tmp/scrcpy-server.jar,33188");
  std::string gotStr(pushedBytes.begin(), pushedBytes.end());
  EXPECT_EQ(gotStr, sampleData);

  EXPECT_EQ(AdbClient::detectVersionFromPath("/tmp/scrcpy-server-v5.0.1"), "5.0.1");
  EXPECT_EQ(AdbClient::detectVersionFromPath("/usr/share/scrcpy-server-3.3.4.jar"), "3.3.4");
  EXPECT_EQ(AdbClient::extractExpectedVersionFromError(
              "ERROR: The server version (5.0.1) does not match"), "5.0.1");

  stopMock = true;
  mockAdb.join();
  ::close(srv);
}

TEST(ScrcpyControlTest, SerializesMessagesAndParsesClipboard)
{
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

  ScrcpyControl ctrl;
  ctrl.attach(fds[0]);

  ctrl.handleVncPointer(100, 200, 1, 1920, 1080); // left down
  ctrl.handleVncPointer(150, 250, 1, 1920, 1080); // move
  ctrl.handleVncPointer(150, 250, 0, 1920, 1080); // left up
  ctrl.handleVncPointer(150, 250, 8, 1920, 1080); // wheel up
  ctrl.handleVncKey(0x0061 /* 'a' */, 0, true);
  ctrl.handleVncKey(0x0061 /* 'a' */, 0, false);
  ctrl.sendResetVideo();

  uint8_t msg1[32], msg2[32], msg3[32], scroll[21], key1[14], key2[14], rst[1];
  readAllTimeout(fds[1], msg1, 32);
  readAllTimeout(fds[1], msg2, 32);
  readAllTimeout(fds[1], msg3, 32);
  readAllTimeout(fds[1], scroll, 21);
  readAllTimeout(fds[1], key1, 14);
  readAllTimeout(fds[1], key2, 14);
  readAllTimeout(fds[1], rst, 1);

  EXPECT_EQ(msg1[0], 2);
  EXPECT_EQ(msg1[1], 0); // ACTION_DOWN
  EXPECT_EQ(readBe32(msg1 + 10), 100u);
  EXPECT_EQ(readBe32(msg1 + 14), 200u);
  EXPECT_EQ(readBe16(msg1 + 18), 1920u);
  EXPECT_EQ(readBe16(msg1 + 20), 1080u);
  EXPECT_EQ(readBe32(msg1 + 24), 1u); // action_button = PRIMARY
  EXPECT_EQ(readBe32(msg1 + 28), 1u); // buttons = PRIMARY

  EXPECT_EQ(msg2[1], 2); // ACTION_MOVE
  EXPECT_EQ(msg3[1], 1); // ACTION_UP
  EXPECT_EQ(readBe32(msg3 + 28), 0u);

  EXPECT_EQ(scroll[0], 3);
  EXPECT_GT(readBeS16(scroll + 15), 0);

  EXPECT_EQ(key1[0], 0);
  EXPECT_EQ(key1[1], 0); // ACTION_DOWN
  EXPECT_EQ(readBe32(key1 + 2), 29u); // AKEYCODE_A
  EXPECT_EQ(key2[1], 1); // ACTION_UP
  EXPECT_EQ(rst[0], 17);

  // Test device clipboard message parsing
  struct ClipCb : public ScrcpyControl::Callbacks {
    std::string clip;
    void onDeviceClipboard(const std::string& text) override { clip = text; }
  } cb;

  const char* text = "hello-android";
  uint8_t devMsg[5 + 13];
  devMsg[0] = 0; // DEVICE_MSG_TYPE_CLIPBOARD
  writeBe32(devMsg + 1, 13);
  std::memcpy(devMsg + 5, text, 13);
  writeAll(fds[1], devMsg, sizeof(devMsg));

  ctrl.processReadable(cb);
  EXPECT_EQ(cb.clip, "hello-android");

  ::close(fds[1]);
}

#ifdef HAVE_H264
TEST(ScrcpyVncEndToEndTest, HandshakeContinuousUpdatesResizeAndH264Decode)
{
  rfb::SecurityServer::secTypes.setParam("None");

  int videoFds[2];
  int ctrlFds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, videoFds), 0);
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, ctrlFds), 0);

  ScrcpyServer server;
  server.attachStreamsForTest(videoFds[0], ctrlFds[0],
                              ScrcpyProtocol::V2, 1920, 1080, "WayDroidTest");

  LoopbackListener listener(0);
  std::list<network::SocketListener*> listeners;
  listeners.push_back(&listener);
  int port = listener.getMyPort();
  ASSERT_GT(port, 0);

  volatile bool stopServer = false;
  std::thread serverThread([&]() {
    while (!stopServer) {
      if (!server.step(listeners, 20))
        break;
    }
  });

  // Client 1: supports 50, -313 (ContinuousUpdates), -308 (ExtendedDesktopSize), -223, -312
  network::TcpSocket clientSock("127.0.0.1", port);
  int cfd = clientSock.getFd();

  char ver[12];
  readAllTimeout(cfd, ver, 12);
  ASSERT_EQ(std::memcmp(ver, "RFB 003.008\n", 12), 0);
  writeAll(cfd, "RFB 003.008\n", 12);

  uint8_t nSec = 0;
  readAllTimeout(cfd, &nSec, 1);
  ASSERT_GE(nSec, 1);
  std::vector<uint8_t> secList(nSec);
  readAllTimeout(cfd, secList.data(), nSec);
  uint8_t secNone = 1;
  writeAll(cfd, &secNone, 1);

  uint32_t secRes = 1;
  readAllTimeout(cfd, &secRes, 4);
  EXPECT_EQ(readBe32(reinterpret_cast<const uint8_t*>(&secRes)), 0u);

  uint8_t clientInit = 1;
  writeAll(cfd, &clientInit, 1);

  uint8_t serverInit[24];
  readAllTimeout(cfd, serverInit, 24);
  EXPECT_EQ(readBe16(serverInit), 1920u);
  EXPECT_EQ(readBe16(serverInit + 2), 1080u);
  uint32_t nameLen = readBe32(serverInit + 20);
  std::string srvName(nameLen, '\0');
  readAllTimeout(cfd, &srvName[0], nameLen);
  EXPECT_EQ(srvName, "WayDroidTest");

  // Send SetEncodings: 50 (H.264), -313 (ContinuousUpdates), -308 (ExtendedDesktopSize), -223, -312
  int32_t encs[] = {
    rfb::encodingH264,
    rfb::pseudoEncodingContinuousUpdates,
    rfb::pseudoEncodingExtendedDesktopSize,
    rfb::pseudoEncodingDesktopSize,
    rfb::pseudoEncodingFence,
  };
  std::vector<uint8_t> setEncMsg(4 + sizeof(encs));
  setEncMsg[0] = rfb::msgTypeSetEncodings;
  setEncMsg[1] = 0;
  writeBe16(setEncMsg.data() + 2, static_cast<uint16_t>(sizeof(encs) / 4));
  for (size_t i = 0; i < sizeof(encs) / 4; ++i)
    writeBe32(setEncMsg.data() + 4 + i * 4, static_cast<uint32_t>(encs[i]));
  writeAll(cfd, setEncMsg.data(), setEncMsg.size());

  // Expect Fence(type=0) then EndOfContinuousUpdates
  uint8_t srvMsg = 0;
  readAllTimeout(cfd, &srvMsg, 1);
  EXPECT_EQ(srvMsg, rfb::msgTypeServerFence);
  uint8_t fenceBody[8];
  readAllTimeout(cfd, fenceBody, 8);
  uint8_t fenceLen = fenceBody[7];
  if (fenceLen > 0) {
    std::vector<uint8_t> fdata(fenceLen);
    readAllTimeout(cfd, fdata.data(), fenceLen);
  }

  readAllTimeout(cfd, &srvMsg, 1);
  EXPECT_EQ(srvMsg, rfb::msgTypeEndOfContinuousUpdates);

  // Enable ContinuousUpdates without sending any FramebufferUpdateRequest
  uint8_t enableCU[10];
  enableCU[0] = rfb::msgTypeEnableContinuousUpdates;
  enableCU[1] = 1;
  writeBe16(enableCU + 2, 0);
  writeBe16(enableCU + 4, 0);
  writeBe16(enableCU + 6, 1920);
  writeBe16(enableCU + 8, 1080);
  writeAll(cfd, enableCU, sizeof(enableCU));

  // Verify RESET_VIDEO (0x11) arrived on control socket when CU was enabled
  uint8_t ctrlByte = 0;
  readAllTimeout(ctrlFds[1], &ctrlByte, 1);
  EXPECT_EQ(ctrlByte, 17u);

  // Stream scrcpy Config (SPS+PPS) followed by raw IDR slice (without inline SPS/PPS)
  sendScrcpyPacket(videoFds[1], UINT64_C(1) << 63, kFrame1080, kFrame1080SpsPpsLen);
  sendScrcpyPacket(videoFds[1], UINT64_C(1) << 62,
                   kFrame1080 + kFrame1080SpsPpsLen,
                   sizeof(kFrame1080) - kFrame1080SpsPpsLen);

  auto readH264Update = [&](int fd, uint16_t* outW, uint16_t* outH,
                            uint32_t* outFlags, std::vector<uint8_t>* outBuf) {
    uint8_t hdr[16];
    readAllTimeout(fd, hdr, sizeof(hdr));
    ASSERT_EQ(hdr[0], rfb::msgTypeFramebufferUpdate);
    ASSERT_EQ(readBe16(hdr + 2), 1u);
    *outW = readBe16(hdr + 8);
    *outH = readBe16(hdr + 10);
    ASSERT_EQ(readBeS32(hdr + 12), rfb::encodingH264);

    uint8_t h264Hdr[8];
    readAllTimeout(fd, h264Hdr, sizeof(h264Hdr));
    uint32_t payLen = readBe32(h264Hdr);
    *outFlags = readBe32(h264Hdr + 4);

    outBuf->resize(8 + payLen);
    std::memcpy(outBuf->data(), h264Hdr, 8);
    readAllTimeout(fd, outBuf->data() + 8, payLen);
  };

  uint16_t rw = 0, rh = 0;
  uint32_t rflags = 99;
  std::vector<uint8_t> rbuf;
  readH264Update(cfd, &rw, &rh, &rflags, &rbuf);

  EXPECT_EQ(rw, 1920u);
  EXPECT_EQ(rh, 1080u);
  EXPECT_EQ(rflags, 1u); // Reset flag set on first IDR
  EXPECT_EQ(rbuf.size(), 8u + sizeof(kFrame1080)); // Cached SPS/PPS prepended!
  EXPECT_EQ(std::memcmp(rbuf.data() + 8, kFrame1080, kFrame1080SpsPpsLen), 0);

  rfb::H264Decoder decoder;
  rfb::ServerParams sparams;
  rfb::PixelFormat pf(32, 24, false, true, 255, 255, 255, 16, 8, 0);
  sparams.setDimensions(1920, 1080);
  sparams.setPF(pf);
  rfb::ManagedPixelBuffer pb1080(pf, 1920, 1080);
  decoder.decodeRect({0, 0, 1920, 1080}, rbuf.data(), rbuf.size(), sparams, &pb1080);
  // Decode twice in case decoder buffers 1 frame
  uint8_t secondHdr[8];
  writeBe32(secondHdr, static_cast<uint32_t>(sizeof(kFrame1080) - kFrame1080SpsPpsLen));
  writeBe32(secondHdr + 4, 0u);
  std::vector<uint8_t> secondFrame(8 + sizeof(kFrame1080) - kFrame1080SpsPpsLen);
  std::memcpy(secondFrame.data(), secondHdr, 8);
  std::memcpy(secondFrame.data() + 8, kFrame1080 + kFrame1080SpsPpsLen,
              sizeof(kFrame1080) - kFrame1080SpsPpsLen);
  decoder.decodeRect({0, 0, 1920, 1080}, secondFrame.data(), secondFrame.size(),
                     sparams, &pb1080);

  int stride = 0;
  uint8_t rgb[3];
  const uint8_t* px = pb1080.getBuffer(core::Rect(960, 540, 961, 541), &stride);
  pf.rgbFromBuffer(rgb, px, 1);
  EXPECT_NEAR(rgb[0], 0x80, 8);
  EXPECT_NEAR(rgb[1], 0x80, 8);
  EXPECT_NEAR(rgb[2], 0x80, 8);

  // Send non-incremental FramebufferUpdateRequest -> must resend IDR with SPS/PPS + reset flag 1
  uint8_t fbur[10];
  fbur[0] = rfb::msgTypeFramebufferUpdateRequest;
  fbur[1] = 0; // incremental = 0
  writeBe16(fbur + 2, 0);
  writeBe16(fbur + 4, 0);
  writeBe16(fbur + 6, 1920);
  writeBe16(fbur + 8, 1080);
  writeAll(cfd, fbur, sizeof(fbur));

  readH264Update(cfd, &rw, &rh, &rflags, &rbuf);
  EXPECT_EQ(rflags, 1u);
  EXPECT_EQ(rbuf.size(), 8u + sizeof(kFrame1080));

  // Connect Client 2 advertising ONLY DesktopSize (-223), not ExtendedDesktopSize (-308)
  network::TcpSocket client2("127.0.0.1", port);
  int c2 = client2.getFd();
  readAllTimeout(c2, ver, 12);
  writeAll(c2, "RFB 003.008\n", 12);
  readAllTimeout(c2, &nSec, 1);
  secList.resize(nSec);
  readAllTimeout(c2, secList.data(), nSec);
  writeAll(c2, &secNone, 1);
  readAllTimeout(c2, &secRes, 4);
  writeAll(c2, &clientInit, 1);
  readAllTimeout(c2, serverInit, 24);
  nameLen = readBe32(serverInit + 20);
  srvName.resize(nameLen);
  readAllTimeout(c2, &srvName[0], nameLen);

  int32_t encs2[] = {
    rfb::encodingH264,
    rfb::pseudoEncodingContinuousUpdates,
    rfb::pseudoEncodingDesktopSize,
  };
  std::vector<uint8_t> setEnc2(4 + sizeof(encs2));
  setEnc2[0] = rfb::msgTypeSetEncodings;
  setEnc2[1] = 0;
  writeBe16(setEnc2.data() + 2, 3);
  for (size_t i = 0; i < 3; ++i)
    writeBe32(setEnc2.data() + 4 + i * 4, static_cast<uint32_t>(encs2[i]));
  writeAll(c2, setEnc2.data(), setEnc2.size());
  readAllTimeout(c2, &srvMsg, 1);
  EXPECT_EQ(srvMsg, rfb::msgTypeEndOfContinuousUpdates);
  writeAll(c2, enableCU, sizeof(enableCU));
  // Drain Client 2's initial cached IDR
  readH264Update(c2, &rw, &rh, &rflags, &rbuf);

  // Trigger resolution change to 2560x1440 on scrcpy stream!
  sendScrcpyPacket(videoFds[1], UINT64_C(1) << 63, kFrame1440, kFrame1440SpsPpsLen);
  sendScrcpyPacket(videoFds[1], UINT64_C(1) << 62,
                   kFrame1440 + kFrame1440SpsPpsLen,
                   sizeof(kFrame1440) - kFrame1440SpsPpsLen);

  // Client 1 must receive ExtendedDesktopSize (-308) then 2560x1440 H.264 IDR with flags=3
  uint8_t extHdr[16];
  readAllTimeout(cfd, extHdr, sizeof(extHdr));
  EXPECT_EQ(extHdr[0], rfb::msgTypeFramebufferUpdate);
  EXPECT_EQ(readBe16(extHdr + 2), 1u);
  EXPECT_EQ(readBe16(extHdr + 4), rfb::reasonServer);
  EXPECT_EQ(readBe16(extHdr + 8), 2560u);
  EXPECT_EQ(readBe16(extHdr + 10), 1440u);
  EXPECT_EQ(readBeS32(extHdr + 12), rfb::pseudoEncodingExtendedDesktopSize);
  uint8_t extBody[20]; // 1 byte num_screens + 3 pad + 16 byte screen
  readAllTimeout(cfd, extBody, sizeof(extBody));
  EXPECT_EQ(extBody[0], 1u);

  readH264Update(cfd, &rw, &rh, &rflags, &rbuf);
  EXPECT_EQ(rw, 2560u);
  EXPECT_EQ(rh, 1440u);
  EXPECT_EQ(rflags, 3u); // resetContext | resetAllContexts
  EXPECT_EQ(rbuf.size(), 8u + sizeof(kFrame1440));

  sparams.setDimensions(2560, 1440);
  rfb::ManagedPixelBuffer pb1440(pf, 2560, 1440);
  decoder.decodeRect({0, 0, 2560, 1440}, rbuf.data(), rbuf.size(), sparams, &pb1440);
  writeBe32(secondHdr, static_cast<uint32_t>(sizeof(kFrame1440) - kFrame1440SpsPpsLen));
  writeBe32(secondHdr + 4, 0u);
  std::vector<uint8_t> second1440(8 + sizeof(kFrame1440) - kFrame1440SpsPpsLen);
  std::memcpy(second1440.data(), secondHdr, 8);
  std::memcpy(second1440.data() + 8, kFrame1440 + kFrame1440SpsPpsLen,
              sizeof(kFrame1440) - kFrame1440SpsPpsLen);
  decoder.decodeRect({0, 0, 2560, 1440}, second1440.data(), second1440.size(),
                     sparams, &pb1440);
  px = pb1440.getBuffer(core::Rect(1280, 720, 1281, 721), &stride);
  pf.rgbFromBuffer(rgb, px, 1);
  EXPECT_NEAR(rgb[0], 0x80, 8);
  EXPECT_NEAR(rgb[1], 0x80, 8);
  EXPECT_NEAR(rgb[2], 0x80, 8);

  // Client 2 must receive DesktopSize (-223) then 2560x1440 H.264 IDR with flags=3
  uint8_t dsHdr[16];
  readAllTimeout(c2, dsHdr, sizeof(dsHdr));
  EXPECT_EQ(dsHdr[0], rfb::msgTypeFramebufferUpdate);
  EXPECT_EQ(readBe16(dsHdr + 8), 2560u);
  EXPECT_EQ(readBe16(dsHdr + 10), 1440u);
  EXPECT_EQ(readBeS32(dsHdr + 12), rfb::pseudoEncodingDesktopSize);
  readH264Update(c2, &rw, &rh, &rflags, &rbuf);
  EXPECT_EQ(rw, 2560u);
  EXPECT_EQ(rh, 1440u);
  EXPECT_EQ(rflags, 3u);

  stopServer = true;
  server.terminate();
  serverThread.join();
  ::close(videoFds[1]);
  ::close(ctrlFds[1]);
}
#endif
