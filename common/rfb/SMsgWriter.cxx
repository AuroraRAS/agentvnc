/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright (C) 2011 D. R. Commander.  All Rights Reserved.
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
#include <string.h>

#include <algorithm>
#include <stdexcept>

#include <zlib.h>

#include <core/LogWriter.h>
#include <core/string.h>

#include <rdr/OutStream.h>
#include <rdr/MemOutStream.h>
#include <rdr/ZlibOutStream.h>

#include <rfb/msgTypes.h>
#include <rfb/fenceTypes.h>
#include <rfb/giiTypes.h>
#include <rfb/clipboardTypes.h>
#include <rfb/ClientParams.h>
#include <rfb/Cursor.h>
#include <rfb/UpdateTracker.h>
#include <rfb/Encoder.h>
#include <rfb/ScreenSet.h>
#include <rfb/SMsgWriter.h>
#include <rfb/encodings.h>
#include <rfb/ledStates.h>

using namespace rfb;

static core::LogWriter vlog("SMsgWriter");

struct rfb::Tight2DeflateState {
  z_stream zs;
  bool initialized;

  Tight2DeflateState() : initialized(false)
  {
    memset(&zs, 0, sizeof(zs));
  }

  ~Tight2DeflateState()
  {
    if (initialized)
      deflateEnd(&zs);
  }
};

static void writeU64(rdr::OutStream* os, uint64_t val)
{
  os->writeU32(static_cast<uint32_t>(val >> 32));
  os->writeU32(static_cast<uint32_t>(val & 0xFFFFFFFFU));
}

static void writeBE16(std::vector<uint8_t>* out, uint16_t v)
{
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out->push_back(static_cast<uint8_t>(v & 0xFF));
}

static void writeBE32(std::vector<uint8_t>* out, uint32_t v)
{
  out->push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out->push_back(static_cast<uint8_t>(v & 0xFF));
}

static void writeBE64(std::vector<uint8_t>* out, uint64_t v)
{
  writeBE32(out, static_cast<uint32_t>(v >> 32));
  writeBE32(out, static_cast<uint32_t>(v & 0xFFFFFFFFU));
}

static std::vector<uint8_t> deflateTight13Block(const uint8_t* data, size_t len,
                                                int level)
{
  if (len == 0 || data == nullptr)
    return std::vector<uint8_t>();

  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  int zlevel = std::max(1, std::min(level, 9));
  if (deflateInit2(&zs, zlevel, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
    throw std::runtime_error("zlib deflateInit2 failed");

  std::vector<uint8_t> out(deflateBound(&zs, static_cast<uLong>(len)) + 32);
  zs.next_in = const_cast<Bytef*>(data);
  zs.avail_in = static_cast<uInt>(len);
  zs.next_out = out.data();
  zs.avail_out = static_cast<uInt>(out.size());

  int rc = deflate(&zs, Z_FINISH);
  size_t written = zs.total_out;
  uInt remainingIn = zs.avail_in;
  deflateEnd(&zs);
  if (rc != Z_STREAM_END || remainingIn != 0)
    throw std::runtime_error("zlib deflate failed");

  out.resize(written);
  return out;
}

SMsgWriter::SMsgWriter(ClientParams* client_, rdr::OutStream* os_)
  : client(client_), os(os_),
    nRectsInUpdate(0), nRectsInHeader(0),
    needSetDesktopName(false), needCursor(false),
    needCursorPos(false), needLEDState(false),
    needQEMUKeyEvent(false), needExtMouseButtonsEvent(false),
    tight2Deflate(nullptr)
{
}

SMsgWriter::~SMsgWriter()
{
  delete tight2Deflate;
}

std::vector<uint8_t> SMsgWriter::deflateTight2Block(const uint8_t* data,
                                                      size_t len)
{
  if (len == 0 || data == nullptr)
    return std::vector<uint8_t>();

  if (tight2Deflate == nullptr)
    tight2Deflate = new Tight2DeflateState();

  if (!tight2Deflate->initialized) {
    memset(&tight2Deflate->zs, 0, sizeof(tight2Deflate->zs));
    if (deflateInit2(&tight2Deflate->zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
      throw std::runtime_error("Tight2 deflateInit2 failed");
    }
    tight2Deflate->initialized = true;
  }

  z_stream& zs = tight2Deflate->zs;
  std::vector<uint8_t> out(deflateBound(&zs, static_cast<uLong>(len)) + 32);
  zs.next_in = const_cast<Bytef*>(data);
  zs.avail_in = static_cast<uInt>(len);
  zs.next_out = out.data();
  zs.avail_out = static_cast<uInt>(out.size());

  uLong before = zs.total_out;
  int rc = deflate(&zs, Z_SYNC_FLUSH);
  if (rc != Z_OK || zs.avail_in != 0)
    throw std::runtime_error("Tight2 deflate failed");

  out.resize(zs.total_out - before);
  return out;
}

void SMsgWriter::writeServerInit(uint16_t width, uint16_t height,
                                 const PixelFormat& pf, const char* name)
{
  writeServerInit(width, height, pf, name, {}, {}, {});
}

void SMsgWriter::writeServerInit(uint16_t width, uint16_t height,
                                 const PixelFormat& pf, const char* name,
                                 const std::vector<TightCapability>& serverMsgs,
                                 const std::vector<TightCapability>& clientMsgs,
                                 const std::vector<TightCapability>& encodings)
{
  os->writeU16(width);
  os->writeU16(height);
  pf.write(os);
  os->writeU32(strlen(name));
  os->writeBytes((const uint8_t*)name, strlen(name));

  if (client->tightEnabled) {
    auto writeCaps = [this](const std::vector<TightCapability>& caps) {
      for (const TightCapability& cap : caps) {
        os->writeS32(cap.code);
        os->writeBytes(reinterpret_cast<const uint8_t*>(cap.vendor), 4);
        os->writeBytes(reinterpret_cast<const uint8_t*>(cap.signature), 8);
      }
    };

    os->writeU16(static_cast<uint16_t>(serverMsgs.size()));
    os->writeU16(static_cast<uint16_t>(clientMsgs.size()));
    os->writeU16(static_cast<uint16_t>(encodings.size()));
    os->writeU16(0);

    writeCaps(serverMsgs);
    writeCaps(clientMsgs);
    writeCaps(encodings);
  }

  endMsg();
}

void SMsgWriter::writeSetColourMapEntries(int firstColour, int nColours,
                                          const uint16_t red[],
                                          const uint16_t green[],
                                          const uint16_t blue[])
{
  startMsg(msgTypeSetColourMapEntries);
  os->pad(1);
  os->writeU16(firstColour);
  os->writeU16(nColours);
  for (int i = firstColour; i < firstColour+nColours; i++) {
    os->writeU16(red[i]);
    os->writeU16(green[i]);
    os->writeU16(blue[i]);
  }
  endMsg();
}

void SMsgWriter::writeBell()
{
  startMsg(msgTypeBell);
  endMsg();
}

void SMsgWriter::writeServerCutText(const char* str)
{
  if (strchr(str, '\r') != nullptr)
    throw std::invalid_argument("Invalid carriage return in clipboard data");

  std::string latin1(core::utf8ToLatin1(str));

  startMsg(msgTypeServerCutText);
  os->pad(3);
  os->writeU32(latin1.size());
  os->writeBytes((const uint8_t*)latin1.data(), latin1.size());
  endMsg();
}

void SMsgWriter::writeClipboardCaps(uint32_t caps,
                                    const uint32_t* lengths)
{
  size_t i, count;

  if (!client->supportsEncoding(pseudoEncodingExtendedClipboard))
    throw std::logic_error("Client does not support extended clipboard");

  count = 0;
  for (i = 0;i < 16;i++) {
    if (caps & (1 << i))
      count++;
  }

  startMsg(msgTypeServerCutText);
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

void SMsgWriter::writeClipboardRequest(uint32_t flags)
{
  if (!client->supportsEncoding(pseudoEncodingExtendedClipboard))
    throw std::logic_error("Client does not support extended clipboard");
  if (!(client->clipboardFlags() & clipboardRequest))
    throw std::logic_error("Client does not support clipboard \"request\" action");

  startMsg(msgTypeServerCutText);
  os->pad(3);
  os->writeS32(-4);
  os->writeU32(flags | clipboardRequest);
  endMsg();
}

void SMsgWriter::writeClipboardPeek(uint32_t flags)
{
  if (!client->supportsEncoding(pseudoEncodingExtendedClipboard))
    throw std::logic_error("Client does not support extended clipboard");
  if (!(client->clipboardFlags() & clipboardPeek))
    throw std::logic_error("Client does not support clipboard \"peek\" action");

  startMsg(msgTypeServerCutText);
  os->pad(3);
  os->writeS32(-4);
  os->writeU32(flags | clipboardPeek);
  endMsg();
}

void SMsgWriter::writeClipboardNotify(uint32_t flags)
{
  if (!client->supportsEncoding(pseudoEncodingExtendedClipboard))
    throw std::logic_error("Client does not support extended clipboard");
  if (!(client->clipboardFlags() & clipboardNotify))
    throw std::logic_error("Client does not support clipboard \"notify\" action");

  startMsg(msgTypeServerCutText);
  os->pad(3);
  os->writeS32(-4);
  os->writeU32(flags | clipboardNotify);
  endMsg();
}

void SMsgWriter::writeClipboardProvide(uint32_t flags,
                                      const size_t* lengths,
                                      const uint8_t* const* data)
{
  rdr::MemOutStream mos;
  rdr::ZlibOutStream zos;

  int i, count;

  if (!client->supportsEncoding(pseudoEncodingExtendedClipboard))
    throw std::logic_error("Client does not support extended clipboard");
  if (!(client->clipboardFlags() & clipboardProvide))
    throw std::logic_error("Client does not support clipboard \"provide\" action");

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

  startMsg(msgTypeServerCutText);
  os->pad(3);
  os->writeS32(-(4 + mos.length()));
  os->writeU32(flags | clipboardProvide);
  os->writeBytes(mos.data(), mos.length());
  endMsg();
}

void SMsgWriter::writeFence(uint32_t flags, unsigned len,
                            const uint8_t data[])
{
  if (!client->supportsEncoding(pseudoEncodingFence))
    throw std::logic_error("Client does not support fences");
  if (len > 64)
    throw std::out_of_range("Too large fence payload");
  if ((flags & ~fenceFlagsSupported) != 0)
    throw std::invalid_argument("Unknown fence flags");

  startMsg(msgTypeServerFence);
  os->pad(3);

  os->writeU32(flags);

  os->writeU8(len);

  if (len > 0)
    os->writeBytes(data, len);

  endMsg();
}

void SMsgWriter::writeEndOfContinuousUpdates()
{
  if (!client->supportsEncoding(pseudoEncodingContinuousUpdates))
    throw std::logic_error("Client does not support continuous updates");

  startMsg(msgTypeEndOfContinuousUpdates);
  endMsg();
}

void SMsgWriter::writeDesktopSize(uint16_t reason, uint16_t result)
{
  ExtendedDesktopSizeMsg msg;

  if (!client->supportsEncoding(pseudoEncodingDesktopSize) &&
      !client->supportsEncoding(pseudoEncodingExtendedDesktopSize))
    throw std::logic_error("Client does not support desktop size changes");

  msg.reason = reason;
  msg.result = result;

  extendedDesktopSizeMsgs.push_back(msg);
}

void SMsgWriter::writeSetDesktopName()
{
  if (!client->supportsEncoding(pseudoEncodingDesktopName))
    throw std::logic_error("Client does not support desktop name changes");

  needSetDesktopName = true;
}

void SMsgWriter::writeCursor()
{
  if (!client->supportsEncoding(pseudoEncodingCursor) &&
      !client->supportsEncoding(pseudoEncodingXCursor) &&
      !client->supportsEncoding(pseudoEncodingCursorWithAlpha) &&
      !client->supportsEncoding(pseudoEncodingVMwareCursor))
    throw std::logic_error("Client does not support local cursor");

  needCursor = true;
}

void SMsgWriter::writeCursorPos()
{
  if (!client->supportsEncoding(pseudoEncodingVMwareCursorPosition))
    throw std::logic_error("Client does not support cursor position");

  needCursorPos = true;
}

void SMsgWriter::writeLEDState()
{
  if (!client->supportsEncoding(pseudoEncodingLEDState) &&
      !client->supportsEncoding(pseudoEncodingVMwareLEDState))
    throw std::logic_error("Client does not support LED state");
  if (client->ledState() == ledUnknown)
    throw std::logic_error("Server has not specified LED state");

  needLEDState = true;
}

void SMsgWriter::writeQEMUKeyEvent()
{
  if (!client->supportsEncoding(pseudoEncodingQEMUKeyEvent))
    throw std::logic_error("Client does not support QEMU key events");

  needQEMUKeyEvent = true;
}

void SMsgWriter::writeExtendedMouseButtonsSupport()
{
  if (!client->supportsEncoding(pseudoEncodingExtendedMouseButtons))
    throw std::logic_error("Client does not support Extended Mouse Buttons");

  needExtMouseButtonsEvent = true;
}

bool SMsgWriter::needFakeUpdate()
{
  if (needSetDesktopName)
    return true;
  if (needCursor)
    return true;
  if (needCursorPos)
    return true;
  if (needLEDState)
    return true;
  if (needQEMUKeyEvent)
    return true;
  if (needExtMouseButtonsEvent)
    return true;
  if (needNoDataUpdate())
    return true;

  return false;
}

bool SMsgWriter::needNoDataUpdate()
{
  if (!extendedDesktopSizeMsgs.empty())
    return true;

  return false;
}

void SMsgWriter::writeNoDataUpdate()
{
  int nRects;

  nRects = 0;

  if (!extendedDesktopSizeMsgs.empty()) {
    if (client->supportsEncoding(pseudoEncodingExtendedDesktopSize))
      nRects += extendedDesktopSizeMsgs.size();
    else
      nRects++;
  }

  writeFramebufferUpdateStart(nRects);
  writeNoDataRects();
  writeFramebufferUpdateEnd();
}

void SMsgWriter::writeFramebufferUpdateStart(int nRects)
{
  startMsg(msgTypeFramebufferUpdate);
  os->pad(1);

  if (nRects != 0xFFFF) {
    if (needSetDesktopName)
      nRects++;
    if (needCursor)
      nRects++;
    if (needCursorPos)
      nRects++;
    if (needLEDState)
      nRects++;
    if (needQEMUKeyEvent)
      nRects++;
    if (needExtMouseButtonsEvent)
      nRects++;
  }

  os->writeU16(nRects);

  nRectsInUpdate = 0;
  if (nRects == 0xFFFF)
    nRectsInHeader = 0;
  else
    nRectsInHeader = nRects;

  writePseudoRects();
}

void SMsgWriter::writeFramebufferUpdateEnd()
{
  if (nRectsInUpdate != nRectsInHeader && nRectsInHeader)
    throw std::logic_error("SMsgWriter::writeFramebufferUpdateEnd: "
                           "nRects out of sync");

  if (nRectsInHeader == 0) {
    // Send last rect. marker
    os->writeS16(0);
    os->writeS16(0);
    os->writeU16(0);
    os->writeU16(0);
    os->writeU32(pseudoEncodingLastRect);
  }

  endMsg();
}

void SMsgWriter::writeCopyRect(const core::Rect& r, int srcX, int srcY)
{
  startRect(r,encodingCopyRect);
  os->writeU16(srcX);
  os->writeU16(srcY);
  endRect();
}

void SMsgWriter::startRect(const core::Rect& r, int encoding)
{
  if (++nRectsInUpdate > nRectsInHeader && nRectsInHeader)
    throw std::logic_error("SMsgWriter::startRect: nRects out of sync");

  os->writeS16(r.tl.x);
  os->writeS16(r.tl.y);
  os->writeU16(r.width());
  os->writeU16(r.height());
  os->writeU32(encoding);
}

void SMsgWriter::endRect()
{
  os->flush();
}

void SMsgWriter::startMsg(int type)
{
  os->writeU8(type);
}

void SMsgWriter::endMsg()
{
  os->flush();
}

void SMsgWriter::writePseudoRects()
{
  if (needCursor) {
    const Cursor& cursor = client->cursor();

    if (client->supportsEncoding(pseudoEncodingCursorWithAlpha)) {
      writeSetCursorWithAlphaRect(cursor.width(), cursor.height(),
                                  cursor.hotspot().x, cursor.hotspot().y,
                                  cursor.getBuffer());
    } else if (client->supportsEncoding(pseudoEncodingVMwareCursor)) {
      writeSetVMwareCursorRect(cursor.width(), cursor.height(),
                               cursor.hotspot().x, cursor.hotspot().y,
                               cursor.getBuffer());
    } else if (client->supportsEncoding(pseudoEncodingCursor)) {
      size_t data_len = cursor.width()*cursor.height() *
                        (client->pf().bpp/8);
      std::vector<uint8_t> data(data_len);
      std::vector<uint8_t> mask(cursor.getMask());

      const uint8_t* in;
      uint8_t* out;

      in = cursor.getBuffer();
      out = data.data();
      for (int i = 0;i < cursor.width()*cursor.height();i++) {
        client->pf().bufferFromRGB(out, in, 1);
        in += 4;
        out += client->pf().bpp/8;
      }

      writeSetCursorRect(cursor.width(), cursor.height(),
                         cursor.hotspot().x, cursor.hotspot().y,
                         data.data(), mask.data());
    } else if (client->supportsEncoding(pseudoEncodingXCursor)) {
      std::vector<uint8_t> bitmap(cursor.getBitmap());
      std::vector<uint8_t> mask(cursor.getMask());

      writeSetXCursorRect(cursor.width(), cursor.height(),
                          cursor.hotspot().x, cursor.hotspot().y,
                          bitmap.data(), mask.data());
    } else {
      throw std::logic_error("Client does not support local cursor");
    }

    needCursor = false;
  }

  if (needCursorPos) {
    const core::Point& cursorPos = client->cursorPos();

    if (client->supportsEncoding(pseudoEncodingVMwareCursorPosition)) {
      writeSetVMwareCursorPositionRect(cursorPos.x, cursorPos.y);
    } else {
      throw std::logic_error("Client does not support cursor position");
    }

    needCursorPos = false;
  }

  if (needSetDesktopName) {
    writeSetDesktopNameRect(client->name());
    needSetDesktopName = false;
  }

  if (needLEDState) {
    writeLEDStateRect(client->ledState());
    needLEDState = false;
  }

  if (needQEMUKeyEvent) {
    writeQEMUKeyEventRect();
    needQEMUKeyEvent = false;
  }

  if (needExtMouseButtonsEvent) {
    writeExtendedMouseButtonsRect();
    needExtMouseButtonsEvent = false;
  }
}

void SMsgWriter::writeNoDataRects()
{
  if (!extendedDesktopSizeMsgs.empty()) {
    if (client->supportsEncoding(pseudoEncodingExtendedDesktopSize)) {
      for (ExtendedDesktopSizeMsg msg : extendedDesktopSizeMsgs) {
        // FIXME: We can probably skip multiple reasonServer entries
        writeExtendedDesktopSizeRect(msg.reason, msg.result,
                                     client->width(), client->height(),
                                     client->screenLayout());
      }
    } else if (client->supportsEncoding(pseudoEncodingDesktopSize)) {
      // Some clients assume this is the last rectangle so don't send anything
      // more after this
      writeSetDesktopSizeRect(client->width(), client->height());
    } else {
      throw std::logic_error("Client does not support desktop size changes");
    }

    extendedDesktopSizeMsgs.clear();
  }
}

void SMsgWriter::writeSetDesktopSizeRect(int width, int height)
{
  if (!client->supportsEncoding(pseudoEncodingDesktopSize))
    throw std::logic_error("Client does not support desktop resize");
  if (++nRectsInUpdate > nRectsInHeader && nRectsInHeader)
    throw std::logic_error("SMsgWriter::writeSetDesktopSizeRect: nRects out of sync");

  os->writeS16(0);
  os->writeS16(0);
  os->writeU16(width);
  os->writeU16(height);
  os->writeU32(pseudoEncodingDesktopSize);
}

void SMsgWriter::writeExtendedDesktopSizeRect(uint16_t reason,
                                              uint16_t result,
                                              int fb_width,
                                              int fb_height,
                                              const ScreenSet& layout)
{
  ScreenSet::const_iterator si;

  if (!client->supportsEncoding(pseudoEncodingExtendedDesktopSize))
    throw std::logic_error("Client does not support extended desktop resize");
  if (++nRectsInUpdate > nRectsInHeader && nRectsInHeader)
    throw std::logic_error("SMsgWriter::writeExtendedDesktopSizeRect: nRects out of sync");

  os->writeU16(reason);
  os->writeU16(result);
  os->writeU16(fb_width);
  os->writeU16(fb_height);
  os->writeU32(pseudoEncodingExtendedDesktopSize);

  os->writeU8(layout.num_screens());
  os->pad(3);

  for (si = layout.begin();si != layout.end();++si) {
    os->writeU32(si->id);
    os->writeU16(si->dimensions.tl.x);
    os->writeU16(si->dimensions.tl.y);
    os->writeU16(si->dimensions.width());
    os->writeU16(si->dimensions.height());
    os->writeU32(si->flags);
  }
}

void SMsgWriter::writeSetDesktopNameRect(const char *name)
{
  if (!client->supportsEncoding(pseudoEncodingDesktopName))
    throw std::logic_error("Client does not support desktop rename");
  if (++nRectsInUpdate > nRectsInHeader && nRectsInHeader)
    throw std::logic_error("SMsgWriter::writeSetDesktopNameRect: nRects out of sync");

  os->writeS16(0);
  os->writeS16(0);
  os->writeU16(0);
  os->writeU16(0);
  os->writeU32(pseudoEncodingDesktopName);
  os->writeU32(strlen(name));
  os->writeBytes((const uint8_t*)name, strlen(name));
}

void SMsgWriter::writeSetCursorRect(int width, int height,
                                    int hotspotX, int hotspotY,
                                    const uint8_t* data,
                                    const uint8_t* mask)
{
  if (!client->supportsEncoding(pseudoEncodingCursor))
    throw std::logic_error("Client does not support local cursors");
  if (++nRectsInUpdate > nRectsInHeader && nRectsInHeader)
    throw std::logic_error("SMsgWriter::writeSetCursorRect: nRects out of sync");

  os->writeS16(hotspotX);
  os->writeS16(hotspotY);
  os->writeU16(width);
  os->writeU16(height);
  os->writeU32(pseudoEncodingCursor);
  os->writeBytes(data, width * height * (client->pf().bpp/8));
  os->writeBytes(mask, (width+7)/8 * height);
}

void SMsgWriter::writeSetXCursorRect(int width, int height,
                                     int hotspotX, int hotspotY,
                                     const uint8_t* data,
                                     const uint8_t* mask)
{
  if (!client->supportsEncoding(pseudoEncodingXCursor))
    throw std::logic_error("Client does not support local cursors");
  if (++nRectsInUpdate > nRectsInHeader && nRectsInHeader)
    throw std::logic_error("SMsgWriter::writeSetXCursorRect: nRects out of sync");

  os->writeS16(hotspotX);
  os->writeS16(hotspotY);
  os->writeU16(width);
  os->writeU16(height);
  os->writeU32(pseudoEncodingXCursor);
  if (width * height > 0) {
    os->writeU8(255);
    os->writeU8(255);
    os->writeU8(255);
    os->writeU8(0);
    os->writeU8(0);
    os->writeU8(0);
    os->writeBytes(data, (width+7)/8 * height);
    os->writeBytes(mask, (width+7)/8 * height);
  }
}

void SMsgWriter::writeSetCursorWithAlphaRect(int width, int height,
                                             int hotspotX, int hotspotY,
                                             const uint8_t* data)
{
  if (!client->supportsEncoding(pseudoEncodingCursorWithAlpha))
    throw std::logic_error("Client does not support local cursors");
  if (++nRectsInUpdate > nRectsInHeader && nRectsInHeader)
    throw std::logic_error("SMsgWriter::writeSetCursorWithAlphaRect: nRects out of sync");

  os->writeS16(hotspotX);
  os->writeS16(hotspotY);
  os->writeU16(width);
  os->writeU16(height);
  os->writeU32(pseudoEncodingCursorWithAlpha);

  // FIXME: Use an encoder with compression?
  os->writeU32(encodingRaw);

  // Alpha needs to be pre-multiplied
  for (int i = 0;i < width*height;i++) {
    os->writeU8((unsigned)data[0] * data[3] / 255);
    os->writeU8((unsigned)data[1] * data[3] / 255);
    os->writeU8((unsigned)data[2] * data[3] / 255);
    os->writeU8(data[3]);
    data += 4;
  }
}

void SMsgWriter::writeSetVMwareCursorRect(int width, int height,
                                          int hotspotX, int hotspotY,
                                          const uint8_t* data)
{
  if (!client->supportsEncoding(pseudoEncodingVMwareCursor))
    throw std::logic_error("Client does not support local cursors");
  if (++nRectsInUpdate > nRectsInHeader && nRectsInHeader)
    throw std::logic_error("SMsgWriter::writeSetVMwareCursorRect: nRects out of sync");

  os->writeS16(hotspotX);
  os->writeS16(hotspotY);
  os->writeU16(width);
  os->writeU16(height);
  os->writeU32(pseudoEncodingVMwareCursor);

  os->writeU8(1); // Alpha cursor
  os->pad(1);

  // FIXME: Should alpha be premultiplied?
  os->writeBytes(data, width*height*4);
}

void SMsgWriter::writeSetVMwareCursorPositionRect(int hotspotX, int hotspotY)
{
  if (!client->supportsEncoding(pseudoEncodingVMwareCursorPosition))
    throw std::logic_error("Client does not support cursor position");
  if (++nRectsInUpdate > nRectsInHeader && nRectsInHeader)
    throw std::logic_error("SMsgWriter::writeSetVMwareCursorRect: nRects out of sync");

  os->writeS16(hotspotX);
  os->writeS16(hotspotY);
  os->writeU16(0);
  os->writeU16(0);
  os->writeU32(pseudoEncodingVMwareCursorPosition);
}

void SMsgWriter::writeLEDStateRect(uint8_t state)
{
  if (!client->supportsEncoding(pseudoEncodingLEDState) &&
      !client->supportsEncoding(pseudoEncodingVMwareLEDState))
    throw std::logic_error("Client does not support LED state updates");
  if (client->ledState() == ledUnknown)
    throw std::logic_error("Server does not support LED state updates");
  if (++nRectsInUpdate > nRectsInHeader && nRectsInHeader)
    throw std::logic_error("SMsgWriter::writeLEDStateRect: nRects out of sync");

  os->writeS16(0);
  os->writeS16(0);
  os->writeU16(0);
  os->writeU16(0);
  if (client->supportsEncoding(pseudoEncodingLEDState)) {
    os->writeU32(pseudoEncodingLEDState);
    os->writeU8(state);
  } else {
    os->writeU32(pseudoEncodingVMwareLEDState);
    os->writeU32(state);
  }
}

void SMsgWriter::writeQEMUKeyEventRect()
{
  if (!client->supportsEncoding(pseudoEncodingQEMUKeyEvent))
    throw std::logic_error("Client does not support QEMU extended key events");
  if (++nRectsInUpdate > nRectsInHeader && nRectsInHeader)
    throw std::logic_error("SMsgWriter::writeQEMUKeyEventRect: nRects out of sync");

  os->writeS16(0);
  os->writeS16(0);
  os->writeU16(0);
  os->writeU16(0);
  os->writeU32(pseudoEncodingQEMUKeyEvent);
}

void SMsgWriter::writeExtendedMouseButtonsRect()
{
  if (!client->supportsEncoding(pseudoEncodingExtendedMouseButtons))
    throw std::logic_error("Client does not support extended mouse button events");
  if (++nRectsInUpdate > nRectsInHeader && nRectsInHeader)
    throw std::logic_error("SMsgWriter::writeExtendedMouseButtonsRect: nRects out of sync");

  os->writeS16(0);
  os->writeS16(0);
  os->writeU16(0);
  os->writeU16(0);
  os->writeU32(pseudoEncodingExtendedMouseButtons);
}

void SMsgWriter::writeTight2CompressionSupportReply(bool supported)
{
  os->writeU32(tight2CompressionSupportReply);
  os->writeU8(supported ? 1 : 0);
  endMsg();
}

void SMsgWriter::writeTight2FileListReply(uint8_t compressLevel,
                                          const std::vector<FileEntry>& entries)
{
  std::vector<uint8_t> raw;
  writeBE32(&raw, static_cast<uint32_t>(entries.size()));
  for (const FileEntry& entry : entries) {
    writeBE64(&raw, entry.isDir ? 0 : entry.size);
    uint64_t mtimeMs = (entry.mtime > 0)
      ? static_cast<uint64_t>(entry.mtime) * 1000ULL : 0ULL;
    writeBE64(&raw, mtimeMs);
    uint16_t flags = (entry.isDir ? tight2FlagDir : 0) |
                     (entry.isExecutable ? tight2FlagExec : 0);
    writeBE16(&raw, flags);
    writeBE32(&raw, static_cast<uint32_t>(entry.name.size()));
    raw.insert(raw.end(), entry.name.begin(), entry.name.end());
  }

  os->writeU32(tight2FileListReply);
  if (compressLevel != 0 && !raw.empty()) {
    std::vector<uint8_t> packed = deflateTight2Block(raw.data(), raw.size());
    os->writeU8(1);
    os->writeU32(static_cast<uint32_t>(packed.size()));
    os->writeU32(static_cast<uint32_t>(raw.size()));
    if (!packed.empty())
      os->writeBytes(packed.data(), packed.size());
  } else {
    os->writeU8(0);
    os->writeU32(static_cast<uint32_t>(raw.size()));
    os->writeU32(static_cast<uint32_t>(raw.size()));
    if (!raw.empty())
      os->writeBytes(raw.data(), raw.size());
  }
  endMsg();
}

void SMsgWriter::writeTight2Md5Reply(const uint8_t digest[16])
{
  os->writeU32(tight2Md5Reply);
  os->writeBytes(digest, 16);
  endMsg();
}

void SMsgWriter::writeTight2UploadStartReply()
{
  os->writeU32(tight2UploadStartReply);
  endMsg();
}

void SMsgWriter::writeTight2UploadDataReply()
{
  os->writeU32(tight2UploadDataReply);
  endMsg();
}

void SMsgWriter::writeTight2UploadEndReply()
{
  os->writeU32(tight2UploadEndReply);
  endMsg();
}

void SMsgWriter::writeTight2DownloadStartReply()
{
  os->writeU32(tight2DownloadStartReply);
  endMsg();
}

void SMsgWriter::writeTight2DownloadDataReply(uint8_t compressLevel,
                                              const uint8_t* data, size_t len)
{
  os->writeU32(tight2DownloadDataReply);
  if (compressLevel != 0 && len > 0 && data != nullptr) {
    std::vector<uint8_t> packed = deflateTight2Block(data, len);
    os->writeU8(1);
    os->writeU32(static_cast<uint32_t>(packed.size()));
    os->writeU32(static_cast<uint32_t>(len));
    if (!packed.empty())
      os->writeBytes(packed.data(), packed.size());
  } else {
    os->writeU8(0);
    os->writeU32(static_cast<uint32_t>(len));
    os->writeU32(static_cast<uint32_t>(len));
    if (len > 0 && data != nullptr)
      os->writeBytes(data, len);
  }
  endMsg();
}

void SMsgWriter::writeTight2DownloadEndReply(uint8_t fileFlags,
                                             uint64_t modTimeMs)
{
  os->writeU32(tight2DownloadEndReply);
  os->writeU8(fileFlags);
  writeU64(os, modTimeMs);
  endMsg();
}

void SMsgWriter::writeTight2MkdirReply()
{
  os->writeU32(tight2MkdirReply);
  endMsg();
}

void SMsgWriter::writeTight2RemoveReply()
{
  os->writeU32(tight2RemoveReply);
  endMsg();
}

void SMsgWriter::writeTight2RenameReply()
{
  os->writeU32(tight2RenameReply);
  endMsg();
}

void SMsgWriter::writeTight2DirSizeReply(uint64_t size)
{
  os->writeU32(tight2DirSizeReply);
  writeU64(os, size);
  endMsg();
}

void SMsgWriter::writeTight2LastRequestFailedReply(const std::string& message)
{
  os->writeU32(tight2LastRequestFailedReply);
  os->writeU32(static_cast<uint32_t>(message.size()));
  if (!message.empty())
    os->writeBytes(reinterpret_cast<const uint8_t*>(message.data()),
                   message.size());
  endMsg();
}

void SMsgWriter::writeTight13FileListData(uint8_t flags,
                                          const std::vector<FileEntry>& entries)
{
  startMsg(msgTypeTightFileListData);
  if ((flags & tight13FlagError) != 0) {
    os->writeU8(flags);
    os->writeU16(0);
    os->writeU16(0);
    os->writeU16(0);
    endMsg();
    return;
  }

  bool dirOnly = (flags & tight13FlagDirOnly) != 0;
  std::vector<const FileEntry*> filtered;
  std::string rawNames;

  for (const FileEntry& e : entries) {
    if (dirOnly && !e.isDir)
      continue;
    if (filtered.size() >= 65535)
      break;
    size_t addLen = e.name.size() + 1;
    if (rawNames.size() + addLen > 65535)
      break;
    filtered.push_back(&e);
    rawNames.append(e.name);
    rawNames.push_back('\0');
  }

  uint8_t outFlags = flags;
  uint8_t compLevel = flags & 0x0F;
  std::vector<uint8_t> compNames;
  bool sendCompressed = false;

  if (compLevel != 0 && !rawNames.empty()) {
    compNames = deflateTight13Block(
      reinterpret_cast<const uint8_t*>(rawNames.data()),
      rawNames.size(), compLevel);
    if (compNames.size() < rawNames.size() && compNames.size() <= 65535)
      sendCompressed = true;
    else
      outFlags &= static_cast<uint8_t>(~0x0F);
  } else {
    outFlags &= static_cast<uint8_t>(~0x0F);
  }

  os->writeU8(outFlags);
  os->writeU16(static_cast<uint16_t>(filtered.size()));
  os->writeU16(static_cast<uint16_t>(rawNames.size()));
  os->writeU16(static_cast<uint16_t>(sendCompressed ? compNames.size()
                                                    : rawNames.size()));

  for (const FileEntry* e : filtered) {
    uint32_t sz = e->isDir
      ? tight13DirSize
      : static_cast<uint32_t>(std::min<uint64_t>(e->size, tight13DirSize - 1));
    uint32_t mt = (e->mtime > 0) ? static_cast<uint32_t>(e->mtime) : 0;
    os->writeU32(sz);
    os->writeU32(mt);
  }

  if (sendCompressed) {
    if (!compNames.empty())
      os->writeBytes(compNames.data(), compNames.size());
  } else if (!rawNames.empty()) {
    os->writeBytes(reinterpret_cast<const uint8_t*>(rawNames.data()),
                   rawNames.size());
  }

  endMsg();
}

void SMsgWriter::writeTight13FileDownloadData(uint8_t compressLevel,
                                              const uint8_t* data, uint16_t len)
{
  if (len == 0 || data == nullptr)
    return;

  startMsg(msgTypeTightFileDownloadData);
  if (compressLevel != 0) {
    std::vector<uint8_t> comp = deflateTight13Block(data, len, compressLevel);
    if (comp.size() < len && comp.size() <= 65535) {
      os->writeU8(compressLevel);
      os->writeU16(len);
      os->writeU16(static_cast<uint16_t>(comp.size()));
      os->writeBytes(comp.data(), comp.size());
      endMsg();
      return;
    }
  }

  os->writeU8(0);
  os->writeU16(len);
  os->writeU16(len);
  os->writeBytes(data, len);
  endMsg();
}

void SMsgWriter::writeTight13FileDownloadEnd(uint32_t modTime)
{
  startMsg(msgTypeTightFileDownloadData);
  os->writeU8(0);
  os->writeU16(0);
  os->writeU16(0);
  os->writeU32(modTime);
  endMsg();
}

void SMsgWriter::writeTight13FileUploadCancel(const std::string& reason)
{
  uint16_t len = static_cast<uint16_t>(std::min<size_t>(reason.size(), 65535));
  startMsg(msgTypeTightFileUploadCancel);
  os->writeU8(0);
  os->writeU16(len);
  if (len > 0)
    os->writeBytes(reinterpret_cast<const uint8_t*>(reason.data()), len);
  endMsg();
}

void SMsgWriter::writeTight13FileDownloadFailed(const std::string& reason)
{
  uint16_t len = static_cast<uint16_t>(std::min<size_t>(reason.size(), 65535));
  startMsg(msgTypeTightFileDownloadFailed);
  os->writeU8(0);
  os->writeU16(len);
  if (len > 0)
    os->writeBytes(reinterpret_cast<const uint8_t*>(reason.data()), len);
  endMsg();
}

void SMsgWriter::writeGIIVersion(uint16_t maxVersion, uint16_t minVersion)
{
  if (!client->supportsGII())
    throw std::logic_error("Client does not support GII");

  startMsg(msgTypeGIIServerMessage);
  os->writeU8(giiBigEndian | giiServerVersion);
  os->writeU16(4);
  os->writeU16(maxVersion);
  os->writeU16(minVersion);
  endMsg();
}

void SMsgWriter::writeGIIDeviceCreateResponse(uint32_t deviceOrigin)
{
  if (!client->supportsGII())
    throw std::logic_error("Client does not support GII");

  startMsg(msgTypeGIIServerMessage);
  os->writeU8(giiBigEndian | giiServerDeviceCreateResponse);
  os->writeU16(4);
  os->writeU32(deviceOrigin);
  endMsg();
}
