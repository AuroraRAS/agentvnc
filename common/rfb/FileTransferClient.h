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

#ifndef __RFB_FILETRANSFERCLIENT_H__
#define __RFB_FILETRANSFERCLIENT_H__

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>

#include <rfb/TightCapability.h>
#include <rfb/fileTransferTypes.h>

namespace rfb {

  class CMsgWriter;
  class ServerParams;

  class FileTransferHandler {
  public:
    virtual ~FileTransferHandler() {}

    virtual void onFileTransferReady(const FileTransferCaps& /*caps*/) {}
    virtual void onListComplete(bool /*ok*/,
                                const std::vector<FileEntry>& /*entries*/,
                                const std::string& /*error*/) {}
    virtual void onDownloadStarted(bool /*ok*/, uint64_t /*totalSize*/,
                                   bool /*sizeKnown*/,
                                   const std::string& /*error*/) {}
    virtual void onDownloadData(const uint8_t* /*data*/, size_t /*len*/) {}
    virtual void onDownloadComplete(bool /*ok*/, const std::string& /*error*/) {}
    virtual void onUploadReady(bool /*ok*/, const std::string& /*error*/) {}
    virtual void onUploadChunkAck() {}
    virtual void onUploadComplete(bool /*ok*/, const std::string& /*error*/) {}
    virtual void onCommandComplete(bool /*ok*/, const std::string& /*error*/) {}
    virtual void onCancelComplete() {}
  };

  class FileTransferClient {
  public:
    explicit FileTransferClient(ServerParams* server);

    void setWriter(CMsgWriter* writer) { writer_ = writer; }
    void setHandler(FileTransferHandler* handler) { handler_ = handler; }
    void setPreferredProtocol(FileTransferProtocol proto) { preferredProtocol_ = proto; }

    void reset();
    void abortPendingOp();

    bool isSupported() const { return ready_ && caps_.protocol != FileTransferProtocol::Unsupported; }
    const FileTransferCaps& caps() const { return caps_; }
    bool isBusy() const { return opState_ != OpState::Idle; }

    void probeUltraVnc();

    // High-level operations (one active operation at a time)
    void listDirectory(const std::string& path);
    void startDownload(const std::string& path, uint64_t offset);
    void requestMoreDownloadData();
    void cancelDownload();

    void startUpload(const std::string& path, bool overwrite, uint64_t offset);
    void sendUploadData(const uint8_t* data, size_t len);
    void finishUpload(uint64_t modTimeSec);
    void cancelUpload();

    void createDirectory(const std::string& path);
    void removeFileOrDir(const std::string& path, bool isDir);
    void renameFileOrDir(const std::string& fromPath,
                         const std::string& toPath, bool isDir);

    // Events routed from CConnection
    void onServerTightCaps(const std::vector<TightCapability>& serverMsgs,
                           const std::vector<TightCapability>& clientMsgs,
                           const std::vector<TightCapability>& encodings);
    void onSupportedMessages(const uint8_t client2server[32],
                             const uint8_t server2client[32]);
    void onServerInit();
    void onFenceReply();
    void onTight13CancelFenceReply();

    void onTight13FileListData(uint8_t flags,
                               const std::vector<FileEntry>& entries);
    void onTight13FileDownloadData(const uint8_t* data, size_t len);
    void onTight13FileDownloadEnd(uint32_t modTime);
    void onTight13FileUploadCancel(const std::string& reason);
    void onTight13FileDownloadFailed(const std::string& reason);

    void onTight2CompressionSupportReply(bool supported);
    void onTight2FileListReply(const std::vector<FileEntry>& entries);
    void onTight2Md5Reply(const uint8_t digest[16]);
    void onTight2UploadStartReply();
    void onTight2UploadDataReply();
    void onTight2UploadEndReply();
    void onTight2DownloadStartReply();
    void onTight2DownloadDataReply(const uint8_t* data, size_t len);
    void onTight2DownloadEndReply(uint8_t flags, uint64_t modTimeMs);
    void onTight2MkdirReply();
    void onTight2RemoveReply();
    void onTight2RenameReply();
    void onTight2DirSizeReply(uint64_t size);
    void onTight2LastRequestFailedReply(const std::string& message);

    void onUltraFileTransferMessage(uint8_t contentType,
                                    uint16_t contentParam,
                                    uint32_t size,
                                    uint32_t sizeH,
                                    const uint8_t* payload,
                                    size_t length);

  private:
    enum class OpState {
      Idle,
      Probing,
      Listing,
      Downloading,
      Uploading,
      Command,
      DrainingCancel
    };

    enum class CommandKind {
      NoneOp,
      Mkdir,
      Remove,
      Rename
    };

    enum class Tight2CancelAction {
      NoCancel,
      CloseDownload,
      CloseUpload
    };

    void ensureReadyForOp();
    void failActiveOp(const std::string& error);
    void finishTight2DrainingCancel(bool remoteClosedHandle);
    void handleUltraDirPacket(uint16_t contentParam,
                              const uint8_t* payload, size_t length);
    void handleUltraFilePacket(uint32_t size,
                               const uint8_t* payload, size_t length);

    static std::string toTight2Path(const std::string& ftpPath);
    static std::string toTight13Path(const std::string& ftpPath);
    static std::string toUltraPath(const std::string& ftpPath, bool isDir);
    static std::string decodeUltraFileName(const uint8_t* buf, size_t len,
                                           bool isUnicode);
    static int64_t decodeUltraFileTime(uint32_t low, uint32_t high);
    static std::string formatUltraTimeSuffix(time_t t);

    ServerParams* server_;
    CMsgWriter* writer_;
    FileTransferHandler* handler_;
    FileTransferProtocol preferredProtocol_;

    FileTransferCaps caps_;
    bool ready_;
    bool serverInitDone_;

    OpState opState_;
    CommandKind commandKind_;

    // Tight 2.x tracking
    bool tight2Available_;
    bool tight2CompressionProbeCap_;
    bool tight2ReplyPending_;
    Tight2CancelAction tight2CancelAction_;

    // Tight 1.3 / common transfer tracking
    bool tight13Available_;
    bool tight13SyncPending_;
    bool downloadStarted_;
    bool uploadReady_;
    int tight13StaleListReplies_;
    std::string tight13ExpectedMkdirBase_;

    // UltraVNC tracking
    bool ultraProbeSent_;
    bool ultraProbeInFlight_;
    bool ultraSessionStarted_;
    bool ultraDrivesListing_;
    bool ultraDirEchoReceived_;
    uint64_t ultraDownloadSkipBytes_;
    std::vector<FileEntry> pendingEntries_;
  };

}

#endif
