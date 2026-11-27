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
#include <assert.h>

#include <core/Rect.h>
#include <core/string.h>

#include <rdr/OutStream.h>
#include <rdr/MemOutStream.h>
#include <rdr/ZlibOutStream.h>

#include <rfb/msgTypes.h>
#include <rfb/fenceTypes.h>
#include <rfb/qemuTypes.h>
#include <rfb/giiTypes.h>
#include <rfb/clipboardTypes.h>
#include <rfb/PixelFormat.h>
#include <rfb/ScreenSet.h>
#include <rfb/ServerParams.h>
#include <rfb/CMsgWriter.h>

using namespace rfb;

CMsgWriter::CMsgWriter(ServerParams* server_, rdr::OutStream* os_)
  : server(server_), os(os_)
{
}

CMsgWriter::~CMsgWriter()
{
}

void CMsgWriter::writeClientInit(bool shared)
{
  os->writeU8(shared);
  endMsg();
}

void CMsgWriter::writeSetPixelFormat(const PixelFormat& pf)
{
  startMsg(msgTypeSetPixelFormat);                                 
  os->pad(3);
  pf.write(os);
  endMsg();
}

void CMsgWriter::writeSetEncodings(const std::list<uint32_t> encodings)
{
  startMsg(msgTypeSetEncodings);
  os->pad(1);
  os->writeU16(encodings.size());
  for (uint32_t encoding : encodings)
    os->writeU32(encoding);
  endMsg();
}

void CMsgWriter::writeSetDesktopSize(int width, int height,
                                     const ScreenSet& layout)
{
  if (!server->supportsSetDesktopSize)
    throw std::logic_error("Server does not support SetDesktopSize");

  startMsg(msgTypeSetDesktopSize);
  os->pad(1);

  os->writeU16(width);
  os->writeU16(height);

  os->writeU8(layout.num_screens());
  os->pad(1);

  ScreenSet::const_iterator iter;
  for (iter = layout.begin();iter != layout.end();++iter) {
    os->writeU32(iter->id);
    os->writeU16(iter->dimensions.tl.x);
    os->writeU16(iter->dimensions.tl.y);
    os->writeU16(iter->dimensions.width());
    os->writeU16(iter->dimensions.height());
    os->writeU32(iter->flags);
  }

  endMsg();
}

void CMsgWriter::writeFramebufferUpdateRequest(const core::Rect& r,
                                               bool incremental)
{
  startMsg(msgTypeFramebufferUpdateRequest);
  os->writeU8(incremental);
  os->writeU16(r.tl.x);
  os->writeU16(r.tl.y);
  os->writeU16(r.width());
  os->writeU16(r.height());
  endMsg();
}

void CMsgWriter::writeEnableContinuousUpdates(bool enable,
                                              int x, int y, int w, int h)
{
  if (!server->supportsContinuousUpdates)
    throw std::logic_error("Server does not support continuous updates");

  startMsg(msgTypeEnableContinuousUpdates);

  os->writeU8(!!enable);

  os->writeU16(x);
  os->writeU16(y);
  os->writeU16(w);
  os->writeU16(h);

  endMsg();
}

void CMsgWriter::writeFence(uint32_t flags, unsigned len, const uint8_t data[])
{
  if (!server->supportsFence)
    throw std::logic_error("Server does not support fences");
  if (len > 64)
    throw std::out_of_range("Too large fence payload");
  if ((flags & ~fenceFlagsSupported) != 0)
    throw std::invalid_argument("Unknown fence flags");

  startMsg(msgTypeClientFence);
  os->pad(3);

  os->writeU32(flags);

  os->writeU8(len);
  os->writeBytes(data, len);

  endMsg();
}

void CMsgWriter::writeKeyEvent(uint32_t keysym, uint32_t keycode, bool down)
{
  if (!server->supportsQEMUKeyEvent || !keycode) {
    /* This event isn't meaningful without a valid keysym */
    if (!keysym)
      return;

    startMsg(msgTypeKeyEvent);
    os->writeU8(down);
    os->pad(2);
    os->writeU32(keysym);
    endMsg();
  } else {
    startMsg(msgTypeQEMUClientMessage);
    os->writeU8(qemuExtendedKeyEvent);
    os->writeU16(down);
    os->writeU32(keysym);
    os->writeU32(keycode);
    endMsg();
  }
}


void CMsgWriter::writePointerEvent(const core::Point& pos,
                                   uint16_t buttonMask)
{
  core::Point p(pos);
  bool extendedMouseButtons;

  if (p.x < 0) p.x = 0;
  if (p.y < 0) p.y = 0;
  if (p.x >= server->width()) p.x = server->width() - 1;
  if (p.y >= server->height()) p.y = server->height() - 1;

  /* The highest bit in buttonMask is never sent to the server */
  assert(!(buttonMask & 0x8000));

  /* Only send extended pointerEvent message when needed */
  extendedMouseButtons = buttonMask & 0x7f80;

  startMsg(msgTypePointerEvent);
  if (server->supportsExtendedMouseButtons && extendedMouseButtons) {
    int higherBits;
    int lowerBits;

    higherBits = (buttonMask  >> 7) & 0xff;
    assert(!(higherBits & 0xfc)); /* Bits 2-7 are reserved */

    lowerBits = buttonMask & 0x7f;
    lowerBits |= 0x80; /* Set marker bit to 1 */

    os->writeU8(lowerBits);
    os->writeU16(p.x);
    os->writeU16(p.y);
    os->writeU8(higherBits);
  } else {
    /* Marker bit must be set to 0, otherwise the server might confuse
     * the marker bit with the highest bit in a normal PointerEvent
     * message.
    */
    buttonMask &= 0x7f;
    os->writeU8(buttonMask);
    os->writeU16(p.x);
    os->writeU16(p.y);
  }
  endMsg();
}


void CMsgWriter::writeClientCutText(const char* str)
{
  if (strchr(str, '\r') != nullptr)
    throw std::invalid_argument("Invalid carriage return in clipboard data");

  std::string latin1(core::utf8ToLatin1(str));

  startMsg(msgTypeClientCutText);
  os->pad(3);
  os->writeU32(latin1.size());
  os->writeBytes((const uint8_t*)latin1.data(), latin1.size());
  endMsg();
}

void CMsgWriter::writeClipboardCaps(uint32_t caps,
                                    const uint32_t* lengths)
{
  size_t i, count;

  if (!(server->clipboardFlags() & clipboardCaps))
    throw std::logic_error("Server does not support clipboard \"caps\" action");

  count = 0;
  for (i = 0;i < 16;i++) {
    if (caps & (1 << i))
      count++;
  }

  startMsg(msgTypeClientCutText);
  os->pad(3);
  os->writeS32(-(4 + 4 * count));

  os->writeU32(caps | clipboardCaps);

  count = 0;
  for (i = 0;i < 16;i++) {
    if (caps & (1 << i))
      os->writeU32(lengths[count++]);
  }

  endMsg();
}

void CMsgWriter::writeClipboardRequest(uint32_t flags)
{
  if (!(server->clipboardFlags() & clipboardRequest))
    throw std::logic_error("Server does not support clipboard \"request\" action");

  startMsg(msgTypeClientCutText);
  os->pad(3);
  os->writeS32(-4);
  os->writeU32(flags | clipboardRequest);
  endMsg();
}

void CMsgWriter::writeClipboardPeek(uint32_t flags)
{
  if (!(server->clipboardFlags() & clipboardPeek))
    throw std::logic_error("Server does not support clipboard \"peek\" action");

  startMsg(msgTypeClientCutText);
  os->pad(3);
  os->writeS32(-4);
  os->writeU32(flags | clipboardPeek);
  endMsg();
}

void CMsgWriter::writeClipboardNotify(uint32_t flags)
{
  if (!(server->clipboardFlags() & clipboardNotify))
    throw std::logic_error("Server does not support clipboard \"notify\" action");

  startMsg(msgTypeClientCutText);
  os->pad(3);
  os->writeS32(-4);
  os->writeU32(flags | clipboardNotify);
  endMsg();
}

void CMsgWriter::writeClipboardProvide(uint32_t flags,
                                      const size_t* lengths,
                                      const uint8_t* const* data)
{
  rdr::MemOutStream mos;
  rdr::ZlibOutStream zos;

  int i, count;

  if (!(server->clipboardFlags() & clipboardProvide))
    throw std::logic_error("Server does not support clipboard \"provide\" action");

  zos.setUnderlying(&mos);

  count = 0;
  for (i = 0;i < 16;i++) {
    if (!(flags & (1 << i)))
      continue;
    zos.writeU32(lengths[count]);
    zos.writeBytes(data[count], lengths[count]);
    count++;
  }

  zos.flush();

  startMsg(msgTypeClientCutText);
  os->pad(3);
  os->writeS32(-(4 + mos.length()));
  os->writeU32(flags | clipboardProvide);
  os->writeBytes(mos.data(), mos.length());
  endMsg();
}

void CMsgWriter::writeQEMUAudioEnable(bool enable)
{
  startMsg(msgTypeQEMUClientMessage);
  os->writeU8(qemuAudio);
  os->writeU16(enable ? msgToQemuEnableAudio : msgToQemuDisableAudio);
  endMsg();
}

void CMsgWriter::writeQEMUAudioSetFormat(uint8_t sampleFormat,
                                         uint8_t channels,
                                         uint32_t frequency)
{
  startMsg(msgTypeQEMUClientMessage);
  os->writeU8(qemuAudio);
  os->writeU16(msgToQemuSetAudioFormat);
  os->writeU8(sampleFormat);
  os->writeU8(channels);
  os->writeU32(frequency);
  endMsg();
}

static void writeFixedString(rdr::OutStream* os, const std::string& str,
                             size_t fieldLen)
{
  size_t len = str.size();
  if (len >= fieldLen)
    len = fieldLen - 1;
  if (len > 0)
    os->writeBytes((const uint8_t*)str.data(), len);
  os->pad(fieldLen - len);
}

static size_t giiEventWireSize(const GIIEvent& event)
{
  switch (event.type) {
  case giiEventKeyPress:
  case giiEventKeyRelease:
  case giiEventKeyRepeat:
    return giiKeyEventSize;
  case giiEventPtrRelative:
  case giiEventPtrAbsolute:
    return giiPtrMoveEventSize;
  case giiEventPtrButtonPress:
  case giiEventPtrButtonRelease:
    return giiPtrButtonEventSize;
  case giiEventValuatorRelative:
  case giiEventValuatorAbsolute:
    if (event.values.size() > giiMaxValuatorsPerEvent)
      throw std::out_of_range("Too many GII valuators in event");
    return giiValuatorEventHeaderSize + event.values.size() * 4;
  default:
    throw std::invalid_argument("Unknown GII event type");
  }
}

static void writeSingleGIIEvent(rdr::OutStream* os, const GIIEvent& event,
                                size_t eventSize)
{
  os->writeU8((uint8_t)eventSize);
  os->writeU8(event.type);
  os->pad(2);
  os->writeU32(event.origin);

  switch (event.type) {
  case giiEventKeyPress:
  case giiEventKeyRelease:
  case giiEventKeyRepeat:
    os->writeU32(event.modifiers);
    os->writeU32(event.symbol);
    os->writeU32(event.label);
    os->writeU32(event.button);
    break;
  case giiEventPtrRelative:
  case giiEventPtrAbsolute:
    os->writeS32(event.x);
    os->writeS32(event.y);
    os->writeS32(event.z);
    os->writeS32(event.wheel);
    break;
  case giiEventPtrButtonPress:
  case giiEventPtrButtonRelease:
    os->writeU32(event.button);
    break;
  case giiEventValuatorRelative:
  case giiEventValuatorAbsolute:
    os->writeU32(event.first);
    os->writeU32((uint32_t)event.values.size());
    for (int32_t val : event.values)
      os->writeS32(val);
    break;
  }
}

void CMsgWriter::writeGIIVersion(uint16_t version)
{
  if (!server->supportsGII)
    throw std::logic_error("Server does not support GII");

  startMsg(msgTypeGIIClientMessage);
  os->writeU8(giiBigEndian | giiClientVersion);
  os->writeU16(2);
  os->writeU16(version);
  endMsg();
}

void CMsgWriter::writeGIIDeviceCreate(const GIIDevice& device)
{
  if (!server->supportsGII)
    throw std::logic_error("Server does not support GII");
  if (device.valuators.size() > giiMaxValuatorsPerDevice)
    throw std::out_of_range("Too many GII valuators");

  uint16_t length = (uint16_t)(giiDeviceCreateFixedSize +
                               device.valuators.size() * giiValuatorWireSize);

  startMsg(msgTypeGIIClientMessage);
  os->writeU8(giiBigEndian | giiClientDeviceCreate);
  os->writeU16(length);

  writeFixedString(os, device.name, giiDeviceNameLen);
  os->writeU32(device.vendorID);
  os->writeU32(device.productID);
  os->writeU32(device.canGenerate);
  os->writeU32(device.numRegisters);
  os->writeU32((uint32_t)device.valuators.size());
  os->writeU32(device.numButtons);

  for (const GIIValuator& val : device.valuators) {
    os->writeU32(val.index);
    writeFixedString(os, val.longName, giiValuatorLongNameLen);
    writeFixedString(os, val.shortName, giiValuatorShortNameLen);
    os->writeS32(val.rangeMin);
    os->writeS32(val.rangeCenter);
    os->writeS32(val.rangeMax);
    os->writeU32(val.siUnit);
    os->writeS32(val.siAdd);
    os->writeS32(val.siMul);
    os->writeS32(val.siDiv);
    os->writeS32(val.siShift);
  }

  endMsg();
}

void CMsgWriter::writeGIIDeviceDestroy(uint32_t deviceOrigin)
{
  if (!server->supportsGII)
    throw std::logic_error("Server does not support GII");

  startMsg(msgTypeGIIClientMessage);
  os->writeU8(giiBigEndian | giiClientDeviceDestroy);
  os->writeU16(4);
  os->writeU32(deviceOrigin);
  endMsg();
}

void CMsgWriter::writeGIIEvent(const GIIEvent& event)
{
  std::vector<GIIEvent> events(1, event);
  writeGIIEvents(events);
}

void CMsgWriter::writeGIIEvents(const std::vector<GIIEvent>& events)
{
  if (!server->supportsGII)
    throw std::logic_error("Server does not support GII");
  if (events.empty())
    return;

  std::vector<size_t> sizes(events.size());
  for (size_t i = 0; i < events.size(); i++)
    sizes[i] = giiEventWireSize(events[i]);

  size_t idx = 0;
  while (idx < events.size()) {
    size_t chunkStart = idx;
    size_t chunkBytes = 0;

    while (idx < events.size() && (chunkBytes + sizes[idx]) <= 65535) {
      chunkBytes += sizes[idx];
      idx++;
    }

    startMsg(msgTypeGIIClientMessage);
    os->writeU8(giiBigEndian | giiClientEvent);
    os->writeU16((uint16_t)chunkBytes);

    for (size_t j = chunkStart; j < idx; j++)
      writeSingleGIIEvent(os, events[j], sizes[j]);

    endMsg();
  }
}

void CMsgWriter::startMsg(int type)
{
  os->writeU8(type);
}

void CMsgWriter::endMsg()
{
  os->flush();
}
