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
#ifndef __RFB_GIITYPES_H__
#define __RFB_GIITYPES_H__

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace rfb {

  // Header flags & protocol versions
  const uint8_t giiBigEndian = 0x80;
  const uint8_t giiSubTypeMask = 0x7f;

  const uint16_t giiVersionMin = 1;
  const uint16_t giiVersionMax = 1;

  // Client-to-server sub-types
  const uint8_t giiClientEvent = 0;
  const uint8_t giiClientVersion = 1;
  const uint8_t giiClientDeviceCreate = 2;
  const uint8_t giiClientDeviceDestroy = 3;

  // Server-to-client sub-types
  const uint8_t giiServerVersion = 1;
  const uint8_t giiServerDeviceCreateResponse = 2;

  // Event types
  const uint8_t giiEventKeyPress = 5;
  const uint8_t giiEventKeyRelease = 6;
  const uint8_t giiEventKeyRepeat = 7;
  const uint8_t giiEventPtrRelative = 8;
  const uint8_t giiEventPtrAbsolute = 9;
  const uint8_t giiEventPtrButtonPress = 10;
  const uint8_t giiEventPtrButtonRelease = 11;
  const uint8_t giiEventValuatorRelative = 12;
  const uint8_t giiEventValuatorAbsolute = 13;

  // Event capability masks
  const uint32_t giiEventMaskKeyPress = 1U << giiEventKeyPress;
  const uint32_t giiEventMaskKeyRelease = 1U << giiEventKeyRelease;
  const uint32_t giiEventMaskKeyRepeat = 1U << giiEventKeyRepeat;
  const uint32_t giiEventMaskPtrRelative = 1U << giiEventPtrRelative;
  const uint32_t giiEventMaskPtrAbsolute = 1U << giiEventPtrAbsolute;
  const uint32_t giiEventMaskPtrButtonPress = 1U << giiEventPtrButtonPress;
  const uint32_t giiEventMaskPtrButtonRelease = 1U << giiEventPtrButtonRelease;
  const uint32_t giiEventMaskValuatorRelative = 1U << giiEventValuatorRelative;
  const uint32_t giiEventMaskValuatorAbsolute = 1U << giiEventValuatorAbsolute;

  const uint32_t giiEventMaskKey =
    giiEventMaskKeyPress | giiEventMaskKeyRelease | giiEventMaskKeyRepeat;
  const uint32_t giiEventMaskPointer =
    giiEventMaskPtrRelative | giiEventMaskPtrAbsolute |
    giiEventMaskPtrButtonPress | giiEventMaskPtrButtonRelease |
    giiEventMaskValuatorRelative | giiEventMaskValuatorAbsolute;
  const uint32_t giiEventMaskAll = giiEventMaskKey | giiEventMaskPointer;

  // Valuator SI units
  const uint32_t giiUnitUnknown = 0;
  const uint32_t giiUnitTime = 1;
  const uint32_t giiUnitFreq = 2;
  const uint32_t giiUnitLength = 3;
  const uint32_t giiUnitVelocity = 4;
  const uint32_t giiUnitAccel = 5;
  const uint32_t giiUnitAngle = 6;
  const uint32_t giiUnitAngVel = 7;
  const uint32_t giiUnitAngAccel = 8;
  const uint32_t giiUnitArea = 9;
  const uint32_t giiUnitVolume = 10;
  const uint32_t giiUnitMass = 11;
  const uint32_t giiUnitForce = 12;
  const uint32_t giiUnitPressure = 13;
  const uint32_t giiUnitTorque = 14;
  const uint32_t giiUnitEnergy = 15;
  const uint32_t giiUnitPower = 16;
  const uint32_t giiUnitTemp = 17;
  const uint32_t giiUnitCurrent = 18;
  const uint32_t giiUnitVoltage = 19;
  const uint32_t giiUnitResistance = 20;
  const uint32_t giiUnitCapacitance = 21;
  const uint32_t giiUnitInductance = 22;

  // Wire sizes and bounds
  const size_t giiDeviceNameLen = 32;
  const size_t giiValuatorLongNameLen = 75;
  const size_t giiValuatorShortNameLen = 5;
  const size_t giiDeviceCreateFixedSize = 56;
  const size_t giiValuatorWireSize = 116;

  const size_t giiKeyEventSize = 24;
  const size_t giiPtrMoveEventSize = 24;
  const size_t giiPtrButtonEventSize = 12;
  const size_t giiValuatorEventHeaderSize = 16;

  const uint32_t giiMaxValuatorsPerEvent = 59;
  const uint32_t giiMaxValuatorsPerDevice = 564;

  struct GIIValuator {
    GIIValuator()
      : index(0), rangeMin(0), rangeCenter(0), rangeMax(0),
        siUnit(giiUnitUnknown), siAdd(0), siMul(0), siDiv(0), siShift(0)
    {}

    uint32_t index;
    std::string longName;
    std::string shortName;
    int32_t rangeMin;
    int32_t rangeCenter;
    int32_t rangeMax;
    uint32_t siUnit;
    int32_t siAdd;
    int32_t siMul;
    int32_t siDiv;
    int32_t siShift;
  };

  struct GIIDevice {
    GIIDevice()
      : vendorID(0), productID(0), canGenerate(0),
        numRegisters(0), numButtons(0)
    {}

    std::string name;
    uint32_t vendorID;
    uint32_t productID;
    uint32_t canGenerate;
    uint32_t numRegisters;
    uint32_t numButtons;
    std::vector<GIIValuator> valuators;
  };

  struct GIIEvent {
    GIIEvent()
      : type(0), origin(0),
        modifiers(0), symbol(0), label(0), button(0),
        x(0), y(0), z(0), wheel(0),
        first(0)
    {}

    uint8_t type;
    uint32_t origin;

    // Key events (press / release / repeat)
    uint32_t modifiers;
    uint32_t symbol;
    uint32_t label;
    // Also used as button-number for pointer button events
    uint32_t button;

    // Pointer move events (relative / absolute)
    int32_t x;
    int32_t y;
    int32_t z;
    int32_t wheel;

    // Valuator events (relative / absolute)
    uint32_t first;
    std::vector<int32_t> values;
  };

  inline bool giiIsKeyEvent(uint8_t type)
  {
    return (type == giiEventKeyPress) ||
           (type == giiEventKeyRelease) ||
           (type == giiEventKeyRepeat);
  }

  inline bool giiIsPointerEvent(uint8_t type)
  {
    return (type == giiEventPtrRelative) ||
           (type == giiEventPtrAbsolute) ||
           (type == giiEventPtrButtonPress) ||
           (type == giiEventPtrButtonRelease) ||
           (type == giiEventValuatorRelative) ||
           (type == giiEventValuatorAbsolute);
  }

}

#endif
