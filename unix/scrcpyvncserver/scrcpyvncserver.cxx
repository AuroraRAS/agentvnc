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

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <fstream>
#include <list>
#include <stdexcept>

#include <core/Configuration.h>
#include <core/Exception.h>
#include <core/LogWriter.h>
#include <core/Logger_stdio.h>
#include <core/i18n.h>
#include <core/string.h>
#include <network/TcpSocket.h>
#include <network/UnixSocket.h>

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-daemon.h>
#endif

#include "ScrcpyServer.h"
#include "parameters.h"

static core::LogWriter vlog("Main");

static volatile sig_atomic_t caughtSignal = 0;
char* programName = nullptr;

static void CleanupSignalHandler(int /*sig*/)
{
  caughtSignal = 1;
}

static bool hasSystemdListeners()
{
#ifdef HAVE_LIBSYSTEMD
  return sd_listen_fds(0) > 0;
#else
  return false;
#endif
}

static int createSystemdListeners(std::list<network::SocketListener*>* listeners,
                                  network::ConnectionFilter* filter)
{
#ifdef HAVE_LIBSYSTEMD
  int count = sd_listen_fds(1);
  if (count < 0)
    throw std::runtime_error(
      core::format(_("Error getting listening sockets from systemd: %s"),
                   strerror(-count)));

  for (int i = 0; i < count; ++i) {
    int fd = SD_LISTEN_FDS_START + i;
    if (sd_is_socket(fd, AF_UNSPEC, SOCK_STREAM, 1) <= 0)
      throw std::runtime_error(_("Expected a listening stream socket from systemd"));

    auto* listener = new network::TcpListener(fd);
    listeners->push_back(listener);
    if (filter && sd_is_socket_inet(fd, AF_UNSPEC, SOCK_STREAM, 1, 0) > 0)
      listener->setFilter(filter);
  }

  return count;
#else
  (void)listeners;
  (void)filter;
  return 0;
#endif
}

class FileTcpFilter : public network::TcpFilter {
public:
  explicit FileTcpFilter(const char* fname)
    : TcpFilter("-"), fileName_(fname)
  {
  }

  bool verifyConnection(network::Socket* s) override
  {
    if (!reloadRules()) {
      vlog.error(_("Could not read IP filtering rules, "
                   "rejecting all clients"));
      return false;
    }
    return TcpFilter::verifyConnection(s);
  }

private:
  bool reloadRules()
  {
    std::ifstream rules(fileName_);
    if (!rules)
      return false;

    std::list<Pattern> patterns;
    try {
      std::string line;
      while (std::getline(rules, line)) {
        size_t last = line.find_last_not_of(" \t\r");
        if (last == std::string::npos)
          continue;
        line.erase(last + 1);
        if (line.find('\0') != std::string::npos)
          return false;
        if (strchr("+-?", line[0]) != nullptr)
          patterns.push_back(parsePattern(line.c_str()));
      }
    } catch (const std::exception& e) {
      vlog.error("%s: %s", fileName_.c_str(), e.what());
      return false;
    }
    if (rules.bad() || !rules.eof())
      return false;

    filter.swap(patterns);
    return true;
  }

  std::string fileName_;
};

static void printVersion(FILE* fp)
{
  fprintf(fp, _("TigerVNC server version %s, built %s\n"),
          PACKAGE_VERSION, BUILD_TIMESTAMP);
}

static void usage()
{
  printVersion(stderr);
  fprintf(stderr,
          _("\n"
            "Usage: %s [<parameters>]\n"
            "       %s --version\n"),
          programName, programName);
  fprintf(stderr,
          _("\n"
            "Parameters can be turned on with -<param> or off with -<param>=0\n"
            "Parameters which take a value can be specified as "
            "-<param> <value>\n"
            "Other valid forms are <param>=<value> -<param>=<value> "
            "--<param>=<value>\n"
            "Parameter names are case-insensitive.  The parameters are:\n\n"));
  core::Configuration::listParams(79, 14);
  exit(1);
}

int main(int argc, char** argv)
{
  setlocale(LC_ALL, "");

  core::initStdIOLoggers();
  core::LogWriter::setLogParams("*:stderr:30");

  programName = argv[0];

  // Remove parameters not applicable to zero-transcoding passthrough
  core::Configuration::removeParam("CompareFB");
  core::Configuration::removeParam("FrameRate");
  core::Configuration::removeParam("AcceptSetDesktopSize");
  core::Configuration::removeParam("QueryConnect");
  core::Configuration::removeParam("ImprovedHextile");

  if (hasSystemdListeners())
    rfbport.setParam(-1);

  for (int i = 1; i < argc;) {
    int ret = core::Configuration::handleParamArg(argc, argv, i);
    if (ret > 0) {
      i += ret;
      continue;
    }

    if (strcmp(argv[i], "-h") == 0 ||
        strcmp(argv[i], "-help") == 0 ||
        strcmp(argv[i], "--help") == 0) {
      usage();
    }

    if (strcmp(argv[i], "-v") == 0 ||
        strcmp(argv[i], "-version") == 0 ||
        strcmp(argv[i], "--version") == 0) {
      printVersion(stdout);
      return 0;
    }

    if (argv[i][0] == '-') {
      fprintf(stderr, _("%s: Unrecognized option '%s'\n"),
              programName, argv[i]);
      fprintf(stderr, _("See '%s --help' for more information.\n"),
              programName);
      exit(1);
    }

    fprintf(stderr, _("%s: Extra argument '%s'\n"), programName, argv[i]);
    fprintf(stderr, _("See '%s --help' for more information.\n"),
            programName);
    exit(1);
  }

  signal(SIGHUP, CleanupSignalHandler);
  signal(SIGINT, CleanupSignalHandler);
  signal(SIGTERM, CleanupSignalHandler);
  signal(SIGPIPE, SIG_IGN);

  std::list<network::SocketListener*> listeners;

  try {
    ScrcpyServer server;
    server.startScrcpy();

    FileTcpFilter fileTcpFilter(hostsFile);

    network::ConnectionFilter* filter = strlen(hostsFile) ? &fileTcpFilter : nullptr;
    if (createSystemdListeners(&listeners, filter) > 0)
      vlog.info(_("Listening for VNC connections on systemd sockets"));

    if ((int)rfbport != -1) {
      std::list<network::SocketListener*> tcpListeners;
      const char* addr = interface;

      if (strcasecmp(addr, "all") == 0)
        addr = nullptr;
      if (localhostOnly)
        network::createLocalTcpListeners(&tcpListeners, (int)rfbport);
      else
        network::createTcpListeners(&tcpListeners, addr, (int)rfbport);

      if (!tcpListeners.empty()) {
        for (network::SocketListener* listener : tcpListeners)
          listener->setFilter(filter);
        listeners.splice(listeners.end(), tcpListeners);
        if (localhostOnly) {
          vlog.info(_("Listening for VNC connections on local "
                      "interfaces, port %d"), (int)rfbport);
        } else {
          vlog.info(_("Listening for VNC connections on interface %s, "
                      "port %d"),
                    (const char*)interface, (int)rfbport);
        }
      }

    }

    if (rfbunixpath.getValueStr()[0] != '\0') {
      listeners.push_back(new network::UnixListener(rfbunixpath, rfbunixmode));
      vlog.info(_("Listening for VNC connections on %s (mode %04o)"),
                (const char*)rfbunixpath, (int)rfbunixmode);
    }

    if (listeners.empty()) {
      vlog.error(_("No path or port configured for incoming connections"));
      return 1;
    }

    for (network::SocketListener* listener : listeners) {
      int fd = listener->getFd();
      int flags = fcntl(fd, F_GETFL, 0);
      if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        throw core::posix_error(_("Could not set listener to nonblocking mode"), errno);
    }

    server.run(listeners, &caughtSignal);
  } catch (std::exception& e) {
    vlog.error("%s", e.what());
    for (network::SocketListener* listener : listeners)
      delete listener;
    return 1;
  }

  for (network::SocketListener* listener : listeners)
    delete listener;

  vlog.info(_("Terminated"));
  return 0;
}
