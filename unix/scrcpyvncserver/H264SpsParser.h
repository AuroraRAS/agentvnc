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

#ifndef __SCRCPYVNCSERVER_H264SPSPARSER_H__
#define __SCRCPYVNCSERVER_H264SPSPARSER_H__

#include <stddef.h>
#include <stdint.h>

#include <vector>

struct H264PacketInfo {
  bool hasSps = false;
  bool hasPps = false;
  bool hasIdr = false;
  std::vector<uint8_t> spsPps;
};

H264PacketInfo inspectAnnexB(const uint8_t* data, size_t len);

bool parseSpsDimensions(const uint8_t* data, size_t len,
                        int* width, int* height);

#endif // __SCRCPYVNCSERVER_H264SPSPARSER_H__
