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

#include "AdbClient.h"

#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <core/Exception.h>
#include <core/LogWriter.h>
#include <core/string.h>

static core::LogWriter vlog("AdbClient");

namespace {

void writeAll(int fd, const uint8_t* data, size_t len)
{
  size_t off = 0;
  while (off < len) {
    ssize_t n = ::send(fd, data + off, len - off, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      throw core::socket_error("ADB send failed", errno);
    }
    if (n == 0)
      throw std::runtime_error("ADB connection closed during write");
    off += static_cast<size_t>(n);
  }
}

void readAll(int fd, uint8_t* data, size_t len)
{
  size_t off = 0;
  while (off < len) {
    ssize_t n = ::recv(fd, data + off, len - off, 0);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      throw core::socket_error("ADB recv failed", errno);
    }
    if (n == 0)
      throw std::runtime_error("ADB connection closed unexpectedly");
    off += static_cast<size_t>(n);
  }
}

bool parseHex4(const char* buf, size_t* out)
{
  size_t val = 0;
  for (int i = 0; i < 4; ++i) {
    char c = buf[i];
    val <<= 4;
    if (c >= '0' && c <= '9')
      val |= static_cast<size_t>(c - '0');
    else if (c >= 'a' && c <= 'f')
      val |= static_cast<size_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      val |= static_cast<size_t>(c - 'A' + 10);
    else
      return false;
  }
  *out = val;
  return true;
}

void putLe32(uint8_t* dst, uint32_t val)
{
  dst[0] = static_cast<uint8_t>(val & 0xff);
  dst[1] = static_cast<uint8_t>((val >> 8) & 0xff);
  dst[2] = static_cast<uint8_t>((val >> 16) & 0xff);
  dst[3] = static_cast<uint8_t>((val >> 24) & 0xff);
}

uint32_t getLe32(const uint8_t* src)
{
  return static_cast<uint32_t>(src[0]) |
         (static_cast<uint32_t>(src[1]) << 8) |
         (static_cast<uint32_t>(src[2]) << 16) |
         (static_cast<uint32_t>(src[3]) << 24);
}

class FdGuard {
public:
  explicit FdGuard(int fd) : fd_(fd) {}
  ~FdGuard()
  {
    if (fd_ >= 0)
      ::close(fd_);
  }
  int release()
  {
    int tmp = fd_;
    fd_ = -1;
    return tmp;
  }
  int get() const { return fd_; }
private:
  FdGuard(const FdGuard&) = delete;
  FdGuard& operator=(const FdGuard&) = delete;
  int fd_;
};

} // namespace

AdbClient::AdbClient(const std::string& host, int port)
  : host_(host), port_(port)
{
}

int AdbClient::connectDaemon() const
{
  struct addrinfo hints;
  struct addrinfo* res = nullptr;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  char portStr[16];
  std::snprintf(portStr, sizeof(portStr), "%d", port_);

  int rc = ::getaddrinfo(host_.c_str(), portStr, &hints, &res);
  if (rc != 0)
    throw core::getaddrinfo_error("Failed to resolve ADB host", rc);

  int sock = -1;
  int lastErr = 0;

  for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
    sock = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (sock < 0) {
      lastErr = errno;
      continue;
    }

    int ret;
    do {
      ret = ::connect(sock, p->ai_addr, p->ai_addrlen);
    } while (ret < 0 && errno == EINTR);

    if (ret == 0)
      break;

    lastErr = errno;
    ::close(sock);
    sock = -1;
  }

  ::freeaddrinfo(res);

  if (sock < 0) {
    throw core::socket_error(
      core::format("Failed to connect to ADB daemon %s:%d",
                   host_.c_str(), port_),
      lastErr);
  }

  ::fcntl(sock, F_SETFD, FD_CLOEXEC);

  int one = 1;
  if (::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0)
    vlog.debug("Failed to set TCP_NODELAY on ADB socket: %s",
               std::strerror(errno));

  return sock;
}

void AdbClient::sendCommand(int fd, const std::string& cmd)
{
  if (cmd.size() > 0xffff)
    throw std::invalid_argument("ADB command too long");

  char prefix[5];
  std::snprintf(prefix, sizeof(prefix), "%04x",
                static_cast<unsigned>(cmd.size()));
  writeAll(fd, reinterpret_cast<const uint8_t*>(prefix), 4);
  if (!cmd.empty())
    writeAll(fd, reinterpret_cast<const uint8_t*>(cmd.data()), cmd.size());
}

bool AdbClient::readStatus(int fd, std::string* errorMsg)
{
  char status[4];
  readAll(fd, reinterpret_cast<uint8_t*>(status), 4);

  if (std::memcmp(status, "OKAY", 4) == 0)
    return true;

  if (std::memcmp(status, "FAIL", 4) == 0) {
    std::string msg = readLengthPrefixed(fd);
    if (errorMsg)
      *errorMsg = msg;
    return false;
  }

  throw std::runtime_error(
    core::format("Unexpected ADB status: %.4s", status));
}

std::string AdbClient::readLengthPrefixed(int fd)
{
  char hexLen[4];
  readAll(fd, reinterpret_cast<uint8_t*>(hexLen), 4);

  size_t len = 0;
  if (!parseHex4(hexLen, &len))
    throw std::runtime_error("Invalid hex length from ADB daemon");

  std::string result(len, '\0');
  if (len > 0)
    readAll(fd, reinterpret_cast<uint8_t*>(&result[0]), len);
  return result;
}

void AdbClient::selectTransport(int fd, const std::string& serial)
{
  sendCommand(fd, "host:transport:" + serial);
  std::string err;
  if (!readStatus(fd, &err))
    throw std::runtime_error(
      core::format("ADB transport selection failed for '%s': %s",
                   serial.c_str(), err.c_str()));
}

std::vector<AdbDevice> AdbClient::listDevices() const
{
  FdGuard fd(connectDaemon());
  sendCommand(fd.get(), "host:devices");

  std::string err;
  if (!readStatus(fd.get(), &err))
    throw std::runtime_error(
      core::format("ADB host:devices failed: %s", err.c_str()));

  std::string payload = readLengthPrefixed(fd.get());
  std::vector<AdbDevice> devices;

  size_t pos = 0;
  while (pos < payload.size()) {
    size_t eol = payload.find('\n', pos);
    if (eol == std::string::npos)
      eol = payload.size();

    std::string line = payload.substr(pos, eol - pos);
    pos = eol + 1;

    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.pop_back();
    if (line.empty())
      continue;

    size_t sep = line.find('\t');
    if (sep == std::string::npos)
      sep = line.find(' ');
    if (sep == std::string::npos)
      continue;

    AdbDevice dev;
    dev.serial = line.substr(0, sep);
    size_t stateStart = sep + 1;
    while (stateStart < line.size() &&
           (line[stateStart] == ' ' || line[stateStart] == '\t'))
      stateStart++;
    dev.state = line.substr(stateStart);
    devices.push_back(dev);
  }

  return devices;
}

std::string AdbClient::resolveSerial(const std::string& requestedSerial) const
{
  if (!requestedSerial.empty())
    return requestedSerial;

  std::vector<AdbDevice> all = listDevices();
  std::vector<AdbDevice> online;
  for (const AdbDevice& d : all) {
    if (d.state == "device")
      online.push_back(d);
  }

  if (online.empty()) {
    if (!all.empty()) {
      throw std::runtime_error(
        core::format("No online Android device found (saw '%s' in state '%s')",
                     all[0].serial.c_str(), all[0].state.c_str()));
    }
    throw std::runtime_error("No Android devices attached to ADB daemon");
  }

  if (online.size() > 1) {
    std::string list;
    for (size_t i = 0; i < online.size(); ++i) {
      if (i > 0)
        list += ", ";
      list += online[i].serial;
    }
    throw std::runtime_error(
      core::format("Multiple Android devices online (%s); specify -Serial",
                   list.c_str()));
  }

  return online[0].serial;
}

int AdbClient::connectLocalAbstract(const std::string& serial,
                                    const std::string& socketName) const
{
  FdGuard fd(connectDaemon());
  selectTransport(fd.get(), serial);

  sendCommand(fd.get(), "localabstract:" + socketName);
  std::string err;
  if (!readStatus(fd.get(), &err)) {
    throw std::runtime_error(
      core::format("ADB localabstract:%s failed: %s",
                   socketName.c_str(), err.c_str()));
  }

  return fd.release();
}

void AdbClient::pushFile(const std::string& serial,
                         const std::string& localPath,
                         const std::string& remotePath,
                         unsigned int mode) const
{
  int inFd = ::open(localPath.c_str(), O_RDONLY);
  if (inFd < 0)
    throw core::posix_error(
      core::format("Failed to open local file '%s'", localPath.c_str()),
      errno);
  FdGuard fileGuard(inFd);

  struct stat st;
  if (::fstat(inFd, &st) < 0)
    throw core::posix_error("fstat failed on local file", errno);

  FdGuard fd(connectDaemon());
  selectTransport(fd.get(), serial);

  sendCommand(fd.get(), "sync:");
  std::string err;
  if (!readStatus(fd.get(), &err))
    throw std::runtime_error(
      core::format("ADB sync: failed: %s", err.c_str()));

  std::string remoteSpec =
    core::format("%s,%u", remotePath.c_str(), mode);
  if (remoteSpec.size() > 1024)
    throw std::invalid_argument("Remote path specification too long");

  uint8_t header[8];
  std::memcpy(header, "SEND", 4);
  putLe32(header + 4, static_cast<uint32_t>(remoteSpec.size()));
  writeAll(fd.get(), header, 8);
  writeAll(fd.get(), reinterpret_cast<const uint8_t*>(remoteSpec.data()),
           remoteSpec.size());

  static const size_t kChunkMax = 65536;
  std::vector<uint8_t> buf(8 + kChunkMax);
  std::memcpy(buf.data(), "DATA", 4);

  while (true) {
    ssize_t n = ::read(inFd, buf.data() + 8, kChunkMax);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      throw core::posix_error("Failed reading local file", errno);
    }
    if (n == 0)
      break;

    putLe32(buf.data() + 4, static_cast<uint32_t>(n));
    writeAll(fd.get(), buf.data(), 8 + static_cast<size_t>(n));
  }

  std::memcpy(header, "DONE", 4);
  putLe32(header + 4, static_cast<uint32_t>(st.st_mtime));
  writeAll(fd.get(), header, 8);

  uint8_t resp[8];
  readAll(fd.get(), resp, 8);
  uint32_t respLen = getLe32(resp + 4);

  if (std::memcmp(resp, "FAIL", 4) == 0) {
    std::string failMsg(respLen, '\0');
    if (respLen > 0)
      readAll(fd.get(), reinterpret_cast<uint8_t*>(&failMsg[0]), respLen);
    throw std::runtime_error(
      core::format("ADB push to '%s' failed: %s",
                   remotePath.c_str(), failMsg.c_str()));
  }
  if (std::memcmp(resp, "OKAY", 4) != 0)
    throw std::runtime_error("Unexpected ADB sync response");

  std::memcpy(header, "QUIT", 4);
  putLe32(header + 4, 0);
  writeAll(fd.get(), header, 8);
}

int AdbClient::startShell(const std::string& serial,
                          const std::string& command) const
{
  FdGuard fd(connectDaemon());
  selectTransport(fd.get(), serial);

  sendCommand(fd.get(), "shell:" + command);
  std::string err;
  if (!readStatus(fd.get(), &err))
    throw std::runtime_error(
      core::format("ADB shell command failed: %s", err.c_str()));

  int flags = ::fcntl(fd.get(), F_GETFL, 0);
  if (flags >= 0)
    ::fcntl(fd.get(), F_SETFL, flags | O_NONBLOCK);

  return fd.release();
}

std::string AdbClient::detectVersionFromPath(const std::string& path)
{
  size_t slash = path.find_last_of('/');
  std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);

  for (size_t i = 0; i < base.size(); ++i) {
    unsigned char ch = static_cast<unsigned char>(base[i]);
    if (!std::isdigit(ch))
      continue;
    if (i > 0) {
      char prev = base[i - 1];
      if (prev != 'v' && prev != 'V' && prev != '-' && prev != '_')
        continue;
    }

    size_t j = i;
    int dots = 0;
    while (j < base.size()) {
      unsigned char c = static_cast<unsigned char>(base[j]);
      if (std::isdigit(c)) {
        ++j;
      } else if (base[j] == '.' && j + 1 < base.size() &&
                 std::isdigit(static_cast<unsigned char>(base[j + 1]))) {
        ++dots;
        ++j;
      } else {
        break;
      }
    }
    if (dots >= 1 && dots <= 2)
      return base.substr(i, j - i);
  }

  return "";
}

std::string AdbClient::extractExpectedVersionFromError(const std::string& text)
{
  static const char kNeedle[] = "The server version (";
  size_t pos = text.find(kNeedle);
  if (pos == std::string::npos)
    return "";
  pos += sizeof(kNeedle) - 1;
  size_t end = text.find(')', pos);
  if (end == std::string::npos || end == pos)
    return "";
  return text.substr(pos, end - pos);
}
