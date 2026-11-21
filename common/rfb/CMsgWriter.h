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
//
// CMsgWriter - class for writing RFB messages on the server side.
//

#ifndef __RFB_CMSGWRITER_H__
#define __RFB_CMSGWRITER_H__

#include <list>
#include <string>

#include <stdint.h>

namespace core {
  struct Point;
  struct Rect;
}

namespace rdr { class OutStream; }

namespace rfb {

  class PixelFormat;
  class ServerParams;
  struct ScreenSet;

  class CMsgWriter {
  public:
    CMsgWriter(ServerParams* server, rdr::OutStream* os);
    virtual ~CMsgWriter();

    void writeClientInit(bool shared);

    void writeSetPixelFormat(const PixelFormat& pf);
    void writeSetEncodings(const std::list<uint32_t> encodings);
    void writeSetDesktopSize(int width, int height, const ScreenSet& layout);

    void writeFramebufferUpdateRequest(const core::Rect& r,
                                       bool incremental);
    void writeEnableContinuousUpdates(bool enable, int x, int y, int w, int h);

    void writeFence(uint32_t flags, unsigned len, const uint8_t data[]);

    void writeKeyEvent(uint32_t keysym, uint32_t keycode, bool down);
    void writePointerEvent(const core::Point& pos, uint16_t buttonMask);

    void writeClientCutText(const char* str);

    void writeClipboardCaps(uint32_t caps, const uint32_t* lengths);
    void writeClipboardRequest(uint32_t flags);
    void writeClipboardPeek(uint32_t flags);
    void writeClipboardNotify(uint32_t flags);
    void writeClipboardProvide(uint32_t flags, const size_t* lengths,
                               const uint8_t* const* data);

    void writeQEMUAudioEnable(bool enable);
    void writeQEMUAudioSetFormat(uint8_t sampleFormat, uint8_t channels,
                                 uint32_t frequency);

    // TightVNC 1.3 File Transfer
    void writeTight13FileListRequest(uint8_t flags, const std::string& dir);
    void writeTight13FileDownloadRequest(uint8_t compressLevel,
                                         uint32_t position,
                                         const std::string& path);
    void writeTight13FileUploadRequest(uint8_t compressLevel,
                                       uint32_t position,
                                       const std::string& path);
    void writeTight13FileUploadData(uint8_t compressLevel,
                                    const uint8_t* data, uint16_t len);
    void writeTight13FileUploadEnd(uint32_t modTime);
    void writeTight13FileDownloadCancel(const std::string& reason);
    void writeTight13FileUploadFailed(const std::string& reason);
    void writeTight13FileCreateDirRequest(const std::string& dir);

    // TightVNC 2.x File Transfer
    void writeTight2CompressionSupportRequest();
    void writeTight2FileListRequest(uint8_t compressLevel,
                                    const std::string& path);
    void writeTight2DownloadStartRequest(const std::string& path,
                                         uint64_t offset);
    void writeTight2DownloadDataRequest(uint8_t compressLevel, uint32_t size);
    void writeTight2UploadStartRequest(const std::string& path,
                                       bool overwrite, uint64_t offset);
    void writeTight2UploadDataRequest(const uint8_t* data, uint32_t size);
    void writeTight2UploadEndRequest(uint16_t fileFlags, uint64_t modTimeMs);
    void writeTight2MkdirRequest(const std::string& path);
    void writeTight2RemoveRequest(const std::string& path);
    void writeTight2RenameRequest(const std::string& oldPath,
                                  const std::string& newPath);
    void writeTight2DirSizeRequest(const std::string& path);

    // UltraVNC File Transfer
    void writeUltraFileTransfer(uint8_t contentType, uint16_t contentParam,
                                uint32_t size, const uint8_t* data,
                                uint32_t length, bool hasSizeH = false,
                                uint32_t sizeH = 0);

  protected:
    void startMsg(int type);
    void endMsg();

    ServerParams* server;
    rdr::OutStream* os;
  };
}
#endif
