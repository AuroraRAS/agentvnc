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

#include "KeyMap.h"

#include <algorithm>
#include <freerdp/input.h>

namespace rdpvnc {

namespace {

constexpr UINT32 kOrientationLandscape = 0;

struct KeysymMapEntry {
  uint32_t keysym;
  uint16_t rdpScancode; // bit 0x0100 == extended, low 8 bits == XT code
};

// Static X11 keysym -> PC XT scancode table for non-printable/control keys.
constexpr KeysymMapEntry kKeysymTable[] = {
  { 0xFF08, 0x000E }, // BackSpace
  { 0xFF09, 0x000F }, // Tab
  { 0xFE20, 0x000F }, // ISO_Left_Tab
  { 0xFF0A, 0x001C }, // Linefeed
  { 0xFF0D, 0x001C }, // Return
  { 0xFF14, 0x0046 }, // Scroll_Lock
  { 0xFF15, 0x0137 }, // Sys_Req
  { 0xFF1B, 0x0001 }, // Escape

  { 0xFF50, 0x0147 }, // Home
  { 0xFF51, 0x014B }, // Left
  { 0xFF52, 0x0148 }, // Up
  { 0xFF53, 0x014D }, // Right
  { 0xFF54, 0x0150 }, // Down
  { 0xFF55, 0x0149 }, // Prior / Page_Up
  { 0xFF56, 0x0151 }, // Next / Page_Down
  { 0xFF57, 0x014F }, // End

  { 0xFF61, 0x0137 }, // Print
  { 0xFF63, 0x0152 }, // Insert
  { 0xFF67, 0x015D }, // Menu
  { 0xFF7F, 0x0045 }, // Num_Lock

  // Keypad keys
  { 0xFF8D, 0x011C }, // KP_Enter
  { 0xFF95, 0x0047 }, // KP_Home
  { 0xFF96, 0x004B }, // KP_Left
  { 0xFF97, 0x0048 }, // KP_Up
  { 0xFF98, 0x004D }, // KP_Right
  { 0xFF99, 0x0050 }, // KP_Down
  { 0xFF9A, 0x0049 }, // KP_Prior
  { 0xFF9B, 0x0051 }, // KP_Next
  { 0xFF9C, 0x004F }, // KP_End
  { 0xFF9E, 0x0052 }, // KP_Insert
  { 0xFF9F, 0x0053 }, // KP_Delete
  { 0xFFAA, 0x0037 }, // KP_Multiply
  { 0xFFAB, 0x004E }, // KP_Add
  { 0xFFAD, 0x004A }, // KP_Subtract
  { 0xFFAE, 0x0053 }, // KP_Decimal
  { 0xFFAF, 0x0135 }, // KP_Divide
  { 0xFFB0, 0x0052 }, // KP_0
  { 0xFFB1, 0x004F }, // KP_1
  { 0xFFB2, 0x0050 }, // KP_2
  { 0xFFB3, 0x0051 }, // KP_3
  { 0xFFB4, 0x004B }, // KP_4
  { 0xFFB5, 0x004C }, // KP_5
  { 0xFFB6, 0x004D }, // KP_6
  { 0xFFB7, 0x0047 }, // KP_7
  { 0xFFB8, 0x0048 }, // KP_8
  { 0xFFB9, 0x0049 }, // KP_9

  // Function keys F1..F24
  { 0xFFBE, 0x003B }, // F1
  { 0xFFBF, 0x003C }, // F2
  { 0xFFC0, 0x003D }, // F3
  { 0xFFC1, 0x003E }, // F4
  { 0xFFC2, 0x003F }, // F5
  { 0xFFC3, 0x0040 }, // F6
  { 0xFFC4, 0x0041 }, // F7
  { 0xFFC5, 0x0042 }, // F8
  { 0xFFC6, 0x0043 }, // F9
  { 0xFFC7, 0x0044 }, // F10
  { 0xFFC8, 0x0057 }, // F11
  { 0xFFC9, 0x0058 }, // F12
  { 0xFFCA, 0x0064 }, // F13
  { 0xFFCB, 0x0065 }, // F14
  { 0xFFCC, 0x0066 }, // F15
  { 0xFFCD, 0x0067 }, // F16
  { 0xFFCE, 0x0068 }, // F17
  { 0xFFCF, 0x0069 }, // F18
  { 0xFFD0, 0x006A }, // F19
  { 0xFFD1, 0x006B }, // F20
  { 0xFFD2, 0x006C }, // F21
  { 0xFFD3, 0x006D }, // F22
  { 0xFFD4, 0x006E }, // F23
  { 0xFFD5, 0x006F }, // F24

  // Modifiers & locks
  { 0xFFE1, 0x002A }, // Shift_L
  { 0xFFE2, 0x0036 }, // Shift_R
  { 0xFFE3, 0x001D }, // Control_L
  { 0xFFE4, 0x011D }, // Control_R
  { 0xFFE5, 0x003A }, // Caps_Lock
  { 0xFFE7, 0x015B }, // Meta_L
  { 0xFFE8, 0x015C }, // Meta_R
  { 0xFFE9, 0x0038 }, // Alt_L
  { 0xFFEA, 0x0138 }, // Alt_R
  { 0xFE03, 0x0138 }, // ISO_Level3_Shift
  { 0xFFEB, 0x015B }, // Super_L
  { 0xFFEC, 0x015C }, // Super_R

  { 0xFFFF, 0x0153 }, // Delete
};

uint32_t keysymToCodepoint(uint32_t keysym)
{
  if ((keysym >= 0x0020 && keysym <= 0x007E) ||
      (keysym >= 0x00A0 && keysym <= 0x00FF)) {
    return keysym;
  }
  if (keysym >= 0x01000100 && keysym <= 0x0110FFFF) {
    return keysym - 0x01000000U;
  }
  if (keysym == 0x20AC) {
    return 0x20AC; // EuroSign
  }
  return 0;
}

uint32_t decodeUtf8(const std::string& utf8, size_t& i)
{
  uint8_t c = static_cast<uint8_t>(utf8[i]);
  if (c < 0x80) {
    ++i;
    return c;
  }
  size_t rem = utf8.size() - i;
  uint32_t cp = 0xFFFD;
  size_t len = 1;
  if ((c & 0xE0) == 0xC0 && rem >= 2) {
    uint8_t c1 = static_cast<uint8_t>(utf8[i + 1]);
    if ((c1 & 0xC0) == 0x80) {
      uint32_t v = ((c & 0x1FU) << 6) | (c1 & 0x3FU);
      if (v >= 0x80) {
        cp = v;
        len = 2;
      }
    }
  } else if ((c & 0xF0) == 0xE0 && rem >= 3) {
    uint8_t c1 = static_cast<uint8_t>(utf8[i + 1]);
    uint8_t c2 = static_cast<uint8_t>(utf8[i + 2]);
    if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80) {
      uint32_t v = ((c & 0x0FU) << 12) | ((c1 & 0x3FU) << 6) | (c2 & 0x3FU);
      if (v >= 0x800 && !(v >= 0xD800 && v <= 0xDFFF)) {
        cp = v;
        len = 3;
      }
    }
  } else if ((c & 0xF8) == 0xF0 && rem >= 4) {
    uint8_t c1 = static_cast<uint8_t>(utf8[i + 1]);
    uint8_t c2 = static_cast<uint8_t>(utf8[i + 2]);
    uint8_t c3 = static_cast<uint8_t>(utf8[i + 3]);
    if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80) {
      uint32_t v = ((c & 0x07U) << 18) | ((c1 & 0x3FU) << 12) |
                   ((c2 & 0x3FU) << 6) | (c3 & 0x3FU);
      if (v >= 0x10000 && v <= 0x10FFFF) {
        cp = v;
        len = 4;
      }
    }
  }
  i += len;
  return cp;
}

void appendUtf16LeUnit(std::vector<uint8_t>& out, uint16_t unit)
{
  out.push_back(static_cast<uint8_t>(unit & 0xFF));
  out.push_back(static_cast<uint8_t>((unit >> 8) & 0xFF));
}

void appendUtf8Codepoint(std::string& out, uint32_t cp)
{
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

uint16_t clampCoord(int value, int limit)
{
  if (limit <= 1 || value <= 0)
    return 0;
  if (value >= limit)
    return static_cast<uint16_t>(limit - 1);
  return static_cast<uint16_t>(value);
}

uint32_t clampMonitorDimension(int value, bool makeEven)
{
  int v = std::max(static_cast<int>(DISPLAY_CONTROL_MIN_MONITOR_WIDTH),
                   std::min(value, static_cast<int>(DISPLAY_CONTROL_MAX_MONITOR_WIDTH)));
  if (makeEven && (v & 1) != 0) {
    v = (v > static_cast<int>(DISPLAY_CONTROL_MIN_MONITOR_WIDTH)) ? (v - 1) : (v + 1);
  }
  return static_cast<uint32_t>(v);
}

} // namespace

uint16_t KeyMap::keysymToRdpScancode(uint32_t keysym)
{
  for (const KeysymMapEntry& entry : kKeysymTable) {
    if (entry.keysym == keysym)
      return entry.rdpScancode;
  }
  return 0;
}

KeyAction KeyMap::translateKey(uint32_t keysym, uint32_t keycode, bool down)
{
  KeyAction action;
  uint16_t dirFlag = down ? KBD_FLAGS_DOWN : KBD_FLAGS_RELEASE;

  if (keycode != 0) {
    // TigerVNC QEMU Extended Key Event: 0xc6 encodes Pause/Break.
    if (keycode == 0xC6U) {
      action.kind = down ? KeyAction::Kind::Pause : KeyAction::Kind::None;
      return action;
    }

    action.kind = KeyAction::Kind::Scancode;
    action.code = static_cast<uint16_t>(keycode & 0x7FU);
    action.flags = dirFlag;
    if ((keycode & 0x80U) != 0)
      action.flags |= KBD_FLAGS_EXTENDED;
    return action;
  }

  // Legacy path (keycode == 0): special Pause/Break keysyms first.
  if (keysym == 0xFF13 || keysym == 0xFF6B) {
    action.kind = down ? KeyAction::Kind::Pause : KeyAction::Kind::None;
    return action;
  }

  // Non-printable / control / modifier keysym lookup table.
  uint16_t sc = keysymToRdpScancode(keysym);
  if (sc != 0) {
    action.kind = KeyAction::Kind::Scancode;
    action.code = static_cast<uint16_t>(sc & 0xFFU);
    action.flags = dirFlag;
    if ((sc & 0x0100U) != 0)
      action.flags |= KBD_FLAGS_EXTENDED;
    return action;
  }

  // Printable character fallback via Unicode keyboard event.
  uint32_t cp = keysymToCodepoint(keysym);
  if (cp != 0) {
    action.kind = KeyAction::Kind::Unicode;
    action.flags = dirFlag;
    if (cp <= 0xFFFFU) {
      action.code = static_cast<uint16_t>(cp);
    } else {
      uint32_t adjusted = cp - 0x10000U;
      action.code = static_cast<uint16_t>(0xD800U + (adjusted >> 10));
      action.code2 = static_cast<uint16_t>(0xDC00U + (adjusted & 0x3FFU));
    }
    return action;
  }

  return action;
}

std::vector<PointerAction> KeyMap::translatePointer(int x, int y,
                                                    uint16_t buttonMask,
                                                    uint16_t prevButtonMask,
                                                    int fbWidth, int fbHeight)
{
  std::vector<PointerAction> actions;
  uint16_t cx = clampCoord(x, fbWidth);
  uint16_t cy = clampCoord(y, fbHeight);

  // VNC buttons 1..3 (bits 0..2): 1=Left, 2=Middle->BUTTON3, 3=Right->BUTTON2.
  struct StdButtonMap {
    uint16_t vncBit;
    uint16_t rdpFlag;
  };
  static constexpr StdButtonMap kStdButtons[] = {
    { 0x01U, PTR_FLAGS_BUTTON1 },
    { 0x02U, PTR_FLAGS_BUTTON3 },
    { 0x04U, PTR_FLAGS_BUTTON2 },
  };

  bool emittedStdButton = false;
  for (const StdButtonMap& btn : kStdButtons) {
    bool cur = (buttonMask & btn.vncBit) != 0;
    bool prev = (prevButtonMask & btn.vncBit) != 0;
    if (cur != prev) {
      PointerAction act;
      act.isExtended = false;
      act.flags = btn.rdpFlag | (cur ? PTR_FLAGS_DOWN : 0);
      act.x = cx;
      act.y = cy;
      actions.push_back(act);
      emittedStdButton = true;
    }
  }

  // Scroll wheels (bits 3..6) emit only on button press (rising edge).
  bool emittedWheel = false;
  if ((buttonMask & 0x08U) && !(prevButtonMask & 0x08U)) {
    PointerAction act;
    act.flags = PTR_FLAGS_WHEEL | 0x0078U;
    act.x = cx;
    act.y = cy;
    actions.push_back(act);
    emittedWheel = true;
  }
  if ((buttonMask & 0x10U) && !(prevButtonMask & 0x10U)) {
    PointerAction act;
    act.flags = PTR_FLAGS_WHEEL | PTR_FLAGS_WHEEL_NEGATIVE | 0x0088U;
    act.x = cx;
    act.y = cy;
    actions.push_back(act);
    emittedWheel = true;
  }
  if ((buttonMask & 0x20U) && !(prevButtonMask & 0x20U)) {
    PointerAction act;
    act.flags = PTR_FLAGS_HWHEEL | PTR_FLAGS_WHEEL_NEGATIVE | 0x0088U;
    act.x = cx;
    act.y = cy;
    actions.push_back(act);
    emittedWheel = true;
  }
  if ((buttonMask & 0x40U) && !(prevButtonMask & 0x40U)) {
    PointerAction act;
    act.flags = PTR_FLAGS_HWHEEL | 0x0078U;
    act.x = cx;
    act.y = cy;
    actions.push_back(act);
    emittedWheel = true;
  }

  // Extended buttons 8 (bit 7 = X1) and 9 (bit 8 = X2).
  struct ExtButtonMap {
    uint16_t vncBit;
    uint16_t rdpXFlag;
  };
  static constexpr ExtButtonMap kExtButtons[] = {
    { 0x80U, PTR_XFLAGS_BUTTON1 },
    { 0x100U, PTR_XFLAGS_BUTTON2 },
  };
  bool emittedExt = false;
  for (const ExtButtonMap& btn : kExtButtons) {
    bool cur = (buttonMask & btn.vncBit) != 0;
    bool prev = (prevButtonMask & btn.vncBit) != 0;
    if (cur != prev) {
      PointerAction act;
      act.isExtended = true;
      act.flags = btn.rdpXFlag | (cur ? PTR_XFLAGS_DOWN : 0);
      act.x = cx;
      act.y = cy;
      actions.push_back(act);
      emittedExt = true;
    }
  }

  // Pure pointer movement when no button/wheel/xbutton delta occurred.
  if (!emittedStdButton && !emittedWheel && !emittedExt) {
    PointerAction act;
    act.isExtended = false;
    act.flags = PTR_FLAGS_MOVE;
    act.x = cx;
    act.y = cy;
    actions.push_back(act);
  }

  return actions;
}

std::vector<uint8_t> KeyMap::utf8ToUtf16LeCrlf(const std::string& utf8)
{
  std::vector<uint8_t> out;
  out.reserve((utf8.size() + 1) * 2);

  size_t i = 0;
  while (i < utf8.size()) {
    uint32_t cp = decodeUtf8(utf8, i);
    if (cp == '\r') {
      if (i < utf8.size() && utf8[i] == '\n') {
        ++i;
      }
      appendUtf16LeUnit(out, '\r');
      appendUtf16LeUnit(out, '\n');
    } else if (cp == '\n') {
      appendUtf16LeUnit(out, '\r');
      appendUtf16LeUnit(out, '\n');
    } else if (cp <= 0xFFFFU) {
      appendUtf16LeUnit(out, static_cast<uint16_t>(cp));
    } else {
      uint32_t adj = cp - 0x10000U;
      appendUtf16LeUnit(out, static_cast<uint16_t>(0xD800U + (adj >> 10)));
      appendUtf16LeUnit(out, static_cast<uint16_t>(0xDC00U + (adj & 0x3FFU)));
    }
  }

  // Windows CF_UNICODETEXT requires a trailing UTF-16 NUL terminator.
  appendUtf16LeUnit(out, 0x0000);
  return out;
}

std::string KeyMap::utf16LeToUtf8Lf(const uint8_t* data, size_t byteLen)
{
  std::string out;
  if (!data || byteLen < 2)
    return out;

  size_t unitCount = byteLen / 2;
  out.reserve(unitCount);

  for (size_t i = 0; i < unitCount; ++i) {
    uint16_t u1 = static_cast<uint16_t>(data[i * 2] | (data[i * 2 + 1] << 8));
    if (u1 == 0x0000)
      break;

    if (u1 == '\r') {
      if (i + 1 < unitCount) {
        uint16_t u2 = static_cast<uint16_t>(data[(i + 1) * 2] |
                                            (data[(i + 1) * 2 + 1] << 8));
        if (u2 == '\n')
          ++i;
      }
      out.push_back('\n');
      continue;
    }

    uint32_t cp = u1;
    if (u1 >= 0xD800U && u1 <= 0xDBFFU) {
      if (i + 1 < unitCount) {
        uint16_t u2 = static_cast<uint16_t>(data[(i + 1) * 2] |
                                            (data[(i + 1) * 2 + 1] << 8));
        if (u2 >= 0xDC00U && u2 <= 0xDFFFU) {
          cp = 0x10000U + ((static_cast<uint32_t>(u1 - 0xD800U) << 10) |
                           static_cast<uint32_t>(u2 - 0xDC00U));
          ++i;
        } else {
          cp = 0xFFFD;
        }
      } else {
        cp = 0xFFFD;
      }
    } else if (u1 >= 0xDC00U && u1 <= 0xDFFFU) {
      cp = 0xFFFD;
    }

    appendUtf8Codepoint(out, cp);
  }

  return out;
}

std::vector<DISPLAY_CONTROL_MONITOR_LAYOUT> KeyMap::buildMonitorLayout(
    int fbWidth, int fbHeight, const rfb::ScreenSet& layout)
{
  std::vector<DISPLAY_CONTROL_MONITOR_LAYOUT> monitors;

  if (layout.num_screens() > 0) {
    int idx = 0;
    for (rfb::ScreenSet::const_iterator it = layout.begin();
         it != layout.end(); ++it, ++idx) {
      DISPLAY_CONTROL_MONITOR_LAYOUT m{};
      m.Flags = (idx == 0) ? DISPLAY_CONTROL_MONITOR_PRIMARY : 0;
      m.Left = it->dimensions.tl.x;
      m.Top = it->dimensions.tl.y;
      m.Width = clampMonitorDimension(it->dimensions.width(), true);
      m.Height = clampMonitorDimension(it->dimensions.height(), false);
      m.Orientation = kOrientationLandscape;
      m.DesktopScaleFactor = 100;
      m.DeviceScaleFactor = 100;
      monitors.push_back(m);
    }
  }

  if (monitors.empty()) {
    DISPLAY_CONTROL_MONITOR_LAYOUT m{};
    m.Flags = DISPLAY_CONTROL_MONITOR_PRIMARY;
    m.Left = 0;
    m.Top = 0;
    m.Width = clampMonitorDimension(fbWidth, true);
    m.Height = clampMonitorDimension(fbHeight, false);
    m.Orientation = kOrientationLandscape;
    m.DesktopScaleFactor = 100;
    m.DeviceScaleFactor = 100;
    monitors.push_back(m);
  }

  return monitors;
}

} // namespace rdpvnc
