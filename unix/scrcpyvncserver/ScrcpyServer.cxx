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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "ScrcpyServer.h"
#include "AdbClient.h"
#include "ScrcpyClient.h"
#include "parameters.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <core/Exception.h>
#include <core/LogWriter.h>
#include <core/i18n.h>
#include <core/string.h>
#include <core/time.h>
#include <network/Socket.h>
#include <rdr/FdOutStream.h>
#include <rfb/ServerCore.h>

static core::LogWriter vlog("ScrcpyServer");
static core::LogWriter connectionsLog("Connections");

namespace {

static const size_t kMaxUnauthClients = 40;
static const char* kDeviceServerPath = "/data/local/tmp/scrcpy-server.jar";
static const char* kDefaultFallbackVersion = "3.3.4";

void sleepMs(int ms)
{
  ::poll(nullptr, 0, ms);
}

std::string quoteShellArgument(const std::string& value)
{
  std::string quoted = "'";
  for (char c : value) {
    if (c == '\'')
      quoted += "'\\''";
    else
      quoted += c;
  }
  quoted += '\'';
  return quoted;
}

} // namespace

ScrcpyServer::ScrcpyServer()
  : pointerClient_(nullptr),
    pointerClientTime_(0),
    clipboardClient_(nullptr),
    shellFd_(-1),
    clipboardSeq_(1),
    lastIdrHasInlineSpsPps_(false),
    lastIdrValid_(false),
    hasResetReq_(false),
    terminateRequested_(false),
    idleTimer_(this),
    disconnectTimer_(this),
    connectTimer_(this)
{
  std::memset(&lastResetReq_, 0, sizeof(lastResetReq_));

  if (rfb::Server::maxIdleTime)
    idleTimer_.start(core::secsToMillis(rfb::Server::maxIdleTime));
  if (rfb::Server::maxDisconnectionTime)
    disconnectTimer_.start(core::secsToMillis(rfb::Server::maxDisconnectionTime));
}

ScrcpyServer::~ScrcpyServer()
{
  closeClients(_("Server shutting down"));
  while (!clients_.empty()) {
    ScrcpyClient* client = clients_.front();
    clients_.pop_front();
    network::Socket* sock = client->getSock();
    delete client;
    delete sock;
  }
  shutdownScrcpy();
}

void ScrcpyServer::shutdownScrcpy()
{
  stream_.close();
  control_.close();
  if (shellFd_ >= 0) {
    ::close(shellFd_);
    shellFd_ = -1;
  }
}

std::string ScrcpyServer::resolveServerPath() const
{
  std::string configured = static_cast<const char*>(scrcpyServer);
  if (!configured.empty()) {
    if (::access(configured.c_str(), R_OK) != 0) {
      throw core::posix_error(
        core::format("Cannot read scrcpy-server at '%s'", configured.c_str()),
        errno);
    }
    return configured;
  }

  const char* envPath = ::getenv("SCRCPY_SERVER_PATH");
  if (envPath && envPath[0] != '\0' && ::access(envPath, R_OK) == 0)
    return envPath;

  static const char* kSearchPaths[] = {
    "/usr/share/scrcpy/scrcpy-server",
    "/usr/local/share/scrcpy/scrcpy-server",
    "/opt/scrcpy/scrcpy-server",
    "/usr/share/scrcpy/scrcpy-server.jar",
    "/usr/local/share/scrcpy/scrcpy-server.jar",
  };
  for (const char* p : kSearchPaths) {
    if (::access(p, R_OK) == 0)
      return p;
  }

  throw std::runtime_error(
    "Cannot find scrcpy-server jar; pass -ScrcpyServer <path> or set SCRCPY_SERVER_PATH");
}

std::string ScrcpyServer::buildServerCommand(const std::string& version) const
{
  std::string cmd = core::format(
    "CLASSPATH=%s app_process / com.genymobile.scrcpy.Server %s"
    " tunnel_forward=true audio=false video_codec=h264"
    " log_level=info control=%s video_bit_rate=%d",
    kDeviceServerPath,
    quoteShellArgument(version).c_str(),
    static_cast<bool>(controlEnabled) ? "true" : "false",
    static_cast<int>(bitRate));

  if (static_cast<int>(scid) >= 0)
    cmd += core::format(" scid=%08x", static_cast<int>(scid));

  if (static_cast<int>(maxSize) > 0)
    cmd += core::format(" max_size=%d", static_cast<int>(maxSize));

  if (static_cast<int>(maxFps) > 0)
    cmd += core::format(" max_fps=%d", static_cast<int>(maxFps));

  if (static_cast<int>(iFrameInterval) > 0)
    cmd += core::format(" video_codec_options=i-frame-interval=%d",
                        static_cast<int>(iFrameInterval));

  std::string enc = static_cast<const char*>(videoEncoder);
  if (!enc.empty())
    cmd += " " + quoteShellArgument("video_encoder=" + enc);

  if (static_cast<bool>(stayAwake))
    cmd += " stay_awake=true";

  if (!static_cast<bool>(sendDummyByte))
    cmd += " send_dummy_byte=false";

  if (!static_cast<bool>(sendDeviceMeta))
    cmd += " send_device_meta=false";

  return cmd;
}

bool ScrcpyServer::drainShellOutput()
{
  if (shellFd_ < 0)
    return false;

  char buf[1024];
  for (int i = 0; i < 16; ++i) {
    ssize_t n = ::recv(shellFd_, buf, sizeof(buf), MSG_DONTWAIT);
    if (n > 0) {
      shellOutput_.append(buf, static_cast<size_t>(n));
      size_t nl;
      while ((nl = shellOutput_.find('\n')) != std::string::npos) {
        std::string line = shellOutput_.substr(0, nl);
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        if (!line.empty()) {
          vlog.info("[scrcpy-server] %s", line.c_str());
          std::string ver = AdbClient::extractExpectedVersionFromError(line);
          if (!ver.empty())
            detectedShellVersion_ = ver;
        }
        shellOutput_.erase(0, nl + 1);
      }
      if (shellOutput_.size() > 16384)
        shellOutput_.erase(0, shellOutput_.size() - 16384);
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return true;
    if (!shellOutput_.empty()) {
      vlog.info("[scrcpy-server] %s", shellOutput_.c_str());
      std::string ver = AdbClient::extractExpectedVersionFromError(shellOutput_);
      if (!ver.empty())
        detectedShellVersion_ = ver;
      shellOutput_.clear();
    }
    return false;
  }
  return true;
}

void ScrcpyServer::startScrcpy()
{
  std::string socketName = static_cast<const char*>(scrcpySocket);
  if (!attachMode && socketName != "scrcpy")
    throw std::invalid_argument("ScrcpySocket requires -Attach; use -Scid when launching scrcpy-server");
  if (static_cast<int>(scid) >= 0)
    socketName = core::format("scrcpy_%08x", static_cast<int>(scid));

  AdbClient adb(static_cast<const char*>(adbHost), static_cast<int>(adbPort));
  std::string devSerial = adb.resolveSerial(static_cast<const char*>(serial));

  vlog.info("Using Android device '%s' via %s:%d (localabstract:%s)",
            devSerial.c_str(),
            static_cast<const char*>(adbHost),
            static_cast<int>(adbPort),
            socketName.c_str());

  if (static_cast<bool>(attachMode)) {
    control_.setServerVersion(static_cast<const char*>(scrcpyVersion));
    int videoFd = adb.connectLocalAbstract(devSerial, socketName);
    try {
      if (static_cast<bool>(sendDummyByte) && !stream_.readDummyByte(videoFd, 5000))
        throw std::runtime_error(
          "Attached scrcpy socket closed before sending dummy byte");

      if (static_cast<bool>(controlEnabled)) {
        try {
          int ctrlFd = adb.connectLocalAbstract(devSerial, socketName);
          control_.attach(ctrlFd);
        } catch (std::exception& e) {
          vlog.info("Control socket not available in attach mode (%s); view-only",
                    e.what());
        }
      }

      if (!stream_.readStreamHeader(videoFd,
                                    static_cast<bool>(sendDeviceMeta), 5000))
        throw std::runtime_error(
          "Attached scrcpy socket closed before sending stream header");
    } catch (...) {
      control_.close();
      ::close(videoFd);
      throw;
    }
  } else {
    std::string serverPath = resolveServerPath();
    vlog.info("Pushing %s to %s", serverPath.c_str(), kDeviceServerPath);
    adb.pushFile(devSerial, serverPath, kDeviceServerPath, 0100644);

    std::string version = static_cast<const char*>(scrcpyVersion);
    const bool autoVersion = version.empty();
    if (version.empty())
      version = AdbClient::detectVersionFromPath(serverPath);
    if (version.empty())
      version = kDefaultFallbackVersion;

    bool launched = false;
    for (int attempt = 0; attempt < 2 && !launched; ++attempt) {
      std::string cmd = buildServerCommand(version);
      vlog.debug("Launching scrcpy-server: %s", cmd.c_str());

      shellOutput_.clear();
      detectedShellVersion_.clear();
      shellFd_ = adb.startShell(devSerial, cmd);

      int videoFd = -1;
      std::string connectionError;
      auto deadline = std::chrono::steady_clock::now() +
                      std::chrono::seconds(5);

      do {
        if (!drainShellOutput())
          break;

        int candFd = -1;
        try {
          candFd = adb.connectLocalAbstract(devSerial, socketName);
          if (static_cast<bool>(sendDummyByte) &&
              !stream_.readDummyByte(candFd, 3000)) {
            ::close(candFd);
            candFd = -1;
          } else {
            if (static_cast<bool>(controlEnabled)) {
              int ctrlFd = adb.connectLocalAbstract(devSerial, socketName);
              control_.attach(ctrlFd);
            }
            if (stream_.readStreamHeader(candFd,
                                         static_cast<bool>(sendDeviceMeta),
                                         5000)) {
              videoFd = candFd;
              break;
            }
            control_.close();
            ::close(candFd);
            candFd = -1;
          }
        } catch (const std::exception& e) {
          control_.close();
          if (candFd >= 0)
            ::close(candFd);
          connectionError = e.what();
        }

        sleepMs(50);
      } while (std::chrono::steady_clock::now() < deadline);

      if (videoFd >= 0) {
        control_.setServerVersion(version);
        launched = true;
        break;
      }

      // Collect any remaining shell output to inspect version mismatch
      if (shellFd_ >= 0) {
        sleepMs(50);
        drainShellOutput();
        ::close(shellFd_);
        shellFd_ = -1;
      }

      if (attempt == 0 && autoVersion && !detectedShellVersion_.empty() &&
          detectedShellVersion_ != version) {
        vlog.info("Retrying scrcpy-server with detected version %s",
                  detectedShellVersion_.c_str());
        version = detectedShellVersion_;
        continue;
      }

      std::string reason = "Failed to connect to scrcpy-server video stream";
      if (!connectionError.empty())
        reason += ": " + connectionError;
      if (!detectedShellVersion_.empty())
        reason += " (server version " + detectedShellVersion_ + ")";
      throw std::runtime_error(reason);
    }
  }

  std::string cfgName = static_cast<const char*>(::desktopName);
  if (!cfgName.empty())
    desktopName_ = cfgName;
  else if (!stream_.deviceName().empty())
    desktopName_ = stream_.deviceName();
  else
    desktopName_ = devSerial;
}

void ScrcpyServer::attachStreamsForTest(int videoFd, int controlFd,
                                        ScrcpyProtocol protocol,
                                        int width, int height,
                                        const std::string& deviceName)
{
  stream_.attachForTest(videoFd, protocol, width, height);
  if (controlFd >= 0)
    control_.attach(controlFd);
  std::string cfgName = static_cast<const char*>(::desktopName);
  desktopName_ = cfgName.empty() ? deviceName : cfgName;
}

bool ScrcpyServer::addSocket(network::Socket* sock,
                             rfb::AccessRights accessRights)
{
  // The standard RFB socket streams use select() internally.
  if (sock->getFd() >= FD_SETSIZE) {
    connectionsLog.error(_("Client socket exceeds the descriptor limit"));
    return false;
  }

  const char* address = sock->getPeerAddress();
  if (blacklist_.isBlackmarked(address)) {
    connectionsLog.error(_("Blacklisted: %s"), address);
    try {
      rdr::OutStream& os = sock->outStream();
      os.writeBytes(reinterpret_cast<const uint8_t*>("RFB 003.003\n"), 12);
      os.writeU32(0);
      const char* reason = _("Too many security failures");
      os.writeU32(std::strlen(reason));
      os.writeBytes(reinterpret_cast<const uint8_t*>(reason),
                    std::strlen(reason));
      os.flush();
    } catch (std::exception&) {
    }
    return false;
  }

  if (clients_.size() >= kMaxUnauthClients) {
    size_t unauth = 0;
    for (ScrcpyClient* c : clients_) {
      if (!c->authenticated())
        unauth++;
    }
    if (unauth >= kMaxUnauthClients) {
      connectionsLog.error(_("Too many unauthenticated connections: %s"),
                           sock->getPeerEndpoint());
      return false;
    }
  }

  connectionsLog.info(_("Accepted: %s"), sock->getPeerEndpoint());

  try {
    std::unique_ptr<ScrcpyClient> client(new ScrcpyClient(this, sock, accessRights));
    client->init();
    clients_.push_front(client.get());
    client.release();
  } catch (std::exception& e) {
    connectionsLog.error(_("Error accepting client: %s"), e.what());
    return false;
  }

  return true;
}

void ScrcpyServer::removeSocket(network::Socket* sock)
{
  for (auto it = clients_.begin(); it != clients_.end(); ++it) {
    ScrcpyClient* c = *it;
    if (c->getSock() == sock) {
      if (pointerClient_ == c) {
        control_.releasePointer(width(), height());
        pointerClient_ = nullptr;
      }
      if (clipboardClient_ == c)
        clientClipboardAnnounce(c, false);

      std::string peer(c->getPeerEndpoint());
      delete c;
      clients_.erase(it);

      connectionsLog.info(_("Closed: %s"), peer.c_str());

      if (authClientCount() == 0) {
        connectTimer_.stop();
        if (rfb::Server::maxDisconnectionTime && !disconnectTimer_.isStarted())
          disconnectTimer_.start(
            core::secsToMillis(rfb::Server::maxDisconnectionTime));
      }
      return;
    }
  }

  throw std::invalid_argument("Invalid Socket in ScrcpyServer");
}

void ScrcpyServer::closeClients(const char* reason, ScrcpyClient* except)
{
  for (ScrcpyClient* c : clients_) {
    if (c != except)
      c->close(reason);
  }
}

int ScrcpyServer::authClientCount() const
{
  int count = 0;
  for (ScrcpyClient* c : clients_) {
    if (c->authenticated())
      count++;
  }
  return count;
}

void ScrcpyServer::clearBlacklist(const char* address)
{
  blacklist_.clearBlackmark(address);
}

void ScrcpyServer::clientReady(ScrcpyClient* client, bool shared)
{
  if (rfb::Server::maxConnectionTime && !connectTimer_.isStarted())
    connectTimer_.start(core::secsToMillis(rfb::Server::maxConnectionTime));
  disconnectTimer_.stop();

  if (!shared) {
    if (rfb::Server::disconnectClients &&
        client->accessCheck(rfb::AccessNonShared)) {
      closeClients(_("Non-shared connection requested"), client);
    } else if (authClientCount() > 1) {
      client->close(_("Server is already in use"));
      return;
    }
  }

  if (rfb::Server::neverShared &&
      !rfb::Server::disconnectClients &&
      authClientCount() > 1) {
    client->close(_("The server is already in use"));
  }
}

void ScrcpyServer::requestVideoReset()
{
  if (control_.fd() < 0)
    return;

  struct timeval now;
  gettimeofday(&now, nullptr);
  if (hasResetReq_ && core::msSince(&lastResetReq_) < 200)
    return;

  lastResetReq_ = now;
  hasResetReq_ = true;
  control_.sendResetVideo();
}

void ScrcpyServer::clientRequestsKeyFrame(ScrcpyClient* client)
{
  if (lastIdrValid_ && !lastIdrFrame_.empty() && client->readyForVideo()) {
    if (client->sendH264Frame(stream_.width(), stream_.height(),
                              stream_.spsPps(),
                              lastIdrFrame_.data(), lastIdrFrame_.size(),
                              true, lastIdrHasInlineSpsPps_)) {
      return;
    }
  }
  requestVideoReset();
}

void ScrcpyServer::clientPointerEvent(ScrcpyClient* client,
                                      int x, int y, uint16_t buttonMask)
{
  if (!rfb::Server::acceptPointerEvents)
    return;

  time_t now = ::time(nullptr);
  if (rfb::Server::maxIdleTime)
    idleTimer_.start(core::secsToMillis(rfb::Server::maxIdleTime));

  if (pointerClient_ != nullptr && pointerClient_ != client &&
      (now - pointerClientTime_) < 10)
    return;

  if (pointerClient_ != nullptr && pointerClient_ != client)
    control_.releasePointer(width(), height());
  pointerClientTime_ = now;
  pointerClient_ = (buttonMask & 0x0187u) ? client : nullptr;

  control_.handleVncPointer(x, y, buttonMask, width(), height());
}

void ScrcpyServer::clientKeyEvent(uint32_t keysym, uint32_t keycode, bool down)
{
  if (!rfb::Server::acceptKeyEvents)
    return;

  if (rfb::Server::maxIdleTime)
    idleTimer_.start(core::secsToMillis(rfb::Server::maxIdleTime));

  control_.handleVncKey(keysym, keycode, down);
}

void ScrcpyServer::clientClipboardRequest(ScrcpyClient* client)
{
  if (!rfb::Server::sendCutText)
    return;
  if (!deviceClipboard_.empty())
    client->sendClipboardData(deviceClipboard_.c_str());
}

void ScrcpyServer::clientClipboardAnnounce(ScrcpyClient* client, bool available)
{
  if (available) {
    if (!rfb::Server::acceptCutText)
      return;
    clipboardClient_ = client;
    client->requestClipboard();
  } else if (client == clipboardClient_) {
    clipboardClient_ = nullptr;
  }
}

void ScrcpyServer::clientClipboardData(ScrcpyClient* client, const char* data)
{
  if (!rfb::Server::acceptCutText || client != clipboardClient_ || !data)
    return;
  if (rfb::Server::maxIdleTime)
    idleTimer_.start(core::secsToMillis(rfb::Server::maxIdleTime));
  control_.sendSetClipboard(clipboardSeq_++, data, false);
}

void ScrcpyServer::onResize(int newWidth, int newHeight)
{
  vlog.info("Android display resized to %dx%d", newWidth, newHeight);
  lastIdrValid_ = false;
  lastIdrFrame_.clear();

  for (ScrcpyClient* c : clients_)
    c->onDesktopResize(newWidth, newHeight);
}

void ScrcpyServer::onConfig(const std::vector<uint8_t>& /*spsPps*/)
{
  lastIdrValid_ = false;
  lastIdrFrame_.clear();
  vlog.debug("Cached H.264 SPS/PPS config (%u bytes)",
             static_cast<unsigned>(stream_.spsPps().size()));
  for (ScrcpyClient* c : clients_)
    c->onConfigUpdated();
}

void ScrcpyServer::onFrame(const uint8_t* data, size_t len,
                           bool isKeyFrame, bool hasInlineSpsPps)
{
  if (isKeyFrame) {
    lastIdrFrame_.assign(data, data + len);
    lastIdrHasInlineSpsPps_ = hasInlineSpsPps;
    lastIdrValid_ = true;
  } else {
    lastIdrValid_ = false;
  }

  for (ScrcpyClient* c : clients_) {
    c->sendH264Frame(stream_.width(), stream_.height(),
                     stream_.spsPps(), data, len,
                     isKeyFrame, hasInlineSpsPps);
  }
}

void ScrcpyServer::onDeviceClipboard(const std::string& text)
{
  deviceClipboard_ = core::convertLF(text.c_str(), text.size());
  if (!rfb::Server::sendCutText)
    return;
  for (ScrcpyClient* c : clients_)
    c->announceClipboardOrClose(!deviceClipboard_.empty());
}

void ScrcpyServer::handleTimeout(core::Timer* t)
{
  if (t == &idleTimer_) {
    vlog.info(_("Maximum idle time reached, exiting"));
    terminate();
  } else if (t == &disconnectTimer_) {
    vlog.info(_("Maximum disconnected time reached, exiting"));
    terminate();
  } else if (t == &connectTimer_) {
    vlog.info(_("Maximum connected time reached, exiting"));
    terminate();
  }
}

void ScrcpyServer::terminate()
{
  terminateRequested_ = true;
}

bool ScrcpyServer::step(const std::list<network::SocketListener*>& listeners,
                        int timeoutMs)
{
  core::Timer::checkTimeouts();
  if (terminateRequested_)
    return false;

  // Reap closed client sockets
  std::vector<network::Socket*> toRemove;
  for (ScrcpyClient* c : clients_) {
    if (c->getSock()->isShutdownRead())
      toRemove.push_back(c->getSock());
  }
  for (network::Socket* sock : toRemove) {
    removeSocket(sock);
    delete sock;
  }

  std::vector<struct pollfd> pfds;
  std::vector<network::SocketListener*> listenerOrder;
  std::vector<ScrcpyClient*> clientOrder;

  for (network::SocketListener* l : listeners) {
    struct pollfd pfd;
    pfd.fd = l->getFd();
    pfd.events = POLLIN;
    pfd.revents = 0;
    pfds.push_back(pfd);
    listenerOrder.push_back(l);
  }

  for (ScrcpyClient* c : clients_) {
    struct pollfd pfd;
    pfd.fd = c->getSock()->getFd();
    pfd.events = POLLIN | (c->hasBufferedWriteData() ? POLLOUT : 0);
    pfd.revents = 0;
    pfds.push_back(pfd);
    clientOrder.push_back(c);
  }

  size_t videoIdx = static_cast<size_t>(-1);
  if (stream_.fd() >= 0) {
    videoIdx = pfds.size();
    struct pollfd pfd;
    pfd.fd = stream_.fd();
    pfd.events = POLLIN;
    pfd.revents = 0;
    pfds.push_back(pfd);
  }

  size_t ctrlIdx = static_cast<size_t>(-1);
  if (control_.fd() >= 0) {
    ctrlIdx = pfds.size();
    struct pollfd pfd;
    pfd.fd = control_.fd();
    pfd.events = POLLIN | (control_.hasPendingWrites() ? POLLOUT : 0);
    pfd.revents = 0;
    pfds.push_back(pfd);
  }

  size_t shellIdx = static_cast<size_t>(-1);
  if (shellFd_ >= 0) {
    shellIdx = pfds.size();
    struct pollfd pfd;
    pfd.fd = shellFd_;
    pfd.events = POLLIN;
    pfd.revents = 0;
    pfds.push_back(pfd);
  }

  int nextTimeout = core::Timer::getNextTimeout();
  int effectiveTimeout = timeoutMs;
  if (nextTimeout >= 0 && (effectiveTimeout < 0 || nextTimeout < effectiveTimeout))
    effectiveTimeout = nextTimeout;

  int n = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()),
                 effectiveTimeout);
  if (n < 0) {
    if (errno == EINTR)
      return true;
    throw core::socket_error("poll", errno);
  }

  core::Timer::checkTimeouts();
  if (terminateRequested_)
    return false;

  // Accept incoming VNC connections
  for (size_t i = 0; i < listenerOrder.size(); ++i) {
    if (pfds[i].revents & POLLIN) {
      network::Socket* sock;
      try {
        sock = listenerOrder[i]->accept();
      } catch (const core::socket_error& e) {
        if (e.err == EINTR || e.err == EAGAIN || e.err == EWOULDBLOCK ||
            e.err == ECONNABORTED)
          continue;
        throw;
      }
      if (sock) {
        if (!addSocket(sock))
          delete sock;
      } else {
        vlog.info(_("Client connection rejected"));
      }
    }
  }

  // Handle VNC client reads and writes
  size_t clientBase = listenerOrder.size();
  for (size_t i = 0; i < clientOrder.size(); ++i) {
    ScrcpyClient* c = clientOrder[i];
    short revents = pfds[clientBase + i].revents;
    if (revents & (POLLIN | POLLERR | POLLHUP))
      c->processSocketReadEvent();
    if (revents & POLLOUT)
      c->processSocketWriteEvent();
  }

  // Handle scrcpy control channel
  if (ctrlIdx != static_cast<size_t>(-1)) {
    short revents = pfds[ctrlIdx].revents;
    if (revents & POLLOUT)
      control_.processWritable();
    if ((revents & (POLLIN | POLLERR | POLLHUP)) && control_.fd() >= 0) {
      if (!control_.processReadable(*this))
        vlog.info("scrcpy control channel closed");
    }
  }

  // Handle scrcpy-server shell stdout/stderr
  if (shellIdx != static_cast<size_t>(-1) &&
      (pfds[shellIdx].revents & (POLLIN | POLLERR | POLLHUP))) {
    if (!drainShellOutput()) {
      vlog.info("scrcpy-server shell exited");
      ::close(shellFd_);
      shellFd_ = -1;
    }
  }

  // Handle incoming scrcpy H.264 video frames
  if (videoIdx != static_cast<size_t>(-1) &&
      (pfds[videoIdx].revents & (POLLIN | POLLERR | POLLHUP))) {
    if (!stream_.processReadable(*this)) {
      vlog.info("scrcpy video stream closed");
      return false;
    }
  }

  return !terminateRequested_;
}

void ScrcpyServer::run(const std::list<network::SocketListener*>& listeners,
                       const volatile sig_atomic_t* caughtSignal)
{
  while (!terminateRequested_ && !(caughtSignal && *caughtSignal)) {
    // Bound the wait if a signal arrives between the flag check and poll().
    if (!step(listeners, caughtSignal ? 1000 : -1))
      break;
  }
}
