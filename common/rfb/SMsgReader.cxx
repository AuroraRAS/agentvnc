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
#include <string.h>

#include <vector>

#include <zlib.h>

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
#include <rfb/fileTransferTypes.h>
#include <rfb/TightCapability.h>
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

static const uint32_t maxFileTransferPayload = 16*1024*1024;
static const uint32_t maxTight2StringLen = 65536;

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

static uint64_t readU64(rdr::InStream* is)
{
  uint64_t hi = is->readU32();
  uint64_t lo = is->readU32();
  return (hi << 32) | lo;
}

static bool readTight2String(rdr::InStream* is, std::string* out)
{
  if (!is->hasDataOrRestore(4))
    return false;
  uint32_t len = is->readU32();
  if (len > maxTight2StringLen) {
    is->clearRestorePoint();
    throw protocol_error(_("TightVNC string is too long"));
  }
  if (!is->hasDataOrRestore(len))
    return false;
  out->assign(len, '\0');
  if (len > 0)
    is->readBytes(reinterpret_cast<uint8_t*>(&(*out)[0]), len);
  return true;
}

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

  // Drain any trailing Z_SYNC_FLUSH marker or Z_FINISH trailer
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
    zs.avail_in = static_cast<uInt>(compressedSize);
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(uncompressedSize);

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

SMsgReader::SMsgReader(SMsgHandler* handler_, rdr::InStream* is_)
  : handler(handler_), is(is_), state(MSGSTATE_IDLE),
    currentMsgType(0), tight2Inflate(nullptr)
{
}

SMsgReader::~SMsgReader()
{
  delete tight2Inflate;
}

std::vector<uint8_t> SMsgReader::inflateTight2Block(const uint8_t* compressed,
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
  case msgTypeTightFileListRequest:
    if (!handler->client.tightEnabled) {
      vlog.error(_("Unknown message type %d"), currentMsgType);
      throw protocol_error(_("Unknown message type"));
    }
    ret = readTight13FileListRequest();
    break;
  case msgTypeTightFileDownloadRequest:
    if (!handler->client.tightEnabled) {
      vlog.error(_("Unknown message type %d"), currentMsgType);
      throw protocol_error(_("Unknown message type"));
    }
    ret = readTight13FileDownloadRequest();
    break;
  case msgTypeTightFileUploadRequest:
    if (!handler->client.tightEnabled) {
      vlog.error(_("Unknown message type %d"), currentMsgType);
      throw protocol_error(_("Unknown message type"));
    }
    ret = readTight13FileUploadRequest();
    break;
  case msgTypeTightFileUploadData:
    if (!handler->client.tightEnabled) {
      vlog.error(_("Unknown message type %d"), currentMsgType);
      throw protocol_error(_("Unknown message type"));
    }
    ret = readTight13FileUploadData();
    break;
  case msgTypeTightFileDownloadCancel:
    if (!handler->client.tightEnabled) {
      vlog.error(_("Unknown message type %d"), currentMsgType);
      throw protocol_error(_("Unknown message type"));
    }
    ret = readTight13FileDownloadCancel();
    break;
  case msgTypeTightFileUploadFailed:
    if (!handler->client.tightEnabled) {
      vlog.error(_("Unknown message type %d"), currentMsgType);
      throw protocol_error(_("Unknown message type"));
    }
    ret = readTight13FileUploadFailed();
    break;
  case msgTypeTightFileCreateDirRequest:
    if (!handler->client.tightEnabled) {
      vlog.error(_("Unknown message type %d"), currentMsgType);
      throw protocol_error(_("Unknown message type"));
    }
    ret = readTight13FileCreateDirRequest();
    break;
  case msgTypeTightExt:
    if (!handler->client.tightEnabled) {
      vlog.error(_("Unknown message type %d"), currentMsgType);
      throw protocol_error(_("Unknown message type"));
    }
    ret = readTight2Message();
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

bool SMsgReader::readTight13FileListRequest()
{
  if (!is->hasData(1 + 2))
    return false;

  is->setRestorePoint();
  uint8_t flags = is->readU8();
  uint16_t dirLen = is->readU16();
  if (!is->hasDataOrRestore(dirLen))
    return false;

  std::string dir(dirLen, '\0');
  if (dirLen > 0)
    is->readBytes(reinterpret_cast<uint8_t*>(&dir[0]), dirLen);
  is->clearRestorePoint();

  handler->tight13FileListRequest(flags, dir);
  return true;
}

bool SMsgReader::readTight13FileDownloadRequest()
{
  if (!is->hasData(1 + 2 + 4))
    return false;

  is->setRestorePoint();
  uint8_t compressLevel = is->readU8();
  uint16_t pathLen = is->readU16();
  uint32_t position = is->readU32();
  if (!is->hasDataOrRestore(pathLen))
    return false;

  std::string path(pathLen, '\0');
  if (pathLen > 0)
    is->readBytes(reinterpret_cast<uint8_t*>(&path[0]), pathLen);
  is->clearRestorePoint();

  handler->tight13FileDownloadRequest(compressLevel, position, path);
  return true;
}

bool SMsgReader::readTight13FileUploadRequest()
{
  if (!is->hasData(1 + 2 + 4))
    return false;

  is->setRestorePoint();
  uint8_t compressLevel = is->readU8();
  uint16_t pathLen = is->readU16();
  uint32_t position = is->readU32();
  if (!is->hasDataOrRestore(pathLen))
    return false;

  std::string path(pathLen, '\0');
  if (pathLen > 0)
    is->readBytes(reinterpret_cast<uint8_t*>(&path[0]), pathLen);
  is->clearRestorePoint();

  handler->tight13FileUploadRequest(compressLevel, position, path);
  return true;
}

bool SMsgReader::readTight13FileUploadData()
{
  if (!is->hasData(1 + 2 + 2))
    return false;

  is->setRestorePoint();
  uint8_t compressLevel = is->readU8();
  uint16_t realSize = is->readU16();
  uint16_t compSize = is->readU16();

  if (realSize == 0 && compSize == 0) {
    if (!is->hasDataOrRestore(4))
      return false;
    uint32_t modTime = is->readU32();
    is->clearRestorePoint();
    handler->tight13FileUploadEnd(modTime);
    return true;
  }

  if (!is->hasDataOrRestore(compSize))
    return false;

  std::vector<uint8_t> rawData(compSize);
  if (compSize > 0)
    is->readBytes(rawData.data(), compSize);
  is->clearRestorePoint();

  if (compressLevel != 0 && compSize != realSize) {
    std::vector<uint8_t> out = inflateTightBlock(rawData.data(), rawData.size(),
                                                 realSize);
    handler->tight13FileUploadData(out.empty() ? nullptr : out.data(),
                                   out.size());
  } else {
    handler->tight13FileUploadData(rawData.empty() ? nullptr : rawData.data(),
                                   rawData.size());
  }
  return true;
}

bool SMsgReader::readTight13FileDownloadCancel()
{
  if (!is->hasData(1 + 2))
    return false;

  is->setRestorePoint();
  is->skip(1);
  uint16_t len = is->readU16();
  if (!is->hasDataOrRestore(len))
    return false;

  std::string reason(len, '\0');
  if (len > 0)
    is->readBytes(reinterpret_cast<uint8_t*>(&reason[0]), len);
  is->clearRestorePoint();

  handler->tight13FileDownloadCancel(reason);
  return true;
}

bool SMsgReader::readTight13FileUploadFailed()
{
  if (!is->hasData(1 + 2))
    return false;

  is->setRestorePoint();
  is->skip(1);
  uint16_t len = is->readU16();
  if (!is->hasDataOrRestore(len))
    return false;

  std::string reason(len, '\0');
  if (len > 0)
    is->readBytes(reinterpret_cast<uint8_t*>(&reason[0]), len);
  is->clearRestorePoint();

  handler->tight13FileUploadFailed(reason);
  return true;
}

bool SMsgReader::readTight13FileCreateDirRequest()
{
  if (!is->hasData(1 + 2))
    return false;

  is->setRestorePoint();
  is->skip(1);
  uint16_t len = is->readU16();
  if (!is->hasDataOrRestore(len))
    return false;

  std::string dir(len, '\0');
  if (len > 0)
    is->readBytes(reinterpret_cast<uint8_t*>(&dir[0]), len);
  is->clearRestorePoint();

  handler->tight13FileCreateDirRequest(dir);
  return true;
}

bool SMsgReader::readTight2Message()
{
  if (!is->hasData(3))
    return false;

  is->setRestorePoint();

  uint8_t b1 = is->readU8();
  uint8_t b2 = is->readU8();
  uint8_t b3 = is->readU8();
  uint32_t msgId = (static_cast<uint32_t>(msgTypeTightExt) << 24) |
                   (static_cast<uint32_t>(b1) << 16) |
                   (static_cast<uint32_t>(b2) << 8) |
                   static_cast<uint32_t>(b3);

  switch (msgId) {
  case tight2CompressionSupportReq:
    is->clearRestorePoint();
    handler->tight2CompressionSupportRequest();
    return true;

  case tight2FileListReq: {
    if (!is->hasDataOrRestore(1))
      return false;
    uint8_t compressLevel = is->readU8();
    std::string path;
    if (!readTight2String(is, &path))
      return false;
    is->clearRestorePoint();
    handler->tight2FileListRequest(compressLevel, path);
    return true;
  }

  case tight2Md5Req: {
    std::string path;
    if (!readTight2String(is, &path))
      return false;
    if (!is->hasDataOrRestore(8 + 8))
      return false;
    uint64_t offset = readU64(is);
    uint64_t dataLen = readU64(is);
    is->clearRestorePoint();
    handler->tight2Md5Request(path, offset, dataLen);
    return true;
  }

  case tight2UploadStartReq: {
    std::string path;
    if (!readTight2String(is, &path))
      return false;
    if (!is->hasDataOrRestore(1 + 8))
      return false;
    uint8_t flags = is->readU8();
    uint64_t offset = readU64(is);
    is->clearRestorePoint();
    handler->tight2UploadStartRequest(path, flags, offset);
    return true;
  }

  case tight2UploadDataReq: {
    if (!is->hasDataOrRestore(1 + 4 + 4))
      return false;
    uint8_t compressLevel = is->readU8();
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
    if (compressLevel != 0 && (csize > 0 || usize > 0))
      block = inflateTight2Block(raw.data(), raw.size(), usize);
    else
      block.swap(raw);

    handler->tight2UploadDataRequest(block.empty() ? nullptr : block.data(),
                                     block.size());
    return true;
  }

  case tight2UploadEndReq: {
    if (!is->hasDataOrRestore(2 + 8))
      return false;
    uint16_t fileFlags = is->readU16();
    uint64_t modTimeMs = readU64(is);
    is->clearRestorePoint();
    handler->tight2UploadEndRequest(fileFlags, modTimeMs);
    return true;
  }

  case tight2DownloadStartReq: {
    std::string path;
    if (!readTight2String(is, &path))
      return false;
    if (!is->hasDataOrRestore(8))
      return false;
    uint64_t offset = readU64(is);
    is->clearRestorePoint();
    handler->tight2DownloadStartRequest(path, offset);
    return true;
  }

  case tight2DownloadDataReq: {
    if (!is->hasDataOrRestore(1 + 4))
      return false;
    uint8_t compressLevel = is->readU8();
    uint32_t reqSize = is->readU32();
    is->clearRestorePoint();
    handler->tight2DownloadDataRequest(compressLevel, reqSize);
    return true;
  }

  case tight2MkdirReq: {
    std::string path;
    if (!readTight2String(is, &path))
      return false;
    is->clearRestorePoint();
    handler->tight2MkdirRequest(path);
    return true;
  }

  case tight2RemoveReq: {
    std::string path;
    if (!readTight2String(is, &path))
      return false;
    is->clearRestorePoint();
    handler->tight2RemoveRequest(path);
    return true;
  }

  case tight2RenameReq: {
    std::string oldPath;
    std::string newPath;
    if (!readTight2String(is, &oldPath))
      return false;
    if (!readTight2String(is, &newPath))
      return false;
    is->clearRestorePoint();
    handler->tight2RenameRequest(oldPath, newPath);
    return true;
  }

  case tight2DirSizeReq: {
    std::string path;
    if (!readTight2String(is, &path))
      return false;
    is->clearRestorePoint();
    handler->tight2DirSizeRequest(path);
    return true;
  }

  default:
    is->clearRestorePoint();
    throw protocol_error(
      core::format(_("Unknown TightVNC extension message 0x%08x"), msgId));
  }
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
