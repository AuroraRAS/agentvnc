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

#include "RdpH264Passthrough.h"

#include <cstring>

namespace rdpvnc {

namespace {

bool findNextStartCode(const uint8_t* data, size_t size, size_t pos,
                       size_t& outPos, size_t& outLen)
{
  for (size_t i = pos; i + 3 <= size; ++i) {
    if (data[i] == 0x00 && data[i + 1] == 0x00) {
      if (i + 4 <= size && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
        outPos = i;
        outLen = 4;
        return true;
      }
      if (data[i + 2] == 0x01) {
        outPos = i;
        outLen = 3;
        return true;
      }
    }
  }
  return false;
}

void writeU32BE(std::vector<uint8_t>& out, uint32_t value)
{
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(value & 0xFF));
}

} // namespace

void RdpH264StreamState::reset()
{
  cachedSps_.clear();
  cachedPps_.clear();
  pendingResetContext_ = false;
  pendingResetAll_ = true;
}

void RdpH264StreamState::markContextReset()
{
  pendingResetContext_ = true;
}

void RdpH264StreamState::markAllContextsReset()
{
  pendingResetAll_ = true;
}

std::vector<uint8_t> RdpH264StreamState::buildAnnexBNal(const uint8_t* body,
                                                        size_t len)
{
  std::vector<uint8_t> out;
  out.reserve(4 + len);
  out.push_back(0x00);
  out.push_back(0x00);
  out.push_back(0x00);
  out.push_back(0x01);
  out.insert(out.end(), body, body + len);
  return out;
}

bool RdpH264StreamState::parseAnnexBNalUnits(const uint8_t* data, size_t size,
                                             std::vector<AnnexBNalUnit>& units)
{
  units.clear();
  if (!data || size < 4)
    return false;

  size_t scPos = 0;
  size_t scLen = 0;
  if (!findNextStartCode(data, size, 0, scPos, scLen))
    return false;

  while (scPos + scLen < size) {
    size_t nalStart = scPos + scLen;
    size_t nextScPos = 0;
    size_t nextScLen = 0;
    bool hasNext = findNextStartCode(data, size, nalStart, nextScPos, nextScLen);
    size_t nalEnd = hasNext ? nextScPos : size;

    // Trim trailing zero_byte padding preceding the next start code.
    while (hasNext && nalEnd > nalStart && data[nalEnd - 1] == 0x00) {
      --nalEnd;
    }

    if (nalEnd > nalStart) {
      AnnexBNalUnit unit;
      unit.nalType = data[nalStart] & 0x1F;
      unit.body = data + nalStart;
      unit.bodySize = nalEnd - nalStart;
      unit.startCodeLen = scLen;
      units.push_back(unit);
    }

    if (!hasNext)
      break;
    scPos = nextScPos;
    scLen = nextScLen;
  }

  return !units.empty();
}

bool RdpH264StreamState::processAccessUnit(const uint8_t* data, size_t size,
                                           ProcessedH264Frame& out)
{
  out.annexB.clear();
  out.flags = 0;
  out.hasIdr = false;
  out.hasSps = false;
  out.hasPps = false;

  std::vector<AnnexBNalUnit> units;
  if (!parseAnnexBNalUnits(data, size, units))
    return false;

  for (const AnnexBNalUnit& u : units) {
    if (u.nalType == 7) {
      cachedSps_ = buildAnnexBNal(u.body, u.bodySize);
      out.hasSps = true;
    } else if (u.nalType == 8) {
      cachedPps_ = buildAnnexBNal(u.body, u.bodySize);
      out.hasPps = true;
    } else if (u.nalType == 5) {
      out.hasIdr = true;
    }
  }

  bool prependSps = out.hasIdr && !out.hasSps && !cachedSps_.empty();
  bool prependPps = out.hasIdr && !out.hasPps && !cachedPps_.empty();

  out.annexB.reserve((prependSps ? cachedSps_.size() : 0) +
                     (prependPps ? cachedPps_.size() : 0) + size);
  if (prependSps) {
    out.annexB.insert(out.annexB.end(), cachedSps_.begin(), cachedSps_.end());
    out.hasSps = true;
  }
  if (prependPps) {
    out.annexB.insert(out.annexB.end(), cachedPps_.begin(), cachedPps_.end());
    out.hasPps = true;
  }
  out.annexB.insert(out.annexB.end(), data, data + size);

  if (pendingResetAll_) {
    out.flags |= kH264FlagResetAllContexts;
    pendingResetAll_ = false;
    pendingResetContext_ = false;
  } else if (pendingResetContext_) {
    out.flags |= kH264FlagResetContext;
    pendingResetContext_ = false;
  }

  return true;
}

std::vector<uint8_t> RdpH264StreamState::packetizeRfbRect(const uint8_t* payload,
                                                          size_t payloadLen,
                                                          uint32_t flags)
{
  std::vector<uint8_t> packet;
  packet.reserve(8 + payloadLen);
  writeU32BE(packet, static_cast<uint32_t>(payloadLen));
  writeU32BE(packet, flags);
  if (payload && payloadLen > 0) {
    packet.insert(packet.end(), payload, payload + payloadLen);
  }
  return packet;
}

bool RdpH264StreamState::isValidJfif(const uint8_t* data, size_t len)
{
  if (!data || len < 4)
    return false;
  if (data[0] != 0xFF || data[1] != 0xD8)
    return false;
  if (data[len - 2] != 0xFF || data[len - 1] != 0xD9)
    return false;

  bool sawSofOrJfif = false;
  bool sawSos = false;
  size_t pos = 2;

  while (pos < len - 2) {
    if (data[pos] != 0xFF)
      return false;
    while (pos < len && data[pos] == 0xFF)
      ++pos;
    if (pos >= len)
      return false;

    uint8_t marker = data[pos++];
    if (marker == 0x00 || marker == 0xD8)
      return false;
    if (marker == 0xD9)
      return pos == len && sawSos && sawSofOrJfif;
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
      continue;

    if (pos + 2 > len - 2)
      return false;
    uint16_t segLen = static_cast<uint16_t>((data[pos] << 8) | data[pos + 1]);
    if (segLen < 2 || pos + segLen > len - 2)
      return false;

    if (marker == 0xE0 && segLen >= 7 &&
        std::memcmp(data + pos + 2, "JFIF\0", 5) == 0) {
      sawSofOrJfif = true;
    } else if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
      sawSofOrJfif = true;
    } else if (marker == 0xDA) {
      sawSos = true;
      break;
    }

    pos += segLen;
  }

  return sawSos && sawSofOrJfif;
}

} // namespace rdpvnc
