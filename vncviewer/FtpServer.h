/* Copyright (C) 2026 TigerVNC Team
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

#ifndef __FTPSERVER_H__
#define __FTPSERVER_H__

#include <stdint.h>
#include <stddef.h>
#include <ctime>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include <rfb/FileTransferClient.h>

class FtpEventLoop {
public:
  typedef void (*FdCallback)(int fd, bool readable, bool writable, void* data);
  typedef void (*TimerCallback)(void* data);

  virtual ~FtpEventLoop() {}
  virtual void setFdWatch(int fd, bool read, bool write,
                          FdCallback cb, void* data) = 0;
  virtual void removeFd(int fd) = 0;
  virtual void addTimer(int delayMs, TimerCallback cb, void* data) = 0;
  virtual void removeTimer(TimerCallback cb, void* data) = 0;
};

class FtpBackend {
public:
  virtual ~FtpBackend() {}
  virtual bool isViewOnly() const = 0;
  virtual bool isConnected() const = 0;
  virtual rfb::FileTransferClient* fileTransfer() = 0;
  virtual void flushOutput() = 0;
  virtual bool isOutputCongested() const = 0;
};

struct FtpServerConfig {
  std::string bindAddress;
  int port;
  std::string username;
  std::string password;

  FtpServerConfig() : bindAddress("127.0.0.1"), port(2121) {}
};

class FtpServer : public rfb::FileTransferHandler {
public:
  FtpServer(FtpEventLoop* loop, FtpBackend* backend);
  ~FtpServer();

  bool start(const FtpServerConfig& config, std::string* errorMsg = nullptr);
  void stop();

  bool isRunning() const { return !listenFds_.empty(); }
  int listenPort() const { return boundPort_; }
  std::string listenHost() const { return config_.bindAddress; }
  std::string url() const;

  void onBackendOutputDrained();

  // rfb::FileTransferHandler overrides
  void onFileTransferReady(const rfb::FileTransferCaps& caps) override;
  void onListComplete(bool ok, const std::vector<rfb::FileEntry>& entries,
                      const std::string& error) override;
  void onDownloadStarted(bool ok, uint64_t totalSize, bool sizeKnown,
                         const std::string& error) override;
  void onDownloadData(const uint8_t* data, size_t len) override;
  void onDownloadComplete(bool ok, const std::string& error) override;
  void onUploadReady(bool ok, const std::string& error) override;
  void onUploadChunkAck() override;
  void onUploadComplete(bool ok, const std::string& error) override;
  void onCommandComplete(bool ok, const std::string& error) override;
  void onCancelComplete() override;

private:
  enum class OpKind {
    Idle,
    List,
    Mlsd,
    Nlst,
    Cwd,
    StatSize,
    StatMdtm,
    StatMlst,
    AppeStat,
    Retr,
    Stor,
    Appe,
    Mkd,
    Rmd,
    Dele,
    Rename
  };

  struct ClientSession {
    enum class DataMode {
      Unconfigured,
      PassiveListen,
      ActiveTarget
    };

    enum class DataTransferState {
      Idle,
      WaitingRfbStart,
      WaitingDataChannel,
      Transferring,
      Canceling
    };

    int id;
    int ctrlFd;
    std::string peerIp;
    std::string localIp;

    std::string inBuf;
    std::string outBuf;
    size_t outOff;
    bool closeAfterFlush;

    bool authenticated;
    std::string pendingUser;
    std::string cwd;
    bool hasRnfr;
    std::string rnfrPath;
    bool hasRest;
    uint64_t restOffset;

    DataMode dataMode;
    int pasvListenFd;
    std::string activeHost;
    int activePort;

    int dataFd;
    bool dataConnectPending;
    bool dataConnected;
    DataTransferState dataState;

    std::string dataOutBuf;
    size_t dataOutOff;
    bool dataOutEof;
    bool downloadPullDeferred;

    bool uploadAckPending;
    bool uploadCongestedPaused;
    bool uploadReadEof;
    bool abortReplyPending;

    ClientSession()
      : id(0), ctrlFd(-1), outOff(0), closeAfterFlush(false),
        authenticated(false), cwd("/"), hasRnfr(false),
        hasRest(false), restOffset(0), dataMode(DataMode::Unconfigured),
        pasvListenFd(-1), activePort(0), dataFd(-1),
        dataConnectPending(false), dataConnected(false),
        dataState(DataTransferState::Idle), dataOutOff(0),
        dataOutEof(false), downloadPullDeferred(false),
        uploadAckPending(false), uploadCongestedPaused(false),
        uploadReadEof(false), abortReplyPending(false) {}
  };

  struct QueuedOp {
    int sessionId;
    OpKind kind;
    std::string path;
    std::string path2;
    std::string rawArg;
    uint64_t offset;
    bool overwrite;
    bool isDir;
    time_t startedAt;

    QueuedOp()
      : sessionId(0), kind(OpKind::Idle), offset(0),
        overwrite(true), isDir(false), startedAt(0) {}
  };

  struct DirCacheEntry {
    time_t fetchedAt;
    std::vector<rfb::FileEntry> entries;
    DirCacheEntry() : fetchedAt(0) {}
  };

  struct FdRef {
    enum class Kind { Listener, Control, PasvListener, Data };
    FtpServer* server;
    Kind kind;
    int sessionId;
  };

  static void onFdEvent(int fd, bool readable, bool writable, void* data);
  static void onTimeoutTimer(void* data);

  void handleListenerRead(int fd);
  void handleControlEvent(ClientSession* s, bool readable, bool writable);
  void handlePasvAccept(ClientSession* s);
  void handleDataEvent(ClientSession* s, bool readable, bool writable);

  void processControlLine(ClientSession* s, const std::string& line);
  void sendReply(ClientSession* s, const std::string& line);
  void updateControlWatch(ClientSession* s);
  void updateDataWatch(ClientSession* s);

  bool startPassiveListener(ClientSession* s, bool ipv4Only);
  bool beginActiveConnect(ClientSession* s);
  void closeDataChannel(ClientSession* s);
  void destroySession(int sessionId);

  void enqueueOp(const QueuedOp& op);
  void pumpQueue();
  void startActiveOp(const QueuedOp& op);
  void finishActiveOp();
  void handleStatResult(ClientSession* s, const QueuedOp& op,
                        const std::vector<rfb::FileEntry>& entries);
  void beginDataChannelTransfer(ClientSession* s);
  void beginPortOrWaitTransfer(ClientSession* s);
  void abortActiveOpAndSession(ClientSession* s, const std::string& msg,
                               const char* ftpCode);

  bool getCachedDir(const std::string& dirPath,
                    std::vector<rfb::FileEntry>* entries) const;
  void putCachedDir(const std::string& dirPath,
                    const std::vector<rfb::FileEntry>& entries);
  void invalidateCacheFor(const std::string& path);

  void armTimeoutTimer();
  void stopTimeoutTimer();
  void checkTimeout();

  ClientSession* findSession(int sessionId);
  void registerFdWatch(int fd, FdRef::Kind kind, int sessionId,
                       bool read, bool write);
  void unregisterFd(int fd);

  static std::string socketPeerIp(int fd);
  static std::string socketLocalIp(int fd);
  static int socketLocalPort(int fd);
  static void setNonBlockingCloExec(int fd);

  FtpEventLoop* loop_;
  FtpBackend* backend_;
  FtpServerConfig config_;

  std::vector<int> listenFds_;
  int boundPort_;
  int nextSessionId_;

  std::map<int, ClientSession*> sessions_;
  std::map<int, FdRef*> fdRefs_;

  std::deque<QueuedOp> opQueue_;
  QueuedOp activeOp_;
  bool rfbDownloadFinished_;
  bool rfbDownloadOk_;
  std::string rfbDownloadError_;

  std::map<std::string, DirCacheEntry> dirCache_;
  bool timerArmed_;
};

#endif
