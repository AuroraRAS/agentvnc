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

#ifndef __RDPVNCSERVER_RDPH264PASSTHROUGH_H__
#define __RDPVNCSERVER_RDPH264PASSTHROUGH_H__

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rdpvnc {

/**
 * RFB encodingH264 (50) rectangle header flags defined by TigerVNC / RFB.
 */
constexpr uint32_t kH264FlagResetContext = 0x01;
constexpr uint32_t kH264FlagResetAllContexts = 0x02;

/**
 * Single parsed H.264 Annex-B NAL unit view (non-owning).
 */
struct AnnexBNalUnit {
  uint8_t nalType = 0;
  const uint8_t* body = nullptr;
  size_t bodySize = 0;
  size_t startCodeLen = 0;
};

/**
 * Normalized Annex-B frame ready for RFB encodingH264 packetization.
 */
struct ProcessedH264Frame {
  std::vector<uint8_t> annexB;
  uint32_t flags = 0;
  bool hasIdr = false;
  bool hasSps = false;
  bool hasPps = false;
};

/**
 * Maintains per-stream H.264 Annex-B parameter sets (SPS/PPS) and decoder
 * context reset flags across AVC420 WireToSurface1 frames, so mid-stream
 * joining clients receive self-contained IDR keyframes and resizes/reconnects
 * emit explicit context-reset flags.
 */
class RdpH264StreamState {
public:
  RdpH264StreamState() = default;

  /**
   * Resets all cached parameter sets and flags the next emitted frame with
   * kH264FlagResetAllContexts.
   */
  void reset();

  /**
   * Marks the next emitted frame with kH264FlagResetContext (e.g. surface resize
   * or surface recreation).
   */
  void markContextReset();

  /**
   * Marks the next emitted frame with kH264FlagResetAllContexts (e.g. session
   * reconnect or discontinuity).
   */
  void markAllContextsReset();

  /**
   * Parses an incoming H.264 Annex-B access unit, updates cached SPS/PPS,
   * prepends cached SPS/PPS before IDR slices when omitted, and consumes any
   * pending reset flags into `out.flags`. Returns false if no valid NAL units
   * exist in `data`.
   */
  bool processAccessUnit(const uint8_t* data, size_t size,
                         ProcessedH264Frame& out);

  bool hasCachedParameters() const {
    return !cachedSps_.empty() && !cachedPps_.empty();
  }
  const std::vector<uint8_t>& cachedSps() const { return cachedSps_; }
  const std::vector<uint8_t>& cachedPps() const { return cachedPps_; }

  /**
   * Parses Annex-B 3-byte (00 00 01) and 4-byte (00 00 00 01) start-code NAL
   * units into `units`. Returns true if at least one NAL unit was extracted.
   */
  static bool parseAnnexBNalUnits(const uint8_t* data, size_t size,
                                  std::vector<AnnexBNalUnit>& units);

  /**
   * Encodes an H.264 Annex-B payload into the RFB Open H.264 rectangle wire
   * format: [U32 length][U32 flags][U8[length] annexB].
   */
  static std::vector<uint8_t> packetizeRfbRect(const uint8_t* payload,
                                               size_t payloadLen,
                                               uint32_t flags);

  /**
   * Validates basic JFIF / baseline JPEG framing (SOI 0xFFD8, structurally
   * valid marker chain with JFIF/SOF + SOS, and EOI 0xFFD9) before zero-decode
   * JPEG passthrough.
   */
  static bool isValidJfif(const uint8_t* data, size_t len);

private:
  static std::vector<uint8_t> buildAnnexBNal(const uint8_t* body, size_t len);

  std::vector<uint8_t> cachedSps_;
  std::vector<uint8_t> cachedPps_;
  bool pendingResetContext_ = false;
  bool pendingResetAll_ = true;
};

} // namespace rdpvnc

#endif // __RDPVNCSERVER_RDPH264PASSTHROUGH_H__
