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

#include <poll.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <list>
#include <memory>
#include <string>
#include <vector>

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-daemon.h>
#endif

#include <core/Configuration.h>
#include <core/LogWriter.h>
#include <core/Logger_stdio.h>
#include <core/Timer.h>
#include <network/TcpSocket.h>
#include <network/UnixSocket.h>
#include <rdr/FdInStream.h>
#include <rdr/FdOutStream.h>
#include <rfb/VNCServerST.h>

#include "RDPDesktop.h"

namespace {

core::LogWriter vlog("main");

volatile sig_atomic_t caughtSignal = 0;

void handleSignal(int sig)
{
  caughtSignal = sig;
}

core::StringParameter desktopName("desktop",
                                  "Name of desktop reported to VNC clients",
                                  "rdpvnc");
core::IntParameter rfbport("rfbport",
                           "TCP port to listen for RFB protocol", 5900);
core::StringParameter rfbunixpath("rfbunixpath",
                                  "Unix socket path to listen for RFB protocol",
                                  "");
core::IntParameter rfbunixmode("rfbunixmode",
                               "Unix socket permission mode", 0600);
core::StringParameter interfaceOpt("interface",
                                   "Interface address to listen on", "all");
core::BoolParameter localhostOnly("localhost",
                                  "Only listen on loopback interface", false);
core::StringParameter hostsFile("HostsFile",
                                "File with IP access control rules", "");

core::StringParameter rdpHost("RdpHost", "Upstream RDP hostname or IP",
                              "127.0.0.1");
core::IntParameter rdpPort("RdpPort", "Upstream RDP port", 3389);
core::StringParameter rdpUser("RdpUser", "Upstream RDP username", "");
core::StringParameter rdpPassword("RdpPassword", "Upstream RDP password", "");
core::StringParameter rdpPasswordFile(
    "RdpPasswordFile", "Path to file containing upstream RDP password", "");
core::StringParameter rdpDomain("RdpDomain", "Upstream RDP domain", "");
core::BoolParameter rdpNla("RdpNla", "Enable NLA (CredSSP)", true);
core::BoolParameter rdpTls("RdpTls", "Enable TLS security", true);
core::BoolParameter rdpSecurity("RdpSecurity", "Allow legacy RDP security",
                                true);
core::BoolParameter rdpIgnoreCert("RdpIgnoreCert",
                                  "Ignore TLS certificate verification failures",
                                  false);
core::IntParameter rdpWidth("RdpWidth", "Initial upstream desktop width",
                            1280);
core::IntParameter rdpHeight("RdpHeight", "Initial upstream desktop height",
                             720);
core::BoolParameter rdpGfx("RdpGfx", "Enable MS-RDPEGFX graphics pipeline",
                           true);
core::BoolParameter rdpH264Passthrough(
    "RdpH264Passthrough",
    "Enable zero-transcode AVC420 H.264 passthrough over encodingH264", true);
core::BoolParameter rdpAllowRawPassthrough(
    "RdpAllowRawPassthrough",
    "Enable direct BGRX32 Raw passthrough on bit-for-bit pixel format match",
    false);
core::BoolParameter rdpJpegPassthrough(
    "RdpJpegPassthrough",
    "Enable zero-decode JFIF passthrough over Tight/JPEG", true);
core::BoolParameter rdpClipboard("RdpClipboard",
                                 "Enable bidirectional CLIPRDR clipboard", true);
core::BoolParameter rdpDynamicResize(
    "RdpDynamicResize", "Enable MS-RDPEDISP dynamic resize", true);
core::IntParameter rdpResizeDebounceMs(
    "RdpResizeDebounceMs",
    "MS-RDPEDISP resize debounce delay in milliseconds (clamped 300-500)", 400);
core::IntParameter rdpIdleDisconnectSec(
    "RdpIdleDisconnectSec",
    "Seconds before closing upstream RDP after last VNC client disconnects",
    0);
core::IntParameter rdpReconnectAttempts(
    "RdpReconnectAttempts",
    "Maximum bounded upstream reconnect attempts before closing VNC clients",
    0);
core::IntParameter rdpConnectTimeoutMs(
    "RdpConnectTimeoutMs", "Upstream RDP TCP connect timeout in milliseconds",
    15000);

std::string readPasswordFromFile(const char* path)
{
  std::ifstream in(path);
  if (!in.is_open()) {
    throw std::runtime_error(std::string("Unable to open RdpPasswordFile: ") +
                             path);
  }
  std::string line;
  std::getline(in, line);
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
    line.pop_back();
  return line;
}

void printVersion(FILE* fp)
{
  std::fprintf(fp, "\nTigerVNC rdpvncserver %s\n", PACKAGE_VERSION);
}

bool hasSystemdListeners()
{
#ifdef HAVE_LIBSYSTEMD
  return sd_listen_fds(0) != 0;
#else
  return false;
#endif
}

class FileTcpFilter : public network::TcpFilter {
public:
  explicit FileTcpFilter(std::string fname)
    : network::TcpFilter("-"), fileName_(std::move(fname)), lastModTime_(0)
  {
  }

  bool verifyConnection(network::Socket* s) override
  {
    if (!reloadRules()) {
      vlog.error("Could not read IP filtering rules, rejecting client");
      filter.clear();
      filter.push_back(parsePattern("-"));
      return false;
    }
    return network::TcpFilter::verifyConnection(s);
  }

private:
  bool reloadRules()
  {
    if (fileName_.empty())
      return true;
    struct stat st{};
    if (::stat(fileName_.c_str(), &st) != 0)
      return false;
    if (st.st_mtime != lastModTime_) {
      std::ifstream in(fileName_);
      if (!in.is_open())
        return false;
      filter.clear();
      std::string line;
      while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        if (!line.empty() && std::strchr("+-?", line[0]) != nullptr)
          filter.push_back(parsePattern(line.c_str()));
      }
      lastModTime_ = st.st_mtime;
    }
    return true;
  }

  std::string fileName_;
  time_t lastModTime_;
};

void printUsage(const char* prog)
{
  std::fprintf(stderr,
               "\nusage: %s [parameters]\n"
               "\n"
               "Bridge an upstream FreeRDP 3.x session to downstream VNC/RFB "
               "clients.\n"
               "\nParameters can be turned on with -<param> or off with "
               "-<param>=0\n"
               "Parameters which take a value can be specified as "
               "-<param> <value>\n"
               "Other valid forms are <param>=<value> -<param>=<value> "
               "--<param>=<value>\n"
               "Parameter names are case-insensitive. The parameters are:\n\n",
               prog);
  core::Configuration::listParams(79, 14);
}

} // namespace

int main(int argc, char** argv)
{
  core::initStdIOLoggers();
  core::LogWriter::setLogParams("*:stderr:30");

  if (hasSystemdListeners())
    rfbport.setParam(-1);

  for (int i = 1; i < argc;) {
    if (std::strcmp(argv[i], "-h") == 0 ||
        std::strcmp(argv[i], "--help") == 0 ||
        std::strcmp(argv[i], "-help") == 0 ||
        std::strcmp(argv[i], "-?") == 0) {
      printUsage(argv[0]);
      return 0;
    }

    if (std::strcmp(argv[i], "-v") == 0 ||
        std::strcmp(argv[i], "--version") == 0 ||
        std::strcmp(argv[i], "-version") == 0) {
      printVersion(stderr);
      return 0;
    }

    int handled = core::Configuration::handleParamArg(argc, argv, i);
    if (handled > 0) {
      i += handled;
      continue;
    }

    std::fprintf(stderr, "Unrecognized parameter: %s\n", argv[i]);
    std::fprintf(stderr, "Run '%s --help' for supported parameters.\n",
                 argv[0]);
    return 1;
  }

  std::signal(SIGHUP, handleSignal);
  std::signal(SIGINT, handleSignal);
  std::signal(SIGTERM, handleSignal);
  std::signal(SIGPIPE, SIG_IGN);

  try {
    rdpvnc::RdpConfig rdpCfg;
    rdpCfg.host = rdpHost.getValueStr();
    rdpCfg.port = rdpPort;
    rdpCfg.username = rdpUser.getValueStr();
    rdpCfg.password = rdpPassword.getValueStr();
    if (rdpCfg.password.empty()) {
      std::string passFile = rdpPasswordFile.getValueStr();
      if (!passFile.empty()) {
        rdpCfg.password = readPasswordFromFile(passFile.c_str());
      } else if (const char* envPass = std::getenv("RDPVNCSERVER_PASSWORD")) {
        rdpCfg.password = envPass;
      }
    }
    rdpCfg.domain = rdpDomain.getValueStr();
    rdpCfg.nla = rdpNla;
    rdpCfg.tls = rdpTls;
    rdpCfg.rdpSecurity = rdpSecurity;
    rdpCfg.ignoreCert = rdpIgnoreCert;
    rdpCfg.initialWidth = std::max(200, static_cast<int>(rdpWidth));
    rdpCfg.initialHeight = std::max(200, static_cast<int>(rdpHeight));
    rdpCfg.enableGfx = rdpGfx;
    rdpCfg.enableH264Passthrough = rdpH264Passthrough;
    rdpCfg.allowRawPassthrough = rdpAllowRawPassthrough;
    rdpCfg.enableJpegPassthrough = rdpJpegPassthrough;
    rdpCfg.enableClipboard = rdpClipboard;
    rdpCfg.enableDynamicResize = rdpDynamicResize;
    rdpCfg.connectTimeoutMs = std::max(1000, static_cast<int>(rdpConnectTimeoutMs));

    rdpvnc::DesktopPolicyConfig policy;
    policy.resizeDebounceMs = rdpResizeDebounceMs;
    policy.idleDisconnectSec = std::max(0, static_cast<int>(rdpIdleDisconnectSec));
    policy.maxReconnectAttempts = std::max(0, static_cast<int>(rdpReconnectAttempts));

    rdpvnc::RDPDesktop desktop(rdpCfg, policy);
    rfb::VNCServerST server(desktopName.getValueStr().c_str(), &desktop);

    std::vector<std::unique_ptr<network::SocketListener>> listeners;

#ifdef HAVE_LIBSYSTEMD
    int sdFds = sd_listen_fds(0);
    for (int i = 0; i < sdFds; ++i) {
      listeners.emplace_back(new network::TcpListener(SD_LISTEN_FDS_START + i));
    }
#endif

    std::unique_ptr<FileTcpFilter> tcpFilter;
    std::string hostsPath = hostsFile.getValueStr();
    if (!hostsPath.empty()) {
      tcpFilter.reset(new FileTcpFilter(hostsPath));
    }

    if (listeners.empty()) {
      std::string unixPath = rfbunixpath.getValueStr();
      if (!unixPath.empty()) {
        auto* ul = new network::UnixListener(unixPath.c_str(), rfbunixmode);
        if (tcpFilter)
          ul->setFilter(tcpFilter.get());
        listeners.emplace_back(ul);
        vlog.info("Listening on UNIX socket %s", unixPath.c_str());
      } else if (static_cast<int>(rfbport) > 0) {
        std::list<network::SocketListener*> tcpList;
        if (localhostOnly) {
          network::createLocalTcpListeners(&tcpList, static_cast<int>(rfbport));
        } else {
          std::string ifaceStr = interfaceOpt.getValueStr();
          const char* listenAddr =
              (ifaceStr != "all" && !ifaceStr.empty()) ? ifaceStr.c_str()
                                                       : nullptr;
          network::createTcpListeners(&tcpList, listenAddr,
                                      static_cast<int>(rfbport));
        }
        for (network::SocketListener* l : tcpList) {
          if (tcpFilter)
            l->setFilter(tcpFilter.get());
          listeners.emplace_back(l);
        }
        vlog.info("Listening on RFB TCP port %d", static_cast<int>(rfbport));
      }
    }

    if (listeners.empty()) {
      vlog.error("No RFB listening socket configured");
      return 1;
    }

    while (!caughtSignal) {
      std::vector<struct pollfd> fds;
      std::list<network::Socket*> rawSockets;
      server.getSockets(&rawSockets);

      std::vector<network::Socket*> activeSockets;
      activeSockets.reserve(rawSockets.size());
      for (network::Socket* sock : rawSockets) {
        if (sock->isShutdownRead()) {
          server.removeSocket(sock);
          delete sock;
        } else {
          activeSockets.push_back(sock);
        }
      }

      fds.reserve(listeners.size() + activeSockets.size() + 1);

      struct pollfd evFd{};
      evFd.fd = desktop.eventFd();
      evFd.events = POLLIN;
      fds.push_back(evFd);

      size_t listenerStart = fds.size();
      for (const auto& l : listeners) {
        struct pollfd pfd{};
        pfd.fd = l->getFd();
        pfd.events = POLLIN;
        fds.push_back(pfd);
      }

      size_t socketStart = fds.size();
      for (network::Socket* sock : activeSockets) {
        struct pollfd pfd{};
        pfd.fd = sock->getFd();
        pfd.events = POLLIN;
        if (sock->outStream().hasBufferedData())
          pfd.events |= POLLOUT;
        fds.push_back(pfd);
      }

      int nextTimeout = core::Timer::checkTimeouts();
      int n = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), nextTimeout);
      if (n < 0) {
        if (errno == EINTR)
          continue;
        throw std::runtime_error(std::string("poll() failed: ") +
                                 std::strerror(errno));
      }

      if (fds[0].revents & POLLIN) {
        desktop.processEvents();
      }

      for (size_t i = 0; i < listeners.size(); ++i) {
        if (fds[listenerStart + i].revents & POLLIN) {
          network::Socket* client = listeners[i]->accept();
          if (client) {
            if (!server.addSocket(client))
              delete client;
          }
        }
      }

      for (size_t i = 0; i < activeSockets.size(); ++i) {
        short revents = fds[socketStart + i].revents;
        network::Socket* sock = activeSockets[i];
        if (revents & POLLIN)
          server.processSocketReadEvent(sock);
        if (revents & POLLOUT)
          server.processSocketWriteEvent(sock);
        if (sock->isShutdownRead()) {
          server.removeSocket(sock);
          delete sock;
        }
      }

      core::Timer::checkTimeouts();
    }

    desktop.terminate();
  } catch (std::exception& e) {
    vlog.error("%s", e.what());
    return 1;
  }

  return 0;
}
