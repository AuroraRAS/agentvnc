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
// SMsgHandler - class to handle incoming messages on the server side.
//

#ifndef __RFB_SMSGHANDLER_H__
#define __RFB_SMSGHANDLER_H__

#include <stdint.h>

#include <string>
#include <vector>

#include <rfb/ClientParams.h>
#include <rfb/TightCapability.h>
#include <rfb/fileTransferTypes.h>

namespace rfb {

  class SMsgHandler {
  public:
    virtual ~SMsgHandler() {}

    // The following methods are called as corresponding messages are
    // read. A derived class must override these methods.

    virtual void clientInit(bool shared) = 0;

    virtual void setPixelFormat(const PixelFormat& pf) = 0;
    virtual void setEncodings(int nEncodings,
                              const int32_t* encodings) = 0;
    virtual void framebufferUpdateRequest(const core::Rect& r,
                                          bool incremental) = 0;
    virtual void setDesktopSize(int fb_width, int fb_height,
                                const ScreenSet& layout) = 0;
    virtual void fence(uint32_t flags, unsigned len,
                       const uint8_t data[]) = 0;
    virtual void enableContinuousUpdates(bool enable,
                                         int x, int y,
                                         int w, int h) = 0;

    virtual void keyEvent(uint32_t keysym, uint32_t keycode,
                          bool down) = 0;
    virtual void pointerEvent(const core::Point& pos,
                              uint16_t buttonMask) = 0;

    virtual void clientCutText(const char* str) = 0;

    virtual void clipboardCaps(uint32_t flags,
                               const uint32_t* lengths) = 0;
    virtual void clipboardRequest(uint32_t flags) = 0;
    virtual void clipboardPeek() = 0;
    virtual void clipboardNotify(uint32_t flags) = 0;
    virtual void clipboardProvide(uint32_t flags,
                                  const size_t* lengths,
                                  const uint8_t* const* data) = 0;

    // TightVNC 2.x File Transfer
    virtual void tight2CompressionSupportRequest() {}
    virtual void tight2FileListRequest(uint8_t /*compressLevel*/,
                                       const std::string& /*path*/) {}
    virtual void tight2Md5Request(const std::string& /*path*/,
                                  uint64_t /*offset*/,
                                  uint64_t /*dataLen*/) {}
    virtual void tight2UploadStartRequest(const std::string& /*path*/,
                                          uint8_t /*flags*/,
                                          uint64_t /*offset*/) {}
    virtual void tight2UploadDataRequest(const uint8_t* /*data*/,
                                         size_t /*len*/) {}
    virtual void tight2UploadEndRequest(uint16_t /*fileFlags*/,
                                        uint64_t /*modTimeMs*/) {}
    virtual void tight2DownloadStartRequest(const std::string& /*path*/,
                                            uint64_t /*offset*/) {}
    virtual void tight2DownloadDataRequest(uint8_t /*compressLevel*/,
                                           uint32_t /*reqSize*/) {}
    virtual void tight2MkdirRequest(const std::string& /*path*/) {}
    virtual void tight2RemoveRequest(const std::string& /*path*/) {}
    virtual void tight2RenameRequest(const std::string& /*oldPath*/,
                                     const std::string& /*newPath*/) {}
    virtual void tight2DirSizeRequest(const std::string& /*path*/) {}

    // TightVNC 1.3.x File Transfer
    virtual void tight13FileListRequest(uint8_t /*flags*/,
                                        const std::string& /*dir*/) {}
    virtual void tight13FileDownloadRequest(uint8_t /*compressLevel*/,
                                            uint32_t /*position*/,
                                            const std::string& /*path*/) {}
    virtual void tight13FileUploadRequest(uint8_t /*compressLevel*/,
                                          uint32_t /*position*/,
                                          const std::string& /*path*/) {}
    virtual void tight13FileUploadData(const uint8_t* /*data*/,
                                       size_t /*len*/) {}
    virtual void tight13FileUploadEnd(uint32_t /*modTime*/) {}
    virtual void tight13FileDownloadCancel(const std::string& /*reason*/) {}
    virtual void tight13FileUploadFailed(const std::string& /*reason*/) {}
    virtual void tight13FileCreateDirRequest(const std::string& /*dir*/) {}

    ClientParams client;
  };
}
#endif
