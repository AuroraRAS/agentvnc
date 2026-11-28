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

#ifndef __SCRCPYVNCSERVER_ADBFILETRANSFER_H__
#define __SCRCPYVNCSERVER_ADBFILETRANSFER_H__

#include <stdint.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rfb/fileTransferTypes.h>

#include "AdbClient.h"

class AdbFileTransferSession {
public:
  struct Reply {
    enum class Kind {
      Tight2CompressionSupport,
      Tight2FileList,
      Tight2Md5,
      Tight2UploadStart,
      Tight2UploadData,
      Tight2UploadEnd,
      Tight2DownloadStart,
      Tight2DownloadData,
      Tight2DownloadEnd,
      Tight2Mkdir,
      Tight2Remove,
      Tight2Rename,
      Tight2DirSize,
      Tight2Failed,
      Tight13FileList,
      Tight13DownloadData,
      Tight13DownloadEnd,
      Tight13DownloadFailed,
      Tight13UploadCancel,
      FenceReply
    };

    Kind kind = Kind::Tight2Failed;
    bool boolVal = false;
    uint8_t compressLevel = 0;
    uint8_t flags = 0;
    uint64_t u64Val = 0;
    uint32_t u32Val = 0;
    uint8_t md5[16] = {};
    std::string text;
    std::vector<uint8_t> data;
    std::vector<rfb::FileEntry> entries;
  };

  AdbFileTransferSession(std::string adbHost, int adbPort,
                         std::string serial, std::string rootDir);
  ~AdbFileTransferSession();

  int wakeupFd() const { return wakeupPipe_[0]; }
  void stop();

  // Normalizes a client-supplied path and confines it within rootDir.
  static bool normalizePath(const std::string& rawPath,
                            const std::string& rootDir,
                            bool allowEmptyAsRoot,
                            std::string* devicePathOut,
                            std::string* virtualPathOut,
                            std::string* errorOut);

  void enqueueTight2CompressionSupport();
  void enqueueTight2FileList(uint8_t compressLevel, const std::string& path);
  void enqueueTight2Md5(const std::string& path, uint64_t offset,
                        uint64_t dataLen);
  void enqueueTight2UploadStart(const std::string& path, uint8_t flags,
                                uint64_t offset);
  void enqueueTight2UploadData(const uint8_t* data, size_t len);
  void enqueueTight2UploadEnd(uint16_t fileFlags, uint64_t modTimeMs);
  void enqueueTight2DownloadStart(const std::string& path, uint64_t offset);
  void enqueueTight2DownloadData(uint8_t compressLevel, uint32_t reqSize);
  void enqueueTight2Mkdir(const std::string& path);
  void enqueueTight2Remove(const std::string& path);
  void enqueueTight2Rename(const std::string& oldPath,
                           const std::string& newPath);
  void enqueueTight2DirSize(const std::string& path);

  void enqueueTight13FileList(uint8_t flags, const std::string& dir);
  void enqueueTight13DownloadStart(uint8_t compressLevel, uint32_t position,
                                   const std::string& path, uint64_t serial);
  void enqueueTight13DownloadNextChunk();
  void enqueueTight13DownloadCancel();
  void enqueueTight13UploadStart(uint32_t position, const std::string& path);
  void enqueueTight13UploadData(const uint8_t* data, size_t len);
  void enqueueTight13UploadEnd(uint32_t modTimeSec);
  void enqueueTight13UploadFailed();
  void enqueueTight13CreateDir(const std::string& dir);
  void enqueueFenceBarrier(uint32_t flags, const uint8_t* data, size_t len);

  std::vector<Reply> drainReplies();

private:
  struct Request {
    enum class Kind {
      Tight2CompressionSupport,
      Tight2FileList,
      Tight2Md5,
      Tight2UploadStart,
      Tight2UploadData,
      Tight2UploadEnd,
      Tight2DownloadStart,
      Tight2DownloadData,
      Tight2Mkdir,
      Tight2Remove,
      Tight2Rename,
      Tight2DirSize,
      Tight13FileList,
      Tight13DownloadStart,
      Tight13DownloadNextChunk,
      Tight13DownloadCancel,
      Tight13UploadStart,
      Tight13UploadData,
      Tight13UploadEnd,
      Tight13UploadFailed,
      Tight13CreateDir,
      FenceBarrier
    };

    Kind kind = Kind::Tight2CompressionSupport;
    uint8_t u8Val = 0;
    uint16_t u16Val = 0;
    uint32_t u32Val = 0;
    uint64_t u64Val = 0;
    uint64_t u64Val2 = 0;
    std::string path;
    std::string path2;
    std::vector<uint8_t> data;
  };

  enum class UploadMode {
    None,
    SyncSend,
    ShellV2Append,
    TempFileFallback
  };

  void enqueue(Request req);
  void postReply(Reply reply);
  void notifyMainThread();
  void workerMain();
  void ensureFeatures();

  void setActiveIoFd(int fd);
  void closeManagedFd(int* fd);
  void closeDownload();
  void abortUpload();
  bool isPathInUse(const std::string& devicePath) const;

  void handleRequest(const Request& req);
  void handleTight2FileList(uint8_t compressLevel, const std::string& rawPath);
  void handleTight2Md5(const std::string& rawPath, uint64_t offset,
                       uint64_t dataLen);
  void handleTight2DownloadStart(const std::string& rawPath, uint64_t offset);
  void handleTight2DownloadData(uint8_t compressLevel, uint32_t reqSize);
  void handleTight2UploadStart(const std::string& rawPath, uint8_t flags,
                               uint64_t offset);
  void handleTight2UploadData(const std::vector<uint8_t>& data);
  void handleTight2UploadEnd(uint16_t fileFlags, uint64_t modTimeMs);
  void handleTight2Mkdir(const std::string& rawPath);
  void handleTight2Remove(const std::string& rawPath);
  void handleTight2Rename(const std::string& oldRaw,
                          const std::string& newRaw);
  void handleTight2DirSize(const std::string& rawPath);

  void handleTight13FileList(uint8_t flags, const std::string& rawPath);
  void handleTight13DownloadStart(uint8_t compressLevel, uint32_t position,
                                  const std::string& rawPath, uint64_t serial);
  void handleTight13DownloadNextChunk();
  void handleTight13UploadStart(uint32_t position, const std::string& rawPath);
  void handleTight13UploadData(const std::vector<uint8_t>& data);
  void handleTight13UploadEnd(uint32_t modTimeSec);
  void handleTight13CreateDir(const std::string& rawPath);

  bool openDownloadInternal(const std::string& rawPath, uint64_t offset,
                            bool allowEmptyCancel, std::string* errOut);
  bool readDownloadChunk(size_t maxBytes, std::vector<uint8_t>* out,
                         bool* eofOut, std::string* errOut);
  bool openUploadInternal(const std::string& rawPath, bool overwrite,
                          uint64_t offset, std::string* errOut);
  bool writeUploadChunkInternal(const std::vector<uint8_t>& data,
                                std::string* errOut);
  bool finishUploadInternal(uint16_t fileFlags, uint64_t modTimeMs,
                            std::string* errOut);
  bool computeDirSizeFallback(int syncFd, const std::string& dirPath,
                              int depth, uint64_t* totalOut);

  AdbClient adb_;
  std::string serial_;
  std::string rootDir_;

  int wakeupPipe_[2];

  std::mutex reqMu_;
  std::condition_variable reqCv_;
  std::deque<Request> reqQueue_;
  bool stopping_;
  bool workerStarted_;
  std::thread worker_;

  std::mutex replyMu_;
  std::deque<Reply> replyQueue_;

  std::mutex ioFdMu_;
  int activeIoFd_;

  bool featuresQueried_;
  AdbFeatures features_;

  // Persistent worker-owned transfer handles
  int downloadSyncFd_;
  std::string downloadPath_;
  uint8_t downloadFlags_;
  uint64_t downloadMtimeMs_;
  uint8_t downloadCompressLevel_;
  bool downloadEof_;
  bool tight13DownloadActive_;
  uint64_t tight13DownloadSerial_ = 0;
  std::vector<uint8_t> downloadPrefetch_;

  UploadMode uploadMode_;
  int uploadFd_;
  int uploadTempFd_;
  std::string uploadTempPath_;
  std::string uploadPath_;
  bool tight13UploadActive_;

  AdbFileTransferSession(const AdbFileTransferSession&) = delete;
  AdbFileTransferSession& operator=(const AdbFileTransferSession&) = delete;
};

#endif // __SCRCPYVNCSERVER_ADBFILETRANSFER_H__
