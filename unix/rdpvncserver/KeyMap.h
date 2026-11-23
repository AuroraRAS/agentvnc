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

#ifndef __RDPVNCSERVER_KEYMAP_H__
#define __RDPVNCSERVER_KEYMAP_H__

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <freerdp/channels/disp.h>
#include <rfb/ScreenSet.h>

namespace rdpvnc {

/**
 * Translated RDP keyboard event ready for freerdp_input_send_* calls.
 */
struct KeyAction {
  enum class Kind {
    None,
    Scancode,
    Unicode,
    Pause,
  };

  Kind kind = Kind::None;
  uint16_t flags = 0; // KBD_FLAGS_DOWN / KBD_FLAGS_RELEASE / KBD_FLAGS_EXTENDED
  uint16_t code = 0;  // 8-bit XT scancode or UTF-16 code unit
  uint16_t code2 = 0; // Non-zero for UTF-16 surrogate pairs
};

/**
 * Translated RDP mouse/wheel/extended-mouse event ready for FreeRDP input.
 */
struct PointerAction {
  bool isExtended = false;
  uint16_t flags = 0; // PTR_FLAGS_* or PTR_XFLAGS_*
  uint16_t x = 0;
  uint16_t y = 0;
};

/**
 * Translates VNC/RFB keyboard, pointer, clipboard, and monitor-layout inputs
 * into FreeRDP 3 protocol equivalents.
 */
class KeyMap {
public:
  /**
   * Translates RFB keysym + QEMU extended XT keycode into an RDP KeyAction.
   * Non-zero keycodes map 1:1 (bit 0x80 -> KBD_FLAGS_EXTENDED); keycode == 0
   * uses the static X11 keysym -> XT scancode lookup table for control keys
   * and falls back to Unicode events for printable characters.
   */
  static KeyAction translateKey(uint32_t keysym, uint32_t keycode, bool down);

  /**
   * Looks up a PC XT scancode (high bit 0x0100 = extended) for non-printable
   * X11 keysyms. Returns 0 if no XT scancode mapping applies.
   */
  static uint16_t keysymToRdpScancode(uint32_t keysym);

  /**
   * Translates RFB pointer position and button mask delta into FreeRDP mouse
   * actions, clamping coordinates to the active desktop dimensions and swapping
   * VNC button 2/3 (middle/right) to match RDP PTR_FLAGS_BUTTON2/3.
   */
  static std::vector<PointerAction> translatePointer(int x, int y,
                                                     uint16_t buttonMask,
                                                     uint16_t prevButtonMask,
                                                     int fbWidth, int fbHeight);

  /**
   * Converts UTF-8 text (LF or CRLF) into NUL-terminated UTF-16LE with CRLF
   * line endings for Windows CF_UNICODETEXT clipboard transfer.
   */
  static std::vector<uint8_t> utf8ToUtf16LeCrlf(const std::string& utf8);

  /**
   * Converts Windows CF_UNICODETEXT UTF-16LE payload into UTF-8 with LF line
   * endings, stopping at the first NUL code unit.
   */
  static std::string utf16LeToUtf8Lf(const uint8_t* data, size_t byteLen);

  /**
   * Translates a TigerVNC ScreenSet into FreeRDP DISPLAY_CONTROL_MONITOR_LAYOUT
   * structures clamped to MS-RDPEDISP limits [200, 8192].
   */
  static std::vector<DISPLAY_CONTROL_MONITOR_LAYOUT> buildMonitorLayout(
      int fbWidth, int fbHeight, const rfb::ScreenSet& layout);
};

} // namespace rdpvnc

#endif // __RDPVNCSERVER_KEYMAP_H__
