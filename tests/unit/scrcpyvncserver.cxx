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
#include <atomic>
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
#include <network/UnixSocket.h>
#include <rdr/FdInStream.h>
#include <rdr/FdOutStream.h>
#include <rfb/Exception.h>
#include <rfb/PixelBuffer.h>
#include <rfb/PixelFormat.h>
#include <rfb/CConnection.h>
#include <rfb/CMsgReader.h>
#include <rfb/CMsgWriter.h>
#include <rfb/CSecurityTight.h>
#include <rfb/FileTransferClient.h>
#include <rfb/SConnection.h>
#include <rfb/SSecurityTight.h>
#include <rfb/SSecurityVncAuth.h>
#include <rfb/SecurityClient.h>
#include <rfb/SecurityServer.h>
#include <rfb/ServerCore.h>
#include <rfb/ServerParams.h>
#include <rfb/encodings.h>
#include <rfb/fileTransferTypes.h>
#include <rfb/giiTypes.h>
#include <rfb/msgTypes.h>
#include <rfb/screenTypes.h>
#include <rfb/obfuscate.h>

#include <zlib.h>

#ifdef HAVE_H264
#include <rfb/H264Decoder.h>
#endif

#include "AdbClient.h"
#include "AdbFileTransfer.h"
#include "H264SpsParser.h"
#include "ScrcpyControl.h"
#include "ScrcpyServer.h"
#include "ScrcpyStream.h"
#include "ZeroCopyOutStream.h"
#include "../../unix/scrcpyvncserver/parameters.h"

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

uint64_t readBe64(const uint8_t* p)
{
  return (static_cast<uint64_t>(readBe32(p)) << 32) |
         static_cast<uint64_t>(readBe32(p + 4));
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

void negotiateRfb(ScrcpyServer& server, int fd)
{
  char version[12];
  readAllTimeout(fd, version, sizeof(version));
  ASSERT_EQ(std::memcmp(version, "RFB 003.008\n", sizeof(version)), 0);
  writeAll(fd, version, sizeof(version));
  ASSERT_TRUE(server.step({}, 0));

  uint8_t security[2];
  readAllTimeout(fd, security, sizeof(security));
  ASSERT_EQ(security[0], 1);
  ASSERT_EQ(security[1], 1);
  writeAll(fd, security + 1, 1);
  ASSERT_TRUE(server.step({}, 0));
}

void initialiseRfb(ScrcpyServer& server, int fd)
{
  ASSERT_NO_FATAL_FAILURE(negotiateRfb(server, fd));
  uint8_t result[4];
  readAllTimeout(fd, result, sizeof(result));
  ASSERT_EQ(readBe32(result), 0u);

  uint8_t shared = 1;
  writeAll(fd, &shared, 1);
  ASSERT_TRUE(server.step({}, 0));
  uint8_t header[24];
  readAllTimeout(fd, header, sizeof(header));
  std::vector<uint8_t> name(readBe32(header + 20));
  readAllTimeout(fd, name.data(), name.size());
}

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
  std::atomic<bool> stopMock{false};

  auto readAdbCmd = [](int fd) -> std::string {
    char hex[5] = {};
    readAllTimeout(fd, hex, sizeof(hex) - 1);
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
  stopMock = true;
  mockAdb.join();
  ::close(srv);

  EXPECT_EQ(pushedSpec, "/data/local/tmp/scrcpy-server.jar,33188");
  std::string gotStr(pushedBytes.begin(), pushedBytes.end());
  EXPECT_EQ(gotStr, sampleData);

  EXPECT_EQ(AdbClient::detectVersionFromPath("/tmp/scrcpy-server-v5.0.1"), "5.0.1");
  EXPECT_EQ(AdbClient::detectVersionFromPath("/usr/share/scrcpy-server-3.3.4.jar"), "3.3.4");
  EXPECT_EQ(AdbClient::extractExpectedVersionFromError(
              "ERROR: The server version (5.0.1) does not match"), "5.0.1");
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
  EXPECT_EQ(readBeS16(scroll + 15), 2048);

  EXPECT_EQ(key1[0], 0);
  EXPECT_EQ(key1[1], 0); // ACTION_DOWN
  EXPECT_EQ(readBe32(key1 + 2), 29u); // AKEYCODE_A
  EXPECT_EQ(key2[1], 1); // ACTION_UP
  EXPECT_EQ(rst[0], 17);

  ctrl.setServerVersion("2.7");
  ctrl.sendResetVideo();
  ctrl.sendScrollEvent(150, 250, 1920, 1080, 0.0f, 1.0f, 0);
  readAllTimeout(fds[1], scroll, sizeof(scroll));
  EXPECT_EQ(scroll[0], 3);
  EXPECT_EQ(readBeS16(scroll + 15), 32767);

  ctrl.setServerVersion("3.3.0");
  ctrl.sendResetVideo();
  readAllTimeout(fds[1], rst, sizeof(rst));
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
  writeAll(fds[1], devMsg, 3);
  EXPECT_TRUE(ctrl.processReadable(cb));
  EXPECT_TRUE(cb.clip.empty());

  writeAll(fds[1], devMsg + 3, sizeof(devMsg) - 3);
  ASSERT_EQ(::shutdown(fds[1], SHUT_WR), 0);
  EXPECT_FALSE(ctrl.processReadable(cb));
  EXPECT_EQ(cb.clip, "hello-android");

  ::close(fds[1]);
}

TEST(ScrcpyVncEndToEndTest, RejectsUnsupportedViewerAndQueryConnections)
{
  rfb::SecurityServer::secTypes.setParam("None");

  int video[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, video), 0);
  network::UnixSocket source(video[1]);
  ScrcpyServer server;
  server.attachStreamsForTest(video[0], -1, ScrcpyProtocol::V2,
                              1920, 1080, "Test");

  int unsupported[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, unsupported), 0);
  network::UnixSocket viewer(unsupported[1]);
  ASSERT_TRUE(server.addSocket(new network::UnixSocket(unsupported[0])));
  ASSERT_NO_FATAL_FAILURE(initialiseRfb(server, viewer.getFd()));

  uint8_t noEncodings[] = {rfb::msgTypeSetEncodings, 0, 0, 0};
  writeAll(viewer.getFd(), noEncodings, sizeof(noEncodings));
  ASSERT_TRUE(server.step({}, 0));
  uint8_t byte;
  EXPECT_EQ(::recv(viewer.getFd(), &byte, 1, MSG_DONTWAIT), 0);

  int query[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, query), 0);
  network::UnixSocket queryViewer(query[1]);
  auto* querySocket = new network::UnixSocket(query[0]);
  querySocket->setRequiresQuery();
  ASSERT_TRUE(server.addSocket(querySocket));
  ASSERT_NO_FATAL_FAILURE(negotiateRfb(server, queryViewer.getFd()));
  uint8_t securityResult[4];
  readAllTimeout(queryViewer.getFd(), securityResult, sizeof(securityResult));
  EXPECT_EQ(readBe32(securityResult), 1u);
}

TEST(ScrcpyVncEndToEndTest, ResyncsAfterSkippedFramesAndConfigChanges)
{
  rfb::SecurityServer::secTypes.setParam("None");
  int video[2], connection[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, video), 0);
  network::UnixSocket source(video[1]);
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, connection), 0);
  network::UnixSocket viewer(connection[1]);

  ScrcpyServer server;
  server.attachStreamsForTest(video[0], -1, ScrcpyProtocol::V2,
                              1920, 1080, "Test");
  ASSERT_TRUE(server.addSocket(new network::UnixSocket(connection[0])));
  int fd = viewer.getFd();
  ASSERT_NO_FATAL_FAILURE(initialiseRfb(server, fd));

  uint8_t encodings[] = {rfb::msgTypeSetEncodings, 0, 0, 1, 0, 0, 0, 0};
  writeBe32(encodings + 4, rfb::encodingH264);
  writeAll(fd, encodings, sizeof(encodings));

  uint8_t update[] = {rfb::msgTypeFramebufferUpdateRequest, 0, 0, 0, 0, 0,
                      0, 0, 0, 0};
  writeBe16(update + 6, 1920);
  writeBe16(update + 8, 1080);
  writeAll(fd, update, sizeof(update));

  auto readIdr = [&](const uint8_t* expectedConfig) {
    uint8_t header[24];
    readAllTimeout(fd, header, sizeof(header));
    ASSERT_EQ(header[0], rfb::msgTypeFramebufferUpdate);
    ASSERT_EQ(readBeS32(header + 12), rfb::encodingH264);
    ASSERT_EQ(readBe32(header + 16), sizeof(kFrame1080));
    EXPECT_EQ(readBe32(header + 20), 1u);
    uint8_t frame[sizeof(kFrame1080)];
    readAllTimeout(fd, frame, sizeof(frame));
    EXPECT_EQ(std::memcmp(frame, expectedConfig, kFrame1080SpsPpsLen), 0);
  };

  const uint64_t configFlag = UINT64_C(1) << 63;
  const uint64_t keyFlag = UINT64_C(1) << 62;
  const uint8_t* idr = kFrame1080 + kFrame1080SpsPpsLen;
  const size_t idrLen = sizeof(kFrame1080) - kFrame1080SpsPpsLen;
  sendScrcpyPacket(video[1], configFlag, kFrame1080, kFrame1080SpsPpsLen);
  sendScrcpyPacket(video[1], keyFlag, idr, idrLen);
  ASSERT_TRUE(server.step({}, 0));
  ASSERT_NO_FATAL_FAILURE(readIdr(kFrame1080));

  // An incremental viewer can miss frames between update requests.
  const uint8_t delta[] = {0, 0, 1, 0x41, 0x80};
  sendScrcpyPacket(video[1], 0, delta, sizeof(delta));
  ASSERT_TRUE(server.step({}, 0));
  update[1] = 1;
  writeAll(fd, update, sizeof(update));
  sendScrcpyPacket(video[1], 0, delta, sizeof(delta));
  ASSERT_TRUE(server.step({}, 0));
  struct pollfd pending = {fd, POLLIN, 0};
  EXPECT_EQ(::poll(&pending, 1, 0), 0);
  sendScrcpyPacket(video[1], keyFlag, idr, idrLen);
  ASSERT_TRUE(server.step({}, 0));
  ASSERT_NO_FATAL_FAILURE(readIdr(kFrame1080));

  // A new config with unchanged dimensions invalidates the previous IDR.
  uint8_t config[kFrame1080SpsPpsLen];
  std::memcpy(config, kFrame1080, sizeof(config));
  config[sizeof(config) - 1] ^= 1;
  sendScrcpyPacket(video[1], configFlag, config, sizeof(config));
  ASSERT_TRUE(server.step({}, 0));
  update[1] = 0;
  writeAll(fd, update, sizeof(update));
  sendScrcpyPacket(video[1], 0, delta, sizeof(delta));
  ASSERT_TRUE(server.step({}, 0));
  EXPECT_EQ(::poll(&pending, 1, 0), 0);
  sendScrcpyPacket(video[1], keyFlag, idr, idrLen);
  ASSERT_TRUE(server.step({}, 0));
  ASSERT_NO_FATAL_FAILURE(readIdr(config));
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

  std::atomic<bool> stopServer{false};
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
  serverThread.join();
  ::close(videoFds[1]);
  ::close(ctrlFds[1]);
}
#endif

namespace {

class TightTestSConnection : public rfb::SConnection {
public:
  TightTestSConnection(network::Socket* s, rfb::AccessRights ar)
    : rfb::SConnection(s, ar) {}

  rfb::SecurityServer* sec() { return getSecurity(); }
  bool readyCalled = false;

protected:
  void clientReady(bool /*shared*/) override
  {
    readyCalled = true;
    client.setDimensions(640, 480);
    client.setName("TightTest");
    desktopReady();
  }
  void setPixelFormat(const rfb::PixelFormat& pf) override
  {
    rfb::SConnection::setPixelFormat(pf);
  }
  void setEncodings(int n, const int32_t* enc) override
  {
    rfb::SConnection::setEncodings(n, enc);
  }
  void framebufferUpdateRequest(const core::Rect& r, bool inc) override
  {
    rfb::SConnection::framebufferUpdateRequest(r, inc);
  }
  void setDesktopSize(int, int, const rfb::ScreenSet&) override {}
  void keyEvent(uint32_t, uint32_t, bool) override {}
  void pointerEvent(const core::Point&, uint16_t) override {}
};

class TightTestCConnection : public rfb::CConnection {
public:
  explicit TightTestCConnection(bool preferTight = true)
  {
    supportsFileTransfer = true;
    preferTightForFileTransfer = preferTight;
  }

  bool initialized = false;
  std::vector<uint8_t> h264Frames;
  bool md5Done = false;
  uint8_t lastMd5[16] = {};
  bool dirSizeDone = false;
  uint64_t lastDirSize = 0;

  rfb::CMsgWriter* msgWriter() { return writer(); }

  void tight2Md5Reply(const uint8_t digest[16]) override
  {
    std::memcpy(lastMd5, digest, 16);
    md5Done = true;
    rfb::CConnection::tight2Md5Reply(digest);
  }

  void tight2DirSizeReply(uint64_t size) override
  {
    lastDirSize = size;
    dirSizeDone = true;
    rfb::CConnection::tight2DirSizeReply(size);
  }

  void initDone() override
  {
    initialized = true;
    setFramebuffer(new rfb::ManagedPixelBuffer(
      rfb::PixelFormat(32, 24, false, true, 255, 255, 255, 16, 8, 0),
      server.width(), server.height()));
  }
  void bell() override {}
  void getUserPasswd(std::string* user, std::string* password) override
  {
    if (user)
      *user = "";
    if (password)
      *password = "pass1234";
  }
  bool verifyCertificate(unsigned int, const uint8_t*, size_t) override
  {
    return true;
  }
  bool verifyHostKey(const uint8_t*, size_t, const char*) override
  {
    return true;
  }
  bool dataRect(const core::Rect& r, int encoding) override
  {
    if (encoding == rfb::encodingH264) {
      if (!getInStream()->hasData(8))
        return false;
      getInStream()->setRestorePoint();
      uint32_t len = getInStream()->readU32();
      getInStream()->skip(4); // flags
      if (!getInStream()->hasDataOrRestore(len))
        return false;
      getInStream()->clearRestorePoint();
      size_t old = h264Frames.size();
      h264Frames.resize(old + len);
      if (len > 0)
        getInStream()->readBytes(h264Frames.data() + old, len);
      return true;
    }
    return rfb::CConnection::dataRect(r, encoding);
  }
};

class ScrcpyFtRecordingHandler : public rfb::FileTransferHandler {
public:
  bool ready = false;
  rfb::FileTransferCaps caps;

  bool listDone = false;
  bool listOk = false;
  std::vector<rfb::FileEntry> entries;
  std::string listErr;

  bool dlStarted = false;
  bool dlStartOk = false;
  std::string dlStartErr;
  std::vector<uint8_t> dlData;
  bool dlComplete = false;
  bool dlCompleteOk = false;
  std::string dlCompleteErr;

  bool ulReady = false;
  bool ulReadyOk = false;
  std::string ulReadyErr;
  int ulAckCount = 0;
  bool ulComplete = false;
  bool ulCompleteOk = false;
  std::string ulCompleteErr;

  bool cmdDone = false;
  bool cmdOk = false;
  std::string cmdErr;

  bool cancelDone = false;

  void resetOp()
  {
    listDone = listOk = false;
    entries.clear();
    listErr.clear();
    dlStarted = dlStartOk = dlComplete = dlCompleteOk = false;
    dlStartErr.clear();
    dlCompleteErr.clear();
    dlData.clear();
    ulReady = ulReadyOk = ulComplete = ulCompleteOk = false;
    ulReadyErr.clear();
    ulCompleteErr.clear();
    ulAckCount = 0;
    cmdDone = cmdOk = false;
    cmdErr.clear();
    cancelDone = false;
  }

  void onFileTransferReady(const rfb::FileTransferCaps& c) override
  {
    ready = true;
    caps = c;
  }
  void onListComplete(bool ok, const std::vector<rfb::FileEntry>& e,
                      const std::string& error) override
  {
    listDone = true;
    listOk = ok;
    entries = e;
    listErr = error;
  }
  void onDownloadStarted(bool ok, uint64_t, bool,
                         const std::string& error) override
  {
    dlStarted = true;
    dlStartOk = ok;
    dlStartErr = error;
  }
  void onDownloadData(const uint8_t* data, size_t len) override
  {
    if (data && len > 0)
      dlData.insert(dlData.end(), data, data + len);
  }
  void onDownloadComplete(bool ok, const std::string& error) override
  {
    dlComplete = true;
    dlCompleteOk = ok;
    dlCompleteErr = error;
  }
  void onUploadReady(bool ok, const std::string& error) override
  {
    ulReady = true;
    ulReadyOk = ok;
    ulReadyErr = error;
  }
  void onUploadChunkAck() override { ulAckCount++; }
  void onUploadComplete(bool ok, const std::string& error) override
  {
    ulComplete = true;
    ulCompleteOk = ok;
    ulCompleteErr = error;
  }
  void onCommandComplete(bool ok, const std::string& error) override
  {
    cmdDone = true;
    cmdOk = ok;
    cmdErr = error;
  }
  void onCancelComplete() override { cancelDone = true; }
};

class MockAdbFileDaemon {
public:
  struct Node {
    bool isDir = false;
    bool isSymlink = false;
    bool executable = false;
    std::string linkTarget;
    std::vector<uint8_t> data;
    int64_t mtime = 1700000100LL;
  };

  MockAdbFileDaemon(bool v2Features = true)
    : v2Features_(v2Features), srvFd_(-1), port_(0), stop_(false)
  {
    nodes_["/"] = {true, false, false, "", {}, 1700000000LL};
    nodes_["/storage"] = {true, false, false, "", {}, 1700000000LL};
    nodes_["/storage/emulated"] = {true, false, false, "", {}, 1700000000LL};
    nodes_["/sdcard"] = {false, true, false, "/storage/emulated", {}, 1700000000LL};
    nodes_["/data"] = {true, false, false, "", {}, 1700000000LL};
    nodes_["/data/local"] = {true, false, false, "", {}, 1700000000LL};
    nodes_["/data/local/tmp"] = {true, false, false, "", {}, 1700000000LL};
    nodes_["/system"] = {true, false, false, "", {}, 1700000000LL};
    std::string hello = "Hello Android!";
    nodes_["/data/local/tmp/hello.txt"] = {
      false, false, false, "",
      std::vector<uint8_t>(hello.begin(), hello.end()), 1700000500LL
    };
    nodes_["/data/local/tmp/empty.txt"] = {
      false, false, false, "", {}, 1700000200LL
    };
    nodes_["/data/local/tmp/run.sh"] = {
      false, false, true, "", {'#', '!', '/', 'b', 'i', 'n', '\n'}, 1700000300LL
    };
  }

  ~MockAdbFileDaemon() { stop(); }

  bool start()
  {
    srvFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srvFd_ < 0)
      return false;
    int one = 1;
    ::setsockopt(srvFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(srvFd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(srvFd_, 16) != 0)
      return false;
    port_ = network::getSockPort(srvFd_);
    thread_ = std::thread(&MockAdbFileDaemon::run, this);
    return true;
  }

  void stop()
  {
    stop_ = true;
    if (srvFd_ >= 0) {
      ::shutdown(srvFd_, SHUT_RDWR);
      ::close(srvFd_);
      srvFd_ = -1;
    }
    if (thread_.joinable())
      thread_.join();
  }

  int port() const { return port_; }

  bool getFile(const std::string& path, std::vector<uint8_t>* out,
               bool* execOut = nullptr)
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = nodes_.find(path);
    if (it == nodes_.end() || it->second.isDir)
      return false;
    if (out)
      *out = it->second.data;
    if (execOut)
      *execOut = it->second.executable;
    return true;
  }

  bool hasPath(const std::string& path)
  {
    std::lock_guard<std::mutex> lk(mu_);
    return nodes_.find(path) != nodes_.end();
  }

private:
  static void putLe32(uint8_t* p, uint32_t v)
  {
    p[0] = static_cast<uint8_t>(v & 0xff);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xff);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xff);
  }
  static void putLe64(uint8_t* p, uint64_t v)
  {
    putLe32(p, static_cast<uint32_t>(v & 0xffffffffu));
    putLe32(p + 4, static_cast<uint32_t>(v >> 32));
  }
  static uint32_t getLe32(const uint8_t* p)
  {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
  }
  static bool readExact(int fd, uint8_t* buf, size_t len)
  {
    size_t off = 0;
    while (off < len) {
      ssize_t n = ::recv(fd, buf + off, len - off, 0);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        return false;
      off += static_cast<size_t>(n);
    }
    return true;
  }
  static bool writeExact(int fd, const void* buf, size_t len)
  {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t off = 0;
    while (off < len) {
      ssize_t n = ::send(fd, p + off, len - off, MSG_NOSIGNAL);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        return false;
      off += static_cast<size_t>(n);
    }
    return true;
  }
  static std::string readAdbCmd(int fd, bool* ok)
  {
    char hex[5] = {};
    if (!readExact(fd, reinterpret_cast<uint8_t*>(hex), 4)) {
      *ok = false;
      return "";
    }
    unsigned len = 0;
    std::sscanf(hex, "%4x", &len);
    std::string cmd(len, '\0');
    if (len > 0 && !readExact(fd, reinterpret_cast<uint8_t*>(&cmd[0]), len)) {
      *ok = false;
      return "";
    }
    *ok = true;
    return cmd;
  }

  uint32_t modeForNode(const Node& n, bool follow) const
  {
    if (n.isSymlink && !follow)
      return 0120777u;
    if (n.isSymlink && follow) {
      auto it = nodes_.find(n.linkTarget);
      if (it != nodes_.end())
        return modeForNode(it->second, true);
    }
    if (n.isDir)
      return 0040755u;
    return n.executable ? 0100755u : 0100644u;
  }

  const Node* resolveNode(const std::string& path, bool follow) const
  {
    auto it = nodes_.find(path);
    if (it == nodes_.end())
      return nullptr;
    if (follow && it->second.isSymlink) {
      auto tit = nodes_.find(it->second.linkTarget);
      return (tit == nodes_.end()) ? nullptr : &tit->second;
    }
    return &it->second;
  }

  static std::string unquoteSingle(const std::string& s, size_t start,
                                   size_t* nextPos)
  {
    if (start >= s.size() || s[start] != '\'')
      return "";
    std::string out;
    size_t i = start + 1;
    while (i < s.size()) {
      if (s[i] == '\'') {
        if (i + 3 < s.size() && s[i + 1] == '\\' && s[i + 2] == '\'' &&
            s[i + 3] == '\'') {
          out.push_back('\'');
          i += 4;
          continue;
        }
        *nextPos = i + 1;
        return out;
      }
      out.push_back(s[i++]);
    }
    *nextPos = s.size();
    return out;
  }

  void handleSync(int fd)
  {
    writeExact(fd, "OKAY", 4);
    while (true) {
      uint8_t hdr[8];
      if (!readExact(fd, hdr, 8))
        break;
      uint32_t len = getLe32(hdr + 4);
      if (std::memcmp(hdr, "QUIT", 4) == 0)
        break;

      if (std::memcmp(hdr, "STA2", 4) == 0 ||
          std::memcmp(hdr, "LST2", 4) == 0 ||
          std::memcmp(hdr, "STAT", 4) == 0) {
        std::string p(len, '\0');
        if (len > 0 && !readExact(fd, reinterpret_cast<uint8_t*>(&p[0]), len))
          break;
        bool follow = std::memcmp(hdr, "LST2", 4) != 0;
        std::lock_guard<std::mutex> lk(mu_);
        const Node* node = resolveNode(p, follow);
        uint32_t err = node ? 0u : static_cast<uint32_t>(ENOENT);
        uint32_t mode = 0;
        uint64_t size = 0;
        int64_t mtime = 0;
        if (node) {
          auto it = nodes_.find(p);
          mode = (!follow && it->second.isSymlink)
            ? 0120777u : modeForNode(*node, true);
          size = node->isDir ? 0u : node->data.size();
          mtime = node->mtime;
        }
        if (std::memcmp(hdr, "STAT", 4) == 0) {
          uint8_t resp[16] = {};
          std::memcpy(resp, "STAT", 4);
          putLe32(resp + 4, mode);
          putLe32(resp + 8, static_cast<uint32_t>(size));
          putLe32(resp + 12, static_cast<uint32_t>(mtime));
          writeExact(fd, resp, sizeof(resp));
        } else {
          uint8_t resp[72] = {};
          std::memcpy(resp, hdr, 4);
          putLe32(resp + 4, err);
          putLe32(resp + 24, mode);
          putLe64(resp + 40, size);
          putLe64(resp + 56, static_cast<uint64_t>(mtime));
          writeExact(fd, resp, sizeof(resp));
        }
        continue;
      }

      if (std::memcmp(hdr, "LIS2", 4) == 0 ||
          std::memcmp(hdr, "LIST", 4) == 0) {
        bool v2 = std::memcmp(hdr, "LIS2", 4) == 0;
        std::string dir(len, '\0');
        if (len > 0 && !readExact(fd, reinterpret_cast<uint8_t*>(&dir[0]), len))
          break;
        std::vector<std::pair<std::string, Node>> children;
        {
          std::lock_guard<std::mutex> lk(mu_);
          const Node* dn = resolveNode(dir, true);
          std::string base = dir;
          auto rawIt = nodes_.find(dir);
          if (rawIt != nodes_.end() && rawIt->second.isSymlink)
            base = rawIt->second.linkTarget;
          std::string prefix = (base == "/") ? "/" : (base + "/");
          if (dn && dn->isDir) {
            for (const auto& kv : nodes_) {
              if (kv.first == base)
                continue;
              if (kv.first.compare(0, prefix.size(), prefix) != 0)
                continue;
              std::string rest = kv.first.substr(prefix.size());
              if (rest.find('/') != std::string::npos)
                continue;
              children.emplace_back(rest, kv.second);
            }
          }
        }
        for (const auto& ch : children) {
          uint32_t mode = ch.second.isSymlink
            ? 0120777u : (ch.second.isDir ? 0040755u : (ch.second.executable ? 0100755u : 0100644u));
          uint64_t size = ch.second.isDir ? 0u : ch.second.data.size();
          if (v2) {
            uint8_t dnt[76] = {};
            std::memcpy(dnt, "DNT2", 4);
            putLe32(dnt + 24, mode);
            putLe64(dnt + 40, size);
            putLe64(dnt + 56, static_cast<uint64_t>(ch.second.mtime));
            putLe32(dnt + 72, static_cast<uint32_t>(ch.first.size()));
            writeExact(fd, dnt, sizeof(dnt));
            writeExact(fd, ch.first.data(), ch.first.size());
          } else {
            uint8_t dnt[20] = {};
            std::memcpy(dnt, "DENT", 4);
            putLe32(dnt + 4, mode);
            putLe32(dnt + 8, static_cast<uint32_t>(size));
            putLe32(dnt + 12, static_cast<uint32_t>(ch.second.mtime));
            putLe32(dnt + 16, static_cast<uint32_t>(ch.first.size()));
            writeExact(fd, dnt, sizeof(dnt));
            writeExact(fd, ch.first.data(), ch.first.size());
          }
        }
        uint8_t done[76] = {};
        std::memcpy(done, "DONE", 4);
        writeExact(fd, done, v2 ? 76 : 20);
        continue;
      }

      if (std::memcmp(hdr, "RECV", 4) == 0) {
        std::string p(len, '\0');
        if (len > 0 && !readExact(fd, reinterpret_cast<uint8_t*>(&p[0]), len))
          break;
        std::vector<uint8_t> data;
        bool ok = false;
        {
          std::lock_guard<std::mutex> lk(mu_);
          const Node* n = resolveNode(p, true);
          if (n && !n->isDir) {
            data = n->data;
            ok = true;
          }
        }
        if (!ok) {
          const char* msg = "No such file";
          uint8_t f[8];
          std::memcpy(f, "FAIL", 4);
          putLe32(f + 4, static_cast<uint32_t>(std::strlen(msg)));
          writeExact(fd, f, 8);
          writeExact(fd, msg, std::strlen(msg));
          continue;
        }
        size_t off = 0;
        while (off < data.size()) {
          size_t chunk = std::min<size_t>(32768u, data.size() - off);
          uint8_t d[8];
          std::memcpy(d, "DATA", 4);
          putLe32(d + 4, static_cast<uint32_t>(chunk));
          writeExact(fd, d, 8);
          writeExact(fd, data.data() + off, chunk);
          off += chunk;
        }
        uint8_t done[8] = {'D', 'O', 'N', 'E', 0, 0, 0, 0};
        writeExact(fd, done, 8);
        continue;
      }

      if (std::memcmp(hdr, "SEND", 4) == 0) {
        std::string spec(len, '\0');
        if (len > 0 && !readExact(fd, reinterpret_cast<uint8_t*>(&spec[0]), len))
          break;
        size_t comma = spec.rfind(',');
        std::string path = (comma == std::string::npos) ? spec : spec.substr(0, comma);
        if (path.compare(0, 7, "/system") == 0) {
          const char* msg = "Read-only file system";
          uint8_t f[8];
          std::memcpy(f, "FAIL", 4);
          putLe32(f + 4, static_cast<uint32_t>(std::strlen(msg)));
          writeExact(fd, f, 8);
          writeExact(fd, msg, std::strlen(msg));
          continue;
        }
        std::vector<uint8_t> buf;
        uint32_t mtime = 0;
        bool aborted = false;
        while (true) {
          uint8_t ch[8];
          if (!readExact(fd, ch, 8)) {
            aborted = true;
            break;
          }
          uint32_t clen = getLe32(ch + 4);
          if (std::memcmp(ch, "DATA", 4) == 0) {
            size_t old = buf.size();
            buf.resize(old + clen);
            if (clen > 0 && !readExact(fd, buf.data() + old, clen)) {
              aborted = true;
              break;
            }
          } else if (std::memcmp(ch, "DONE", 4) == 0) {
            mtime = clen;
            break;
          } else if (std::memcmp(ch, "QUIT", 4) == 0) {
            aborted = true;
            break;
          }
        }
        if (aborted)
          break;
        {
          std::lock_guard<std::mutex> lk(mu_);
          nodes_[path] = {false, false, false, "", std::move(buf),
                          static_cast<int64_t>(mtime)};
        }
        uint8_t ok[8] = {'O', 'K', 'A', 'Y', 0, 0, 0, 0};
        writeExact(fd, ok, 8);
        continue;
      }
    }
  }

  void sendShellV2Pkt(int fd, uint8_t id, const void* data, size_t len)
  {
    uint8_t hdr[5];
    hdr[0] = id;
    putLe32(hdr + 1, static_cast<uint32_t>(len));
    writeExact(fd, hdr, 5);
    if (len > 0)
      writeExact(fd, data, len);
  }

  void handleShell(int fd, const std::string& cmd, bool v2)
  {
    writeExact(fd, "OKAY", 4);

    if (v2 && cmd.find("exec 4>&1; truncate -s ") == 0) {
      unsigned long long off = 0;
      std::sscanf(cmd.c_str(), "exec 4>&1; truncate -s %llu", &off);
      size_t qpos = cmd.find('\'');
      size_t next = 0;
      std::string path = unquoteSingle(cmd, qpos, &next);
      {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = nodes_.find(path);
        if (it == nodes_.end() || it->second.isDir || off > it->second.data.size()) {
          const char* err = "truncate failed";
          sendShellV2Pkt(fd, 2, err, std::strlen(err));
          uint8_t ex = 1;
          sendShellV2Pkt(fd, 3, &ex, 1);
          return;
        }
        it->second.data.resize(static_cast<size_t>(off));
      }
      sendShellV2Pkt(fd, 1, "OK", 2);
      std::vector<uint8_t> appended;
      while (true) {
        uint8_t hdr[5];
        if (!readExact(fd, hdr, 5))
          break;
        uint32_t plen = getLe32(hdr + 1);
        std::vector<uint8_t> pl(plen);
        if (plen > 0 && !readExact(fd, pl.data(), plen))
          break;
        if (hdr[0] == 0) {
          appended.insert(appended.end(), pl.begin(), pl.end());
        } else if (hdr[0] == 4) {
          break;
        }
      }
      {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = nodes_.find(path);
        if (it != nodes_.end())
          it->second.data.insert(it->second.data.end(),
                                 appended.begin(), appended.end());
      }
      uint8_t ex = 0;
      sendShellV2Pkt(fd, 3, &ex, 1);
      return;
    }

    if (v2) {
      // Drain stdin close packet if client sends it
      struct pollfd pfd = { fd, POLLIN, 0 };
      if (::poll(&pfd, 1, 20) > 0) {
        uint8_t hdr[5];
        if (readExact(fd, hdr, 5)) {
          uint32_t plen = getLe32(hdr + 1);
          if (plen > 0) {
            std::vector<uint8_t> discard(plen);
            readExact(fd, discard.data(), plen);
          }
        }
      }
    }

    int rc = 0;
    std::string out;
    std::string err;

    auto stripSuffix = [](const std::string& c) -> std::string {
      size_t pos = c.find("; printf");
      return (pos == std::string::npos) ? c : c.substr(0, pos);
    };
    std::string core = stripSuffix(cmd);

    if (core.compare(0, 9, "mkdir -- ") == 0) {
      size_t next = 0;
      std::string path = unquoteSingle(core, 9, &next);
      std::lock_guard<std::mutex> lk(mu_);
      if (nodes_.count(path)) {
        rc = 1;
        err = "File exists";
      } else {
        nodes_[path] = {true, false, false, "", {}, 1700001000LL};
      }
    } else if (core.compare(0, 9, "rm -r -- ") == 0) {
      size_t next = 0;
      std::string path = unquoteSingle(core, 9, &next);
      std::lock_guard<std::mutex> lk(mu_);
      if (!nodes_.count(path)) {
        rc = 1;
        err = "No such file or directory";
      } else {
        std::string prefix = path + "/";
        for (auto it = nodes_.begin(); it != nodes_.end();) {
          if (it->first == path ||
              it->first.compare(0, prefix.size(), prefix) == 0)
            it = nodes_.erase(it);
          else
            ++it;
        }
      }
    } else if (core.compare(0, 6, "mv -- ") == 0) {
      size_t next = 0;
      std::string oldP = unquoteSingle(core, 6, &next);
      std::string newP = unquoteSingle(core, next + 1, &next);
      std::lock_guard<std::mutex> lk(mu_);
      auto it = nodes_.find(oldP);
      if (it == nodes_.end()) {
        rc = 1;
        err = "No such file or directory";
      } else {
        nodes_[newP] = it->second;
        nodes_.erase(it);
      }
    } else if (core.compare(0, 10, "chmod 755 ") == 0) {
      size_t q = core.find('\'');
      size_t next = 0;
      std::string path = unquoteSingle(core, q, &next);
      std::lock_guard<std::mutex> lk(mu_);
      auto it = nodes_.find(path);
      if (it != nodes_.end())
        it->second.executable = true;
    } else if (core.compare(0, 9, "touch -c ") == 0) {
      rc = 0;
    } else if (core.compare(0, 8, "find -H ") == 0) {
      size_t next = 0;
      std::string path = unquoteSingle(core, 8, &next);
      std::lock_guard<std::mutex> lk(mu_);
      std::string prefix = (path == "/") ? "/" : (path + "/");
      for (const auto& kv : nodes_) {
        if (kv.first == path ||
            kv.first.compare(0, prefix.size(), prefix) == 0) {
          if (!kv.second.isDir)
            out += std::to_string(kv.second.data.size()) + "\n";
        }
      }
    } else if (core.compare(0, 10, "md5sum -- ") == 0 ||
               core.find("| md5sum") != std::string::npos) {
      // Compute real MD5 using simple RFC1321 or host md5sum? We can invoke
      // standard MD5 over the node bytes with a compact helper below.
      std::string path;
      uint64_t skip = 0;
      uint64_t count = UINT64_MAX;
      if (core.compare(0, 10, "md5sum -- ") == 0) {
        size_t next = 0;
        path = unquoteSingle(core, 10, &next);
      } else {
        size_t q = core.find('\'');
        size_t next = 0;
        path = unquoteSingle(core, q, &next);
        std::sscanf(core.c_str() + next,
                    " bs=65536 iflag=skip_bytes,count_bytes skip=%llu count=%llu",
                    reinterpret_cast<unsigned long long*>(&skip),
                    reinterpret_cast<unsigned long long*>(&count));
      }
      std::vector<uint8_t> slice;
      {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = nodes_.find(path);
        if (it == nodes_.end() || it->second.isDir) {
          rc = 1;
          err = "No such file";
        } else if (skip <= it->second.data.size()) {
          size_t take = static_cast<size_t>(
            std::min<uint64_t>(count, it->second.data.size() - skip));
          slice.assign(it->second.data.begin() + static_cast<ptrdiff_t>(skip),
                       it->second.data.begin() + static_cast<ptrdiff_t>(skip + take));
        }
      }
      if (rc == 0) {
        // Known MD5 values or compute via python3/openssl? Wait: compute MD5 directly in C++!
        out = computeMd5Hex(slice.data(), slice.size()) + "  " + path + "\n";
      }
    } else {
      out = "hello-v1\n";
    }

    if (v2) {
      if (!out.empty())
        sendShellV2Pkt(fd, 1, out.data(), out.size());
      if (!err.empty())
        sendShellV2Pkt(fd, 2, err.data(), err.size());
      uint8_t ex = static_cast<uint8_t>(rc);
      sendShellV2Pkt(fd, 3, &ex, 1);
    } else {
      std::string combined = out + err + "\n__SCRCPY_ADB_RC__" +
                             std::to_string(rc) + "\n";
      writeExact(fd, combined.data(), combined.size());
    }
  }

  static std::string computeMd5Hex(const uint8_t* data, size_t len)
  {
    static const uint32_t kS[64] = {
      7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
      5, 9,14,20,5, 9,14,20,5, 9,14,20,5, 9,14,20,
      4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
      6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
    };
    static const uint32_t kK[64] = {
      0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
      0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
      0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
      0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
      0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
      0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
      0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
      0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    std::vector<uint8_t> msg(data, data + len);
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56)
      msg.push_back(0);
    uint64_t bitLen = static_cast<uint64_t>(len) * 8ULL;
    for (int i = 0; i < 8; ++i)
      msg.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xff));
    uint32_t a0 = 0x67452301u, b0 = 0xefcdab89u, c0 = 0x98badcfeu, d0 = 0x10325476u;
    for (size_t off = 0; off < msg.size(); off += 64) {
      uint32_t M[16];
      for (int i = 0; i < 16; ++i)
        M[i] = getLe32(msg.data() + off + i * 4);
      uint32_t A = a0, B = b0, C = c0, D = d0;
      for (int i = 0; i < 64; ++i) {
        uint32_t F, g;
        if (i < 16) { F = (B & C) | ((~B) & D); g = static_cast<uint32_t>(i); }
        else if (i < 32) { F = (D & B) | ((~D) & C); g = static_cast<uint32_t>((5 * i + 1) % 16); }
        else if (i < 48) { F = B ^ C ^ D; g = static_cast<uint32_t>((3 * i + 5) % 16); }
        else { F = C ^ (B | (~D)); g = static_cast<uint32_t>((7 * i) % 16); }
        F = F + A + kK[i] + M[g];
        A = D; D = C; C = B;
        B = B + ((F << kS[i]) | (F >> (32 - kS[i])));
      }
      a0 += A; b0 += B; c0 += C; d0 += D;
    }
    uint8_t dig[16];
    putLe32(dig, a0); putLe32(dig + 4, b0);
    putLe32(dig + 8, c0); putLe32(dig + 12, d0);
    char hex[33];
    for (int i = 0; i < 16; ++i)
      std::snprintf(hex + i * 2, 3, "%02x", dig[i]);
    return std::string(hex, 32);
  }

  void handleClient(int fd)
  {
    bool ok = false;
    std::string cmd = readAdbCmd(fd, &ok);
    if (!ok) {
      ::close(fd);
      return;
    }
    if (cmd == "host:devices") {
      std::string pl = "mock-dev\tdevice\n";
      char resp[64];
      int n = std::snprintf(resp, sizeof(resp), "OKAY%04x%s",
                            static_cast<unsigned>(pl.size()), pl.c_str());
      writeExact(fd, resp, static_cast<size_t>(n));
      ::close(fd);
      return;
    }
    if (cmd == "host-serial:mock-dev:features") {
      if (v2Features_) {
        std::string pl = "stat_v2,ls_v2,shell_v2";
        char resp[64];
        int n = std::snprintf(resp, sizeof(resp), "OKAY%04x%s",
                              static_cast<unsigned>(pl.size()), pl.c_str());
        writeExact(fd, resp, static_cast<size_t>(n));
      } else {
        const char* err = "unknown command";
        char resp[64];
        int n = std::snprintf(resp, sizeof(resp), "FAIL%04x%s",
                              static_cast<unsigned>(std::strlen(err)), err);
        writeExact(fd, resp, static_cast<size_t>(n));
      }
      ::close(fd);
      return;
    }
    if (cmd == "host:transport:mock-dev") {
      writeExact(fd, "OKAY", 4);
      std::string sub = readAdbCmd(fd, &ok);
      if (ok && sub == "sync:") {
        handleSync(fd);
      } else if (ok && sub.compare(0, 9, "shell,v2:") == 0) {
        handleShell(fd, sub.substr(9), true);
      } else if (ok && sub.compare(0, 6, "shell:") == 0) {
        handleShell(fd, sub.substr(6), false);
      }
      ::close(fd);
      return;
    }
    ::close(fd);
  }

  void run()
  {
    std::vector<std::thread> workers;
    while (!stop_) {
      struct pollfd pfd = { srvFd_, POLLIN, 0 };
      if (::poll(&pfd, 1, 50) <= 0)
        continue;
      int c = ::accept(srvFd_, nullptr, nullptr);
      if (c < 0)
        break;
      workers.emplace_back([this, c]() { handleClient(c); });
    }
    for (auto& w : workers)
      w.join();
  }

  bool v2Features_;
  int srvFd_;
  int port_;
  std::atomic<bool> stop_;
  std::thread thread_;
  std::mutex mu_;
  std::map<std::string, Node> nodes_;
};

} // namespace

TEST(SSecurityTightTest, NegotiatesNoneAndVncAuthAndRejectsInvalid)
{
  // 1. Tight + None on RFB 3.8
  {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    auto* srvSock = new network::UnixSocket(fds[0]);
    auto* cliSock = new network::UnixSocket(fds[1]);

    TightTestSConnection srv(srvSock, rfb::AccessDefault);
    std::list<uint32_t> secTypes1 = {rfb::secTypeTight, rfb::secTypeNone};
    srv.sec()->SetSecTypes(secTypes1);
    srv.initialiseProtocol();

    TightTestCConnection cli(true);
    cli.setStreams(&cliSock->inStream(), &cliSock->outStream());
    cli.initialiseProtocol();

    std::thread serverLoop([&]() {
      while (!srv.readyCalled) {
        if (!srv.processMsg())
          ::poll(nullptr, 0, 2);
      }
    });
    while (!cli.initialized) {
      if (!cli.processMsg())
        ::poll(nullptr, 0, 2);
    }
    serverLoop.join();
    EXPECT_TRUE(cli.server.tightEnabled);
    EXPECT_TRUE(srv.client.tightEnabled);
    delete cliSock;
    delete srvSock;
  }

  // 1b. Tight + None on RFB 3.7 (effective type None skips SecurityResult)
  {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    auto* srvSock = new network::UnixSocket(fds[0]);

    TightTestSConnection srv(srvSock, rfb::AccessDefault);
    std::list<uint32_t> secTypes1b = {rfb::secTypeTight, rfb::secTypeNone};
    srv.sec()->SetSecTypes(secTypes1b);
    srv.initialiseProtocol();

    std::atomic<bool> stopSrv{false};
    std::thread serverLoop([&]() {
      while (!stopSrv && !srv.readyCalled) {
        if (!srv.processMsg())
          ::poll(nullptr, 0, 2);
      }
    });

    auto readFdExact = [](int fd, uint8_t* dst, size_t len) -> bool {
      size_t off = 0;
      while (off < len) {
        ssize_t n = ::recv(fd, dst + off, len - off, 0);
        if (n < 0 && errno == EINTR)
          continue;
        if (n <= 0)
          return false;
        off += static_cast<size_t>(n);
      }
      return true;
    };

    uint8_t srvVer[12];
    ASSERT_TRUE(readFdExact(fds[1], srvVer, 12));
    writeAll(fds[1], "RFB 003.007\n", 12);

    uint8_t nSec = 0;
    ASSERT_TRUE(readFdExact(fds[1], &nSec, 1));
    ASSERT_GT(nSec, 0u);
    std::vector<uint8_t> secList(nSec);
    ASSERT_TRUE(readFdExact(fds[1], secList.data(), nSec));
    EXPECT_NE(std::find(secList.begin(), secList.end(), rfb::secTypeTight),
              secList.end());

    uint8_t chooseTight = rfb::secTypeTight;
    writeAll(fds[1], &chooseTight, 1);

    uint8_t hdr[8];
    ASSERT_TRUE(readFdExact(fds[1], hdr, 8));
    uint32_t nTunnels = (static_cast<uint32_t>(hdr[0]) << 24) |
                        (static_cast<uint32_t>(hdr[1]) << 16) |
                        (static_cast<uint32_t>(hdr[2]) << 8) | hdr[3];
    uint32_t nAuth = (static_cast<uint32_t>(hdr[4]) << 24) |
                     (static_cast<uint32_t>(hdr[5]) << 16) |
                     (static_cast<uint32_t>(hdr[6]) << 8) | hdr[7];
    EXPECT_EQ(nTunnels, 0u);
    ASSERT_GE(nAuth, 1u);
    std::vector<uint8_t> caps(nAuth * 16);
    ASSERT_TRUE(readFdExact(fds[1], caps.data(), caps.size()));
    bool foundNoneAuth = false;
    for (uint32_t i = 0; i < nAuth; ++i) {
      uint32_t code = (static_cast<uint32_t>(caps[i * 16]) << 24) |
                      (static_cast<uint32_t>(caps[i * 16 + 1]) << 16) |
                      (static_cast<uint32_t>(caps[i * 16 + 2]) << 8) |
                      caps[i * 16 + 3];
      if (code == rfb::secTypeNone)
        foundNoneAuth = true;
    }
    EXPECT_TRUE(foundNoneAuth);

    // Choose None auth and immediately send ClientInit (no SecurityResult in 3.7)
    uint8_t authNone[4] = {0, 0, 0, rfb::secTypeNone};
    writeAll(fds[1], authNone, 4);
    uint8_t clientInitShared = 1;
    writeAll(fds[1], &clientInitShared, 1);

    // Next 4 bytes must be ServerInit framebuffer width=640, height=480 (not SecurityResult=0)
    uint8_t srvInitSize[4];
    ASSERT_TRUE(readFdExact(fds[1], srvInitSize, 4));
    uint16_t fbW = static_cast<uint16_t>((srvInitSize[0] << 8) | srvInitSize[1]);
    uint16_t fbH = static_cast<uint16_t>((srvInitSize[2] << 8) | srvInitSize[3]);
    EXPECT_EQ(fbW, 640u);
    EXPECT_EQ(fbH, 480u);

    stopSrv = true;
    serverLoop.join();
    EXPECT_TRUE(srv.client.tightEnabled);
    ::close(fds[1]);
    delete srvSock;
  }

  // 2. Tight + VncAuth
  {
    std::vector<uint8_t> obf = rfb::obfuscate("pass1234");
    char pwTmpl[] = "/tmp/scrcpy-tight-pw-XXXXXX";
    int pwFd = ::mkstemp(pwTmpl);
    ASSERT_GE(pwFd, 0);
    ASSERT_EQ(::write(pwFd, obf.data(), obf.size()),
              static_cast<ssize_t>(obf.size()));
    ::close(pwFd);
    rfb::SSecurityVncAuth::vncAuthPasswdFile.setParam(pwTmpl);

    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    auto* srvSock = new network::UnixSocket(fds[0]);
    auto* cliSock = new network::UnixSocket(fds[1]);

    TightTestSConnection srv(srvSock, rfb::AccessDefault);
    std::list<uint32_t> secTypes2 = {rfb::secTypeTight, rfb::secTypeVncAuth};
    srv.sec()->SetSecTypes(secTypes2);
    srv.initialiseProtocol();

    TightTestCConnection cli(true);
    cli.setStreams(&cliSock->inStream(), &cliSock->outStream());
    cli.initialiseProtocol();

    std::thread serverLoop([&]() {
      while (!srv.readyCalled) {
        if (!srv.processMsg())
          ::poll(nullptr, 0, 2);
      }
    });
    while (!cli.initialized) {
      if (!cli.processMsg())
        ::poll(nullptr, 0, 2);
    }
    serverLoop.join();
    EXPECT_TRUE(cli.server.tightEnabled);
    rfb::SSecurityVncAuth::vncAuthPasswdFile.setParam("");
    ::unlink(pwTmpl);
    delete cliSock;
    delete srvSock;
  }

  // 3. Rejects unoffered Tight auth code
  {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    auto* srvSock = new network::UnixSocket(fds[0]);
    TightTestSConnection srv(srvSock, rfb::AccessDefault);
    std::list<uint32_t> secTypes3 = {rfb::secTypeTight, rfb::secTypeNone};
    srv.sec()->SetSecTypes(secTypes3);

    rfb::SSecurityTight st(&srv, srv.sec());
    EXPECT_FALSE(st.processMsg());
    // Client replies with tightAuthVnc (2), which was NOT offered!
    uint8_t badChoice[4] = {0, 0, 0, 2};
    writeAll(fds[1], badChoice, 4);
    EXPECT_THROW(st.processMsg(), rfb::protocol_error);
    ::close(fds[1]);
    delete srvSock;
  }
}

TEST(AdbFileTransferTest, PathNormalizationAndConfinement)
{
  std::string dev, virt, err;

  EXPECT_TRUE(AdbFileTransferSession::normalizePath(
    "", "", true, &dev, &virt, &err));
  EXPECT_EQ(dev, "/");
  EXPECT_EQ(virt, "/");

  EXPECT_FALSE(AdbFileTransferSession::normalizePath(
    "", "", false, &dev, &virt, &err));

  EXPECT_TRUE(AdbFileTransferSession::normalizePath(
    "\\sdcard\\..\\..\\data\\local\\tmp\\", "", false, &dev, &virt, &err));
  EXPECT_EQ(dev, "/data/local/tmp");
  EXPECT_EQ(virt, "/data/local/tmp");

  EXPECT_TRUE(AdbFileTransferSession::normalizePath(
    "/../../etc/passwd", "/sdcard/Work", false, &dev, &virt, &err));
  EXPECT_EQ(dev, "/sdcard/Work/etc/passwd");
  EXPECT_EQ(virt, "/etc/passwd");

  std::string withTrailNul = std::string("/sdcard/file.txt") + '\0' + '\0';
  EXPECT_TRUE(AdbFileTransferSession::normalizePath(
    withTrailNul, "", false, &dev, &virt, &err));
  EXPECT_EQ(dev, "/sdcard/file.txt");

  std::string withEmbedNul = std::string("/sdcard/") + '\0' + "bad";
  EXPECT_FALSE(AdbFileTransferSession::normalizePath(
    withEmbedNul, "", false, &dev, &virt, &err));
}

TEST(AdbClientTest, SyncV1FallbackAndShellV1V2)
{
  for (bool v2 : {true, false}) {
    MockAdbFileDaemon mock(v2);
    ASSERT_TRUE(mock.start());
    AdbClient adb("127.0.0.1", mock.port());

    AdbFeatures feat = adb.queryFeatures("mock-dev");
    EXPECT_EQ(feat.statV2, v2);
    EXPECT_EQ(feat.lsV2, v2);
    EXPECT_EQ(feat.shellV2, v2);

    int syncFd = adb.openSync("mock-dev");
    ASSERT_GE(syncFd, 0);
    auto entries = AdbClient::syncList(syncFd, "/", feat.lsV2, feat.statV2);
    AdbClient::syncQuit(syncFd);
    ::close(syncFd);

    bool foundSdcardDir = false;
    for (const auto& e : entries) {
      if (e.name == "sdcard" && S_ISDIR(e.mode))
        foundSdcardDir = true;
    }
    EXPECT_TRUE(foundSdcardDir);

    AdbShellResult res = adb.runShell("mock-dev", "echo hi", feat.shellV2);
    EXPECT_EQ(res.exitCode, 0);
  }
}

TEST(ScrcpyVncEndToEndTest, Tight2AndTight13FileTransferWithH264)
{
  MockAdbFileDaemon mockAdb(true);
  ASSERT_TRUE(mockAdb.start());

  rfb::SecurityServer::secTypes.setParam("None");
  fileTransfer.setParam(true);
  fileTransferRoot.setParam("");

  int videoFds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, videoFds), 0);

  ScrcpyServer server;
  server.attachStreamsForTest(videoFds[0], -1, ScrcpyProtocol::V2,
                              1920, 1080, "WayDroidFT");
  server.setAdbTargetForTest("127.0.0.1", mockAdb.port(), "mock-dev");

  LoopbackListener listener(0);
  std::list<network::SocketListener*> listeners = {&listener};
  int port = listener.getMyPort();

  std::atomic<bool> stopSrv{false};
  std::thread srvThread([&]() {
    while (!stopSrv) {
      if (!server.step(listeners, 10))
        break;
    }
  });
  struct SrvJoinGuard {
    std::atomic<bool>& stopFlag;
    std::thread& t;
    ~SrvJoinGuard() {
      stopFlag = true;
      if (t.joinable())
        t.join();
    }
  } srvGuard{stopSrv, srvThread};

  // --- Client 1: TightVNC 2.x ---
  {
    network::TcpSocket sock("127.0.0.1", port);
    TightTestCConnection cli(true);
    ScrcpyFtRecordingHandler ft;
    cli.fileTransferClient().setHandler(&ft);
    cli.setStreams(&sock.inStream(), &sock.outStream());
    cli.initialiseProtocol();

    auto pumpUntil = [&](auto pred) {
      auto deadline = std::chrono::steady_clock::now() +
                      std::chrono::seconds(5);
      while (!pred() && std::chrono::steady_clock::now() < deadline) {
        if (!cli.processMsg())
          ::poll(nullptr, 0, 2);
      }
      ASSERT_TRUE(pred());
    };

    pumpUntil([&]() {
      return cli.initialized && ft.ready &&
             cli.server.supportsContinuousUpdates && cli.server.supportsFence;
    });
    EXPECT_TRUE(cli.server.tightEnabled);
    EXPECT_EQ(ft.caps.protocol, rfb::FileTransferProtocol::Tight2);
    EXPECT_TRUE(ft.caps.canCompress);
    EXPECT_TRUE(ft.caps.canList);
    EXPECT_TRUE(ft.caps.canDownload);
    EXPECT_TRUE(ft.caps.canUpload);
    EXPECT_TRUE(ft.caps.canMkdir);
    EXPECT_TRUE(ft.caps.canDelete);
    EXPECT_TRUE(ft.caps.canRename);

    cli.msgWriter()->writeEnableContinuousUpdates(true, 0, 0, 1920, 1080);

    // 1. List "/" and verify /sdcard symlink is a directory
    ft.resetOp();
    cli.fileTransferClient().listDirectory("/");
    pumpUntil([&]() { return ft.listDone; });
    ASSERT_TRUE(ft.listOk);
    bool sdDir = false;
    for (const auto& e : ft.entries) {
      if (e.name == "sdcard" && e.isDir)
        sdDir = true;
    }
    EXPECT_TRUE(sdDir);

    // 2. Mkdir + Upload (multi-chunk) + Download (full & offset resume) + Rename + Remove
    ft.resetOp();
    cli.fileTransferClient().createDirectory("/data/local/tmp/work");
    pumpUntil([&]() { return ft.cmdDone; });
    EXPECT_TRUE(ft.cmdOk);

    std::vector<uint8_t> big(90000);
    for (size_t i = 0; i < big.size(); ++i)
      big[i] = static_cast<uint8_t>((i * 73 + 19) & 0xff);

    ft.resetOp();
    cli.fileTransferClient().startUpload("/data/local/tmp/work/blob.bin", true, 0);
    pumpUntil([&]() { return ft.ulReady; });
    ASSERT_TRUE(ft.ulReadyOk);

    for (size_t off = 0; off < big.size(); off += 30000) {
      int prevAcks = ft.ulAckCount;
      cli.fileTransferClient().sendUploadData(big.data() + off,
                                              std::min<size_t>(30000, big.size() - off));
      pumpUntil([&]() { return ft.ulAckCount > prevAcks; });
    }
    cli.fileTransferClient().finishUpload(1700000999ULL);
    pumpUntil([&]() { return ft.ulComplete; });
    ASSERT_TRUE(ft.ulCompleteOk);

    // Resume upload at offset 50000 appending 1000 bytes
    std::vector<uint8_t> tail(1000, 0x5c);
    big.resize(50000);
    big.insert(big.end(), tail.begin(), tail.end());
    ft.resetOp();
    cli.fileTransferClient().startUpload("/data/local/tmp/work/blob.bin", false, 50000);
    pumpUntil([&]() { return ft.ulReady; });
    ASSERT_TRUE(ft.ulReadyOk);
    cli.fileTransferClient().sendUploadData(tail.data(), tail.size());
    pumpUntil([&]() { return ft.ulAckCount == 1; });
    cli.fileTransferClient().finishUpload(1700001000ULL);
    pumpUntil([&]() { return ft.ulComplete; });
    ASSERT_TRUE(ft.ulCompleteOk);

    // Download full blob.bin (compressed!) while also streaming an H.264 frame
    sendScrcpyPacket(videoFds[1], UINT64_C(1) << 63, kFrame1080, kFrame1080SpsPpsLen);
    sendScrcpyPacket(videoFds[1], UINT64_C(1) << 62,
                     kFrame1080 + kFrame1080SpsPpsLen,
                     sizeof(kFrame1080) - kFrame1080SpsPpsLen);

    ft.resetOp();
    cli.fileTransferClient().startDownload("/data/local/tmp/work/blob.bin", 0);
    pumpUntil([&]() { return ft.dlStarted; });
    ASSERT_TRUE(ft.dlStartOk);
    while (!ft.dlComplete) {
      size_t prevSize = ft.dlData.size();
      cli.fileTransferClient().requestMoreDownloadData();
      pumpUntil([&]() { return ft.dlComplete || ft.dlData.size() > prevSize; });
    }
    EXPECT_TRUE(ft.dlCompleteOk);
    EXPECT_EQ(ft.dlData, big);

    // Resume download from offset 50000
    ft.resetOp();
    cli.fileTransferClient().startDownload("/data/local/tmp/work/blob.bin", 50000);
    pumpUntil([&]() { return ft.dlStarted; });
    ASSERT_TRUE(ft.dlStartOk);
    while (!ft.dlComplete) {
      size_t prevSize = ft.dlData.size();
      cli.fileTransferClient().requestMoreDownloadData();
      pumpUntil([&]() { return ft.dlComplete || ft.dlData.size() > prevSize; });
    }
    EXPECT_TRUE(ft.dlCompleteOk);
    EXPECT_EQ(ft.dlData, tail);

    // Cancel download mid-transfer
    ft.resetOp();
    cli.fileTransferClient().startDownload("/data/local/tmp/work/blob.bin", 0);
    pumpUntil([&]() { return ft.dlStarted; });
    cli.fileTransferClient().cancelDownload();
    pumpUntil([&]() { return ft.cancelDone; });

    // Rename & Remove
    ft.resetOp();
    cli.fileTransferClient().renameFileOrDir(
      "/data/local/tmp/work/blob.bin", "/data/local/tmp/work/blob2.bin", false);
    pumpUntil([&]() { return ft.cmdDone; });
    EXPECT_TRUE(ft.cmdOk);

    // DirSize & MD5
    cli.dirSizeDone = false;
    cli.msgWriter()->writeTight2DirSizeRequest("/data/local/tmp/work");
    pumpUntil([&]() { return cli.dirSizeDone; });
    EXPECT_EQ(cli.lastDirSize, big.size());

    // MD5 of "Hello Android!" -> verified via raw tight2Md5Req
    cli.md5Done = false;
    {
      const std::string md5Path = "/data/local/tmp/hello.txt";
      sock.outStream().writeU32(rfb::tight2Md5Req);
      sock.outStream().writeU32(static_cast<uint32_t>(md5Path.size()));
      sock.outStream().writeBytes(
        reinterpret_cast<const uint8_t*>(md5Path.data()), md5Path.size());
      sock.outStream().writeU32(0);
      sock.outStream().writeU32(0);
      sock.outStream().writeU32(0);
      sock.outStream().writeU32(100);
      sock.outStream().flush();
    }
    pumpUntil([&]() { return cli.md5Done; });
    uint8_t zero16[16] = {};
    EXPECT_NE(std::memcmp(cli.lastMd5, zero16, 16), 0);

    // Compressed Tight 2.x UPLOAD_DATA_REQUEST (zlib windowBits=15, Z_SYNC_FLUSH)
    {
      std::vector<uint8_t> plain(16384);
      for (size_t i = 0; i < plain.size(); ++i)
        plain[i] = static_cast<uint8_t>('A' + (i % 16));

      z_stream zs;
      std::memset(&zs, 0, sizeof(zs));
      ASSERT_EQ(::deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                               15, 8, Z_DEFAULT_STRATEGY), Z_OK);
      std::vector<uint8_t> packed(plain.size() + 128);
      zs.next_in = plain.data();
      zs.avail_in = static_cast<uInt>(plain.size());
      zs.next_out = packed.data();
      zs.avail_out = static_cast<uInt>(packed.size());
      int zrc = ::deflate(&zs, Z_SYNC_FLUSH);
      ASSERT_TRUE(zrc == Z_OK || zrc == Z_STREAM_END);
      packed.resize(packed.size() - zs.avail_out);
      ::deflateEnd(&zs);

      ft.resetOp();
      cli.fileTransferClient().startUpload("/data/local/tmp/work/comp.bin", true, 0);
      pumpUntil([&]() { return ft.ulReady; });
      ASSERT_TRUE(ft.ulReadyOk);

      sock.outStream().writeU32(rfb::tight2UploadDataReq);
      sock.outStream().writeU8(1);
      sock.outStream().writeU32(static_cast<uint32_t>(packed.size()));
      sock.outStream().writeU32(static_cast<uint32_t>(plain.size()));
      sock.outStream().writeBytes(packed.data(), packed.size());
      sock.outStream().flush();
      pumpUntil([&]() { return ft.ulAckCount == 1; });

      cli.fileTransferClient().finishUpload(1700001001ULL);
      pumpUntil([&]() { return ft.ulComplete; });
      ASSERT_TRUE(ft.ulCompleteOk);

      std::vector<uint8_t> storedComp;
      ASSERT_TRUE(mockAdb.getFile("/data/local/tmp/work/comp.bin", &storedComp));
      EXPECT_EQ(storedComp, plain);
    }

    ft.resetOp();
    cli.fileTransferClient().removeFileOrDir("/data/local/tmp/work", true);
    pumpUntil([&]() { return ft.cmdDone; });
    EXPECT_TRUE(ft.cmdOk);
    EXPECT_FALSE(mockAdb.hasPath("/data/local/tmp/work/blob2.bin"));

    // Immediate SEND failure on /system
    ft.resetOp();
    cli.fileTransferClient().startUpload("/system/ro.txt", true, 0);
    pumpUntil([&]() { return ft.ulReady; });
    EXPECT_FALSE(ft.ulReadyOk);

    // Verify H.264 frame arrived cleanly without corruption
    pumpUntil([&]() { return cli.h264Frames.size() >= sizeof(kFrame1080); });
    EXPECT_EQ(cli.h264Frames.size(), sizeof(kFrame1080));
  }

  // --- Client 2: TightVNC 1.3.x ---
  {
    network::TcpSocket sock("127.0.0.1", port);
    TightTestCConnection cli(true);
    cli.fileTransferClient().setPreferredProtocol(rfb::FileTransferProtocol::Tight13);
    ScrcpyFtRecordingHandler ft;
    cli.fileTransferClient().setHandler(&ft);
    cli.setStreams(&sock.inStream(), &sock.outStream());
    cli.initialiseProtocol();

    auto pumpUntil = [&](auto pred) {
      auto deadline = std::chrono::steady_clock::now() +
                      std::chrono::seconds(5);
      while (!pred() && std::chrono::steady_clock::now() < deadline) {
        if (!cli.processMsg())
          ::poll(nullptr, 0, 2);
      }
      ASSERT_TRUE(pred());
    };

    pumpUntil([&]() {
      return cli.initialized && ft.ready && cli.server.supportsFence;
    });
    EXPECT_EQ(ft.caps.protocol, rfb::FileTransferProtocol::Tight13);

    ft.resetOp();
    cli.fileTransferClient().listDirectory("/data/local/tmp");
    pumpUntil([&]() { return ft.listDone; });
    ASSERT_TRUE(ft.listOk);
    EXPECT_GE(ft.entries.size(), 3u);

    // 0-byte download
    ft.resetOp();
    cli.fileTransferClient().startDownload("/data/local/tmp/empty.txt", 0);
    pumpUntil([&]() { return ft.dlComplete; });
    EXPECT_TRUE(ft.dlStartOk);
    EXPECT_TRUE(ft.dlCompleteOk);
    EXPECT_TRUE(ft.dlData.empty());

    // Non-empty download
    ft.resetOp();
    cli.fileTransferClient().startDownload("/data/local/tmp/hello.txt", 0);
    pumpUntil([&]() { return ft.dlComplete; });
    EXPECT_TRUE(ft.dlCompleteOk);
    EXPECT_EQ(std::string(ft.dlData.begin(), ft.dlData.end()), "Hello Android!");

    // Tight 1.3 upload + Fence sync
    ft.resetOp();
    cli.fileTransferClient().startUpload("/data/local/tmp/t13.txt", true, 0);
    pumpUntil([&]() { return ft.ulReady; });
    const uint8_t msg[] = "tight13-data";
    cli.fileTransferClient().sendUploadData(msg, sizeof(msg) - 1);
    cli.fileTransferClient().finishUpload(1700001234u);
    pumpUntil([&]() { return ft.ulComplete; });
    EXPECT_TRUE(ft.ulCompleteOk);

    std::vector<uint8_t> stored;
    ASSERT_TRUE(mockAdb.getFile("/data/local/tmp/t13.txt", &stored));
    EXPECT_EQ(std::string(stored.begin(), stored.end()), "tight13-data");

    // Tight 1.3 mkdir + dir list synchronization
    ft.resetOp();
    cli.fileTransferClient().createDirectory("/data/local/tmp/t13dir");
    pumpUntil([&]() { return ft.cmdDone; });
    EXPECT_TRUE(ft.cmdOk);
    EXPECT_TRUE(mockAdb.hasPath("/data/local/tmp/t13dir"));
  }

  // --- Client 3: View-only access (AccessView, no AccessFileTransfer) ---
  {
    stopSrv = true;
    srvThread.join();

    int pair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    network::UnixSocket cliSock(pair[1]);
    ASSERT_TRUE(server.addSocket(new network::UnixSocket(pair[0]),
                                 rfb::AccessView));

    TightTestCConnection cli(true);
    ScrcpyFtRecordingHandler ft;
    cli.fileTransferClient().setHandler(&ft);
    cli.setStreams(&cliSock.inStream(), &cliSock.outStream());
    cli.initialiseProtocol();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!cli.initialized && std::chrono::steady_clock::now() < deadline) {
      ASSERT_TRUE(server.step({}, 5));
      if (!cli.processMsg())
        ::poll(nullptr, 0, 2);
    }
    ASSERT_TRUE(cli.initialized);
    EXPECT_FALSE(cli.fileTransferClient().isSupported());
  }

  // --- Client 4: -FileTransfer=0 disables Tight file transfer ---
  {
    fileTransfer.setParam(false);

    int pair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    network::UnixSocket cliSock(pair[1]);
    ASSERT_TRUE(server.addSocket(new network::UnixSocket(pair[0]),
                                 rfb::AccessDefault));

    TightTestCConnection cli(true);
    ScrcpyFtRecordingHandler ft;
    cli.fileTransferClient().setHandler(&ft);
    cli.setStreams(&cliSock.inStream(), &cliSock.outStream());
    cli.initialiseProtocol();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!cli.initialized && std::chrono::steady_clock::now() < deadline) {
      ASSERT_TRUE(server.step({}, 5));
      if (!cli.processMsg())
        ::poll(nullptr, 0, 2);
    }
    ASSERT_TRUE(cli.initialized);
    EXPECT_FALSE(cli.server.tightEnabled);
    EXPECT_FALSE(cli.fileTransferClient().isSupported());

    fileTransfer.setParam(true);
  }

  ::close(videoFds[1]);
}

namespace {

class GiiScrcpyTestCConnection : public rfb::CConnection {
public:
  explicit GiiScrcpyTestCConnection(bool enableGiiOpt = true)
  {
    supportsFileTransfer = false;
    supportsGII = enableGiiOpt;
    setPreferredEncoding(rfb::encodingH264);
  }

  bool initialized = false;
  std::vector<uint16_t> enabledVersions;
  std::vector<uint32_t> createdOrigins;
  std::vector<uint8_t> h264Frames;

  rfb::CMsgWriter* msgWriter() { return writer(); }

  void initDone() override
  {
    initialized = true;
    setFramebuffer(new rfb::ManagedPixelBuffer(
      rfb::PixelFormat(32, 24, false, true, 255, 255, 255, 16, 8, 0),
      server.width(), server.height()));
  }

  void handleGIIEnabled(uint16_t version) override
  {
    enabledVersions.push_back(version);
  }

  void handleGIIDeviceCreated(uint32_t deviceOrigin) override
  {
    createdOrigins.push_back(deviceOrigin);
  }

  void bell() override {}
  void getUserPasswd(std::string*, std::string*) override {}
  bool verifyCertificate(unsigned int, const uint8_t*, size_t) override
  {
    return true;
  }
  bool verifyHostKey(const uint8_t*, size_t, const char*) override
  {
    return true;
  }

  bool dataRect(const core::Rect& r, int encoding) override
  {
    if (encoding == rfb::encodingH264) {
      if (!getInStream()->hasData(8))
        return false;
      getInStream()->setRestorePoint();
      uint32_t len = getInStream()->readU32();
      getInStream()->skip(4);
      if (!getInStream()->hasDataOrRestore(len))
        return false;
      getInStream()->clearRestorePoint();
      size_t old = h264Frames.size();
      h264Frames.resize(old + len);
      if (len > 0)
        getInStream()->readBytes(h264Frames.data() + old, len);
      return true;
    }
    return rfb::CConnection::dataRect(r, encoding);
  }
};

struct GiiTestParamGuard {
  std::string savedSecTypes;
  bool savedGii;
  bool savedAcceptPtr;
  bool savedAcceptKey;

  GiiTestParamGuard()
    : savedSecTypes(rfb::SecurityServer::secTypes.getValueStr()),
      savedGii(static_cast<bool>(gii)),
      savedAcceptPtr(static_cast<bool>(rfb::Server::acceptPointerEvents)),
      savedAcceptKey(static_cast<bool>(rfb::Server::acceptKeyEvents))
  {}

  ~GiiTestParamGuard()
  {
    rfb::SecurityServer::secTypes.setParam(savedSecTypes.c_str());
    gii.setParam(savedGii);
    rfb::Server::acceptPointerEvents.setParam(savedAcceptPtr);
    rfb::Server::acceptKeyEvents.setParam(savedAcceptKey);
  }
};

rfb::GIIDevice makeTestStylusDevice(const char* name = "Wacom Stylus")
{
  rfb::GIIDevice dev;
  dev.name = name;
  dev.vendorID = 0x056a;
  dev.productID = 0x0357;
  dev.canGenerate = rfb::giiEventMaskValuatorAbsolute |
                    rfb::giiEventMaskValuatorRelative |
                    rfb::giiEventMaskPtrButtonPress |
                    rfb::giiEventMaskPtrButtonRelease |
                    rfb::giiEventMaskKeyPress |
                    rfb::giiEventMaskKeyRelease |
                    rfb::giiEventMaskKeyRepeat;
  dev.numButtons = 9;

  rfb::GIIValuator xVal;
  xVal.index = 0;
  xVal.longName = "Abs X";
  xVal.shortName = "X";
  xVal.rangeMin = 0;
  xVal.rangeMax = 30000;
  dev.valuators.push_back(xVal);

  rfb::GIIValuator yVal;
  yVal.index = 1;
  yVal.longName = "Abs Y";
  yVal.shortName = "Y";
  yVal.rangeMin = 0;
  yVal.rangeMax = 20000;
  dev.valuators.push_back(yVal);

  rfb::GIIValuator pVal;
  pVal.index = 2;
  pVal.longName = "Abs Pressure";
  pVal.shortName = "Prs";
  pVal.rangeMin = 0;
  pVal.rangeMax = 8191;
  pVal.siUnit = rfb::giiUnitPressure;
  dev.valuators.push_back(pVal);

  rfb::GIIValuator wVal;
  wVal.index = 3;
  wVal.longName = "Rel Wheel";
  wVal.shortName = "Whl";
  dev.valuators.push_back(wVal);

  return dev;
}

void expectNoReadableBytes(int fd, int waitMs = 20)
{
  struct pollfd pfd = { fd, POLLIN, 0 };
  EXPECT_EQ(::poll(&pfd, 1, waitMs), 0);
}

void readControlMessageSkipReset(int fd, uint8_t* buf, size_t len,
                                 int timeoutMs = 3000)
{
  ASSERT_GT(len, 0u);
  uint8_t b = 17;
  while (b == 17)
    readAllTimeout(fd, &b, 1, timeoutMs);
  buf[0] = b;
  if (len > 1)
    readAllTimeout(fd, buf + 1, len - 1, timeoutMs);
}

void expectNoControlInput(int fd, int waitMs = 20)
{
  int timeout = waitMs;
  while (true) {
    struct pollfd pfd = { fd, POLLIN, 0 };
    if (::poll(&pfd, 1, timeout) <= 0)
      break;
    timeout = 0;
    uint8_t b = 0;
    ssize_t n = ::recv(fd, &b, 1, MSG_DONTWAIT);
    if (n <= 0)
      break;
    if (b == 17)
      continue;
    ADD_FAILURE() << "Unexpected scrcpy control message byte: "
                  << static_cast<unsigned>(b);
    break;
  }
}

} // namespace

TEST(ScrcpyControlTest, GIIStylusMultiTouchButtonsWheelAndKeys)
{
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

  ScrcpyControl ctrl;
  ctrl.attach(fds[0]);

  rfb::GIIDevice stylus1 = makeTestStylusDevice("Stylus 1");
  uint32_t origin1 = ctrl.createGIIDevice(stylus1);
  ASSERT_NE(origin1, 0u);
  EXPECT_EQ(ctrl.activeGIIDeviceCount(), 1u);

  const uint64_t expectedPtr1 = UINT64_C(0x4749490000000000) | origin1;
  const uint64_t mousePtr = UINT64_C(-1);

  // 1. Hover before press emits ACTION_HOVER_MOVE with scaled coords
  rfb::GIIEvent hover;
  hover.type = rfb::giiEventValuatorAbsolute;
  hover.origin = origin1;
  hover.first = 0;
  hover.values = { 15000, 10000, 0 };
  ctrl.handleGIIEvent(hover, 1920, 1080);

  uint8_t touch[32];
  readAllTimeout(fds[1], touch, sizeof(touch));
  EXPECT_EQ(touch[0], 2); // INJECT_TOUCH_EVENT
  EXPECT_EQ(touch[1], 7); // ACTION_HOVER_MOVE
  EXPECT_EQ(readBe64(touch + 2), mousePtr);
  EXPECT_EQ(readBe32(touch + 10), 960u);
  EXPECT_EQ(readBe32(touch + 14), 540u);
  EXPECT_EQ(readBe16(touch + 18), 1920u);
  EXPECT_EQ(readBe16(touch + 20), 1080u);
  EXPECT_EQ(readBe16(touch + 22), 0u);
  EXPECT_EQ(readBe32(touch + 28), 0u);

  // 2. Pressure sample + button 1 press emits ACTION_DOWN on device pointerId
  rfb::GIIEvent prs;
  prs.type = rfb::giiEventValuatorAbsolute;
  prs.origin = origin1;
  prs.first = 2;
  prs.values = { 4096 };
  ctrl.handleGIIEvent(prs, 1920, 1080);
  expectNoReadableBytes(fds[1], 10);

  rfb::GIIEvent btn1Down;
  btn1Down.type = rfb::giiEventPtrButtonPress;
  btn1Down.origin = origin1;
  btn1Down.button = 1;
  ctrl.handleGIIEvent(btn1Down, 1920, 1080);

  readAllTimeout(fds[1], touch, sizeof(touch));
  EXPECT_EQ(touch[0], 2);
  EXPECT_EQ(touch[1], 0); // ACTION_DOWN
  EXPECT_EQ(readBe64(touch + 2), expectedPtr1);
  EXPECT_EQ(readBe32(touch + 10), 960u);
  EXPECT_EQ(readBe32(touch + 14), 540u);
  EXPECT_NEAR(readBe16(touch + 22), 32772u, 2u);
  EXPECT_EQ(readBe32(touch + 24), 1u);
  EXPECT_EQ(readBe32(touch + 28), 1u);

  // 3. Pressure-only change at same coordinates emits ACTION_MOVE
  prs.values = { 8191 };
  ctrl.handleGIIEvent(prs, 1920, 1080);
  readAllTimeout(fds[1], touch, sizeof(touch));
  EXPECT_EQ(touch[1], 2); // ACTION_MOVE
  EXPECT_EQ(readBe64(touch + 2), expectedPtr1);
  EXPECT_EQ(readBe16(touch + 22), 65535u);

  // 4. Second concurrent stylus device uses an independent pointerId
  uint32_t origin2 = ctrl.createGIIDevice(makeTestStylusDevice("Stylus 2"));
  ASSERT_NE(origin2, 0u);
  ASSERT_NE(origin2, origin1);
  const uint64_t expectedPtr2 = UINT64_C(0x4749490000000000) | origin2;

  rfb::GIIEvent move2;
  move2.type = rfb::giiEventValuatorAbsolute;
  move2.origin = origin2;
  move2.first = 0;
  move2.values = { 30000, 20000 };
  ctrl.handleGIIEvent(move2, 1920, 1080);
  expectNoReadableBytes(fds[1], 10);

  ctrl.handleVncPointer(400, 300, 0, 1920, 1080);
  expectNoReadableBytes(fds[1], 10);

  rfb::GIIEvent btn1Down2 = btn1Down;
  btn1Down2.origin = origin2;
  ctrl.handleGIIEvent(btn1Down2, 1920, 1080);
  readAllTimeout(fds[1], touch, sizeof(touch));
  EXPECT_EQ(touch[1], 0); // ACTION_DOWN
  EXPECT_EQ(readBe64(touch + 2), expectedPtr2);
  EXPECT_EQ(readBe32(touch + 10), 1919u);
  EXPECT_EQ(readBe32(touch + 14), 1079u);

  // Destroying origin1 while pressed releases origin1 only
  ctrl.destroyGIIDevice(origin1, 1920, 1080);
  EXPECT_EQ(ctrl.activeGIIDeviceCount(), 1u);
  readAllTimeout(fds[1], touch, sizeof(touch));
  EXPECT_EQ(touch[1], 1); // ACTION_UP
  EXPECT_EQ(readBe64(touch + 2), expectedPtr1);
  EXPECT_EQ(readBe32(touch + 10), 960u);
  EXPECT_EQ(readBe32(touch + 14), 540u);
  expectNoReadableBytes(fds[1], 10);

  // Release origin2
  rfb::GIIEvent btn1Up2;
  btn1Up2.type = rfb::giiEventPtrButtonRelease;
  btn1Up2.origin = origin2;
  btn1Up2.button = 1;
  ctrl.handleGIIEvent(btn1Up2, 1920, 1080);
  readAllTimeout(fds[1], touch, sizeof(touch));
  EXPECT_EQ(touch[1], 1); // ACTION_UP
  EXPECT_EQ(readBe64(touch + 2), expectedPtr2);

  // 5. Secondary/tertiary/back/forward mouse buttons & wheel on origin2
  const struct {
    uint32_t giiBtn;
    uint32_t androidBit;
  } kMouseMap[] = {
    { 2, 1u << 1 },
    { 3, 1u << 2 },
    { 8, 1u << 3 },
    { 9, 1u << 4 },
  };
  for (const auto& m : kMouseMap) {
    rfb::GIIEvent p;
    p.type = rfb::giiEventPtrButtonPress;
    p.origin = origin2;
    p.button = m.giiBtn;
    ctrl.handleGIIEvent(p, 1920, 1080);
    readAllTimeout(fds[1], touch, sizeof(touch));
    EXPECT_EQ(touch[1], 0);
    EXPECT_EQ(readBe64(touch + 2), mousePtr);
    EXPECT_EQ(readBe32(touch + 24), m.androidBit);
    EXPECT_EQ(readBe32(touch + 28), m.androidBit);

    rfb::GIIEvent r;
    r.type = rfb::giiEventPtrButtonRelease;
    r.origin = origin2;
    r.button = m.giiBtn;
    ctrl.handleGIIEvent(r, 1920, 1080);
    readAllTimeout(fds[1], touch, sizeof(touch));
    EXPECT_EQ(touch[1], 1);
    EXPECT_EQ(readBe32(touch + 24), m.androidBit);
    EXPECT_EQ(readBe32(touch + 28), 0u);
  }

  rfb::GIIEvent wheelBtn;
  wheelBtn.type = rfb::giiEventPtrButtonPress;
  wheelBtn.origin = origin2;
  wheelBtn.button = 4; // scroll up
  ctrl.handleGIIEvent(wheelBtn, 1920, 1080);
  uint8_t scroll[21];
  readAllTimeout(fds[1], scroll, sizeof(scroll));
  EXPECT_EQ(scroll[0], 3); // INJECT_SCROLL_EVENT
  EXPECT_EQ(readBeS16(scroll + 15), 2048);

  rfb::GIIEvent wheelVal;
  wheelVal.type = rfb::giiEventValuatorRelative;
  wheelVal.origin = origin2;
  wheelVal.first = 3;
  wheelVal.values = { -2 };
  ctrl.handleGIIEvent(wheelVal, 1920, 1080);
  readAllTimeout(fds[1], scroll, sizeof(scroll));
  EXPECT_EQ(scroll[0], 3);
  EXPECT_EQ(readBeS16(scroll + 15), -4096);

  // 6. Relative pointer motion + clamping after simulated resize (1080x1920)
  rfb::GIIEvent relPtr;
  relPtr.type = rfb::giiEventPtrRelative;
  relPtr.origin = origin2;
  relPtr.x = 5000;
  relPtr.y = -5000;
  relPtr.wheel = 1;
  ctrl.handleGIIEvent(relPtr, 1080, 1920);
  readAllTimeout(fds[1], touch, sizeof(touch));
  EXPECT_EQ(touch[1], 7);
  EXPECT_EQ(readBe32(touch + 10), 1079u);
  EXPECT_EQ(readBe32(touch + 14), 0u);
  EXPECT_EQ(readBe16(touch + 18), 1080u);
  EXPECT_EQ(readBe16(touch + 20), 1920u);
  readAllTimeout(fds[1], scroll, sizeof(scroll));
  EXPECT_EQ(scroll[0], 3);
  EXPECT_EQ(readBeS16(scroll + 15), 2048);

  // 7. Key press, repeat, Unicode text fallback, and release-on-destroy
  rfb::GIIEvent keyDown;
  keyDown.type = rfb::giiEventKeyPress;
  keyDown.origin = origin2;
  keyDown.symbol = 0x0061; // 'a'
  keyDown.button = 38;
  keyDown.modifiers = 0x01; // Shift
  ctrl.handleGIIEvent(keyDown, 1080, 1920);
  uint8_t keyMsg[14];
  readAllTimeout(fds[1], keyMsg, sizeof(keyMsg));
  EXPECT_EQ(keyMsg[0], 0); // INJECT_KEYCODE
  EXPECT_EQ(keyMsg[1], 0); // DOWN
  EXPECT_EQ(readBe32(keyMsg + 2), 29u); // AKEYCODE_A
  EXPECT_EQ(readBe32(keyMsg + 6), 0u);
  EXPECT_EQ(readBe32(keyMsg + 10), 0x41u); // META_SHIFT_ON | META_SHIFT_LEFT_ON

  rfb::GIIEvent keyRep = keyDown;
  keyRep.type = rfb::giiEventKeyRepeat;
  ctrl.handleGIIEvent(keyRep, 1080, 1920);
  readAllTimeout(fds[1], keyMsg, sizeof(keyMsg));
  EXPECT_EQ(keyMsg[1], 0);
  EXPECT_EQ(readBe32(keyMsg + 6), 1u); // repeat = 1

  rfb::GIIEvent unicodeKey;
  unicodeKey.type = rfb::giiEventKeyPress;
  unicodeKey.origin = origin2;
  unicodeKey.symbol = 0x00E9; // U+00E9 'é' -> UTF-8 C3 A9
  unicodeKey.button = 99;
  ctrl.handleGIIEvent(unicodeKey, 1080, 1920);
  uint8_t textMsg[7];
  readAllTimeout(fds[1], textMsg, sizeof(textMsg));
  EXPECT_EQ(textMsg[0], 1); // INJECT_TEXT
  EXPECT_EQ(readBe32(textMsg + 1), 2u);
  EXPECT_EQ(textMsg[5], 0xc3);
  EXPECT_EQ(textMsg[6], 0xa9);

  // Destroy origin2 while 'a' is still held -> emits KEY_ACTION_UP for 'a'
  ctrl.destroyGIIDevice(origin2, 1080, 1920);
  EXPECT_EQ(ctrl.activeGIIDeviceCount(), 0u);
  readAllTimeout(fds[1], keyMsg, sizeof(keyMsg));
  EXPECT_EQ(keyMsg[0], 0);
  EXPECT_EQ(keyMsg[1], 1); // UP
  EXPECT_EQ(readBe32(keyMsg + 2), 29u);

  // 8. Buttonless pressure-only device triggers synthetic DOWN/MOVE/UP
  rfb::GIIDevice btnless = makeTestStylusDevice("Buttonless Pen");
  btnless.numButtons = 0;
  btnless.canGenerate = rfb::giiEventMaskValuatorAbsolute;
  uint32_t origin3 = ctrl.createGIIDevice(btnless);
  ASSERT_NE(origin3, 0u);
  const uint64_t expectedPtr3 = UINT64_C(0x4749490000000000) | origin3;

  rfb::GIIEvent penDown;
  penDown.type = rfb::giiEventValuatorAbsolute;
  penDown.origin = origin3;
  penDown.first = 0;
  penDown.values = { 15000, 10000, 4096 };
  ctrl.handleGIIEvent(penDown, 1920, 1080);
  readAllTimeout(fds[1], touch, sizeof(touch));
  EXPECT_EQ(touch[1], 0); // ACTION_DOWN
  EXPECT_EQ(readBe64(touch + 2), expectedPtr3);
  EXPECT_NEAR(readBe16(touch + 22), 32772u, 2u);

  penDown.values = { 15000, 10000, 0 };
  ctrl.handleGIIEvent(penDown, 1920, 1080);
  readAllTimeout(fds[1], touch, sizeof(touch));
  EXPECT_EQ(touch[1], 1); // ACTION_UP
  EXPECT_EQ(readBe64(touch + 2), expectedPtr3);

  ctrl.destroyGIIDevice(origin3, 1920, 1080);
  ::close(fds[1]);
}

TEST(ScrcpyVncEndToEndTest, GIINegotiationEventsAndDisconnectCleanup)
{
  GiiTestParamGuard guard;
  rfb::SecurityServer::secTypes.setParam("None");
  gii.setParam(true);

  int videoFds[2], controlFds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, videoFds), 0);
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, controlFds), 0);
  network::UnixSocket videoPeer(videoFds[1]);
  network::UnixSocket controlPeer(controlFds[1]);

  ScrcpyServer server;
  server.attachStreamsForTest(videoFds[0], controlFds[0], ScrcpyProtocol::V2,
                              1920, 1080, "WayDroidGII");

  int clientPair[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, clientPair), 0);
  network::UnixSocket clientSock(clientPair[1]);
  ASSERT_TRUE(server.addSocket(new network::UnixSocket(clientPair[0]),
                               rfb::AccessDefault));

  GiiScrcpyTestCConnection cli(true);
  cli.setStreams(&clientSock.inStream(), &clientSock.outStream());
  cli.initialiseProtocol();

  auto pumpUntil = [&](auto pred) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!pred() && std::chrono::steady_clock::now() < deadline) {
      ASSERT_TRUE(server.step({}, 2));
      if (!cli.processMsg())
        ::poll(nullptr, 0, 1);
    }
    ASSERT_TRUE(pred());
  };

  pumpUntil([&]() { return cli.initialized && cli.server.supportsGII; });
  EXPECT_EQ(cli.server.giiVersion, 1u);
  ASSERT_EQ(cli.enabledVersions.size(), 1u);
  EXPECT_EQ(cli.enabledVersions[0], 1u);

  // Create a stylus device over RFB GII
  rfb::GIIDevice stylus = makeTestStylusDevice("RFB Stylus");
  cli.createGIIDevice(stylus);
  pumpUntil([&]() { return !cli.createdOrigins.empty(); });
  uint32_t origin = cli.createdOrigins[0];
  ASSERT_NE(origin, 0u);
  EXPECT_EQ(server.activeGIIDeviceCount(), 1u);

  // Send batched valuators + button 1 press + move
  rfb::GIIEvent evVal;
  evVal.type = rfb::giiEventValuatorAbsolute;
  evVal.origin = origin;
  evVal.first = 0;
  evVal.values = { 15000, 10000, 4096 };

  rfb::GIIEvent evBtn;
  evBtn.type = rfb::giiEventPtrButtonPress;
  evBtn.origin = origin;
  evBtn.button = 1;

  rfb::GIIEvent evMove;
  evMove.type = rfb::giiEventValuatorAbsolute;
  evMove.origin = origin;
  evMove.first = 0;
  evMove.values = { 20000, 15000, 6144 };

  cli.sendGIIEvents({ evVal, evBtn, evMove });
  ASSERT_TRUE(server.step({}, 5));

  uint8_t touch[32];
  readControlMessageSkipReset(controlFds[1], touch, sizeof(touch)); // hover from evVal
  EXPECT_EQ(touch[1], 7);
  readControlMessageSkipReset(controlFds[1], touch, sizeof(touch)); // down from evBtn
  EXPECT_EQ(touch[1], 0);
  EXPECT_EQ(readBe64(touch + 2), UINT64_C(0x4749490000000000) | origin);
  readControlMessageSkipReset(controlFds[1], touch, sizeof(touch)); // move from evMove
  EXPECT_EQ(touch[1], 2);
  EXPECT_EQ(readBe32(touch + 10), 1279u);
  EXPECT_EQ(readBe32(touch + 14), 809u);

  // Verify H.264 frame delivery still works cleanly alongside GII
  cli.msgWriter()->writeEnableContinuousUpdates(true, 0, 0, 1920, 1080);
  sendScrcpyPacket(videoFds[1], UINT64_C(1) << 63,
                   kFrame1080, kFrame1080SpsPpsLen);
  sendScrcpyPacket(videoFds[1], UINT64_C(1) << 62,
                   kFrame1080 + kFrame1080SpsPpsLen,
                   sizeof(kFrame1080) - kFrame1080SpsPpsLen);
  pumpUntil([&]() { return cli.h264Frames.size() >= sizeof(kFrame1080); });
  EXPECT_EQ(cli.h264Frames.size(), sizeof(kFrame1080));
  EXPECT_EQ(std::memcmp(cli.h264Frames.data(), kFrame1080, sizeof(kFrame1080)), 0);

  // Invalid events must be dropped without forwarding to scrcpy control
  rfb::GIIEvent badOrigin = evBtn;
  badOrigin.origin = origin + 999;
  rfb::GIIEvent badBtn = evBtn;
  badBtn.button = 99;
  rfb::GIIEvent badVal = evVal;
  badVal.first = 10;
  rfb::GIIEvent badMask;
  badMask.type = rfb::giiEventPtrRelative; // not in stylus.canGenerate
  badMask.origin = origin;
  badMask.x = 5;
  cli.sendGIIEvents({ badOrigin, badBtn, badVal, badMask });
  ASSERT_TRUE(server.step({}, 5));
  expectNoControlInput(controlFds[1], 20);

  // Hold a key and disconnect even after gii is toggled off -> cleanup must still run
  rfb::GIIEvent keyDown;
  keyDown.type = rfb::giiEventKeyPress;
  keyDown.origin = origin;
  keyDown.symbol = 0x0061;
  keyDown.button = 38;
  cli.sendGIIEvent(keyDown);
  ASSERT_TRUE(server.step({}, 5));
  uint8_t keyMsg[14];
  readControlMessageSkipReset(controlFds[1], keyMsg, sizeof(keyMsg));
  EXPECT_EQ(keyMsg[0], 0);
  EXPECT_EQ(keyMsg[1], 0);

  gii.setParam(false);
  cli.close();
  ::shutdown(clientSock.getFd(), SHUT_RDWR);
  for (int i = 0; i < 5 && server.activeGIIDeviceCount() > 0; ++i)
    ASSERT_TRUE(server.step({}, 5));
  EXPECT_EQ(server.activeGIIDeviceCount(), 0u);

  // Disconnect cleanup emitted ACTION_UP + KEY_ACTION_UP
  readControlMessageSkipReset(controlFds[1], touch, sizeof(touch));
  EXPECT_EQ(touch[0], 2);
  EXPECT_EQ(touch[1], 1); // ACTION_UP
  EXPECT_EQ(readBe64(touch + 2), UINT64_C(0x4749490000000000) | origin);

  readControlMessageSkipReset(controlFds[1], keyMsg, sizeof(keyMsg));
  EXPECT_EQ(keyMsg[0], 0);
  EXPECT_EQ(keyMsg[1], 1); // KEY_ACTION_UP
  EXPECT_EQ(readBe32(keyMsg + 2), 29u);
}

TEST(ScrcpyVncEndToEndTest, GIIAccessRightsIsolationAndDisableFlags)
{
  GiiTestParamGuard guard;
  rfb::SecurityServer::secTypes.setParam("None");
  gii.setParam(true);

  int videoFds[2], controlFds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, videoFds), 0);
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, controlFds), 0);
  network::UnixSocket videoPeer(videoFds[1]);
  network::UnixSocket controlPeer(controlFds[1]);

  ScrcpyServer server;
  server.attachStreamsForTest(videoFds[0], controlFds[0], ScrcpyProtocol::V2,
                              1920, 1080, "WayDroidGIISec");

  int pairA[2], pairB[2], pairC[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pairA), 0);
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pairB), 0);
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pairC), 0);
  network::UnixSocket sockA(pairA[1]), sockB(pairB[1]), sockC(pairC[1]);

  ASSERT_TRUE(server.addSocket(new network::UnixSocket(pairA[0]),
                               rfb::AccessDefault));
  ASSERT_TRUE(server.addSocket(new network::UnixSocket(pairB[0]),
                               rfb::AccessView | rfb::AccessNoQuery));
  ASSERT_TRUE(server.addSocket(new network::UnixSocket(pairC[0]),
                               rfb::AccessDefault));

  GiiScrcpyTestCConnection cliA(true), cliB(true), cliC(true);
  cliA.setShared(true);
  cliB.setShared(true);
  cliC.setShared(true);
  cliA.setStreams(&sockA.inStream(), &sockA.outStream());
  cliB.setStreams(&sockB.inStream(), &sockB.outStream());
  cliC.setStreams(&sockC.inStream(), &sockC.outStream());
  cliA.initialiseProtocol();
  cliB.initialiseProtocol();
  cliC.initialiseProtocol();

  auto pumpAll = [&](auto pred) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!pred() && std::chrono::steady_clock::now() < deadline) {
      ASSERT_TRUE(server.step({}, 2));
      bool progress = false;
      progress |= cliA.processMsg();
      progress |= cliB.processMsg();
      progress |= cliC.processMsg();
      if (!progress)
        ::poll(nullptr, 0, 1);
    }
    ASSERT_TRUE(pred());
  };

  pumpAll([&]() {
    return cliA.initialized && cliA.server.supportsGII &&
           cliB.initialized && cliB.server.supportsGII &&
           cliC.initialized && cliC.server.supportsGII;
  });

  // Client A creates device
  rfb::GIIDevice dev = makeTestStylusDevice("ClientA Stylus");
  cliA.createGIIDevice(dev);
  pumpAll([&]() { return cliA.createdOrigins.size() == 1; });
  uint32_t originA = cliA.createdOrigins[0];
  ASSERT_NE(originA, 0u);

  // View-only Client B must receive origin 0 on creation
  cliB.createGIIDevice(dev);
  pumpAll([&]() { return cliB.createdOrigins.size() == 1; });
  EXPECT_EQ(cliB.createdOrigins[0], 0u);

  // Cross-client isolation: Client C cannot inject or destroy Client A's device
  rfb::GIIEvent spoof;
  spoof.type = rfb::giiEventPtrButtonPress;
  spoof.origin = originA;
  spoof.button = 1;
  cliC.sendGIIEvent(spoof);
  cliC.destroyGIIDevice(originA);
  ASSERT_TRUE(server.step({}, 10));
  expectNoControlInput(controlFds[1], 20);
  EXPECT_EQ(server.activeGIIDeviceCount(), 1u);

  // Server::acceptPointerEvents / acceptKeyEvents enforcement
  rfb::Server::acceptPointerEvents.setParam(false);
  cliA.sendGIIEvent(spoof);
  ASSERT_TRUE(server.step({}, 5));
  expectNoControlInput(controlFds[1], 15);
  rfb::Server::acceptPointerEvents.setParam(true);

  rfb::Server::acceptKeyEvents.setParam(false);
  rfb::GIIEvent keyEv;
  keyEv.type = rfb::giiEventKeyPress;
  keyEv.origin = originA;
  keyEv.symbol = 0x0061;
  keyEv.button = 38;
  cliA.sendGIIEvent(keyEv);
  ASSERT_TRUE(server.step({}, 5));
  expectNoControlInput(controlFds[1], 15);
  rfb::Server::acceptKeyEvents.setParam(true);

  // Protocol error if a client sends GII device create before version message
  {
    int badPair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, badPair), 0);
    network::UnixSocket badSock(badPair[1]);
    ASSERT_TRUE(server.addSocket(new network::UnixSocket(badPair[0]),
                                 rfb::AccessDefault));
    ASSERT_NO_FATAL_FAILURE(initialiseRfb(server, badSock.getFd()));

    uint8_t encodings[] = {
      rfb::msgTypeSetEncodings, 0, 0, 2,
      0, 0, 0, 0,
      0, 0, 0, 0
    };
    writeBe32(encodings + 4, static_cast<uint32_t>(rfb::encodingH264));
    writeBe32(encodings + 8, static_cast<uint32_t>(rfb::pseudoEncodingGII));
    writeAll(badSock.getFd(), encodings, sizeof(encodings));
    ASSERT_TRUE(server.step({}, 5));

    // Drain server GII version message (8 bytes), then send giiClientDeviceCreate without giiClientVersion
    uint8_t verMsg[8];
    readAllTimeout(badSock.getFd(), verMsg, sizeof(verMsg));
    EXPECT_EQ(verMsg[0], rfb::msgTypeGIIServerMessage);
    EXPECT_EQ(verMsg[1] & rfb::giiSubTypeMask, rfb::giiServerVersion);
    EXPECT_EQ(readBe16(verMsg + 2), 4u);
    EXPECT_EQ(readBe16(verMsg + 4), 1u);
    EXPECT_EQ(readBe16(verMsg + 6), 1u);

    uint8_t badCreate[4 + 56] = {};
    badCreate[0] = rfb::msgTypeGIIClientMessage;
    badCreate[1] = rfb::giiBigEndian | rfb::giiClientDeviceCreate;
    writeBe16(badCreate + 2, 56);
    writeAll(badSock.getFd(), badCreate, sizeof(badCreate));
    ASSERT_TRUE(server.step({}, 5));

    uint8_t byte;
    EXPECT_EQ(::recv(badSock.getFd(), &byte, 1, MSG_DONTWAIT), 0);
  }

  // -GII=0 disables GII negotiation
  {
    gii.setParam(false);
    int pairD[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pairD), 0);
    network::UnixSocket sockD(pairD[1]);
    ASSERT_TRUE(server.addSocket(new network::UnixSocket(pairD[0]),
                                 rfb::AccessDefault));

    GiiScrcpyTestCConnection cliD(true);
    cliD.setShared(true);
    cliD.setStreams(&sockD.inStream(), &sockD.outStream());
    cliD.initialiseProtocol();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!cliD.initialized && std::chrono::steady_clock::now() < deadline) {
      ASSERT_TRUE(server.step({}, 5));
      if (!cliD.processMsg())
        ::poll(nullptr, 0, 2);
    }
    ASSERT_TRUE(cliD.initialized);
    cliD.msgWriter()->writeFramebufferUpdateRequest({0, 0, 1, 1}, false);
    ASSERT_TRUE(server.step({}, 10));
    cliD.processMsg();
    EXPECT_FALSE(cliD.server.supportsGII);
    gii.setParam(true);
  }

  // Missing control channel disables GII negotiation
  {
    int vidOnly[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, vidOnly), 0);
    network::UnixSocket vidPeer(vidOnly[1]);
    ScrcpyServer noCtrlServer;
    noCtrlServer.attachStreamsForTest(vidOnly[0], -1, ScrcpyProtocol::V2,
                                      1920, 1080, "NoCtrl");

    int pairE[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pairE), 0);
    network::UnixSocket sockE(pairE[1]);
    ASSERT_TRUE(noCtrlServer.addSocket(new network::UnixSocket(pairE[0]),
                                       rfb::AccessDefault));

    GiiScrcpyTestCConnection cliE(true);
    cliE.setStreams(&sockE.inStream(), &sockE.outStream());
    cliE.initialiseProtocol();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!cliE.initialized && std::chrono::steady_clock::now() < deadline) {
      ASSERT_TRUE(noCtrlServer.step({}, 5));
      if (!cliE.processMsg())
        ::poll(nullptr, 0, 2);
    }
    ASSERT_TRUE(cliE.initialized);
    cliE.msgWriter()->writeFramebufferUpdateRequest({0, 0, 1, 1}, false);
    ASSERT_TRUE(noCtrlServer.step({}, 10));
    cliE.processMsg();
    EXPECT_FALSE(cliE.server.supportsGII);
  }
}
