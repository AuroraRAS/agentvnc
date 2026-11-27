/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright 2009-2019 Pierre Ossman for Cendio AB
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

#include <stdio.h>

#include <vector>

#include <core/Configuration.h>
#include <core/LogWriter.h>
#include <core/i18n.h>
#include <core/string.h>

#include <rdr/InStream.h>
#include <rdr/ZlibInStream.h>

#include <rfb/msgTypes.h>
#include <rfb/qemuTypes.h>
#include <rfb/giiTypes.h>
#include <rfb/clipboardTypes.h>
#include <rfb/Exception.h>
#include <rfb/PixelFormat.h>
#include <rfb/ScreenSet.h>
#include <rfb/SMsgHandler.h>
#include <rfb/SMsgReader.h>

using namespace rfb;

static core::LogWriter vlog("SMsgReader");

static core::IntParameter maxCutText(
  "MaxCutText",
  _("Maximum permitted length of an incoming clipboard update"),
  256*1024, 0, INT_MAX);

SMsgReader::SMsgReader(SMsgHandler* handler_, rdr::InStream* is_)
  : handler(handler_), is(is_), state(MSGSTATE_IDLE)
{
}

SMsgReader::~SMsgReader()
{
}

bool SMsgReader::readClientInit()
{
  if (!is->hasData(1))
    return false;
  bool shared = is->readU8();
  handler->clientInit(shared);
  return true;
}

bool SMsgReader::readMsg()
{
  bool ret;

  if (state == MSGSTATE_IDLE) {
    if (!is->hasData(1))
      return false;

    currentMsgType = is->readU8();
    state = MSGSTATE_MESSAGE;
  }

  switch (currentMsgType) {
  case msgTypeSetPixelFormat:
    ret = readSetPixelFormat();
    break;
  case msgTypeSetEncodings:
    ret = readSetEncodings();
    break;
  case msgTypeSetDesktopSize:
    ret = readSetDesktopSize();
    break;
  case msgTypeFramebufferUpdateRequest:
    ret = readFramebufferUpdateRequest();
    break;
  case msgTypeEnableContinuousUpdates:
    ret = readEnableContinuousUpdates();
    break;
  case msgTypeClientFence:
    ret = readFence();
    break;
  case msgTypeKeyEvent:
    ret = readKeyEvent();
    break;
  case msgTypePointerEvent:
    ret = readPointerEvent();
    break;
  case msgTypeClientCutText:
    ret = readClientCutText();
    break;
  case msgTypeQEMUClientMessage:
    ret = readQEMUMessage();
    break;
  case msgTypeGIIClientMessage:
    ret = readGIIClientMessage();
    break;
  default:
    vlog.error(_("Unknown message type %d"), currentMsgType);
    throw protocol_error(_("Unknown message type"));
  }

  if (ret)
    state = MSGSTATE_IDLE;

  return ret;
}

bool SMsgReader::readSetPixelFormat()
{
  if (!is->hasData(3 + 16))
    return false;
  is->skip(3);
  PixelFormat pf;
  pf.read(is);
  handler->setPixelFormat(pf);
  return true;
}

bool SMsgReader::readSetEncodings()
{
  if (!is->hasData(1 + 2))
    return false;

  is->setRestorePoint();

  is->skip(1);

  int nEncodings = is->readU16();

  if (!is->hasDataOrRestore(nEncodings * 4))
    return false;
  is->clearRestorePoint();

  std::vector<int32_t> encodings(nEncodings);
  for (size_t i = 0; i < encodings.size(); i++)
    encodings[i] = is->readU32();

  handler->setEncodings(nEncodings, encodings.data());

  return true;
}

bool SMsgReader::readSetDesktopSize()
{
  int width, height;
  int screens, i;
  uint32_t id, flags;
  int sx, sy, sw, sh;
  ScreenSet layout;

  if (!is->hasData(1 + 2 + 2 + 1 + 1))
    return false;

  is->setRestorePoint();

  is->skip(1);

  width = is->readU16();
  height = is->readU16();

  screens = is->readU8();
  is->skip(1);

  if (!is->hasDataOrRestore(screens * (4 + 2 + 2 + 2 + 2 + 4)))
    return false;
  is->clearRestorePoint();

  for (i = 0;i < screens;i++) {
    id = is->readU32();
    sx = is->readU16();
    sy = is->readU16();
    sw = is->readU16();
    sh = is->readU16();
    flags = is->readU32();

    layout.add_screen(Screen(id, sx, sy, sw, sh, flags));
  }

  handler->setDesktopSize(width, height, layout);

  return true;
}

bool SMsgReader::readFramebufferUpdateRequest()
{
  if (!is->hasData(1 + 2 + 2 + 2 + 2))
    return false;
  bool inc = is->readU8();
  int x = is->readU16();
  int y = is->readU16();
  int w = is->readU16();
  int h = is->readU16();
  handler->framebufferUpdateRequest({x, y, x+w, y+h}, inc);
  return true;
}

bool SMsgReader::readEnableContinuousUpdates()
{
  bool enable;
  int x, y, w, h;

  if (!is->hasData(1 + 2 + 2 + 2 + 2))
    return false;

  enable = is->readU8();

  x = is->readU16();
  y = is->readU16();
  w = is->readU16();
  h = is->readU16();

  handler->enableContinuousUpdates(enable, x, y, w, h);

  return true;
}

bool SMsgReader::readFence()
{
  uint32_t flags;
  uint8_t len;
  uint8_t data[64];

  if (!is->hasData(3 + 4 + 1))
    return false;

  is->setRestorePoint();

  is->skip(3);

  flags = is->readU32();

  len = is->readU8();

  if (!is->hasDataOrRestore(len))
    return false;
  is->clearRestorePoint();

  if (len > sizeof(data)) {
    vlog.error(_("Ignoring fence with too large payload"));
    is->skip(len);
    return true;
  }

  is->readBytes(data, len);
  
  handler->fence(flags, len, data);

  return true;
}

bool SMsgReader::readKeyEvent()
{
  if (!is->hasData(1 + 2 + 4))
    return false;
  bool down = is->readU8();
  is->skip(2);
  uint32_t key = is->readU32();
  handler->keyEvent(key, 0, down);
  return true;
}

bool SMsgReader::readPointerEvent()
{
  int mask;
  int x;
  int y;

  if (!is->hasData(1 + 2 + 2))
    return false;

  is->setRestorePoint();

  mask = is->readU8();
  x = is->readU16();
  y = is->readU16();

  if (handler->client.supportsExtendedMouseButtons() && mask & 0x80 ) {
    int highBits;
    int lowBits;

    if (!is->hasDataOrRestore(1))
      return false;

    highBits = is->readU8();
    lowBits = mask & 0x7f; /* Clear marker bit */
    mask = (highBits << 7) | lowBits;
  }

  is->clearRestorePoint();
  handler->pointerEvent({x, y}, mask);
  return true;
}


bool SMsgReader::readClientCutText()
{
  if (!is->hasData(3 + 4))
    return false;

  is->setRestorePoint();

  is->skip(3);
  uint32_t len = is->readU32();

  if (len & 0x80000000) {
    int32_t slen = len;
    slen = -slen;
    if (readExtendedClipboard(slen)) {
      is->clearRestorePoint();
      return true;
    } else {
      is->gotoRestorePoint();
      return false;
    }
  }

  if (!is->hasDataOrRestore(len))
    return false;
  is->clearRestorePoint();

  if (len > (size_t)maxCutText) {
    is->skip(len);
    vlog.error(_("Clipboard too large (%d bytes)"), len);
    return true;
  }

  std::vector<char> ca(len);
  is->readBytes((uint8_t*)ca.data(), len);

  std::string utf8(core::latin1ToUTF8(ca.data(), ca.size()));
  std::string filtered(core::convertLF(utf8.data(), utf8.size()));

  handler->clientCutText(filtered.c_str());

  return true;
}

bool SMsgReader::readExtendedClipboard(int32_t len)
{
  uint32_t flags;
  uint32_t action;

  if (!is->hasData(len))
    return false;

  if (len < 4)
    throw protocol_error(_("Invalid extended clipboard message"));
  if (len > maxCutText) {
    vlog.error(_("Clipboard too large (%d bytes)"), len);
    is->skip(len);
    return true;
  }

  flags = is->readU32();
  action = flags & clipboardActionMask;

  if (action & clipboardCaps) {
    int i;
    size_t num;
    uint32_t lengths[16];

    num = 0;
    for (i = 0;i < 16;i++) {
      if (flags & (1 << i))
        num++;
    }

    if (len < (int32_t)(4 + 4*num))
      throw protocol_error(_("Invalid extended clipboard message"));

    num = 0;
    for (i = 0;i < 16;i++) {
      if (flags & (1 << i))
        lengths[num++] = is->readU32();
    }

    handler->clipboardCaps(flags, lengths);
  } else if (action == clipboardProvide) {
    rdr::ZlibInStream zis;

    int i;
    size_t num;
    size_t lengths[16];
    std::vector<uint8_t> storage[16];
    uint8_t* buffers[16];

    zis.setUnderlying(is, len - 4);

    num = 0;
    for (i = 0;i < 16;i++) {
      if (!(flags & 1 << i))
        continue;

      if (!zis.hasData(4))
        throw protocol_error(_("Invalid extended clipboard message"));

      lengths[num] = zis.readU32();

      if (lengths[num] > (size_t)maxCutText) {
        vlog.error(_("Clipboard too large (%d bytes)"),
                   (unsigned)lengths[num]);

        // Slowly (safely) drain away the data
        while (lengths[num] > 0) {
          size_t chunk;

          if (!zis.hasData(1))
            throw protocol_error(_("Invalid extended clipboard message"));

          chunk = zis.avail();
          if (chunk > lengths[num])
            chunk = lengths[num];

          zis.skip(chunk);
          lengths[num] -= chunk;
        }

        flags &= ~(1 << i);

        continue;
      }

      if (!zis.hasData(lengths[num]))
        throw protocol_error(_("Invalid extended clipboard message"));

      storage[num].resize(lengths[num]);
      buffers[num] = storage[num].data();
      zis.readBytes(buffers[num], lengths[num]);
      num++;
    }

    zis.flushUnderlying();
    zis.setUnderlying(nullptr, 0);

    handler->clipboardProvide(flags, lengths, buffers);
  } else {
    switch (action) {
    case clipboardRequest:
      handler->clipboardRequest(flags);
      break;
    case clipboardPeek:
      handler->clipboardPeek();
      break;
    case clipboardNotify:
      handler->clipboardNotify(flags);
      break;
    default:
      throw protocol_error(_("Invalid extended clipboard message"));
    }
  }

  return true;
}

bool SMsgReader::readQEMUMessage()
{
  int subType;
  bool ret;

  if (!is->hasData(1))
    return false;

  is->setRestorePoint();

  subType = is->readU8();

  switch (subType) {
  case qemuExtendedKeyEvent:
    ret = readQEMUKeyEvent();
    break;
  default:
    throw protocol_error(
      core::format(_("Unknown QEMU submessage type %d"), subType));
  }

  if (!ret) {
    is->gotoRestorePoint();
    return false;
  } else {
    is->clearRestorePoint();
    return true;
  }
}

bool SMsgReader::readQEMUKeyEvent()
{
  if (!is->hasData(2 + 4 + 4))
    return false;
  bool down = is->readU16();
  uint32_t keysym = is->readU32();
  uint32_t keycode = is->readU32();
  if (!keycode) {
    vlog.error(_("Key event without keycode"));
    return true;
  }
  handler->keyEvent(keysym, keycode, down);
  return true;
}

static uint16_t readEU16(rdr::InStream* is, bool bigEndian)
{
  uint8_t b0 = is->readU8();
  uint8_t b1 = is->readU8();
  if (bigEndian)
    return ((uint16_t)b0 << 8) | (uint16_t)b1;
  else
    return ((uint16_t)b1 << 8) | (uint16_t)b0;
}

static uint32_t readEU32(rdr::InStream* is, bool bigEndian)
{
  uint8_t b0 = is->readU8();
  uint8_t b1 = is->readU8();
  uint8_t b2 = is->readU8();
  uint8_t b3 = is->readU8();
  if (bigEndian)
    return ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) |
           ((uint32_t)b2 << 8) | (uint32_t)b3;
  else
    return ((uint32_t)b3 << 24) | ((uint32_t)b2 << 16) |
           ((uint32_t)b1 << 8) | (uint32_t)b0;
}

static int32_t readES32(rdr::InStream* is, bool bigEndian)
{
  return (int32_t)readEU32(is, bigEndian);
}

static std::string readFixedString(rdr::InStream* is, size_t fieldLen)
{
  std::vector<char> buf(fieldLen);
  is->readBytes((uint8_t*)buf.data(), fieldLen);
  size_t len = 0;
  while (len < fieldLen - 1 && buf[len] != '\0')
    len++;
  return std::string(buf.data(), len);
}

bool SMsgReader::readGIIClientMessage()
{
  uint8_t endianAndSubType;
  bool bigEndian;
  uint8_t subType;
  uint16_t length;

  if (!is->hasData(1 + 2))
    return false;

  is->setRestorePoint();

  endianAndSubType = is->readU8();
  bigEndian = (endianAndSubType & giiBigEndian) != 0;
  subType = endianAndSubType & giiSubTypeMask;

  length = readEU16(is, bigEndian);

  if (!is->hasDataOrRestore(length))
    return false;

  is->clearRestorePoint();

  switch (subType) {
  case giiClientVersion:
    if (length != 2)
      throw protocol_error(_("Invalid GII version message"));
    handler->giiVersion(readEU16(is, bigEndian));
    break;

  case giiClientDeviceCreate: {
    if (length < giiDeviceCreateFixedSize)
      throw protocol_error(_("Invalid GII device creation message"));

    GIIDevice device;
    device.name = readFixedString(is, giiDeviceNameLen);
    device.vendorID = readEU32(is, bigEndian);
    device.productID = readEU32(is, bigEndian);
    device.canGenerate = readEU32(is, bigEndian);
    device.numRegisters = readEU32(is, bigEndian);
    uint32_t numValuators = readEU32(is, bigEndian);
    device.numButtons = readEU32(is, bigEndian);

    if ((numValuators > giiMaxValuatorsPerDevice) ||
        (length != giiDeviceCreateFixedSize +
                   numValuators * giiValuatorWireSize))
      throw protocol_error(_("Invalid GII device creation message"));

    device.valuators.resize(numValuators);
    for (uint32_t i = 0; i < numValuators; i++) {
      GIIValuator& val = device.valuators[i];
      val.index = readEU32(is, bigEndian);
      val.longName = readFixedString(is, giiValuatorLongNameLen);
      val.shortName = readFixedString(is, giiValuatorShortNameLen);
      val.rangeMin = readES32(is, bigEndian);
      val.rangeCenter = readES32(is, bigEndian);
      val.rangeMax = readES32(is, bigEndian);
      val.siUnit = readEU32(is, bigEndian);
      val.siAdd = readES32(is, bigEndian);
      val.siMul = readES32(is, bigEndian);
      val.siDiv = readES32(is, bigEndian);
      val.siShift = readES32(is, bigEndian);
    }

    handler->giiDeviceCreate(device);
    break;
  }

  case giiClientDeviceDestroy:
    if (length != 4)
      throw protocol_error(_("Invalid GII device destruction message"));
    handler->giiDeviceDestroy(readEU32(is, bigEndian));
    break;

  case giiClientEvent: {
    size_t remaining = length;
    std::vector<GIIEvent> events;

    while (remaining > 0) {
      if (remaining < 2)
        throw protocol_error(_("Invalid GII event message"));

      uint8_t eventSize = is->readU8();
      uint8_t eventType = is->readU8();

      if ((eventSize < 8) || (eventSize > remaining))
        throw protocol_error(_("Invalid GII event size"));

      switch (eventType) {
      case giiEventKeyPress:
      case giiEventKeyRelease:
      case giiEventKeyRepeat: {
        if (eventSize != giiKeyEventSize)
          throw protocol_error(_("Invalid GII key event size"));
        is->skip(2);
        GIIEvent ev;
        ev.type = eventType;
        ev.origin = readEU32(is, bigEndian);
        ev.modifiers = readEU32(is, bigEndian);
        ev.symbol = readEU32(is, bigEndian);
        ev.label = readEU32(is, bigEndian);
        ev.button = readEU32(is, bigEndian);
        events.push_back(ev);
        break;
      }

      case giiEventPtrRelative:
      case giiEventPtrAbsolute: {
        if (eventSize != giiPtrMoveEventSize)
          throw protocol_error(_("Invalid GII pointer move event size"));
        is->skip(2);
        GIIEvent ev;
        ev.type = eventType;
        ev.origin = readEU32(is, bigEndian);
        ev.x = readES32(is, bigEndian);
        ev.y = readES32(is, bigEndian);
        ev.z = readES32(is, bigEndian);
        ev.wheel = readES32(is, bigEndian);
        events.push_back(ev);
        break;
      }

      case giiEventPtrButtonPress:
      case giiEventPtrButtonRelease: {
        if (eventSize != giiPtrButtonEventSize)
          throw protocol_error(_("Invalid GII pointer button event size"));
        is->skip(2);
        GIIEvent ev;
        ev.type = eventType;
        ev.origin = readEU32(is, bigEndian);
        ev.button = readEU32(is, bigEndian);
        events.push_back(ev);
        break;
      }

      case giiEventValuatorRelative:
      case giiEventValuatorAbsolute: {
        if ((eventSize < giiValuatorEventHeaderSize) ||
            (((eventSize - giiValuatorEventHeaderSize) % 4) != 0))
          throw protocol_error(_("Invalid GII valuator event size"));
        is->skip(2);
        GIIEvent ev;
        ev.type = eventType;
        ev.origin = readEU32(is, bigEndian);
        ev.first = readEU32(is, bigEndian);
        uint32_t count = readEU32(is, bigEndian);
        if ((count > giiMaxValuatorsPerEvent) ||
            (eventSize != giiValuatorEventHeaderSize + count * 4))
          throw protocol_error(_("Invalid GII valuator count"));
        ev.values.resize(count);
        for (uint32_t i = 0; i < count; i++)
          ev.values[i] = readES32(is, bigEndian);
        events.push_back(ev);
        break;
      }

      default:
        vlog.debug("Ignoring unknown GII event type %d (size %d)",
                   (int)eventType, (int)eventSize);
        is->skip(eventSize - 2);
        break;
      }

      remaining -= eventSize;
    }

    for (const GIIEvent& ev : events)
      handler->giiEvent(ev);
    break;
  }

  default:
    vlog.error(_("Unknown GII client submessage type %d"), (int)subType);
    is->skip(length);
    break;
  }

  return true;
}
