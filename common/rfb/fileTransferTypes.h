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

#ifndef __RFB_FILETRANSFERTYPES_H__
#define __RFB_FILETRANSFERTYPES_H__

#include <stdint.h>
#include <string>
#include <vector>

namespace rfb {

  enum class FileTransferProtocol {
    Unsupported,
    Tight13,
    Tight2,
    Ultra
  };

  struct FileEntry {
    std::string name;
    uint64_t size;
    int64_t mtime; // Unix timestamp in seconds
    bool isDir;
    bool isExecutable;

    FileEntry() : size(0), mtime(0), isDir(false), isExecutable(false) {}
  };

  struct FileTransferCaps {
    FileTransferProtocol protocol;
    bool canList;
    bool canDownload;
    bool canUpload;
    bool canResumeDownload;
    bool canResumeUpload;
    bool canMkdir;
    bool canDelete;
    bool canRename;
    bool canCompress;
    int ultraVersion;
    uint32_t ultraBlockSize;
    bool ultraUnicode;

    FileTransferCaps()
      : protocol(FileTransferProtocol::Unsupported),
        canList(false),
        canDownload(false),
        canUpload(false),
        canResumeDownload(false),
        canResumeUpload(false),
        canMkdir(false),
        canDelete(false),
        canRename(false),
        canCompress(false),
        ultraVersion(0),
        ultraBlockSize(8192),
        ultraUnicode(false) {}
  };

  // TightVNC 1.3 capability codes & signatures
  static const int32_t tight13CapFileListData       = 130;
  static const char sigTight13FileListData[]        = "FTS_LSDT";
  static const int32_t tight13CapFileDownloadData   = 131;
  static const char sigTight13FileDownloadData[]    = "FTS_DNDT";
  static const int32_t tight13CapFileUploadCancel   = 132;
  static const char sigTight13FileUploadCancel[]    = "FTS_UPCN";
  static const int32_t tight13CapFileDownloadFailed = 133;
  static const char sigTight13FileDownloadFailed[]  = "FTS_DNFL";

  static const int32_t tight13CapFileListReq        = 130;
  static const char sigTight13FileListReq[]         = "FTC_LSRQ";
  static const int32_t tight13CapFileDownloadReq    = 131;
  static const char sigTight13FileDownloadReq[]     = "FTC_DNRQ";
  static const int32_t tight13CapFileUploadReq      = 132;
  static const char sigTight13FileUploadReq[]       = "FTC_UPRQ";
  static const int32_t tight13CapFileUploadData     = 133;
  static const char sigTight13FileUploadData[]      = "FTC_UPDT";
  static const int32_t tight13CapFileDownloadCancel = 134;
  static const char sigTight13FileDownloadCancel[]  = "FTC_DNCN";
  static const int32_t tight13CapFileUploadFailed   = 135;
  static const char sigTight13FileUploadFailed[]    = "FTC_UPFL";
  static const int32_t tight13CapFileCreateDirReq   = 136;
  static const char sigTight13FileCreateDirReq[]    = "FTC_FCDR";

  static const uint32_t tight13DirSize    = 0xFFFFFFFFU;
  static const uint8_t tight13FlagError   = 0x80;
  static const uint8_t tight13FlagDirOnly = 0x10;

  // TightVNC 2.x 32-bit extension message IDs & capability signatures
  static const uint32_t tight2CompressionSupportReq   = 0xFC000100U;
  static const uint32_t tight2CompressionSupportReply = 0xFC000101U;
  static const uint32_t tight2FileListReq             = 0xFC000102U;
  static const uint32_t tight2FileListReply           = 0xFC000103U;
  static const uint32_t tight2Md5Req                  = 0xFC000104U;
  static const uint32_t tight2Md5Reply                = 0xFC000105U;
  static const uint32_t tight2UploadStartReq          = 0xFC000106U;
  static const uint32_t tight2UploadStartReply        = 0xFC000107U;
  static const uint32_t tight2UploadDataReq           = 0xFC000108U;
  static const uint32_t tight2UploadDataReply         = 0xFC000109U;
  static const uint32_t tight2UploadEndReq            = 0xFC00010AU;
  static const uint32_t tight2UploadEndReply          = 0xFC00010BU;
  static const uint32_t tight2DownloadStartReq        = 0xFC00010CU;
  static const uint32_t tight2DownloadStartReply      = 0xFC00010DU;
  static const uint32_t tight2DownloadDataReq         = 0xFC00010EU;
  static const uint32_t tight2DownloadDataReply       = 0xFC00010FU;
  static const uint32_t tight2DownloadEndReply        = 0xFC000110U;
  static const uint32_t tight2MkdirReq                = 0xFC000111U;
  static const uint32_t tight2MkdirReply              = 0xFC000112U;
  static const uint32_t tight2RemoveReq               = 0xFC000113U;
  static const uint32_t tight2RemoveReply             = 0xFC000114U;
  static const uint32_t tight2RenameReq               = 0xFC000115U;
  static const uint32_t tight2RenameReply             = 0xFC000116U;
  static const uint32_t tight2DirSizeReq              = 0xFC000117U;
  static const uint32_t tight2DirSizeReply            = 0xFC000118U;
  static const uint32_t tight2LastRequestFailedReply  = 0xFC000119U;

  static const char sigTight2CompressionSupportReq[]   = "FTCCSRST";
  static const char sigTight2CompressionSupportReply[] = "FTSCSRLY";
  static const char sigTight2FileListReq[]             = "FTCFLRST";
  static const char sigTight2FileListReply[]           = "FTSFLRLY";
  static const char sigTight2Md5Req[]                  = "FTCM5RST";
  static const char sigTight2Md5Reply[]                = "FTSM5RLY";
  static const char sigTight2UploadStartReq[]          = "FTCFURST";
  static const char sigTight2UploadStartReply[]        = "FTSFURLY";
  static const char sigTight2UploadDataReq[]           = "FTCUDRST";
  static const char sigTight2UploadDataReply[]         = "FTSUDRLY";
  static const char sigTight2UploadEndReq[]            = "FTCUERST";
  static const char sigTight2UploadEndReply[]          = "FTSUERLY";
  static const char sigTight2DownloadStartReq[]        = "FTCFDRST";
  static const char sigTight2DownloadStartReply[]      = "FTSFDRLY";
  static const char sigTight2DownloadDataReq[]         = "FTCDDRST";
  static const char sigTight2DownloadDataReply[]       = "FTSDDRLY";
  static const char sigTight2DownloadEndReply[]        = "FTSDERLY";
  static const char sigTight2MkdirReq[]                = "FTCMDRST";
  static const char sigTight2MkdirReply[]              = "FTSMDRLY";
  static const char sigTight2RemoveReq[]               = "FTCFRRST";
  static const char sigTight2RemoveReply[]             = "FTSFTRLY";
  static const char sigTight2RenameReq[]               = "FTCFMRST";
  static const char sigTight2RenameReply[]             = "FTSFMRLY";
  static const char sigTight2DirSizeReq[]              = "FTCDSRST";
  static const char sigTight2DirSizeReply[]            = "FTSDSRLY";
  static const char sigTight2LastRequestFailedReply[]  = "FTLRFRLY";

  static const uint16_t tight2FlagDir  = 0x01;
  static const uint16_t tight2FlagExec = 0x02;

  // UltraVNC FileTransfer contentType values (RFB message type 7)
  static const uint8_t ultraDirContentRequest      = 1;
  static const uint8_t ultraDirPacket              = 2;
  static const uint8_t ultraFileTransferRequest    = 3;
  static const uint8_t ultraFileHeader             = 4;
  static const uint8_t ultraFilePacket             = 5;
  static const uint8_t ultraEndOfFile              = 6;
  static const uint8_t ultraAbortFileTransfer      = 7;
  static const uint8_t ultraFileTransferOffer      = 8;
  static const uint8_t ultraFileAcceptHeader       = 9;
  static const uint8_t ultraCommand                = 10;
  static const uint8_t ultraCommandReturn          = 11;
  static const uint8_t ultraFileChecksums          = 12;
  static const uint8_t ultraFileTransferAccess     = 14;
  static const uint8_t ultraFileTransferSessionStart = 15;
  static const uint8_t ultraFileTransferSessionEnd = 16;
  static const uint8_t ultraFileTransferProtocolVersion = 17;

  // UltraVNC rfbDirContentRequest contentParam
  static const uint16_t ultraRDirContent           = 1;
  static const uint16_t ultraRDrivesList           = 2;
  static const uint16_t ultraRDirRecursiveList     = 3;
  static const uint16_t ultraRDirRecursiveSize     = 4;
  static const uint16_t ultraRDirContentUnicode    = 0x8000;

  // UltraVNC rfbDirPacket & rfbCommandReturn contentParam
  static const uint16_t ultraADirectory            = 1;
  static const uint16_t ultraAFile                 = 2;
  static const uint16_t ultraADrivesList           = 3;
  static const uint16_t ultraADirCreate            = 4;
  static const uint16_t ultraADirDelete            = 5;
  static const uint16_t ultraAFileCreate           = 6;
  static const uint16_t ultraAFileDelete           = 7;
  static const uint16_t ultraAFileRename           = 8;
  static const uint16_t ultraADirRename            = 9;
  static const uint16_t ultraADirInaccessible      = 18;
  static const uint16_t ultraADirUnicode           = 0x8000;

  // UltraVNC rfbCommand contentParam
  static const uint16_t ultraCDirCreate            = 1;
  static const uint16_t ultraCDirDelete            = 2;
  static const uint16_t ultraCFileCreate           = 3;
  static const uint16_t ultraCFileDelete           = 4;
  static const uint16_t ultraCFileRename           = 5;
  static const uint16_t ultraCDirRename            = 6;

  static const uint32_t ultraError                 = 0xFFFFFFFFU;
  static const uint32_t ultraFileAttrDir           = 0x10U;

}

#endif
