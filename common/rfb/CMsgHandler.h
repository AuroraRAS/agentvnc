/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright 2009-2019 Pierre Ossman for Cendio AB
 * Copyright (C) 2011 D. R. Commander.  All Rights Reserved.
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
// CMsgHandler - class to handle incoming messages on the client side.
//

#ifndef __RFB_CMSGHANDLER_H__
#define __RFB_CMSGHANDLER_H__

#include <stdint.h>

#include <string>
#include <vector>

#include <rfb/ServerParams.h>
#include <rfb/TightCapability.h>
#include <rfb/fileTransferTypes.h>

namespace core {
  struct Point;
  struct Rect;
}

namespace rfb {

  class ModifiablePixelBuffer;
  struct ScreenSet;

  class CMsgHandler {
  public:
    // The following methods are called as corresponding messages are
    // read. A derived class must override these methods.

    virtual void setDesktopSize(int w, int h) = 0;
    virtual void setExtendedDesktopSize(unsigned reason, unsigned result,
                                        int w, int h,
                                        const ScreenSet& layout) = 0;
    virtual void setCursor(int width, int height, const
                           core::Point& hotspot,
                           const uint8_t* data) = 0;
    virtual void setCursorPos(const core::Point& pos) = 0;
    virtual void setName(const char* name) = 0;
    virtual void fence(uint32_t flags, unsigned len,
                       const uint8_t data[]) = 0;
    virtual void endOfContinuousUpdates() = 0;
    virtual void supportsQEMUKeyEvent() = 0;
    virtual void supportsQEMUAudio() = 0;
    virtual void supportsExtendedMouseButtons() = 0;
    virtual void serverInit(int width, int height,
                            const PixelFormat& pf,
                            const char* name) = 0;

    virtual bool readAndDecodeRect(const core::Rect& r, int encoding,
                                   const PixelFormat& pf,
                                   ModifiablePixelBuffer* pb) = 0;

    virtual void framebufferUpdateStart() = 0;
    virtual void framebufferUpdateEnd() = 0;
    virtual bool dataRect(const core::Rect& r, int encoding) = 0;

    virtual void setColourMapEntries(int firstColour, int nColours,
				     uint16_t* rgbs) = 0;
    virtual void bell() = 0;
    virtual void serverCutText(const char* str) = 0;

    virtual void setLEDState(unsigned int state) = 0;

    virtual void clipboardCaps(uint32_t flags,
                               const uint32_t* lengths) = 0;
    virtual void clipboardRequest(uint32_t flags) = 0;
    virtual void clipboardPeek() = 0;
    virtual void clipboardNotify(uint32_t flags) = 0;
    virtual void clipboardProvide(uint32_t flags,
                                  const size_t* lengths,
                                  const uint8_t* const* data) = 0;

    virtual void audioBegin() = 0;
    virtual void audioEnd() = 0;
    virtual void audioData(const uint8_t* data, size_t length) = 0;

    virtual void serverTightCaps(const std::vector<TightCapability>& /*serverMsgs*/,
                                 const std::vector<TightCapability>& /*clientMsgs*/,
                                 const std::vector<TightCapability>& /*encodings*/) {}
    virtual void supportedMessages(const uint8_t /*client2server*/[32],
                                   const uint8_t /*server2client*/[32]) {}

    virtual void tight13FileListData(uint8_t /*flags*/,
                                     const std::vector<FileEntry>& /*entries*/) {}
    virtual void tight13FileDownloadData(const uint8_t* /*data*/, size_t /*len*/) {}
    virtual void tight13FileDownloadEnd(uint32_t /*modTime*/) {}
    virtual void tight13FileUploadCancel(const std::string& /*reason*/) {}
    virtual void tight13FileDownloadFailed(const std::string& /*reason*/) {}

    virtual void tight2CompressionSupportReply(bool /*supported*/) {}
    virtual void tight2FileListReply(const std::vector<FileEntry>& /*entries*/) {}
    virtual void tight2Md5Reply(const uint8_t /*digest*/[16]) {}
    virtual void tight2UploadStartReply() {}
    virtual void tight2UploadDataReply() {}
    virtual void tight2UploadEndReply() {}
    virtual void tight2DownloadStartReply() {}
    virtual void tight2DownloadDataReply(const uint8_t* /*data*/, size_t /*len*/) {}
    virtual void tight2DownloadEndReply(uint8_t /*flags*/, uint64_t /*modTimeMs*/) {}
    virtual void tight2MkdirReply() {}
    virtual void tight2RemoveReply() {}
    virtual void tight2RenameReply() {}
    virtual void tight2DirSizeReply(uint64_t /*size*/) {}
    virtual void tight2LastRequestFailedReply(const std::string& /*message*/) {}

    virtual void ultraFileTransferMessage(uint8_t /*contentType*/,
                                          uint16_t /*contentParam*/,
                                          uint32_t /*size*/,
                                          uint32_t /*sizeH*/,
                                          const uint8_t* /*payload*/,
                                          size_t /*length*/) {}

    virtual void giiServerVersion(uint16_t maxVersion, uint16_t minVersion) = 0;
    virtual void giiDeviceCreated(uint32_t deviceOrigin) = 0;

    ServerParams server;
  };
}
#endif
