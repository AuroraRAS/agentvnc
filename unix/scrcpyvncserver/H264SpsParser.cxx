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

#include "H264SpsParser.h"

namespace {

size_t findStartCode(const uint8_t* data, size_t len, size_t pos, size_t* scLen)
{
  for (size_t i = pos; i + 2 < len; ++i) {
    if (data[i] == 0 && data[i + 1] == 0) {
      if (i + 3 < len && data[i + 2] == 0 && data[i + 3] == 1) {
        *scLen = 4;
        return i;
      }
      if (data[i + 2] == 1) {
        *scLen = 3;
        return i;
      }
    }
  }
  return len;
}

class BitReader {
public:
  BitReader(const uint8_t* data, size_t len)
    : data_(data), len_(len), bytePos_(0), bitPos_(0), overflow_(false)
  {
  }

  bool overflow() const { return overflow_; }

  uint32_t readBit()
  {
    if (bytePos_ >= len_) {
      overflow_ = true;
      return 0;
    }
    uint32_t bit = (data_[bytePos_] >> (7 - bitPos_)) & 1u;
    bitPos_++;
    if (bitPos_ == 8) {
      bitPos_ = 0;
      bytePos_++;
    }
    return bit;
  }

  uint32_t readBits(int count)
  {
    uint32_t val = 0;
    for (int i = 0; i < count; ++i) {
      val = (val << 1) | readBit();
      if (overflow_)
        return 0;
    }
    return val;
  }

  uint32_t readUE()
  {
    int zeros = 0;
    while (!overflow_ && readBit() == 0) {
      zeros++;
      if (zeros > 31) {
        overflow_ = true;
        return 0;
      }
    }
    if (overflow_)
      return 0;
    if (zeros == 0)
      return 0;
    uint32_t suffix = readBits(zeros);
    return ((1u << zeros) - 1u) + suffix;
  }

  int32_t readSE()
  {
    uint32_t ue = readUE();
    if (overflow_)
      return 0;
    if (ue & 1u)
      return static_cast<int32_t>((ue + 1u) >> 1);
    return -static_cast<int32_t>(ue >> 1);
  }

private:
  const uint8_t* data_;
  size_t len_;
  size_t bytePos_;
  int bitPos_;
  bool overflow_;
};

std::vector<uint8_t> removeEmulationPrevention(const uint8_t* nal, size_t len)
{
  std::vector<uint8_t> rbsp;
  rbsp.reserve(len);
  for (size_t i = 0; i < len; ++i) {
    if (i + 2 < len && nal[i] == 0x00 && nal[i + 1] == 0x00 &&
        nal[i + 2] == 0x03) {
      rbsp.push_back(0x00);
      rbsp.push_back(0x00);
      i += 2;
    } else {
      rbsp.push_back(nal[i]);
    }
  }
  return rbsp;
}

bool skipScalingList(BitReader& br, int sizeOfScalingList)
{
  int lastScale = 8;
  int nextScale = 8;
  for (int j = 0; j < sizeOfScalingList; ++j) {
    if (nextScale != 0) {
      int32_t deltaScale = br.readSE();
      if (br.overflow() || deltaScale < -128 || deltaScale > 127)
        return false;
      nextScale = (lastScale + deltaScale + 256) % 256;
    }
    lastScale = (nextScale == 0) ? lastScale : nextScale;
  }
  return true;
}

bool parseSpsNal(const uint8_t* nal, size_t nalLen, int* width, int* height)
{
  if (!nal || nalLen < 4 || (nal[0] & 0x1Fu) != 7)
    return false;

  std::vector<uint8_t> rbsp = removeEmulationPrevention(nal + 1, nalLen - 1);
  if (rbsp.size() < 3)
    return false;

  BitReader br(rbsp.data(), rbsp.size());
  uint32_t profileIdc = br.readBits(8);
  br.readBits(8); // constraint_set0..5_flag + reserved_zero_2bits
  br.readBits(8); // level_idc
  br.readUE();    // seq_parameter_set_id

  uint32_t chromaFormatIdc = 1;
  uint32_t separateColourPlaneFlag = 0;

  if (profileIdc == 100 || profileIdc == 110 || profileIdc == 122 ||
      profileIdc == 244 || profileIdc == 44 || profileIdc == 83 ||
      profileIdc == 86 || profileIdc == 118 || profileIdc == 128 ||
      profileIdc == 138 || profileIdc == 139 || profileIdc == 134 ||
      profileIdc == 135) {
    chromaFormatIdc = br.readUE();
    if (chromaFormatIdc > 3)
      return false;
    if (chromaFormatIdc == 3)
      separateColourPlaneFlag = br.readBit();
    br.readUE(); // bit_depth_luma_minus8
    br.readUE(); // bit_depth_chroma_minus8
    br.readBit(); // qpprime_y_zero_transform_bypass_flag
    if (br.readBit() != 0) { // seq_scaling_matrix_present_flag
      int count = (chromaFormatIdc != 3) ? 8 : 12;
      for (int i = 0; i < count; ++i) {
        if (br.readBit() != 0) {
          if (!skipScalingList(br, i < 6 ? 16 : 64))
            return false;
        }
      }
    }
  }

  br.readUE(); // log2_max_frame_num_minus4
  uint32_t picOrderCntType = br.readUE();
  if (picOrderCntType == 0) {
    br.readUE(); // log2_max_pic_order_cnt_lsb_minus4
  } else if (picOrderCntType == 1) {
    br.readBit(); // delta_pic_order_always_zero_flag
    br.readSE();  // offset_for_non_ref_pic
    br.readSE();  // offset_for_top_to_bottom_field
    uint32_t numRef = br.readUE();
    if (numRef > 256 || br.overflow())
      return false;
    for (uint32_t i = 0; i < numRef; ++i)
      br.readSE();
  } else if (picOrderCntType > 2) {
    return false;
  }

  br.readUE(); // max_num_ref_frames
  br.readBit(); // gaps_in_frame_num_value_allowed_flag

  uint32_t picWidthInMbsMinus1 = br.readUE();
  uint32_t picHeightInMapUnitsMinus1 = br.readUE();
  uint32_t frameMbsOnlyFlag = br.readBit();
  if (!frameMbsOnlyFlag)
    br.readBit(); // mb_adaptive_frame_field_flag

  br.readBit(); // direct_8x8_inference_flag

  uint32_t cropLeft = 0;
  uint32_t cropRight = 0;
  uint32_t cropTop = 0;
  uint32_t cropBottom = 0;
  if (br.readBit() != 0) { // frame_cropping_flag
    cropLeft = br.readUE();
    cropRight = br.readUE();
    cropTop = br.readUE();
    cropBottom = br.readUE();
  }

  if (br.overflow())
    return false;

  uint32_t chromaArrayType = separateColourPlaneFlag ? 0 : chromaFormatIdc;
  uint32_t subWidthC = (chromaArrayType == 1 || chromaArrayType == 2) ? 2 : 1;
  uint32_t subHeightC = (chromaArrayType == 1) ? 2 : 1;
  uint32_t cropUnitX = (chromaArrayType == 0) ? 1 : subWidthC;
  uint32_t cropUnitY =
    ((chromaArrayType == 0) ? 1 : subHeightC) * (2 - frameMbsOnlyFlag);

  int64_t rawW = static_cast<int64_t>(picWidthInMbsMinus1 + 1) * 16;
  int64_t rawH = static_cast<int64_t>(2 - frameMbsOnlyFlag) *
                 static_cast<int64_t>(picHeightInMapUnitsMinus1 + 1) * 16;
  int64_t trimW = (static_cast<int64_t>(cropLeft) + cropRight) *
                  static_cast<int64_t>(cropUnitX);
  int64_t trimH = (static_cast<int64_t>(cropTop) + cropBottom) *
                  static_cast<int64_t>(cropUnitY);

  int64_t w = rawW - trimW;
  int64_t h = rawH - trimH;
  if (w <= 0 || h <= 0 || w > 16384 || h > 16384)
    return false;

  *width = static_cast<int>(w);
  *height = static_cast<int>(h);
  return true;
}

} // namespace

H264PacketInfo inspectAnnexB(const uint8_t* data, size_t len)
{
  H264PacketInfo info;
  if (!data || len < 4)
    return info;

  size_t scLen = 0;
  size_t scPos = findStartCode(data, len, 0, &scLen);
  while (scPos < len) {
    size_t nalStart = scPos + scLen;
    size_t nextScLen = 0;
    size_t nextSc = findStartCode(data, len, nalStart, &nextScLen);

    if (nalStart < nextSc) {
      uint8_t nalType = data[nalStart] & 0x1Fu;
      if (nalType == 7) {
        info.hasSps = true;
        info.spsPps.insert(info.spsPps.end(),
                           data + scPos, data + nextSc);
      } else if (nalType == 8) {
        info.hasPps = true;
        info.spsPps.insert(info.spsPps.end(),
                           data + scPos, data + nextSc);
      } else if (nalType == 5) {
        info.hasIdr = true;
      }
    }

    scPos = nextSc;
    scLen = nextScLen;
  }

  return info;
}

bool parseSpsDimensions(const uint8_t* data, size_t len,
                        int* width, int* height)
{
  if (!data || !width || !height || len < 4)
    return false;

  size_t scLen = 0;
  size_t scPos = findStartCode(data, len, 0, &scLen);
  if (scPos >= len) {
    return parseSpsNal(data, len, width, height);
  }

  while (scPos < len) {
    size_t nalStart = scPos + scLen;
    size_t nextScLen = 0;
    size_t nextSc = findStartCode(data, len, nalStart, &nextScLen);

    if (nalStart < nextSc && (data[nalStart] & 0x1Fu) == 7) {
      if (parseSpsNal(data + nalStart, nextSc - nalStart, width, height))
        return true;
    }

    scPos = nextSc;
    scLen = nextScLen;
  }

  return false;
}
