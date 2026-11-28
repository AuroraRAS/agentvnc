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

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include <zlib.h>

#include <core/LogWriter.h>
#include <core/i18n.h>
#include <core/string.h>

#include <rdr/InStream.h>
#include <rdr/ZlibInStream.h>

#include <rfb/msgTypes.h>
#include <rfb/qemuTypes.h>
#include <rfb/giiTypes.h>
#include <rfb/clipboardTypes.h>
#include <rfb/fileTransferTypes.h>
#include <rfb/TightCapability.h>
#include <rfb/Exception.h>
#include <rfb/CMsgHandler.h>
#include <rfb/CMsgReader.h>
#include <rfb/PixelBuffer.h>
#include <rfb/ScreenSet.h>
#include <rfb/encodings.h>

static core::LogWriter vlog("CMsgReader");

static core::IntParameter maxCutText("MaxCutText",
                                     _("Maximum permitted length of an "
                                       "incoming clipboard update"),
                                     256*1024, 0, INT_MAX);

// The protocol allows any 32 bit length here, but a server has no
// reason to send more than a fraction of a second of audio at a time,
// and we have to be able to buffer whatever we accept.
static const uint32_t maxAudioData = 1024*1024;
static const uint32_t maxFileTransferPayload = 16*1024*1024;

using namespace rfb;

static uint64_t readU64(rdr::InStream* is)
{
  uint64_t hi = is->readU32();
  uint64_t lo = is->readU32();
  return (hi << 32) | lo;
}

static uint32_t readBE32FromBuf(const uint8_t* p)
{
  return ((uint32_t)p[0] << 24) |
         ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) |
         (uint32_t)p[3];
}

static uint64_t readBE64FromBuf(const uint8_t* p)
{
  return ((uint64_t)readBE32FromBuf(p) << 32) |
         (uint64_t)readBE32FromBuf(p + 4);
}

static uint16_t readBE16FromBuf(const uint8_t* p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

struct rfb::Tight2InflateState {
  z_stream zs;
  bool initialized;
  int windowBits;

  Tight2InflateState() : initialized(false), windowBits(15)
  {
    memset(&zs, 0, sizeof(zs));
  }

  ~Tight2InflateState()
  {
    if (initialized)
      inflateEnd(&zs);
  }
};

static bool runTight2InflateBlock(z_stream* zs,
                                  const uint8_t* in, size_t inLen,
                                  uint8_t* outBuf, size_t outLen,
                                  bool* hitStreamEnd)
{
  *hitStreamEnd = false;
  uint8_t dummy = 0;
  zs->next_in = (inLen > 0 && in != nullptr)
    ? const_cast<Bytef*>(in) : &dummy;
  zs->avail_in = static_cast<uInt>(inLen);
  zs->next_out = (outLen > 0 && outBuf != nullptr) ? outBuf : &dummy;
  zs->avail_out = static_cast<uInt>(outLen);

  int rc = Z_OK;
  int guard = 0;
  while (zs->avail_out > 0 && (rc == Z_OK || rc == Z_BUF_ERROR) &&
         guard++ < 64) {
    uInt prevIn = zs->avail_in;
    uInt prevOut = zs->avail_out;
    rc = inflate(zs, Z_SYNC_FLUSH);
    if (rc == Z_STREAM_END)
      break;
    if (rc == Z_BUF_ERROR && zs->avail_in == prevIn && zs->avail_out == prevOut)
      break;
  }

  if (zs->avail_out != 0)
    return false;

  // Drain any trailing Z_SYNC_FLUSH marker (00 00 ff ff) or Z_FINISH trailer
  guard = 0;
  while (rc != Z_STREAM_END && zs->avail_in > 0 && guard++ < 64) {
    uint8_t extra[16];
    zs->next_out = extra;
    zs->avail_out = sizeof(extra);
    uInt prevIn = zs->avail_in;
    rc = inflate(zs, Z_SYNC_FLUSH);
    if (zs->avail_out != sizeof(extra))
      return false;
    if (rc == Z_STREAM_END)
      break;
    if (rc == Z_BUF_ERROR && zs->avail_in == prevIn)
      break;
    if (rc != Z_OK && rc != Z_BUF_ERROR)
      return false;
  }

  *hitStreamEnd = (rc == Z_STREAM_END);
  return zs->avail_in == 0 &&
         (rc == Z_OK || rc == Z_BUF_ERROR || rc == Z_STREAM_END);
}

static std::vector<uint8_t> inflateTightBlock(const uint8_t* compressed,
                                              size_t compressedSize,
                                              size_t uncompressedSize)
{
  if (uncompressedSize == 0)
    return std::vector<uint8_t>();

  std::vector<uint8_t> out(uncompressedSize);
  const int windowModes[2] = { 15, -15 };

  for (int i = 0; i < 2; i++) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, windowModes[i]) != Z_OK)
      continue;

    zs.next_in = const_cast<Bytef*>(compressed);
    zs.avail_in = (uInt)compressedSize;
    zs.next_out = out.data();
    zs.avail_out = (uInt)uncompressedSize;

    int rc = Z_OK;
    int guard = 0;
    while ((rc == Z_OK || rc == Z_BUF_ERROR) &&
           zs.total_out < uncompressedSize && guard++ < 64) {
      rc = inflate(&zs, Z_SYNC_FLUSH);
      if (rc == Z_STREAM_END)
        break;
    }

    bool ok = (zs.total_out == uncompressedSize);
    inflateEnd(&zs);
    if (ok)
      return out;
  }

  throw protocol_error(_("Failed to decompress file transfer data"));
}

CMsgReader::CMsgReader(CMsgHandler* handler_, rdr::InStream* is_)
  : handler(handler_), is(is_),
    state(MSGSTATE_IDLE), cursorEncoding(-1), tight2Inflate(nullptr)
{
}

CMsgReader::~CMsgReader()
{
  delete tight2Inflate;
}

std::vector<uint8_t> CMsgReader::inflateTight2Block(const uint8_t* compressed,
                                                      size_t compressedSize,
                                                      size_t uncompressedSize)
{
  if (compressedSize == 0 && uncompressedSize == 0)
    return std::vector<uint8_t>();

  if (tight2Inflate == nullptr)
    tight2Inflate = new Tight2InflateState();

  std::vector<uint8_t> out(uncompressedSize);
  uint8_t* outPtr = out.empty() ? nullptr : out.data();

  if (!tight2Inflate->initialized) {
    const int windowModes[2] = { 15, -15 };
    for (int i = 0; i < 2; i++) {
      memset(&tight2Inflate->zs, 0, sizeof(tight2Inflate->zs));
      if (inflateInit2(&tight2Inflate->zs, windowModes[i]) != Z_OK)
        continue;
      bool hitStreamEnd = false;
      if (runTight2InflateBlock(&tight2Inflate->zs, compressed, compressedSize,
                                outPtr, uncompressedSize, &hitStreamEnd)) {
        tight2Inflate->initialized = true;
        tight2Inflate->windowBits = windowModes[i];
        if (hitStreamEnd)
          inflateReset(&tight2Inflate->zs);
        return out;
      }
      inflateEnd(&tight2Inflate->zs);
    }
    throw protocol_error(_("Failed to decompress file transfer data"));
  }

  bool hitStreamEnd = false;
  if (runTight2InflateBlock(&tight2Inflate->zs, compressed, compressedSize,
                            outPtr, uncompressedSize, &hitStreamEnd)) {
    if (hitStreamEnd)
      inflateReset(&tight2Inflate->zs);
    return out;
  }

  throw protocol_error(_("Failed to decompress file transfer data"));
}

bool CMsgReader::readServerInit()
{
  int width, height;
  uint32_t len;

  if (!is->hasData(2 + 2 + 16 + 4))
    return false;

  is->setRestorePoint();

  width = is->readU16();
  height = is->readU16();

  PixelFormat pf;
  pf.read(is);

  len = is->readU32();
  if (!is->hasDataOrRestore(len))
    return false;
  std::vector<char> name(len + 1);
  is->readBytes((uint8_t*)name.data(), len);
  name[len] = '\0';

  std::vector<TightCapability> serverMsgs;
  std::vector<TightCapability> clientMsgs;
  std::vector<TightCapability> encodings;

  if (handler->server.tightEnabled) {
    if (!is->hasDataOrRestore(8))
      return false;

    uint16_t nSrv = is->readU16();
    uint16_t nCli = is->readU16();
    uint16_t nEnc = is->readU16();
    is->skip(2);

    size_t totalCaps = (size_t)nSrv + nCli + nEnc;
    if (totalCaps > 4096) {
      is->clearRestorePoint();
      throw protocol_error(_("Too many Tight interaction capabilities"));
    }

    if (!is->hasDataOrRestore(totalCaps * 16))
      return false;

    auto readCaps = [this](uint16_t count,
                           std::vector<TightCapability>& out) {
      out.resize(count);
      for (uint16_t i = 0; i < count; i++) {
        out[i].code = is->readS32();
        is->readBytes((uint8_t*)out[i].vendor, 4);
        out[i].vendor[4] = '\0';
        is->readBytes((uint8_t*)out[i].signature, 8);
        out[i].signature[8] = '\0';
      }
    };

    readCaps(nSrv, serverMsgs);
    readCaps(nCli, clientMsgs);
    readCaps(nEnc, encodings);
  }

  is->clearRestorePoint();

  if (handler->server.tightEnabled)
    handler->serverTightCaps(serverMsgs, clientMsgs, encodings);

  if (core::isValidUTF8(name.data()))
    handler->serverInit(width, height, pf, name.data());
  else
    handler->serverInit(width, height, pf,
                        core::latin1ToUTF8(name.data()).c_str());

  return true;
}

bool CMsgReader::readQEMUServerMessage()
{
  uint8_t submessage;
  uint16_t operation;
  uint32_t length;

  if (!is->hasData(1 + 2))
    return false;

  is->setRestorePoint();

  submessage = is->readU8();
  operation = is->readU16();

  // There is no generic length field here, so anything we do not
  // recognise leaves us unable to find the next message
  if (submessage != qemuAudio)
    throw protocol_error(core::format(_("Unknown QEMU submessage %d"),
                                      (int)submessage));

  switch (operation) {
  case msgFromQemuAudioBegin:
    is->clearRestorePoint();
    handler->audioBegin();
    return true;
  case msgFromQemuAudioEnd:
    is->clearRestorePoint();
    handler->audioEnd();
    return true;
  case msgFromQemuAudioData:
    break;
  default:
    throw protocol_error(core::format(_("Unknown QEMU audio operation %d"),
                                      (int)operation));
  }

  if (!is->hasDataOrRestore(4))
    return false;

  length = is->readU32();
  if (length > maxAudioData)
    throw protocol_error(_("Audio data is too large"));

  if (!is->hasDataOrRestore(length))
    return false;

  is->clearRestorePoint();

  handler->audioData(is->getptr(length), length);
  is->setptr(length);

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

bool CMsgReader::readGIIServerMessage()
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
  case giiServerVersion:
    if (length != 4)
      throw protocol_error(_("Invalid GII version message"));
    {
      uint16_t maxVersion = readEU16(is, bigEndian);
      uint16_t minVersion = readEU16(is, bigEndian);
      handler->giiServerVersion(maxVersion, minVersion);
    }
    break;
  case giiServerDeviceCreateResponse:
    if (length != 4)
      throw protocol_error(_("Invalid GII device creation response"));
    {
      uint32_t origin = readEU32(is, bigEndian);
      handler->giiDeviceCreated(origin);
    }
    break;
  default:
    vlog.error(_("Unknown GII server submessage type %d"), (int)subType);
    is->skip(length);
    break;
  }

  return true;
}

bool CMsgReader::readMsg()
{
  if (state == MSGSTATE_IDLE) {
    if (!is->hasData(1))
      return false;

    currentMsgType = is->readU8();
    state = MSGSTATE_MESSAGE;
  }

  if (currentMsgType != msgTypeFramebufferUpdate) {
    bool ret;

    switch (currentMsgType) {
    case msgTypeSetColourMapEntries:
      ret = readSetColourMapEntries();
      break;
    case msgTypeBell:
      ret = readBell();
      break;
    case msgTypeServerCutText:
      ret = readServerCutText();
      break;
    case msgTypeFileTransfer:
      ret = readUltraFileTransfer();
      break;
    case msgTypeTightFileListData:
      ret = readTight13FileListData();
      break;
    case msgTypeTightFileDownloadData:
      ret = readTight13FileDownloadData();
      break;
    case msgTypeTightFileUploadCancel:
      ret = readTight13FileUploadCancel();
      break;
    case msgTypeTightFileDownloadFailed:
      ret = readTight13FileDownloadFailed();
      break;
    case msgTypeFramebufferUpdate:
      ret = readFramebufferUpdate();
      break;
    case msgTypeServerFence:
      ret = readFence();
      break;
    case msgTypeEndOfContinuousUpdates:
      ret = readEndOfContinuousUpdates();
      break;
    case msgTypeTightExt:
      ret = readTight2Message();
      break;
    case msgTypeQEMUServerMessage:
      ret = readQEMUServerMessage();
      break;
    case msgTypeGIIServerMessage:
      ret = readGIIServerMessage();
      break;
    default:
      throw protocol_error(
        core::format(_("Unknown message type %d"), currentMsgType));
    }

    if (ret)
      state = MSGSTATE_IDLE;

    return ret;
  } else {
    if (state == MSGSTATE_MESSAGE) {
      if (!readFramebufferUpdate())
        return false;

      // Empty update?
      if (nUpdateRectsLeft == 0) {
        state = MSGSTATE_IDLE;
        handler->framebufferUpdateEnd();
        return true;
      }

      state = MSGSTATE_RECT_HEADER;
    }

    if (state == MSGSTATE_RECT_HEADER) {
      if (!is->hasData(2 + 2 + 2 + 2 + 4))
        return false;

      int x = is->readU16();
      int y = is->readU16();
      int w = is->readU16();
      int h = is->readU16();

      dataRect.setXYWH(x, y, w, h);

      rectEncoding = is->readS32();

      state = MSGSTATE_RECT_DATA;
    }

    bool ret;

    switch (rectEncoding) {
    case pseudoEncodingLastRect:
      nUpdateRectsLeft = 1;     // this rectangle is the last one
      ret = true;
      break;
    case pseudoEncodingXCursor:
      ret = readSetXCursor(dataRect.width(), dataRect.height(), dataRect.tl);
      break;
    case pseudoEncodingCursor:
      ret = readSetCursor(dataRect.width(), dataRect.height(), dataRect.tl);
      break;
    case pseudoEncodingCursorWithAlpha:
      ret = readSetCursorWithAlpha(dataRect.width(), dataRect.height(), dataRect.tl);
      break;
    case pseudoEncodingVMwareCursor:
      ret = readSetVMwareCursor(dataRect.width(), dataRect.height(), dataRect.tl);
      break;
    case pseudoEncodingVMwareCursorPosition:
      handler->setCursorPos(dataRect.tl);
      ret = true;
      break;
    case pseudoEncodingDesktopName:
      ret = readSetDesktopName(dataRect.tl.x, dataRect.tl.y,
                               dataRect.width(), dataRect.height());
      break;
    case pseudoEncodingDesktopSize:
      handler->setDesktopSize(dataRect.width(), dataRect.height());
      ret = true;
      break;
    case pseudoEncodingExtendedDesktopSize:
      ret = readExtendedDesktopSize(dataRect.tl.x, dataRect.tl.y,
                                    dataRect.width(), dataRect.height());
      break;
    case pseudoEncodingLEDState:
      ret = readLEDState();
      break;
    case pseudoEncodingVMwareLEDState:
      ret = readVMwareLEDState();
      break;
    case pseudoEncodingQEMUKeyEvent:
      handler->supportsQEMUKeyEvent();
      ret = true;
      break;
    case pseudoEncodingQEMUAudio:
      handler->supportsQEMUAudio();
      ret = true;
      break;
    case pseudoEncodingExtendedMouseButtons:
      handler->supportsExtendedMouseButtons();
      ret = true;
      break;
    case pseudoEncodingSupportedMessages:
      ret = readSupportedMessages();
      break;
    default:
      ret = readRect(dataRect, rectEncoding);
      break;
    };

    if (ret) {
      state = MSGSTATE_RECT_HEADER;
      nUpdateRectsLeft--;
      if (nUpdateRectsLeft == 0) {
        state = MSGSTATE_IDLE;
        handler->framebufferUpdateEnd();
      }
    }

    return ret;
  }
}

bool CMsgReader::readSetColourMapEntries()
{
  if (!is->hasData(1 + 2 + 2))
    return false;

  is->setRestorePoint();

  is->skip(1);
  int firstColour = is->readU16();
  int nColours = is->readU16();

  if (!is->hasDataOrRestore(nColours * 3 * 2))
    return false;
  is->clearRestorePoint();

  std::vector<uint16_t> rgbs(nColours * 3);
  for (size_t i = 0; i < rgbs.size(); i++)
    rgbs[i] = is->readU16();
  handler->setColourMapEntries(firstColour, nColours, rgbs.data());

  return true;
}

bool CMsgReader::readBell()
{
  handler->bell();
  return true;
}

bool CMsgReader::readServerCutText()
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

  handler->serverCutText(filtered.c_str());

  return true;
}

bool CMsgReader::readExtendedClipboard(int32_t len)
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
            throw protocol_error(
              _("Invalid extended clipboard message"));

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

bool CMsgReader::readFence()
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

bool CMsgReader::readEndOfContinuousUpdates()
{
  handler->endOfContinuousUpdates();
  return true;
}

bool CMsgReader::readFramebufferUpdate()
{
  if (!is->hasData(1 + 2))
    return false;

  is->skip(1);
  nUpdateRectsLeft = is->readU16();
  handler->framebufferUpdateStart();

  return true;
}

bool CMsgReader::readRect(const core::Rect& r, int encoding)
{
  if ((r.br.x > handler->server.width()) ||
      (r.br.y > handler->server.height())) {
    vlog.error(
      _("Invalid rectangle received: %dx%d at %d,%d exceeds %dx%d"),
      r.width(), r.height(), r.tl.x, r.tl.y,
      handler->server.width(), handler->server.height());
    throw protocol_error(_("Invalid rectangle received"));
  }

  if (r.is_empty())
    vlog.error(_("Empty rectangle received"));

  return handler->dataRect(r, encoding);
}

bool CMsgReader::readSetXCursor(int width, int height,
                                const core::Point& hotspot)
{
  if (width > maxCursorSize || height > maxCursorSize)
    throw protocol_error(_("Received cursor is too large"));

  std::vector<uint8_t> rgba(width*height*4);

  if (width * height > 0) {
    uint8_t pr, pg, pb;
    uint8_t sr, sg, sb;
    int data_len = ((width+7)/8) * height;
    int mask_len = ((width+7)/8) * height;
    std::vector<uint8_t> data(data_len);
    std::vector<uint8_t> mask(mask_len);

    int x, y;
    uint8_t* out;

    if (!is->hasData(3 + 3 + data_len + mask_len))
      return false;

    pr = is->readU8();
    pg = is->readU8();
    pb = is->readU8();

    sr = is->readU8();
    sg = is->readU8();
    sb = is->readU8();

    is->readBytes(data.data(), data.size());
    is->readBytes(mask.data(), mask.size());

    int maskBytesPerRow = (width+7)/8;
    out = rgba.data();
    for (y = 0;y < height;y++) {
      for (x = 0;x < width;x++) {
        int byte = y * maskBytesPerRow + x / 8;
        int bit = 7 - x % 8;

        if (data[byte] & (1 << bit)) {
          out[0] = pr;
          out[1] = pg;
          out[2] = pb;
        } else {
          out[0] = sr;
          out[1] = sg;
          out[2] = sb;
        }

        if (mask[byte] & (1 << bit))
          out[3] = 255;
        else
          out[3] = 0;

        out += 4;
      }
    }
  }

  handler->setCursor(width, height, hotspot, rgba.data());

  return true;
}

bool CMsgReader::readSetCursor(int width, int height,
                               const core::Point& hotspot)
{
  if (width > maxCursorSize || height > maxCursorSize)
    throw protocol_error(_("Received cursor is too large"));

  int data_len = width * height * (handler->server.pf().bpp/8);
  int mask_len = ((width+7)/8) * height;
  std::vector<uint8_t> data(data_len);
  std::vector<uint8_t> mask(mask_len);

  int x, y;
  std::vector<uint8_t> rgba(width*height*4);
  uint8_t* in;
  uint8_t* out;

  if (!is->hasData(data_len + mask_len))
    return false;

  is->readBytes(data.data(), data.size());
  is->readBytes(mask.data(), mask.size());

  int maskBytesPerRow = (width+7)/8;
  in = data.data();
  out = rgba.data();
  for (y = 0;y < height;y++) {
    for (x = 0;x < width;x++) {
      int byte = y * maskBytesPerRow + x / 8;
      int bit = 7 - x % 8;

      handler->server.pf().rgbFromBuffer(out, in, 1);

      if (mask[byte] & (1 << bit))
        out[3] = 255;
      else
        out[3] = 0;

      in += handler->server.pf().bpp/8;
      out += 4;
    }
  }

  handler->setCursor(width, height, hotspot, rgba.data());

  return true;
}

bool CMsgReader::readSetCursorWithAlpha(int width, int height,
                                        const core::Point& hotspot)
{
  if (width > maxCursorSize || height > maxCursorSize)
    throw protocol_error(_("Received cursor is too large"));

  const PixelFormat rgbaPF(32, 32, false, true, 255, 255, 255, 16, 8, 0);
  ManagedPixelBuffer pb(rgbaPF, width, height);

  bool ret;

  uint8_t* buf;
  int stride;

  // We can't use restore points as the decoder likely wants to as well, so
  // we need to keep track of the read encoding

  if (cursorEncoding == -1) {
    if (!is->hasData(4))
      return false;

    cursorEncoding = is->readS32();
  }

  ret = handler->readAndDecodeRect(pb.getRect(), cursorEncoding,
                                   rgbaPF, &pb);
  if (!ret)
    return false;

  cursorEncoding = -1;

  // On-wire data has pre-multiplied alpha, but we store it
  // non-pre-multiplied
  buf = pb.getBufferRW(pb.getRect(), &stride);
  assert(stride == width);

  for (int i = 0;i < pb.area();i++) {
    uint8_t alpha;

    alpha = buf[3];
    if (alpha == 0)
      alpha = 1; // Avoid division by zero

    buf[0] = (unsigned)buf[0] * 255/alpha;
    buf[1] = (unsigned)buf[1] * 255/alpha;
    buf[2] = (unsigned)buf[2] * 255/alpha;

    buf += 4;
  }

  pb.commitBufferRW(pb.getRect());

  handler->setCursor(width, height, hotspot,
                     pb.getBuffer(pb.getRect(), &stride));

  return true;
}

bool CMsgReader::readSetVMwareCursor(int width, int height,
                                     const core::Point& hotspot)
{
  if (width > maxCursorSize || height > maxCursorSize)
    throw protocol_error(_("Received cursor is too large"));

  uint8_t type;

  if (!is->hasData(1 + 1))
    return false;

  is->setRestorePoint();

  type = is->readU8();
  is->skip(1);

  if (type == 0) {
    int len = width * height * (handler->server.pf().bpp/8);
    std::vector<uint8_t> andMask(len);
    std::vector<uint8_t> xorMask(len);

    std::vector<uint8_t> data(width*height*4);

    uint8_t* andIn;
    uint8_t* xorIn;
    uint8_t* out;
    int Bpp;

    if (!is->hasDataOrRestore(len + len))
      return false;
    is->clearRestorePoint();

    is->readBytes(andMask.data(), andMask.size());
    is->readBytes(xorMask.data(), xorMask.size());

    andIn = andMask.data();
    xorIn = xorMask.data();
    out = data.data();
    Bpp = handler->server.pf().bpp/8;
    for (int y = 0;y < height;y++) {
      for (int x = 0;x < width;x++) {
        Pixel andPixel, xorPixel;

        andPixel = handler->server.pf().pixelFromBuffer(andIn);
        xorPixel = handler->server.pf().pixelFromBuffer(xorIn);
        andIn += Bpp;
        xorIn += Bpp;

        if (andPixel == 0) {
          uint8_t r, g, b;

          // Opaque pixel

          handler->server.pf().rgbFromPixel(xorPixel, &r, &g, &b);
          *out++ = r;
          *out++ = g;
          *out++ = b;
          *out++ = 0xff;
        } else if (xorPixel == 0) {
          // Fully transparent pixel
          *out++ = 0;
          *out++ = 0;
          *out++ = 0;
          *out++ = 0;
        } else if (andPixel == xorPixel) {
          // Inverted pixel

          // We don't really support this, so just turn the pixel black
          // FIXME: Do an outline like WinVNC does?
          *out++ = 0;
          *out++ = 0;
          *out++ = 0;
          *out++ = 0xff;
        } else {
          // Partially transparent/inverted pixel

          // We _really_ can't handle this, just make it black
          *out++ = 0;
          *out++ = 0;
          *out++ = 0;
          *out++ = 0xff;
        }
      }
    }

    handler->setCursor(width, height, hotspot, data.data());
  } else if (type == 1) {
    std::vector<uint8_t> data(width*height*4);

    if (!is->hasDataOrRestore(width*height*4))
      return false;
    is->clearRestorePoint();

    // FIXME: Is alpha premultiplied?
    is->readBytes(data.data(), data.size());

    handler->setCursor(width, height, hotspot, data.data());
  } else {
    throw protocol_error(_("Unknown cursor type"));
  }

  return true;
}

bool CMsgReader::readSetDesktopName(int x, int y, int w, int h)
{
  uint32_t len;

  if (!is->hasData(4))
    return false;

  is->setRestorePoint();

  len = is->readU32();

  if (!is->hasDataOrRestore(len))
    return false;
  is->clearRestorePoint();

  std::vector<char> name(len + 1);
  is->readBytes((uint8_t*)name.data(), len);
  name[len] = '\0';

  if (x || y || w || h) {
    vlog.error(_("Invalid desktop name received"));
    return true;
  }

  if (!core::isValidUTF8(name.data())) {
    vlog.error(_("Invalid desktop name received"));
    return true;
  }

  handler->setName(name.data());

  return true;
}

bool CMsgReader::readExtendedDesktopSize(int x, int y, int w, int h)
{
  unsigned int screens, i;
  uint32_t id, flags;
  int sx, sy, sw, sh;
  ScreenSet layout;

  if (!is->hasData(1 + 3))
    return false;

  is->setRestorePoint();

  screens = is->readU8();
  is->skip(3);

  if (!is->hasDataOrRestore(16 * screens))
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

  handler->setExtendedDesktopSize(x, y, w, h, layout);

  return true;
}

bool CMsgReader::readLEDState()
{
  uint8_t ledState;

  if (!is->hasData(1))
    return false;

  ledState = is->readU8();

  handler->setLEDState(ledState);

  return true;
}

bool CMsgReader::readVMwareLEDState()
{
  uint32_t ledState;

  if (!is->hasData(4))
    return false;

  ledState = is->readU32();

  // As luck has it, this extension uses the same bit definitions,
  // so no conversion required

  handler->setLEDState(ledState);

  return true;
}

bool CMsgReader::readSupportedMessages()
{
  if (!is->hasData(64))
    return false;

  uint8_t c2s[32];
  uint8_t s2c[32];
  is->readBytes(c2s, 32);
  is->readBytes(s2c, 32);

  handler->supportedMessages(c2s, s2c);
  return true;
}

bool CMsgReader::readUltraFileTransfer()
{
  if (!is->hasData(1 + 2 + 4 + 4))
    return false;

  is->setRestorePoint();

  uint8_t contentType = is->readU8();
  uint8_t paramLo = is->readU8();
  uint8_t paramHi = is->readU8();
  uint16_t contentParam = (uint16_t)paramLo | ((uint16_t)paramHi << 8);
  uint32_t size = is->readU32();
  uint32_t length = is->readU32();

  if (length > maxFileTransferPayload) {
    is->clearRestorePoint();
    throw protocol_error(_("UltraVNC file transfer payload is too large"));
  }

  bool hasSizeH = (contentType == ultraFileHeader) &&
                  (handler->server.fileTransferCaps.ultraVersion != 1);

  if (!is->hasDataOrRestore((size_t)length + (hasSizeH ? 4 : 0)))
    return false;

  std::vector<uint8_t> payload(length);
  if (length > 0)
    is->readBytes(payload.data(), length);

  uint32_t sizeH = 0;
  if (hasSizeH)
    sizeH = is->readU32();

  is->clearRestorePoint();

  handler->ultraFileTransferMessage(contentType, contentParam,
                                    size, sizeH,
                                    payload.empty() ? nullptr : payload.data(),
                                    payload.size());
  return true;
}

bool CMsgReader::readTight13FileListData()
{
  if (!is->hasData(1 + 2 + 2 + 2))
    return false;

  is->setRestorePoint();

  uint8_t flags = is->readU8();
  uint16_t numFiles = is->readU16();
  uint16_t dataSize = is->readU16();
  uint16_t compressedSize = is->readU16();

  size_t sizesBytes = (size_t)numFiles * 8;
  if (!is->hasDataOrRestore(sizesBytes + compressedSize))
    return false;

  std::vector<FileEntry> entries(numFiles);
  for (uint16_t i = 0; i < numFiles; i++) {
    uint32_t rawSize = is->readU32();
    uint32_t mtime = is->readU32();
    entries[i].isDir = (rawSize == tight13DirSize);
    entries[i].size = entries[i].isDir ? 0 : (uint64_t)rawSize;
    entries[i].mtime = (int64_t)mtime;
  }

  std::vector<uint8_t> rawNames(compressedSize);
  if (compressedSize > 0)
    is->readBytes(rawNames.data(), compressedSize);

  is->clearRestorePoint();

  std::vector<uint8_t> names;
  if ((flags & 0x0F) != 0 && compressedSize != dataSize)
    names = inflateTightBlock(rawNames.data(), rawNames.size(), dataSize);
  else
    names.swap(rawNames);

  size_t offset = 0;
  for (uint16_t i = 0; i < numFiles; i++) {
    if (offset < names.size()) {
      const char* s = (const char*)names.data() + offset;
      size_t maxlen = names.size() - offset;
      size_t slen = strnlen(s, maxlen);
      entries[i].name.assign(s, slen);
      offset += slen;
      if (offset < names.size() && names[offset] == '\0')
        offset++;
    }
  }

  handler->tight13FileListData(flags, entries);
  return true;
}

bool CMsgReader::readTight13FileDownloadData()
{
  if (!is->hasData(1 + 2 + 2))
    return false;

  is->setRestorePoint();

  uint8_t compressLevel = is->readU8();
  uint16_t realSize = is->readU16();
  uint16_t compressedSize = is->readU16();

  if (realSize == 0 && compressedSize == 0) {
    if (!is->hasDataOrRestore(4))
      return false;
    uint32_t modTime = is->readU32();
    is->clearRestorePoint();
    handler->tight13FileDownloadEnd(modTime);
    return true;
  }

  if (!is->hasDataOrRestore(compressedSize))
    return false;

  std::vector<uint8_t> rawData(compressedSize);
  if (compressedSize > 0)
    is->readBytes(rawData.data(), compressedSize);

  is->clearRestorePoint();

  if (compressLevel != 0 && compressedSize != realSize) {
    std::vector<uint8_t> out = inflateTightBlock(rawData.data(), rawData.size(),
                                                 realSize);
    handler->tight13FileDownloadData(out.data(), out.size());
  } else {
    handler->tight13FileDownloadData(rawData.data(), rawData.size());
  }

  return true;
}

bool CMsgReader::readTight13FileUploadCancel()
{
  if (!is->hasData(1 + 2))
    return false;

  is->setRestorePoint();
  is->skip(1);
  uint16_t reasonLen = is->readU16();

  if (!is->hasDataOrRestore(reasonLen))
    return false;

  std::string reason(reasonLen, '\0');
  if (reasonLen > 0)
    is->readBytes((uint8_t*)&reason[0], reasonLen);
  is->clearRestorePoint();

  handler->tight13FileUploadCancel(reason);
  return true;
}

bool CMsgReader::readTight13FileDownloadFailed()
{
  if (!is->hasData(1 + 2))
    return false;

  is->setRestorePoint();
  is->skip(1);
  uint16_t reasonLen = is->readU16();

  if (!is->hasDataOrRestore(reasonLen))
    return false;

  std::string reason(reasonLen, '\0');
  if (reasonLen > 0)
    is->readBytes((uint8_t*)&reason[0], reasonLen);
  is->clearRestorePoint();

  handler->tight13FileDownloadFailed(reason);
  return true;
}

bool CMsgReader::readTight2Message()
{
  if (!is->hasData(3))
    return false;

  is->setRestorePoint();

  uint8_t b1 = is->readU8();
  uint8_t b2 = is->readU8();
  uint8_t b3 = is->readU8();
  uint32_t msgId = ((uint32_t)msgTypeTightExt << 24) |
                   ((uint32_t)b1 << 16) |
                   ((uint32_t)b2 << 8) |
                   (uint32_t)b3;

  switch (msgId) {
  case tight2CompressionSupportReply: {
    if (!is->hasDataOrRestore(1))
      return false;
    uint8_t supported = is->readU8();
    is->clearRestorePoint();
    handler->tight2CompressionSupportReply(supported != 0);
    return true;
  }

  case tight2FileListReply:
  case tight2DownloadDataReply: {
    if (!is->hasDataOrRestore(1 + 4 + 4))
      return false;

    uint8_t level = is->readU8();
    uint32_t csize = is->readU32();
    uint32_t usize = is->readU32();

    if (csize > maxFileTransferPayload || usize > maxFileTransferPayload) {
      is->clearRestorePoint();
      throw protocol_error(_("TightVNC file transfer block is too large"));
    }

    if (!is->hasDataOrRestore(csize))
      return false;

    std::vector<uint8_t> raw(csize);
    if (csize > 0)
      is->readBytes(raw.data(), csize);

    is->clearRestorePoint();

    std::vector<uint8_t> block;
    if (level != 0 && (csize > 0 || usize > 0))
      block = inflateTight2Block(raw.data(), raw.size(), usize);
    else
      block.swap(raw);

    if (msgId == tight2DownloadDataReply) {
      handler->tight2DownloadDataReply(block.empty() ? nullptr : block.data(),
                                       block.size());
      return true;
    }

    std::vector<FileEntry> entries;
    if (!block.empty()) {
      if (block.size() < 4)
        throw protocol_error(_("Truncated TightVNC file list"));
      uint32_t count = readBE32FromBuf(block.data());
      size_t off = 4;
      constexpr size_t entryHeaderSize = 8 + 8 + 2 + 4;
      if (count > (block.size() - off) / entryHeaderSize)
        throw protocol_error(_("Truncated TightVNC file list"));
      entries.reserve(count);
      for (uint32_t i = 0; i < count; i++) {
        if (entryHeaderSize > block.size() - off)
          throw protocol_error(_("Truncated TightVNC file list entry"));
        FileEntry entry;
        entry.size = readBE64FromBuf(block.data() + off);
        off += 8;
        uint64_t mtimeMs = readBE64FromBuf(block.data() + off);
        off += 8;
        uint16_t flags = readBE16FromBuf(block.data() + off);
        off += 2;
        uint32_t nameLen = readBE32FromBuf(block.data() + off);
        off += 4;
        if (nameLen > block.size() - off)
          throw protocol_error(_("Truncated TightVNC file name"));
        entry.name.assign((const char*)block.data() + off, nameLen);
        off += nameLen;
        while (!entry.name.empty() && entry.name.back() == '\0')
          entry.name.pop_back();
        entry.mtime = (int64_t)(mtimeMs / 1000ULL);
        entry.isDir = (flags & tight2FlagDir) != 0;
        entry.isExecutable = (flags & tight2FlagExec) != 0;
        entries.push_back(entry);
      }
    }
    handler->tight2FileListReply(entries);
    return true;
  }

  case tight2Md5Reply: {
    if (!is->hasDataOrRestore(16))
      return false;
    uint8_t digest[16];
    is->readBytes(digest, 16);
    is->clearRestorePoint();
    handler->tight2Md5Reply(digest);
    return true;
  }

  case tight2UploadStartReply:
    is->clearRestorePoint();
    handler->tight2UploadStartReply();
    return true;

  case tight2UploadDataReply:
    is->clearRestorePoint();
    handler->tight2UploadDataReply();
    return true;

  case tight2UploadEndReply:
    is->clearRestorePoint();
    handler->tight2UploadEndReply();
    return true;

  case tight2DownloadStartReply:
    is->clearRestorePoint();
    handler->tight2DownloadStartReply();
    return true;

  case tight2DownloadEndReply: {
    if (!is->hasDataOrRestore(1 + 8))
      return false;
    uint8_t flags = is->readU8();
    uint64_t modTimeMs = readU64(is);
    is->clearRestorePoint();
    handler->tight2DownloadEndReply(flags, modTimeMs);
    return true;
  }

  case tight2MkdirReply:
    is->clearRestorePoint();
    handler->tight2MkdirReply();
    return true;

  case tight2RemoveReply:
    is->clearRestorePoint();
    handler->tight2RemoveReply();
    return true;

  case tight2RenameReply:
    is->clearRestorePoint();
    handler->tight2RenameReply();
    return true;

  case tight2DirSizeReply: {
    if (!is->hasDataOrRestore(8))
      return false;
    uint64_t dirSize = readU64(is);
    is->clearRestorePoint();
    handler->tight2DirSizeReply(dirSize);
    return true;
  }

  case tight2LastRequestFailedReply: {
    if (!is->hasDataOrRestore(4))
      return false;
    uint32_t msgLen = is->readU32();
    if (msgLen > maxFileTransferPayload) {
      is->clearRestorePoint();
      throw protocol_error(_("TightVNC error message is too large"));
    }
    if (!is->hasDataOrRestore(msgLen))
      return false;
    std::string msg(msgLen, '\0');
    if (msgLen > 0)
      is->readBytes((uint8_t*)&msg[0], msgLen);
    is->clearRestorePoint();
    while (!msg.empty() && msg.back() == '\0')
      msg.pop_back();
    handler->tight2LastRequestFailedReply(msg);
    return true;
  }

  default:
    is->clearRestorePoint();
    throw protocol_error(
      core::format(_("Unknown TightVNC extension message 0x%08x"), msgId));
  }
}
