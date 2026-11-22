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
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <list>

#include <core/Configuration.h>
#include <core/LogWriter.h>
#include <core/Logger_stdio.h>
#include <core/i18n.h>
#include <network/TcpSocket.h>
#include <network/UnixSocket.h>

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-daemon.h>
#endif

#include "ScrcpyServer.h"
#include "parameters.h"

static core::LogWriter vlog("Main");

static volatile bool caughtSignal = false;
char* programName = nullptr;

static void CleanupSignalHandler(int /*sig*/)
{
  caughtSignal = true;
}

static bool hasSystemdListeners()
{
#ifdef HAVE_LIBSYSTEMD
  return sd_listen_fds(0) != 0;
#else
  return false;
#endif
}

static int createSystemdListeners(std::list<network::SocketListener*>* listeners)
{
#ifdef HAVE_LIBSYSTEMD
  int count = sd_listen_fds(0);
  if (count < 0) {
    vlog.error(_("Error getting listening sockets from systemd: %s"),
               strerror(-count));
    return count;
  }

  for (int i = 0; i < count; ++i)
    listeners->push_back(new network::TcpListener(SD_LISTEN_FDS_START + i));

  return count;
#else
  (void)listeners;
  return 0;
#endif
}

class FileTcpFilter : public network::TcpFilter {
public:
  explicit FileTcpFilter(const char* fname)
    : TcpFilter("-"), fileName_(nullptr), lastModTime_(0)
  {
    if (fname != nullptr)
      fileName_ = strdup(fname);
  }

  ~FileTcpFilter() override
  {
    if (fileName_ != nullptr)
      free(fileName_);
  }

  bool verifyConnection(network::Socket* s) override
  {
    if (!reloadRules()) {
      vlog.error(_("Could not read IP filtering rules, "
                   "rejecting all clients"));
      filter.clear();
      filter.push_back(parsePattern("-"));
      return false;
    }
    return TcpFilter::verifyConnection(s);
  }

private:
  bool reloadRules()
  {
    if (fileName_ == nullptr)
      return true;

    struct stat st;
    if (stat(fileName_, &st) != 0)
      return false;

    if (st.st_mtime != lastModTime_) {
      FILE* fp = fopen(fileName_, "r");
      if (fp == nullptr)
        return false;

      filter.clear();

      char buf[64];
      while (readLine(buf, sizeof(buf), fp)) {
        if (buf[0] && strchr("+-?", buf[0]))
          filter.push_back(parsePattern(buf));
      }

      fclose(fp);
      lastModTime_ = st.st_mtime;
    }
    return true;
  }

  bool readLine(char* buf, int bufSize, FILE* fp)
  {
    if (fp == nullptr || buf == nullptr || bufSize == 0)
      return false;

    if (fgets(buf, bufSize, fp) == nullptr)
      return false;

    char* p = strchr(buf, '\n');
    if (p != nullptr) {
      *p = '\0';
    } else if (!feof(fp)) {
      int c;
      do {
        c = getc(fp);
      } while (c != '\n' && c != EOF);
    }
    return true;
  }

  char* fileName_;
  time_t lastModTime_;
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

    if (createSystemdListeners(&listeners) > 0)
      vlog.info(_("Listening for VNC connections on systemd sockets"));

    if (rfbunixpath.getValueStr()[0] != '\0') {
      listeners.push_back(new network::UnixListener(rfbunixpath, rfbunixmode));
      vlog.info(_("Listening for VNC connections on %s (mode %04o)"),
                (const char*)rfbunixpath, (int)rfbunixmode);
    }

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

      if (strlen(hostsFile) != 0) {
        for (network::SocketListener* listener : listeners)
          listener->setFilter(&fileTcpFilter);
      }
    }

    if (listeners.empty()) {
      vlog.error(_("No path or port configured for incoming connections"));
      return 1;
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
