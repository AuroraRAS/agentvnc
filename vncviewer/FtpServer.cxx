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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <core/LogWriter.h>
#include <core/string.h>

#include "FtpProtocol.h"
#include "FtpServer.h"

static core::LogWriter vlog("FtpServer");

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static const int kCacheTtlSeconds = 2;
static const int kOpTimeoutSeconds = 60;
static const size_t kLowWaterMark = 65536;
static const size_t kMaxCtrlLineLen = 16384;
static const size_t kMaxCtrlOutBuf = 262144;

static std::string stripIpv4Mapped(const std::string& ip)
{
  const std::string prefix = "::ffff:";
  if (ip.size() > prefix.size() &&
      ip.compare(0, prefix.size(), prefix) == 0)
    return ip.substr(prefix.size());
  return ip;
}

static bool isLoopbackIp(const std::string& ip)
{
  std::string norm = stripIpv4Mapped(ip);
  return norm == "127.0.0.1" || norm == "::1" || norm == "localhost";
}

static bool isSamePeerOrLoopback(const std::string& a, const std::string& b)
{
  std::string na = stripIpv4Mapped(a);
  std::string nb = stripIpv4Mapped(b);
  if (na == nb)
    return true;
  return isLoopbackIp(na) && isLoopbackIp(nb);
}

static void closeRawFd(int fd)
{
  if (fd >= 0)
    ::close(fd);
}

FtpServer::FtpServer(FtpEventLoop* loop, FtpBackend* backend)
  : loop_(loop),
    backend_(backend),
    boundPort_(0),
    nextSessionId_(1),
    rfbDownloadFinished_(false),
    rfbDownloadOk_(false),
    timerArmed_(false)
{
}

FtpServer::~FtpServer()
{
  stop();
}

void FtpServer::setNonBlockingCloExec(int fd)
{
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags != -1)
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int fdFlags = ::fcntl(fd, F_GETFD, 0);
  if (fdFlags != -1)
    ::fcntl(fd, F_SETFD, fdFlags | FD_CLOEXEC);
}

std::string FtpServer::socketPeerIp(int fd)
{
  struct sockaddr_storage ss;
  socklen_t len = sizeof(ss);
  if (::getpeername(fd, reinterpret_cast<struct sockaddr*>(&ss), &len) != 0)
    return "";
  char host[NI_MAXHOST];
  if (::getnameinfo(reinterpret_cast<const struct sockaddr*>(&ss), len,
                    host, sizeof(host), nullptr, 0, NI_NUMERICHOST) != 0)
    return "";
  // Strip IPv6 zone ID if present
  std::string res(host);
  size_t pct = res.find('%');
  if (pct != std::string::npos)
    res.resize(pct);
  return stripIpv4Mapped(res);
}

std::string FtpServer::socketLocalIp(int fd)
{
  struct sockaddr_storage ss;
  socklen_t len = sizeof(ss);
  if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&ss), &len) != 0)
    return "";
  char host[NI_MAXHOST];
  if (::getnameinfo(reinterpret_cast<const struct sockaddr*>(&ss), len,
                    host, sizeof(host), nullptr, 0, NI_NUMERICHOST) != 0)
    return "";
  std::string res(host);
  size_t pct = res.find('%');
  if (pct != std::string::npos)
    res.resize(pct);
  return stripIpv4Mapped(res);
}

int FtpServer::socketLocalPort(int fd)
{
  struct sockaddr_storage ss;
  socklen_t len = sizeof(ss);
  if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&ss), &len) != 0)
    return 0;
  if (ss.ss_family == AF_INET) {
    const struct sockaddr_in* sa =
      reinterpret_cast<const struct sockaddr_in*>(&ss);
    return ntohs(sa->sin_port);
  }
  if (ss.ss_family == AF_INET6) {
    const struct sockaddr_in6* sa6 =
      reinterpret_cast<const struct sockaddr_in6*>(&ss);
    return ntohs(sa6->sin6_port);
  }
  return 0;
}

bool FtpServer::start(const FtpServerConfig& config, std::string* errorMsg)
{
  stop();
  config_ = config;
  if (config_.bindAddress.empty())
    config_.bindAddress = "127.0.0.1";

  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  char portBuf[16];
  std::snprintf(portBuf, sizeof(portBuf), "%d", config_.port);

  struct addrinfo* ai = nullptr;
  int gai = ::getaddrinfo(config_.bindAddress.c_str(), portBuf, &hints, &ai);
  if (gai != 0) {
    if (errorMsg != nullptr)
      *errorMsg = core::format("Failed to resolve FTP bind address '%s': %s",
                               config_.bindAddress.c_str(), gai_strerror(gai));
    return false;
  }

  int chosenPort = config_.port;
  int lastErrno = 0;

  for (struct addrinfo* cur = ai; cur != nullptr; cur = cur->ai_next) {
    // When binding ephemeral port 0 across multiple returned families
    // (e.g. localhost -> 127.0.0.1 and ::1), reuse the first bound port so
    // all listeners share the same port number.
    struct sockaddr_storage addrCopy;
    if (cur->ai_addrlen > sizeof(addrCopy))
      continue;
    std::memcpy(&addrCopy, cur->ai_addr, cur->ai_addrlen);
    if (chosenPort > 0 && config_.port == 0) {
      if (addrCopy.ss_family == AF_INET) {
        reinterpret_cast<struct sockaddr_in*>(&addrCopy)->sin_port =
          htons(static_cast<uint16_t>(chosenPort));
      } else if (addrCopy.ss_family == AF_INET6) {
        reinterpret_cast<struct sockaddr_in6*>(&addrCopy)->sin6_port =
          htons(static_cast<uint16_t>(chosenPort));
      }
    }

    int fd = ::socket(cur->ai_family, cur->ai_socktype, cur->ai_protocol);
    if (fd < 0) {
      lastErrno = errno;
      continue;
    }

    setNonBlockingCloExec(fd);
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef IPV6_V6ONLY
    if (cur->ai_family == AF_INET6)
      ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
#endif

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addrCopy),
               cur->ai_addrlen) != 0) {
      lastErrno = errno;
      closeRawFd(fd);
      continue;
    }

    if (::listen(fd, 16) != 0) {
      lastErrno = errno;
      closeRawFd(fd);
      continue;
    }

    if (chosenPort == 0)
      chosenPort = socketLocalPort(fd);

    listenFds_.push_back(fd);
    registerFdWatch(fd, FdRef::Kind::Listener, 0, true, false);
  }

  ::freeaddrinfo(ai);

  if (listenFds_.empty()) {
    if (errorMsg != nullptr) {
      *errorMsg = core::format("Failed to bind FTP server on %s:%d: %s",
                               config_.bindAddress.c_str(), config_.port,
                               std::strerror(lastErrno ? lastErrno : EADDRINUSE));
    }
    return false;
  }

  boundPort_ = chosenPort;
  rfb::FileTransferClient* ft =
    backend_ != nullptr ? backend_->fileTransfer() : nullptr;
  if (ft != nullptr)
    ft->setHandler(this);

  vlog.info("FTP server listening on %s", url().c_str());
  return true;
}

void FtpServer::stop()
{
  stopTimeoutTimer();

  rfb::FileTransferClient* ft =
    backend_ != nullptr ? backend_->fileTransfer() : nullptr;
  if (ft != nullptr)
    ft->setHandler(nullptr);

  std::vector<int> sessionIds;
  for (std::map<int, ClientSession*>::iterator it = sessions_.begin();
       it != sessions_.end(); ++it) {
    sessionIds.push_back(it->first);
  }
  for (size_t i = 0; i < sessionIds.size(); i++)
    destroySession(sessionIds[i]);
  sessions_.clear();

  for (size_t i = 0; i < listenFds_.size(); i++) {
    unregisterFd(listenFds_[i]);
    closeRawFd(listenFds_[i]);
  }
  listenFds_.clear();

  // Clean up any leftover FdRefs defensively
  for (std::map<int, FdRef*>::iterator it = fdRefs_.begin();
       it != fdRefs_.end(); ++it) {
    if (loop_ != nullptr)
      loop_->removeFd(it->first);
    delete it->second;
  }
  fdRefs_.clear();

  opQueue_.clear();
  activeOp_ = QueuedOp();
  rfbDownloadFinished_ = false;
  rfbDownloadOk_ = false;
  rfbDownloadError_.clear();
  dirCache_.clear();
  boundPort_ = 0;
}

std::string FtpServer::url() const
{
  std::string host = config_.bindAddress.empty()
    ? std::string("127.0.0.1") : config_.bindAddress;
  if (host.find(':') != std::string::npos)
    return core::format("ftp://[%s]:%d", host.c_str(), boundPort_);
  return core::format("ftp://%s:%d", host.c_str(), boundPort_);
}

FtpServer::ClientSession* FtpServer::findSession(int sessionId)
{
  std::map<int, ClientSession*>::iterator it = sessions_.find(sessionId);
  if (it == sessions_.end())
    return nullptr;
  return it->second;
}

void FtpServer::registerFdWatch(int fd, FdRef::Kind kind, int sessionId,
                                bool read, bool write)
{
  if (fd < 0 || loop_ == nullptr)
    return;

  FdRef* ref = nullptr;
  std::map<int, FdRef*>::iterator it = fdRefs_.find(fd);
  if (it != fdRefs_.end()) {
    ref = it->second;
  } else {
    ref = new FdRef();
    fdRefs_[fd] = ref;
  }
  ref->server = this;
  ref->kind = kind;
  ref->sessionId = sessionId;

  if (!read && !write)
    loop_->removeFd(fd);
  else
    loop_->setFdWatch(fd, read, write, &FtpServer::onFdEvent, ref);
}

void FtpServer::unregisterFd(int fd)
{
  if (fd < 0)
    return;
  if (loop_ != nullptr)
    loop_->removeFd(fd);
  std::map<int, FdRef*>::iterator it = fdRefs_.find(fd);
  if (it != fdRefs_.end()) {
    delete it->second;
    fdRefs_.erase(it);
  }
}

void FtpServer::onFdEvent(int fd, bool readable, bool writable, void* data)
{
  FdRef* ref = static_cast<FdRef*>(data);
  if (ref == nullptr || ref->server == nullptr)
    return;

  FtpServer* self = ref->server;
  FdRef::Kind kind = ref->kind;
  int sid = ref->sessionId;

  if (kind == FdRef::Kind::Listener) {
    self->handleListenerRead(fd);
    return;
  }

  ClientSession* s = self->findSession(sid);
  if (s == nullptr)
    return;

  switch (kind) {
  case FdRef::Kind::Control:
    self->handleControlEvent(s, readable, writable);
    break;
  case FdRef::Kind::PasvListener:
    self->handlePasvAccept(s);
    break;
  case FdRef::Kind::Data:
    self->handleDataEvent(s, readable, writable);
    break;
  case FdRef::Kind::Listener:
    break;
  }
}

void FtpServer::onTimeoutTimer(void* data)
{
  FtpServer* self = static_cast<FtpServer*>(data);
  if (self == nullptr)
    return;
  self->timerArmed_ = false;
  self->checkTimeout();
}

void FtpServer::armTimeoutTimer()
{
  if (loop_ == nullptr || timerArmed_)
    return;
  timerArmed_ = true;
  loop_->addTimer(1000, &FtpServer::onTimeoutTimer, this);
}

void FtpServer::stopTimeoutTimer()
{
  if (loop_ != nullptr && timerArmed_) {
    loop_->removeTimer(&FtpServer::onTimeoutTimer, this);
  }
  timerArmed_ = false;
}

void FtpServer::checkTimeout()
{
  if (activeOp_.kind == OpKind::Idle)
    return;

  time_t now = time(nullptr);
  if (activeOp_.startedAt > 0 && (now - activeOp_.startedAt) >= kOpTimeoutSeconds) {
    ClientSession* s = findSession(activeOp_.sessionId);
    if (s != nullptr) {
      closeDataChannel(s);
      s->dataState = ClientSession::DataTransferState::Idle;
      sendReply(s, "451 Timeout waiting for VNC server response.");
    }
    rfb::FileTransferClient* ft =
      backend_ != nullptr ? backend_->fileTransfer() : nullptr;
    if (ft != nullptr) {
      ft->abortPendingOp();
      if (backend_ != nullptr)
        backend_->flushOutput();
    }
    finishActiveOp();
    return;
  }

  armTimeoutTimer();
}

void FtpServer::handleListenerRead(int fd)
{
  for (;;) {
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    int cfd = ::accept(fd, reinterpret_cast<struct sockaddr*>(&ss), &len);
    if (cfd < 0)
      break;

    setNonBlockingCloExec(cfd);
    int one = 1;
    ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    ClientSession* s = new ClientSession();
    s->id = nextSessionId_++;
    s->ctrlFd = cfd;
    s->peerIp = socketPeerIp(cfd);
    s->localIp = socketLocalIp(cfd);

    sessions_[s->id] = s;
    sendReply(s, "220 TigerVNC RFB-to-FTP Bridge Ready");
    updateControlWatch(s);
  }
}

void FtpServer::sendReply(ClientSession* s, const std::string& line)
{
  if (s == nullptr)
    return;
  if (s->outBuf.size() + line.size() + 2 > kMaxCtrlOutBuf) {
    destroySession(s->id);
    return;
  }
  s->outBuf += line;
  if (line.size() < 2 || line.compare(line.size() - 2, 2, "\r\n") != 0)
    s->outBuf += "\r\n";
  updateControlWatch(s);
}

void FtpServer::updateControlWatch(ClientSession* s)
{
  if (s == nullptr || s->ctrlFd < 0)
    return;
  bool wantWrite = s->outOff < s->outBuf.size();
  bool wantRead = !s->closeAfterFlush;
  registerFdWatch(s->ctrlFd, FdRef::Kind::Control, s->id, wantRead, wantWrite);
}

void FtpServer::updateDataWatch(ClientSession* s)
{
  if (s == nullptr || s->dataFd < 0)
    return;

  if (s->dataConnectPending) {
    registerFdWatch(s->dataFd, FdRef::Kind::Data, s->id, false, true);
    return;
  }

  bool wantRead = false;
  bool wantWrite = false;

  if (s->dataState == ClientSession::DataTransferState::Transferring) {
    if (activeOp_.sessionId == s->id && activeOp_.kind == OpKind::Stor) {
      wantRead = !s->uploadAckPending && !s->uploadCongestedPaused &&
                 !s->uploadReadEof;
    } else {
      // Sending listing or RETR download data
      wantWrite = (s->dataOutOff < s->dataOutBuf.size());
    }
  }

  registerFdWatch(s->dataFd, FdRef::Kind::Data, s->id, wantRead, wantWrite);
}

void FtpServer::handleControlEvent(ClientSession* s,
                                   bool readable, bool writable)
{
  int sid = s->id;

  if (writable && s->outOff < s->outBuf.size()) {
    while (s->outOff < s->outBuf.size()) {
      ssize_t n = ::send(s->ctrlFd, s->outBuf.data() + s->outOff,
                         s->outBuf.size() - s->outOff, MSG_NOSIGNAL);
      if (n > 0) {
        s->outOff += static_cast<size_t>(n);
      } else if (n < 0 && errno == EINTR) {
        continue;
      } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        break;
      } else {
        destroySession(sid);
        return;
      }
    }

    if (s->outOff >= s->outBuf.size()) {
      s->outBuf.clear();
      s->outOff = 0;
      if (s->closeAfterFlush) {
        destroySession(sid);
        return;
      }
    }
    updateControlWatch(s);
  }

  if (!readable)
    return;

  s = findSession(sid);
  if (s == nullptr || s->closeAfterFlush)
    return;

  char buf[4096];
  for (;;) {
    ssize_t n = ::recv(s->ctrlFd, buf, sizeof(buf), 0);
    if (n > 0) {
      if (s->inBuf.size() + static_cast<size_t>(n) > kMaxCtrlLineLen) {
        sendReply(s, "500 Line too long.");
        s->closeAfterFlush = true;
        updateControlWatch(s);
        return;
      }
      s->inBuf.append(buf, static_cast<size_t>(n));
    } else if (n == 0) {
      destroySession(sid);
      return;
    } else {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;
      destroySession(sid);
      return;
    }
  }

  for (;;) {
    s = findSession(sid);
    if (s == nullptr || s->closeAfterFlush)
      return;
    size_t nl = s->inBuf.find('\n');
    if (nl == std::string::npos)
      break;
    std::string rawLine = s->inBuf.substr(0, nl + 1);
    s->inBuf.erase(0, nl + 1);
    processControlLine(s, rawLine);
  }
}

void FtpServer::destroySession(int sessionId)
{
  ClientSession* s = findSession(sessionId);
  if (s == nullptr)
    return;

  // Remove any pending queued ops belonging to this session
  for (std::deque<QueuedOp>::iterator it = opQueue_.begin();
       it != opQueue_.end(); ) {
    if (it->sessionId == sessionId)
      it = opQueue_.erase(it);
    else
      ++it;
  }

  closeDataChannel(s);

  if (s->ctrlFd >= 0) {
    unregisterFd(s->ctrlFd);
    closeRawFd(s->ctrlFd);
    s->ctrlFd = -1;
  }

  // If this session owns the in-flight RFB operation, cancel downloads/uploads
  // cleanly so the RFB stream stays synchronized.
  if (activeOp_.sessionId == sessionId && activeOp_.kind != OpKind::Idle) {
    activeOp_.sessionId = 0;
    rfb::FileTransferClient* ft =
      backend_ != nullptr ? backend_->fileTransfer() : nullptr;
    if (activeOp_.kind == OpKind::Retr && !rfbDownloadFinished_ &&
        ft != nullptr && ft->isBusy()) {
      sessions_.erase(sessionId);
      delete s;
      ft->cancelDownload();
      if (backend_ != nullptr)
        backend_->flushOutput();
      return;
    }
    if (activeOp_.kind == OpKind::Stor && ft != nullptr && ft->isBusy()) {
      sessions_.erase(sessionId);
      delete s;
      ft->cancelUpload();
      if (backend_ != nullptr)
        backend_->flushOutput();
      return;
    }
  }

  sessions_.erase(sessionId);
  delete s;
}

void FtpServer::closeDataChannel(ClientSession* s)
{
  if (s == nullptr)
    return;

  if (s->pasvListenFd >= 0) {
    unregisterFd(s->pasvListenFd);
    closeRawFd(s->pasvListenFd);
    s->pasvListenFd = -1;
  }
  if (s->dataFd >= 0) {
    unregisterFd(s->dataFd);
    closeRawFd(s->dataFd);
    s->dataFd = -1;
  }
  s->dataMode = ClientSession::DataMode::Unconfigured;
  s->activeHost.clear();
  s->activePort = 0;
  s->dataConnectPending = false;
  s->dataConnected = false;
  s->dataOutBuf.clear();
  s->dataOutOff = 0;
  s->dataOutEof = false;
  s->downloadPullDeferred = false;
  s->uploadAckPending = false;
  s->uploadCongestedPaused = false;
  s->uploadReadEof = false;
}

bool FtpServer::startPassiveListener(ClientSession* s, bool ipv4Only)
{
  closeDataChannel(s);

  std::string bindHost = s->localIp;
  if (ipv4Only) {
    if (bindHost.empty() || bindHost.find(':') != std::string::npos)
      bindHost = isLoopbackIp(s->localIp) ? "127.0.0.1" : "0.0.0.0";
  } else if (bindHost.empty()) {
    bindHost = "127.0.0.1";
  }

  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = ipv4Only ? AF_INET : AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICHOST | AI_PASSIVE;

  struct addrinfo* ai = nullptr;
  if (::getaddrinfo(bindHost.c_str(), "0", &hints, &ai) != 0)
    return false;

  int lfd = -1;
  for (struct addrinfo* cur = ai; cur != nullptr; cur = cur->ai_next) {
    lfd = ::socket(cur->ai_family, cur->ai_socktype, cur->ai_protocol);
    if (lfd < 0)
      continue;
    setNonBlockingCloExec(lfd);
    int one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (::bind(lfd, cur->ai_addr, cur->ai_addrlen) == 0 &&
        ::listen(lfd, 1) == 0) {
      break;
    }
    closeRawFd(lfd);
    lfd = -1;
  }
  ::freeaddrinfo(ai);

  if (lfd < 0)
    return false;

  s->pasvListenFd = lfd;
  s->dataMode = ClientSession::DataMode::PassiveListen;
  registerFdWatch(lfd, FdRef::Kind::PasvListener, s->id, true, false);
  return true;
}

void FtpServer::handlePasvAccept(ClientSession* s)
{
  if (s == nullptr || s->pasvListenFd < 0)
    return;

  struct sockaddr_storage ss;
  socklen_t len = sizeof(ss);
  int cfd = ::accept(s->pasvListenFd,
                     reinterpret_cast<struct sockaddr*>(&ss), &len);
  if (cfd < 0)
    return;

  std::string dataPeer = socketPeerIp(cfd);
  if (!isSamePeerOrLoopback(dataPeer, s->peerIp)) {
    vlog.info("Rejected PASV data connection from %s (control peer %s)",
              dataPeer.c_str(), s->peerIp.c_str());
    closeRawFd(cfd);
    return;
  }

  setNonBlockingCloExec(cfd);

  unregisterFd(s->pasvListenFd);
  closeRawFd(s->pasvListenFd);
  s->pasvListenFd = -1;

  if (s->dataFd >= 0) {
    unregisterFd(s->dataFd);
    closeRawFd(s->dataFd);
  }

  s->dataFd = cfd;
  s->dataConnectPending = false;
  s->dataConnected = true;

  if (s->dataState == ClientSession::DataTransferState::WaitingDataChannel)
    beginDataChannelTransfer(s);
}

bool FtpServer::beginActiveConnect(ClientSession* s)
{
  if (s == nullptr || s->activeHost.empty() || s->activePort <= 0)
    return false;

  if (s->dataFd >= 0) {
    unregisterFd(s->dataFd);
    closeRawFd(s->dataFd);
    s->dataFd = -1;
  }

  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICHOST;

  char portBuf[16];
  std::snprintf(portBuf, sizeof(portBuf), "%d", s->activePort);

  struct addrinfo* ai = nullptr;
  if (::getaddrinfo(s->activeHost.c_str(), portBuf, &hints, &ai) != 0)
    return false;

  int dfd = -1;
  bool pending = false;
  bool connected = false;

  for (struct addrinfo* cur = ai; cur != nullptr; cur = cur->ai_next) {
    dfd = ::socket(cur->ai_family, cur->ai_socktype, cur->ai_protocol);
    if (dfd < 0)
      continue;
    setNonBlockingCloExec(dfd);

    int rc = ::connect(dfd, cur->ai_addr, cur->ai_addrlen);
    if (rc == 0) {
      connected = true;
      break;
    }
    if (errno == EINPROGRESS) {
      pending = true;
      break;
    }
    closeRawFd(dfd);
    dfd = -1;
  }
  ::freeaddrinfo(ai);

  if (dfd < 0)
    return false;

  s->dataFd = dfd;
  s->dataConnectPending = pending;
  s->dataConnected = connected;

  if (connected) {
    beginDataChannelTransfer(s);
  } else {
    s->dataState = ClientSession::DataTransferState::WaitingDataChannel;
    updateDataWatch(s);
  }
  return true;
}

void FtpServer::beginPortOrWaitTransfer(ClientSession* s)
{
  if (s == nullptr)
    return;

  if (s->dataConnected) {
    beginDataChannelTransfer(s);
    return;
  }

  if (s->dataMode == ClientSession::DataMode::ActiveTarget) {
    if (!beginActiveConnect(s)) {
      bool cancelRfb = (activeOp_.sessionId == s->id &&
                        (activeOp_.kind == OpKind::Retr ||
                         activeOp_.kind == OpKind::Stor));
      closeDataChannel(s);
      s->dataState = ClientSession::DataTransferState::Idle;
      sendReply(s, "425 Can't open data connection.");
      if (cancelRfb) {
        rfb::FileTransferClient* ft = backend_->fileTransfer();
        if (ft != nullptr && ft->isBusy()) {
          if (activeOp_.kind == OpKind::Retr)
            ft->cancelDownload();
          else
            ft->cancelUpload();
          if (backend_ != nullptr)
            backend_->flushOutput();
          return;
        }
      }
      if (activeOp_.sessionId == s->id)
        finishActiveOp();
    }
    return;
  }

  // Passive listener waiting for the FTP client to complete TCP connect()
  s->dataState = ClientSession::DataTransferState::WaitingDataChannel;
}

void FtpServer::beginDataChannelTransfer(ClientSession* s)
{
  if (s == nullptr)
    return;

  bool isUpload = (activeOp_.sessionId == s->id && activeOp_.kind == OpKind::Stor);
  bool isDownload = (activeOp_.sessionId == s->id && activeOp_.kind == OpKind::Retr);

  if (isUpload)
    sendReply(s, "150 Ok to send data.");
  else if (isDownload)
    sendReply(s, "150 Opening BINARY mode data connection.");
  else
    sendReply(s, "150 Here comes the directory listing.");

  s->dataState = ClientSession::DataTransferState::Transferring;

  if (isDownload && rfbDownloadFinished_) {
    if (!rfbDownloadOk_) {
      closeDataChannel(s);
      s->dataState = ClientSession::DataTransferState::Idle;
      sendReply(s, core::format("451 %s",
                                rfbDownloadError_.empty()
                                  ? "Download failed."
                                  : rfbDownloadError_.c_str()));
      finishActiveOp();
      return;
    }
    s->dataOutEof = true;
  }

  if (!isUpload && s->dataOutEof && s->dataOutOff >= s->dataOutBuf.size()) {
    closeDataChannel(s);
    s->dataState = ClientSession::DataTransferState::Idle;
    sendReply(s, "226 Transfer complete.");
    if (isDownload)
      finishActiveOp();
    return;
  }

  updateDataWatch(s);
}

void FtpServer::handleDataEvent(ClientSession* s,
                                bool readable, bool writable)
{
  if (s == nullptr || s->dataFd < 0)
    return;

  int sid = s->id;

  if (s->dataConnectPending && writable) {
    int err = 0;
    socklen_t errLen = sizeof(err);
    if (::getsockopt(s->dataFd, SOL_SOCKET, SO_ERROR, &err, &errLen) != 0 ||
        err != 0) {
      bool cancelRfb = (activeOp_.sessionId == sid &&
                        (activeOp_.kind == OpKind::Retr ||
                         activeOp_.kind == OpKind::Stor));
      closeDataChannel(s);
      s->dataState = ClientSession::DataTransferState::Idle;
      sendReply(s, "425 Can't open data connection.");
      if (cancelRfb) {
        rfb::FileTransferClient* ft = backend_->fileTransfer();
        if (ft != nullptr && ft->isBusy()) {
          if (activeOp_.kind == OpKind::Retr)
            ft->cancelDownload();
          else
            ft->cancelUpload();
          if (backend_ != nullptr)
            backend_->flushOutput();
          return;
        }
      }
      if (activeOp_.sessionId == sid)
        finishActiveOp();
      return;
    }
    s->dataConnectPending = false;
    s->dataConnected = true;
    beginDataChannelTransfer(s);
    return;
  }

  if (s->dataState != ClientSession::DataTransferState::Transferring)
    return;

  // STOR / APPE upload path (read from dataFd -> push to RFB)
  if (activeOp_.sessionId == sid && activeOp_.kind == OpKind::Stor) {
    if (!readable || s->uploadAckPending || s->uploadCongestedPaused)
      return;

    rfb::FileTransferClient* ft = backend_->fileTransfer();
    if (ft == nullptr) {
      closeDataChannel(s);
      s->dataState = ClientSession::DataTransferState::Idle;
      sendReply(s, "451 VNC connection lost.");
      finishActiveOp();
      return;
    }

    uint8_t buf[65536];
    ssize_t n = ::recv(s->dataFd, buf, sizeof(buf), 0);
    if (n > 0) {
      activeOp_.startedAt = time(nullptr);
      s->uploadAckPending = true;
      ft->sendUploadData(buf, static_cast<size_t>(n));
      backend_->flushOutput();
      // sendUploadData might invoke onUploadChunkAck / onUploadComplete sync
      s = findSession(sid);
      if (s == nullptr || s->dataState != ClientSession::DataTransferState::Transferring)
        return;
      if (!s->uploadAckPending && backend_->isOutputCongested())
        s->uploadCongestedPaused = true;
      updateDataWatch(s);
    } else if (n == 0) {
      s->uploadReadEof = true;
      closeDataChannel(s);
      activeOp_.startedAt = time(nullptr);
      if (!s->uploadAckPending) {
        ft->finishUpload(static_cast<uint64_t>(time(nullptr)));
        backend_->flushOutput();
      }
    } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
      closeDataChannel(s);
      if (ft->isBusy()) {
        s->dataState = ClientSession::DataTransferState::Canceling;
        ft->cancelUpload();
        backend_->flushOutput();
      } else {
        s->dataState = ClientSession::DataTransferState::Idle;
        sendReply(s, "426 Data connection error.");
        finishActiveOp();
      }
    }
    return;
  }

  // LIST / MLSD / NLST / RETR download path (write dataOutBuf -> dataFd)
  if (!writable)
    return;

  while (s->dataOutOff < s->dataOutBuf.size()) {
    ssize_t n = ::send(s->dataFd, s->dataOutBuf.data() + s->dataOutOff,
                       s->dataOutBuf.size() - s->dataOutOff, MSG_NOSIGNAL);
    if (n > 0) {
      s->dataOutOff += static_cast<size_t>(n);
      if (activeOp_.sessionId == sid && activeOp_.kind == OpKind::Retr)
        activeOp_.startedAt = time(nullptr);
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    } else {
      bool isRetr = (activeOp_.sessionId == sid &&
                     activeOp_.kind == OpKind::Retr);
      closeDataChannel(s);
      s->dataState = ClientSession::DataTransferState::Idle;
      if (isRetr && !rfbDownloadFinished_) {
        rfb::FileTransferClient* ft = backend_->fileTransfer();
        if (ft != nullptr && ft->isBusy()) {
          s->dataState = ClientSession::DataTransferState::Canceling;
          ft->cancelDownload();
          backend_->flushOutput();
          return;
        }
      }
      sendReply(s, "426 Connection closed; transfer aborted.");
      if (activeOp_.sessionId == sid)
        finishActiveOp();
      return;
    }
  }

  if (s->dataOutOff >= s->dataOutBuf.size()) {
    s->dataOutBuf.clear();
    s->dataOutOff = 0;
  } else if (s->dataOutOff >= 65536) {
    s->dataOutBuf.erase(0, s->dataOutOff);
    s->dataOutOff = 0;
  }

  if (s->downloadPullDeferred &&
      activeOp_.sessionId == sid && activeOp_.kind == OpKind::Retr &&
      !rfbDownloadFinished_) {
    size_t unread = s->dataOutBuf.size() - s->dataOutOff;
    if (unread < kLowWaterMark) {
      s->downloadPullDeferred = false;
      rfb::FileTransferClient* ft = backend_->fileTransfer();
      if (ft != nullptr)
        ft->requestMoreDownloadData();
    }
  }

  s = findSession(sid);
  if (s == nullptr)
    return;

  if (s->dataOutEof && s->dataOutOff >= s->dataOutBuf.size()) {
    bool isRetr = (activeOp_.sessionId == sid && activeOp_.kind == OpKind::Retr);
    closeDataChannel(s);
    s->dataState = ClientSession::DataTransferState::Idle;
    sendReply(s, "226 Transfer complete.");
    if (isRetr)
      finishActiveOp();
    return;
  }

  updateDataWatch(s);
}

void FtpServer::onBackendOutputDrained()
{
  if (activeOp_.kind != OpKind::Stor || backend_ == nullptr)
    return;
  ClientSession* s = findSession(activeOp_.sessionId);
  if (s == nullptr)
    return;
  if (s->uploadCongestedPaused && !backend_->isOutputCongested()) {
    s->uploadCongestedPaused = false;
    updateDataWatch(s);
  }
}

void FtpServer::processControlLine(ClientSession* s, const std::string& rawLine)
{
  std::string cmd;
  std::string arg;
  if (!ftp::stripTelnetAndParseCommand(rawLine, &cmd, &arg))
    return;

  if (cmd != "RNTO")
    s->hasRnfr = false;

  if (cmd == "NOOP") {
    sendReply(s, "200 NOOP ok.");
    return;
  }

  if (cmd == "QUIT") {
    sendReply(s, "221 Goodbye.");
    s->closeAfterFlush = true;
    updateControlWatch(s);
    return;
  }

  if (cmd == "AUTH") {
    sendReply(s, "502 TLS authentication not supported.");
    return;
  }

  if (cmd == "FEAT") {
    sendReply(s,
      "211-Features:\r\n"
      " MDTM\r\n"
      " REST STREAM\r\n"
      " SIZE\r\n"
      " MLST type*;size*;modify*;\r\n"
      " MLSD\r\n"
      " UTF8\r\n"
      "211 End");
    return;
  }

  if (cmd == "SYST") {
    sendReply(s, "215 UNIX Type: L8");
    return;
  }

  if (cmd == "CLNT") {
    sendReply(s, "200 Noted.");
    return;
  }

  if (cmd == "OPTS") {
    std::string upper = arg;
    for (size_t i = 0; i < upper.size(); i++)
      upper[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(upper[i])));
    if (upper == "UTF8 ON" || upper == "UTF8")
      sendReply(s, "200 Always in UTF8 mode.");
    else if (upper.compare(0, 4, "MLST") == 0)
      sendReply(s, "200 MLST options accepted.");
    else
      sendReply(s, "501 Option not understood.");
    return;
  }

  if (cmd == "HELP") {
    sendReply(s, "214 TigerVNC RFB-to-FTP Bridge.");
    return;
  }

  if (cmd == "USER") {
    s->pendingUser = arg;
    s->authenticated = false;
    sendReply(s, "331 Please specify the password.");
    return;
  }

  if (cmd == "PASS") {
    if (!config_.username.empty() && s->pendingUser != config_.username) {
      s->authenticated = false;
      sendReply(s, "530 Login incorrect.");
      return;
    }
    if (!config_.password.empty() && arg != config_.password) {
      s->authenticated = false;
      sendReply(s, "530 Login incorrect.");
      return;
    }
    s->authenticated = true;
    sendReply(s, "230 Login successful.");
    return;
  }

  if (!s->authenticated) {
    sendReply(s, "530 Please login with USER and PASS.");
    return;
  }

  if (cmd == "TYPE") {
    std::string upper = arg;
    for (size_t i = 0; i < upper.size(); i++)
      upper[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(upper[i])));
    if (upper == "I" || upper == "L 8" || upper == "L8") {
      sendReply(s, "200 Switching to Binary mode.");
    } else if (upper == "A" || upper == "A N" || upper == "AN") {
      sendReply(s, "200 Switching to ASCII mode.");
    } else {
      sendReply(s, "504 Command not implemented for that parameter.");
    }
    return;
  }

  if (cmd == "MODE") {
    std::string upper = arg;
    for (size_t i = 0; i < upper.size(); i++)
      upper[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(upper[i])));
    if (upper == "S")
      sendReply(s, "200 Mode set to S.");
    else
      sendReply(s, "504 Bad MODE command.");
    return;
  }

  if (cmd == "STRU") {
    std::string upper = arg;
    for (size_t i = 0; i < upper.size(); i++)
      upper[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(upper[i])));
    if (upper == "F")
      sendReply(s, "200 Structure set to F.");
    else
      sendReply(s, "504 Bad STRU command.");
    return;
  }

  if (cmd == "PWD" || cmd == "XPWD") {
    std::string escaped = ftp::escapeFtpPath(s->cwd);
    sendReply(s, core::format("257 \"%s\" is the current directory",
                              escaped.c_str()));
    return;
  }

  if (cmd == "REST") {
    if (arg.empty()) {
      sendReply(s, "501 Syntax error in REST parameter.");
      return;
    }
    char* endp = nullptr;
    unsigned long long val = std::strtoull(arg.c_str(), &endp, 10);
    if (endp == arg.c_str() || *endp != '\0') {
      sendReply(s, "501 Syntax error in REST parameter.");
      return;
    }
    s->restOffset = static_cast<uint64_t>(val);
    s->hasRest = true;
    sendReply(s, core::format("350 Restart position accepted (%llu).", val));
    return;
  }

  if (cmd == "PASV") {
    if (!startPassiveListener(s, true)) {
      sendReply(s, "425 Cannot open passive connection.");
      return;
    }
    std::string local = socketLocalIp(s->pasvListenFd);
    int port = socketLocalPort(s->pasvListenFd);
    sendReply(s, ftp::formatPasvReply(local, port));
    return;
  }

  if (cmd == "EPSV") {
    std::string upper = arg;
    for (size_t i = 0; i < upper.size(); i++)
      upper[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(upper[i])));
    if (upper == "ALL") {
      sendReply(s, "200 EPSV ALL ok.");
      return;
    }
    bool ipv4Only = false;
    if (upper == "1") {
      ipv4Only = true;
    } else if (!upper.empty() && upper != "2") {
      sendReply(s, "522 Network protocol not supported.");
      return;
    }
    if (upper == "2" && s->localIp.find(':') == std::string::npos) {
      sendReply(s, "522 Network protocol not supported.");
      return;
    }
    if (!startPassiveListener(s, ipv4Only)) {
      sendReply(s, "425 Cannot open extended passive connection.");
      return;
    }
    int port = socketLocalPort(s->pasvListenFd);
    sendReply(s, ftp::formatEpsvReply(port));
    return;
  }

  if (cmd == "PORT") {
    std::string host;
    int port = 0;
    if (!ftp::parsePortArg(arg, &host, &port)) {
      sendReply(s, "501 Illegal PORT command.");
      return;
    }
    if (!isSamePeerOrLoopback(host, s->peerIp)) {
      sendReply(s, "501 Illegal PORT address.");
      return;
    }
    closeDataChannel(s);
    s->dataMode = ClientSession::DataMode::ActiveTarget;
    s->activeHost = host;
    s->activePort = port;
    sendReply(s, "200 PORT command successful.");
    return;
  }

  if (cmd == "EPRT") {
    int fam = 0;
    std::string host;
    int port = 0;
    if (!ftp::parseEprtArg(arg, &fam, &host, &port)) {
      sendReply(s, "501 Illegal EPRT command.");
      return;
    }
    if (!isSamePeerOrLoopback(host, s->peerIp)) {
      sendReply(s, "501 Illegal EPRT address.");
      return;
    }
    closeDataChannel(s);
    s->dataMode = ClientSession::DataMode::ActiveTarget;
    s->activeHost = host;
    s->activePort = port;
    sendReply(s, "200 EPRT command successful.");
    return;
  }

  if (cmd == "ABOR") {
    for (std::deque<QueuedOp>::iterator it = opQueue_.begin();
         it != opQueue_.end(); ) {
      if (it->sessionId == s->id)
        it = opQueue_.erase(it);
      else
        ++it;
    }

    if (activeOp_.sessionId == s->id &&
        (activeOp_.kind == OpKind::Retr || activeOp_.kind == OpKind::Stor)) {
      bool hadSent150 =
        (s->dataState == ClientSession::DataTransferState::Transferring);
      closeDataChannel(s);
      rfb::FileTransferClient* ft =
        backend_ != nullptr ? backend_->fileTransfer() : nullptr;
      if (ft != nullptr && ft->isBusy()) {
        s->dataState = ClientSession::DataTransferState::Canceling;
        if (hadSent150)
          sendReply(s, "426 Transfer aborted.");
        if (activeOp_.kind == OpKind::Retr)
          ft->cancelDownload();
        else
          ft->cancelUpload();
        if (backend_ != nullptr)
          backend_->flushOutput();
        return;
      }
      s->dataState = ClientSession::DataTransferState::Idle;
      if (hadSent150)
        sendReply(s, "426 Transfer aborted.");
      sendReply(s, "226 Abort successful.");
      finishActiveOp();
      return;
    }

    bool hadActiveTransfer =
      (s->dataState == ClientSession::DataTransferState::Transferring);
    closeDataChannel(s);
    s->dataState = ClientSession::DataTransferState::Idle;
    if (hadActiveTransfer)
      sendReply(s, "426 Transfer aborted.");
    sendReply(s, "226 Abort successful.");
    return;
  }

  // Remaining commands interact with the VNC backend
  if (backend_ == nullptr || !backend_->isConnected()) {
    closeDataChannel(s);
    s->hasRest = false;
    sendReply(s, "421 VNC server is not connected.");
    return;
  }

  rfb::FileTransferClient* ft = backend_->fileTransfer();
  if (ft == nullptr || !ft->isSupported()) {
    closeDataChannel(s);
    s->hasRest = false;
    sendReply(s, "550 File transfer is not supported by the VNC server.");
    return;
  }
  const rfb::FileTransferCaps& caps = ft->caps();

  if (cmd == "CDUP" || cmd == "XCUP") {
    s->cwd = ftp::normalizeFtpPath(s->cwd, "..");
    sendReply(s, "250 Directory successfully changed.");
    return;
  }

  if (cmd == "CWD" || cmd == "XCWD") {
    std::string target = ftp::normalizeFtpPath(s->cwd, arg);
    if (target == "/") {
      s->cwd = "/";
      sendReply(s, "250 Directory successfully changed.");
      return;
    }
    std::vector<rfb::FileEntry> cached;
    if (getCachedDir(target, &cached)) {
      s->cwd = target;
      sendReply(s, "250 Directory successfully changed.");
      return;
    }
    std::string parent, base;
    ftp::splitFtpPath(target, &parent, &base);
    if (getCachedDir(parent, &cached)) {
      bool found = false;
      bool isDir = false;
      for (size_t i = 0; i < cached.size(); i++) {
        if (cached[i].name == base) {
          found = true;
          isDir = cached[i].isDir;
          break;
        }
      }
      if (found && isDir) {
        s->cwd = target;
        sendReply(s, "250 Directory successfully changed.");
        return;
      }
      if (found && !isDir) {
        sendReply(s, "550 Not a directory.");
        return;
      }
      sendReply(s, "550 Failed to change directory.");
      return;
    }
    if (!caps.canList) {
      sendReply(s, "550 Directory listing not supported by server.");
      return;
    }
    QueuedOp op;
    op.sessionId = s->id;
    op.kind = OpKind::Cwd;
    op.path = target;
    enqueueOp(op);
    return;
  }

  if (cmd == "SIZE" || cmd == "MDTM" || cmd == "MLST") {
    std::string target = ftp::normalizeFtpPath(s->cwd, arg);
    if (target == "/") {
      if (cmd == "MLST") {
        rfb::FileEntry rootEntry;
        rootEntry.name = "/";
        rootEntry.isDir = true;
        sendReply(s, core::format("250-Listing /\r\n%s\r\n250 End",
                                  ftp::formatMlstLine("/", rootEntry).c_str()));
      } else {
        sendReply(s, "550 Not a regular file.");
      }
      return;
    }

    QueuedOp op;
    op.sessionId = s->id;
    op.kind = (cmd == "SIZE") ? OpKind::StatSize
            : (cmd == "MDTM") ? OpKind::StatMdtm
                              : OpKind::StatMlst;
    op.path = target;

    std::string parent, base;
    ftp::splitFtpPath(target, &parent, &base);
    std::vector<rfb::FileEntry> cached;
    if (getCachedDir(parent, &cached)) {
      handleStatResult(s, op, cached);
      return;
    }

    if (!caps.canList) {
      sendReply(s, "550 Directory listing not supported by server.");
      return;
    }
    enqueueOp(op);
    return;
  }

  if (cmd == "LIST" || cmd == "MLSD" || cmd == "NLST") {
    if (s->dataMode == ClientSession::DataMode::Unconfigured) {
      sendReply(s, "425 Use PORT or PASV first.");
      return;
    }
    if (!caps.canList) {
      closeDataChannel(s);
      sendReply(s, "550 Directory listing not supported by server.");
      return;
    }
    std::string pathArg = ftp::stripListFlags(arg);
    std::string target = ftp::normalizeFtpPath(s->cwd, pathArg);

    QueuedOp op;
    op.sessionId = s->id;
    op.kind = (cmd == "MLSD") ? OpKind::Mlsd
            : (cmd == "NLST") ? OpKind::Nlst
                              : OpKind::List;
    op.path = target;
    s->dataState = ClientSession::DataTransferState::WaitingRfbStart;
    enqueueOp(op);
    return;
  }

  if (cmd == "RETR") {
    uint64_t off = s->hasRest ? s->restOffset : 0;
    s->hasRest = false;
    s->restOffset = 0;

    if (s->dataMode == ClientSession::DataMode::Unconfigured) {
      sendReply(s, "425 Use PORT or PASV first.");
      return;
    }
    if (!caps.canDownload) {
      closeDataChannel(s);
      sendReply(s, "550 File download not supported by server.");
      return;
    }
    if (off > 0 && !caps.canResumeDownload) {
      closeDataChannel(s);
      sendReply(s, "550 Resume download not supported by server.");
      return;
    }

    std::string target = ftp::normalizeFtpPath(s->cwd, arg);
    if (target == "/") {
      closeDataChannel(s);
      sendReply(s, "550 Cannot retrieve directory.");
      return;
    }

    QueuedOp op;
    op.sessionId = s->id;
    op.kind = OpKind::Retr;
    op.path = target;
    op.offset = off;
    s->dataState = ClientSession::DataTransferState::WaitingRfbStart;
    enqueueOp(op);
    return;
  }

  if (cmd == "STOR" || cmd == "APPE") {
    uint64_t off = s->hasRest ? s->restOffset : 0;
    s->hasRest = false;
    s->restOffset = 0;

    if (s->dataMode == ClientSession::DataMode::Unconfigured) {
      sendReply(s, "425 Use PORT or PASV first.");
      return;
    }
    if (backend_->isViewOnly()) {
      closeDataChannel(s);
      sendReply(s, "550 Permission denied (view-only mode).");
      return;
    }
    if (!caps.canUpload) {
      closeDataChannel(s);
      sendReply(s, "550 File upload not supported by server.");
      return;
    }
    if (cmd == "STOR" && off > 0 && !caps.canResumeUpload) {
      closeDataChannel(s);
      sendReply(s, "550 Resume upload not supported by server.");
      return;
    }

    std::string target = ftp::normalizeFtpPath(s->cwd, arg);
    if (target == "/") {
      closeDataChannel(s);
      sendReply(s, "550 Invalid filename.");
      return;
    }

    QueuedOp op;
    op.sessionId = s->id;
    op.path = target;
    op.overwrite = true;

    if (cmd == "APPE") {
      std::string parent, base;
      ftp::splitFtpPath(target, &parent, &base);
      std::vector<rfb::FileEntry> cached;
      if (getCachedDir(parent, &cached)) {
        uint64_t existingSize = 0;
        for (size_t i = 0; i < cached.size(); i++) {
          if (cached[i].name == base) {
            if (cached[i].isDir) {
              closeDataChannel(s);
              sendReply(s, "550 Target is a directory.");
              return;
            }
            existingSize = cached[i].size;
            break;
          }
        }
        if (existingSize > 0 && !caps.canResumeUpload) {
          closeDataChannel(s);
          sendReply(s, "550 Append not supported by server.");
          return;
        }
        op.kind = OpKind::Stor;
        op.offset = existingSize;
      } else {
        op.kind = OpKind::AppeStat;
      }
    } else {
      op.kind = OpKind::Stor;
      op.offset = off;
    }

    s->dataState = ClientSession::DataTransferState::WaitingRfbStart;
    enqueueOp(op);
    return;
  }

  if (cmd == "MKD" || cmd == "XMKD") {
    if (backend_->isViewOnly()) {
      sendReply(s, "550 Permission denied (view-only mode).");
      return;
    }
    if (!caps.canMkdir) {
      sendReply(s, "550 Create directory not supported by server.");
      return;
    }
    std::string target = ftp::normalizeFtpPath(s->cwd, arg);
    if (target == "/") {
      sendReply(s, "550 Cannot create root directory.");
      return;
    }
    QueuedOp op;
    op.sessionId = s->id;
    op.kind = OpKind::Mkd;
    op.path = target;
    enqueueOp(op);
    return;
  }

  if (cmd == "RMD" || cmd == "XRMD") {
    if (backend_->isViewOnly()) {
      sendReply(s, "550 Permission denied (view-only mode).");
      return;
    }
    if (!caps.canDelete) {
      sendReply(s, "550 Remove directory not supported by server.");
      return;
    }
    std::string target = ftp::normalizeFtpPath(s->cwd, arg);
    if (target == "/") {
      sendReply(s, "550 Cannot remove root directory.");
      return;
    }
    QueuedOp op;
    op.sessionId = s->id;
    op.kind = OpKind::Rmd;
    op.path = target;
    op.isDir = true;
    enqueueOp(op);
    return;
  }

  if (cmd == "DELE") {
    if (backend_->isViewOnly()) {
      sendReply(s, "550 Permission denied (view-only mode).");
      return;
    }
    if (!caps.canDelete) {
      sendReply(s, "550 Delete file not supported by server.");
      return;
    }
    std::string target = ftp::normalizeFtpPath(s->cwd, arg);
    if (target == "/") {
      sendReply(s, "550 Invalid filename.");
      return;
    }
    QueuedOp op;
    op.sessionId = s->id;
    op.kind = OpKind::Dele;
    op.path = target;
    op.isDir = false;
    enqueueOp(op);
    return;
  }

  if (cmd == "RNFR") {
    if (backend_->isViewOnly()) {
      sendReply(s, "550 Permission denied (view-only mode).");
      return;
    }
    if (!caps.canRename) {
      sendReply(s, "550 Rename not supported by server.");
      return;
    }
    std::string target = ftp::normalizeFtpPath(s->cwd, arg);
    if (target == "/") {
      sendReply(s, "550 Invalid source path.");
      return;
    }
    s->rnfrPath = target;
    s->hasRnfr = true;
    sendReply(s, "350 Ready for RNTO.");
    return;
  }

  if (cmd == "RNTO") {
    if (!s->hasRnfr) {
      sendReply(s, "503 RNFR required first.");
      return;
    }
    std::string fromPath = s->rnfrPath;
    s->hasRnfr = false;
    if (backend_->isViewOnly()) {
      sendReply(s, "550 Permission denied (view-only mode).");
      return;
    }
    if (!caps.canRename) {
      sendReply(s, "550 Rename not supported by server.");
      return;
    }
    std::string toPath = ftp::normalizeFtpPath(s->cwd, arg);
    if (toPath == "/") {
      sendReply(s, "550 Invalid destination path.");
      return;
    }
    QueuedOp op;
    op.sessionId = s->id;
    op.kind = OpKind::Rename;
    op.path = fromPath;
    op.path2 = toPath;
    enqueueOp(op);
    return;
  }

  sendReply(s, core::format("502 Command '%s' not implemented.", cmd.c_str()));
}

void FtpServer::enqueueOp(const QueuedOp& op)
{
  opQueue_.push_back(op);
  pumpQueue();
}

void FtpServer::pumpQueue()
{
  if (activeOp_.kind != OpKind::Idle)
    return;

  while (!opQueue_.empty()) {
    QueuedOp next = opQueue_.front();
    opQueue_.pop_front();

    ClientSession* s = findSession(next.sessionId);
    if (s == nullptr)
      continue;

    if (backend_ == nullptr || !backend_->isConnected()) {
      closeDataChannel(s);
      s->dataState = ClientSession::DataTransferState::Idle;
      sendReply(s, "421 VNC server disconnected.");
      continue;
    }

    rfb::FileTransferClient* ft = backend_->fileTransfer();
    if (ft == nullptr || !ft->isSupported()) {
      closeDataChannel(s);
      s->dataState = ClientSession::DataTransferState::Idle;
      sendReply(s, "550 File transfer not available.");
      continue;
    }

    startActiveOp(next);
    return;
  }

  stopTimeoutTimer();
}

void FtpServer::startActiveOp(const QueuedOp& op)
{
  rfb::FileTransferClient* ft = backend_->fileTransfer();
  ClientSession* s = findSession(op.sessionId);
  if (ft == nullptr || s == nullptr) {
    finishActiveOp();
    return;
  }

  activeOp_ = op;
  activeOp_.startedAt = time(nullptr);
  rfbDownloadFinished_ = false;
  rfbDownloadOk_ = false;
  rfbDownloadError_.clear();
  armTimeoutTimer();

  try {
    switch (op.kind) {
    case OpKind::List:
    case OpKind::Mlsd:
    case OpKind::Nlst:
    case OpKind::Cwd:
      ft->listDirectory(op.path);
      break;

    case OpKind::StatSize:
    case OpKind::StatMdtm:
    case OpKind::StatMlst:
    case OpKind::AppeStat: {
      std::string parent, base;
      ftp::splitFtpPath(op.path, &parent, &base);
      ft->listDirectory(parent);
      break;
    }

    case OpKind::Retr:
      s->dataOutBuf.clear();
      s->dataOutOff = 0;
      s->dataOutEof = false;
      s->downloadPullDeferred = false;
      ft->startDownload(op.path, op.offset);
      break;

    case OpKind::Stor:
      s->uploadAckPending = false;
      s->uploadCongestedPaused = false;
      s->uploadReadEof = false;
      ft->startUpload(op.path, op.overwrite, op.offset);
      break;

    case OpKind::Mkd:
      ft->createDirectory(op.path);
      break;

    case OpKind::Rmd:
      ft->removeFileOrDir(op.path, true);
      break;

    case OpKind::Dele:
      ft->removeFileOrDir(op.path, false);
      break;

    case OpKind::Rename:
      ft->renameFileOrDir(op.path, op.path2, false);
      break;

    case OpKind::Appe:
    case OpKind::Idle:
      finishActiveOp();
      return;
    }

    if (backend_ != nullptr)
      backend_->flushOutput();
  } catch (std::exception& e) {
    s = findSession(op.sessionId);
    if (s != nullptr) {
      closeDataChannel(s);
      s->dataState = ClientSession::DataTransferState::Idle;
      sendReply(s, core::format("451 %s", e.what()));
    }
    finishActiveOp();
  }
}

void FtpServer::finishActiveOp()
{
  activeOp_ = QueuedOp();
  rfbDownloadFinished_ = false;
  rfbDownloadOk_ = false;
  rfbDownloadError_.clear();
  pumpQueue();
}

void FtpServer::handleStatResult(ClientSession* s, const QueuedOp& op,
                                 const std::vector<rfb::FileEntry>& entries)
{
  std::string parent, base;
  ftp::splitFtpPath(op.path, &parent, &base);

  bool found = false;
  rfb::FileEntry match;
  for (size_t i = 0; i < entries.size(); i++) {
    if (entries[i].name == base) {
      found = true;
      match = entries[i];
      break;
    }
  }

  if (op.kind == OpKind::AppeStat) {
    if (found && match.isDir) {
      closeDataChannel(s);
      s->dataState = ClientSession::DataTransferState::Idle;
      sendReply(s, "550 Target is a directory.");
      return;
    }
    uint64_t off = found ? match.size : 0;
    rfb::FileTransferClient* ft =
      backend_ != nullptr ? backend_->fileTransfer() : nullptr;
    if (off > 0 && (ft == nullptr || !ft->caps().canResumeUpload)) {
      closeDataChannel(s);
      s->dataState = ClientSession::DataTransferState::Idle;
      sendReply(s, "550 Append not supported by server.");
      return;
    }
    QueuedOp uploadOp;
    uploadOp.sessionId = op.sessionId;
    uploadOp.kind = OpKind::Stor;
    uploadOp.path = op.path;
    uploadOp.overwrite = true;
    uploadOp.offset = off;
    enqueueOp(uploadOp);
    return;
  }

  if (!found) {
    sendReply(s, "550 File not found.");
    return;
  }

  if (op.kind == OpKind::StatMlst) {
    sendReply(s, core::format("250-Listing %s\r\n%s\r\n250 End",
                              op.path.c_str(),
                              ftp::formatMlstLine(op.path, match).c_str()));
    return;
  }

  if (match.isDir) {
    sendReply(s, "550 Not a regular file.");
    return;
  }

  if (op.kind == OpKind::StatSize) {
    sendReply(s, core::format("213 %llu",
                              static_cast<unsigned long long>(match.size)));
  } else if (op.kind == OpKind::StatMdtm) {
    if (match.mtime <= 0)
      sendReply(s, "550 Modification time unavailable.");
    else
      sendReply(s, core::format("213 %s",
                                ftp::formatMdtm(match.mtime).c_str()));
  }
}

bool FtpServer::getCachedDir(const std::string& dirPath,
                             std::vector<rfb::FileEntry>* entries) const
{
  std::map<std::string, DirCacheEntry>::const_iterator it =
    dirCache_.find(dirPath);
  if (it == dirCache_.end())
    return false;
  time_t now = time(nullptr);
  if (now - it->second.fetchedAt > kCacheTtlSeconds)
    return false;
  *entries = it->second.entries;
  return true;
}

void FtpServer::putCachedDir(const std::string& dirPath,
                             const std::vector<rfb::FileEntry>& entries)
{
  DirCacheEntry entry;
  entry.fetchedAt = time(nullptr);
  entry.entries = entries;
  dirCache_[dirPath] = entry;
}

void FtpServer::invalidateCacheFor(const std::string& path)
{
  dirCache_.erase(path);
  std::string parent, base;
  ftp::splitFtpPath(path, &parent, &base);
  dirCache_.erase(parent);
}

void FtpServer::onFileTransferReady(const rfb::FileTransferCaps& caps)
{
  vlog.info("RFB file transfer ready (protocol=%d)",
            static_cast<int>(caps.protocol));
}

void FtpServer::onListComplete(bool ok,
                               const std::vector<rfb::FileEntry>& entries,
                               const std::string& error)
{
  if (activeOp_.kind == OpKind::Idle)
    return;

  QueuedOp op = activeOp_;
  ClientSession* s = findSession(op.sessionId);

  if (op.kind == OpKind::Cwd) {
    if (s != nullptr) {
      if (ok) {
        putCachedDir(op.path, entries);
        s->cwd = op.path;
        sendReply(s, "250 Directory successfully changed.");
      } else {
        sendReply(s, core::format("550 %s",
                                  error.empty() ? "Failed to change directory." : error.c_str()));
      }
    }
    finishActiveOp();
    return;
  }

  if (op.kind == OpKind::StatSize || op.kind == OpKind::StatMdtm ||
      op.kind == OpKind::StatMlst || op.kind == OpKind::AppeStat) {
    std::string parent, base;
    ftp::splitFtpPath(op.path, &parent, &base);
    if (ok)
      putCachedDir(parent, entries);

    if (s != nullptr) {
      if (!ok) {
        if (op.kind == OpKind::AppeStat) {
          closeDataChannel(s);
          s->dataState = ClientSession::DataTransferState::Idle;
        }
        sendReply(s, core::format("550 %s",
                                  error.empty() ? "Directory unavailable." : error.c_str()));
      } else {
        handleStatResult(s, op, entries);
      }
    }
    finishActiveOp();
    return;
  }

  if (op.kind == OpKind::List || op.kind == OpKind::Mlsd ||
      op.kind == OpKind::Nlst) {
    if (ok)
      putCachedDir(op.path, entries);

    // Release activeOp_ right away so queued RFB operations don't stall on
    // this listing socket flush.
    activeOp_ = QueuedOp();

    if (s == nullptr) {
      pumpQueue();
      return;
    }

    if (!ok) {
      closeDataChannel(s);
      s->dataState = ClientSession::DataTransferState::Idle;
      sendReply(s, core::format("550 %s",
                                error.empty() ? "Failed to list directory." : error.c_str()));
      pumpQueue();
      return;
    }

    s->dataOutBuf.clear();
    s->dataOutOff = 0;
    s->dataOutEof = true;
    time_t now = time(nullptr);
    for (size_t i = 0; i < entries.size(); i++) {
      if (op.kind == OpKind::Mlsd)
        s->dataOutBuf += ftp::formatMlsdLine(entries[i]);
      else if (op.kind == OpKind::Nlst)
        s->dataOutBuf += entries[i].name;
      else
        s->dataOutBuf += ftp::formatListLine(entries[i], now);
      s->dataOutBuf += "\r\n";
    }

    beginPortOrWaitTransfer(s);
    pumpQueue();
    return;
  }

  finishActiveOp();
}

void FtpServer::onDownloadStarted(bool ok, uint64_t /*totalSize*/,
                                  bool /*sizeKnown*/,
                                  const std::string& error)
{
  if (activeOp_.kind != OpKind::Retr)
    return;

  ClientSession* s = findSession(activeOp_.sessionId);
  if (s == nullptr) {
    rfb::FileTransferClient* ft = backend_->fileTransfer();
    if (ok && ft != nullptr && ft->isBusy()) {
      ft->cancelDownload();
      if (backend_ != nullptr)
        backend_->flushOutput();
    } else {
      finishActiveOp();
    }
    return;
  }

  if (!ok) {
    closeDataChannel(s);
    s->dataState = ClientSession::DataTransferState::Idle;
    sendReply(s, core::format("550 %s",
                              error.empty() ? "Failed to open remote file." : error.c_str()));
    finishActiveOp();
    return;
  }

  activeOp_.startedAt = time(nullptr);
  rfb::FileTransferClient* ft = backend_->fileTransfer();
  if (ft != nullptr && ft->caps().protocol == rfb::FileTransferProtocol::Tight2) {
    ft->requestMoreDownloadData();
    backend_->flushOutput();
  }

  beginPortOrWaitTransfer(s);
}

void FtpServer::onDownloadData(const uint8_t* data, size_t len)
{
  if (activeOp_.kind != OpKind::Retr)
    return;

  ClientSession* s = findSession(activeOp_.sessionId);
  if (s == nullptr)
    return;

  activeOp_.startedAt = time(nullptr);
  if (data != nullptr && len > 0)
    s->dataOutBuf.append(reinterpret_cast<const char*>(data), len);

  if (s->dataState == ClientSession::DataTransferState::Transferring)
    updateDataWatch(s);

  rfb::FileTransferClient* ft = backend_->fileTransfer();
  if (ft != nullptr &&
      ft->caps().protocol == rfb::FileTransferProtocol::Tight2 &&
      !rfbDownloadFinished_) {
    size_t unread = s->dataOutBuf.size() - s->dataOutOff;
    if (unread < kLowWaterMark) {
      s->downloadPullDeferred = false;
      ft->requestMoreDownloadData();
      backend_->flushOutput();
    } else {
      s->downloadPullDeferred = true;
    }
  }
}

void FtpServer::onDownloadComplete(bool ok, const std::string& error)
{
  if (activeOp_.kind != OpKind::Retr)
    return;

  rfbDownloadFinished_ = true;
  rfbDownloadOk_ = ok;
  rfbDownloadError_ = error;

  ClientSession* s = findSession(activeOp_.sessionId);
  if (s == nullptr) {
    finishActiveOp();
    return;
  }

  if (!ok) {
    bool waitingConn =
      (s->dataState == ClientSession::DataTransferState::WaitingDataChannel ||
       s->dataState == ClientSession::DataTransferState::WaitingRfbStart);
    closeDataChannel(s);
    s->dataState = ClientSession::DataTransferState::Idle;
    sendReply(s, core::format("%s %s", waitingConn ? "550" : "451",
                              error.empty() ? "Download failed." : error.c_str()));
    finishActiveOp();
    return;
  }

  s->dataOutEof = true;
  if (s->dataState == ClientSession::DataTransferState::Transferring) {
    if (s->dataOutOff >= s->dataOutBuf.size()) {
      closeDataChannel(s);
      s->dataState = ClientSession::DataTransferState::Idle;
      sendReply(s, "226 Transfer complete.");
      finishActiveOp();
      return;
    }
    // RFB side is done; release queue and let dataFd finish flushing dataOutBuf
    activeOp_ = QueuedOp();
    pumpQueue();
    updateDataWatch(s);
  }
}

void FtpServer::onUploadReady(bool ok, const std::string& error)
{
  if (activeOp_.kind != OpKind::Stor)
    return;

  ClientSession* s = findSession(activeOp_.sessionId);
  if (s == nullptr) {
    rfb::FileTransferClient* ft = backend_->fileTransfer();
    if (ok && ft != nullptr && ft->isBusy()) {
      ft->cancelUpload();
      if (backend_ != nullptr)
        backend_->flushOutput();
    } else {
      finishActiveOp();
    }
    return;
  }

  if (!ok) {
    closeDataChannel(s);
    s->dataState = ClientSession::DataTransferState::Idle;
    sendReply(s, core::format("550 %s",
                              error.empty() ? "Upload rejected by server." : error.c_str()));
    finishActiveOp();
    return;
  }

  activeOp_.startedAt = time(nullptr);
  beginPortOrWaitTransfer(s);
}

void FtpServer::onUploadChunkAck()
{
  if (activeOp_.kind != OpKind::Stor)
    return;

  ClientSession* s = findSession(activeOp_.sessionId);
  if (s == nullptr)
    return;

  activeOp_.startedAt = time(nullptr);
  s->uploadAckPending = false;

  if (s->uploadReadEof) {
    rfb::FileTransferClient* ft = backend_->fileTransfer();
    if (ft != nullptr) {
      ft->finishUpload(static_cast<uint64_t>(time(nullptr)));
      if (backend_ != nullptr)
        backend_->flushOutput();
    }
    return;
  }

  if (!s->uploadCongestedPaused &&
      s->dataState == ClientSession::DataTransferState::Transferring) {
    updateDataWatch(s);
  }
}

void FtpServer::onUploadComplete(bool ok, const std::string& error)
{
  if (activeOp_.kind != OpKind::Stor)
    return;

  std::string path = activeOp_.path;
  ClientSession* s = findSession(activeOp_.sessionId);

  if (ok)
    invalidateCacheFor(path);

  if (s != nullptr) {
    closeDataChannel(s);
    s->dataState = ClientSession::DataTransferState::Idle;
    if (ok) {
      sendReply(s, "226 Transfer complete.");
    } else {
      sendReply(s, core::format("451 %s",
                                error.empty() ? "Upload failed." : error.c_str()));
    }
  }
  finishActiveOp();
}

void FtpServer::onCommandComplete(bool ok, const std::string& error)
{
  if (activeOp_.kind != OpKind::Mkd && activeOp_.kind != OpKind::Rmd &&
      activeOp_.kind != OpKind::Dele && activeOp_.kind != OpKind::Rename) {
    return;
  }

  QueuedOp op = activeOp_;
  ClientSession* s = findSession(op.sessionId);

  if (ok) {
    invalidateCacheFor(op.path);
    if (!op.path2.empty())
      invalidateCacheFor(op.path2);
  }

  if (s != nullptr) {
    if (ok) {
      if (op.kind == OpKind::Mkd) {
        std::string escaped = ftp::escapeFtpPath(op.path);
        sendReply(s, core::format("257 \"%s\" created", escaped.c_str()));
      } else {
        sendReply(s, "250 Command successful.");
      }
    } else {
      sendReply(s, core::format("550 %s",
                                error.empty() ? "Operation failed." : error.c_str()));
    }
  }

  finishActiveOp();
}

void FtpServer::onCancelComplete()
{
  ClientSession* s = findSession(activeOp_.sessionId);
  if (s != nullptr &&
      s->dataState == ClientSession::DataTransferState::Canceling) {
    closeDataChannel(s);
    s->dataState = ClientSession::DataTransferState::Idle;
    sendReply(s, "226 Abort successful.");
  }
  finishActiveOp();
}
