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
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <core/Exception.h>
#include <core/LogWriter.h>
#include <core/string.h>

static core::LogWriter vlog("AdbClient");

namespace {

static const int kAdbTimeoutMs = 5000;
static const size_t kChunkMax = 65536;
static const size_t kShellV2ChunkMax = 32768;
static const size_t kSyncPathMax = 1024;
static const uint8_t kShellV2IdStdin = 0;
static const uint8_t kShellV2IdStdout = 1;
static const uint8_t kShellV2IdStderr = 2;
static const uint8_t kShellV2IdExit = 3;
static const uint8_t kShellV2IdCloseStdin = 4;

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

uint64_t getLe64(const uint8_t* src)
{
  return static_cast<uint64_t>(getLe32(src)) |
         (static_cast<uint64_t>(getLe32(src + 4)) << 32);
}

int64_t getLeS64(const uint8_t* src)
{
  return static_cast<int64_t>(getLe64(src));
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

int AdbClient::connectDaemon(int timeoutMs) const
{
  if (timeoutMs <= 0)
    timeoutMs = kAdbTimeoutMs;

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

    struct timeval timeout = {
      timeoutMs / 1000,
      static_cast<suseconds_t>((timeoutMs % 1000) * 1000)
    };
    int flags = ::fcntl(sock, F_GETFL, 0);
    if (flags < 0 || ::fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0 ||
        ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
      lastErr = errno;
    } else {
      int ret = ::connect(sock, p->ai_addr, p->ai_addrlen);
      lastErr = ret == 0 ? 0 : errno;
      if (lastErr == EINPROGRESS || lastErr == EINTR) {
        struct pollfd pfd = { sock, POLLOUT, 0 };
        ret = ::poll(&pfd, 1, timeoutMs);
        if (ret > 0) {
          socklen_t len = sizeof(lastErr);
          if (::getsockopt(sock, SOL_SOCKET, SO_ERROR, &lastErr, &len) < 0)
            lastErr = errno;
        } else {
          lastErr = ret == 0 ? ETIMEDOUT : errno;
        }
      }
      if (lastErr == 0) {
        if (::fcntl(sock, F_SETFL, flags) == 0)
          break;
        lastErr = errno;
      }
    }
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

AdbFeatures AdbClient::queryFeatures(const std::string& serial) const
{
  AdbFeatures features;
  try {
    FdGuard fd(connectDaemon());
    sendCommand(fd.get(), "host-serial:" + serial + ":features");
    std::string err;
    if (!readStatus(fd.get(), &err)) {
      vlog.debug("ADB features query failed for '%s': %s",
                 serial.c_str(), err.c_str());
      return features;
    }
    std::string list = readLengthPrefixed(fd.get());
    size_t pos = 0;
    while (pos < list.size()) {
      size_t comma = list.find(',', pos);
      if (comma == std::string::npos)
        comma = list.size();
      std::string tok = list.substr(pos, comma - pos);
      if (tok == "stat_v2")
        features.statV2 = true;
      else if (tok == "ls_v2")
        features.lsV2 = true;
      else if (tok == "shell_v2")
        features.shellV2 = true;
      pos = comma + 1;
    }
  } catch (const std::exception& e) {
    vlog.debug("ADB features query exception for '%s': %s",
               serial.c_str(), e.what());
  }
  return features;
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
  int inFd = ::open(localPath.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (inFd < 0)
    throw core::posix_error(
      core::format("Failed to open local file '%s'", localPath.c_str()),
      errno);
  FdGuard fileGuard(inFd);

  struct stat st;
  if (::fstat(inFd, &st) < 0)
    throw core::posix_error("fstat failed on local file", errno);
  if (!S_ISREG(st.st_mode))
    throw std::invalid_argument("scrcpy-server path must be a regular file");

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
    if (respLen > kChunkMax)
      throw std::runtime_error("Invalid ADB sync error message length");
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

int AdbClient::openSync(const std::string& serial, int timeoutMs) const
{
  FdGuard fd(connectDaemon(timeoutMs));
  selectTransport(fd.get(), serial);

  sendCommand(fd.get(), "sync:");
  std::string err;
  if (!readStatus(fd.get(), &err)) {
    throw std::runtime_error(
      core::format("ADB sync: failed: %s", err.c_str()));
  }

  return fd.release();
}

bool AdbClient::syncStat(int syncFd, const std::string& remotePath,
                         bool useV2, bool followSymlinks, AdbStat* out)
{
  if (!out)
    return false;
  *out = AdbStat();

  if (remotePath.empty() || remotePath.size() > kSyncPathMax) {
    out->error = remotePath.empty() ? EINVAL : ENAMETOOLONG;
    return false;
  }

  const char* cmdId = useV2 ? (followSymlinks ? "STA2" : "LST2") : "STAT";
  uint8_t req[8];
  std::memcpy(req, cmdId, 4);
  putLe32(req + 4, static_cast<uint32_t>(remotePath.size()));
  writeAll(syncFd, req, sizeof(req));
  writeAll(syncFd, reinterpret_cast<const uint8_t*>(remotePath.data()),
           remotePath.size());

  uint8_t respId[4];
  readAll(syncFd, respId, sizeof(respId));

  if (std::memcmp(respId, "FAIL", 4) == 0) {
    uint8_t lenBuf[4];
    readAll(syncFd, lenBuf, sizeof(lenBuf));
    uint32_t msgLen = getLe32(lenBuf);
    if (msgLen > kChunkMax)
      throw std::runtime_error("Invalid ADB sync FAIL length");
    std::string msg(msgLen, '\0');
    if (msgLen > 0)
      readAll(syncFd, reinterpret_cast<uint8_t*>(&msg[0]), msgLen);
    throw std::runtime_error(
      core::format("ADB %s failed: %s", cmdId, msg.c_str()));
  }

  if (useV2) {
    if (std::memcmp(respId, cmdId, 4) != 0)
      throw std::runtime_error("Unexpected ADB sync v2 stat response");
    uint8_t body[68];
    readAll(syncFd, body, sizeof(body));
    out->error = getLe32(body + 0);
    out->mode = getLe32(body + 20);
    out->size = getLe64(body + 36);
    out->mtime = getLeS64(body + 52);
    return out->error == 0;
  }

  if (std::memcmp(respId, "STAT", 4) != 0)
    throw std::runtime_error("Unexpected ADB sync STAT response");
  uint8_t body[12];
  readAll(syncFd, body, sizeof(body));
  out->mode = getLe32(body + 0);
  out->size = getLe32(body + 4);
  out->mtime = static_cast<int64_t>(getLe32(body + 8));
  if (out->mode == 0 && out->size == 0 && out->mtime == 0) {
    out->error = ENOENT;
    return false;
  }
  out->error = 0;
  return true;
}

std::vector<AdbDirEntry> AdbClient::syncList(int syncFd,
                                             const std::string& dirPath,
                                             bool useLsV2,
                                             bool useStatV2)
{
  if (dirPath.empty() || dirPath.size() > kSyncPathMax)
    throw std::invalid_argument("Invalid ADB sync directory path length");

  const char* cmdId = useLsV2 ? "LIS2" : "LIST";
  uint8_t req[8];
  std::memcpy(req, cmdId, 4);
  putLe32(req + 4, static_cast<uint32_t>(dirPath.size()));
  writeAll(syncFd, req, sizeof(req));
  writeAll(syncFd, reinterpret_cast<const uint8_t*>(dirPath.data()),
           dirPath.size());

  std::vector<AdbDirEntry> entries;
  while (true) {
    uint8_t id[4];
    readAll(syncFd, id, sizeof(id));

    if (std::memcmp(id, "DONE", 4) == 0) {
      uint8_t tail[72];
      readAll(syncFd, tail, useLsV2 ? 72 : 16);
      break;
    }

    if (std::memcmp(id, "FAIL", 4) == 0) {
      uint8_t lenBuf[4];
      readAll(syncFd, lenBuf, sizeof(lenBuf));
      uint32_t msgLen = getLe32(lenBuf);
      if (msgLen > kChunkMax)
        throw std::runtime_error("Invalid ADB sync FAIL length");
      std::string msg(msgLen, '\0');
      if (msgLen > 0)
        readAll(syncFd, reinterpret_cast<uint8_t*>(&msg[0]), msgLen);
      throw std::runtime_error(
        core::format("ADB %s failed: %s", cmdId, msg.c_str()));
    }

    if (useLsV2 && std::memcmp(id, "DNT2", 4) == 0) {
      uint8_t hdr[72];
      readAll(syncFd, hdr, sizeof(hdr));
      uint32_t err = getLe32(hdr + 0);
      uint32_t mode = getLe32(hdr + 20);
      uint64_t size = getLe64(hdr + 36);
      int64_t mtime = getLeS64(hdr + 52);
      uint32_t nameLen = getLe32(hdr + 68);
      if (nameLen > kChunkMax)
        throw std::runtime_error("Invalid ADB DNT2 name length");
      std::string name(nameLen, '\0');
      if (nameLen > 0)
        readAll(syncFd, reinterpret_cast<uint8_t*>(&name[0]), nameLen);
      if (name == "." || name == "..")
        continue;
      AdbDirEntry e;
      e.name = std::move(name);
      e.mode = (err == 0) ? mode : 0;
      e.size = (err == 0) ? size : 0;
      e.mtime = (err == 0) ? mtime : 0;
      entries.push_back(std::move(e));
      continue;
    }

    if (!useLsV2 && std::memcmp(id, "DENT", 4) == 0) {
      uint8_t hdr[16];
      readAll(syncFd, hdr, sizeof(hdr));
      uint32_t mode = getLe32(hdr + 0);
      uint32_t size = getLe32(hdr + 4);
      uint32_t mtime = getLe32(hdr + 8);
      uint32_t nameLen = getLe32(hdr + 12);
      if (nameLen > kChunkMax)
        throw std::runtime_error("Invalid ADB DENT name length");
      std::string name(nameLen, '\0');
      if (nameLen > 0)
        readAll(syncFd, reinterpret_cast<uint8_t*>(&name[0]), nameLen);
      if (name == "." || name == "..")
        continue;
      AdbDirEntry e;
      e.name = std::move(name);
      e.mode = mode;
      e.size = size;
      e.mtime = static_cast<int64_t>(mtime);
      entries.push_back(std::move(e));
      continue;
    }

    throw std::runtime_error("Unexpected ADB sync directory entry");
  }

  // Resolve symlinks (e.g. /sdcard -> /storage/self/primary) on the same sync fd.
  std::string base = (dirPath == "/") ? "/" : (dirPath + "/");
  for (AdbDirEntry& e : entries) {
    if (!S_ISLNK(e.mode))
      continue;
    AdbStat st;
    try {
      if (syncStat(syncFd, base + e.name, useStatV2, true, &st) &&
          st.mode != 0) {
        e.mode = st.mode;
        e.size = st.size;
        e.mtime = st.mtime;
      }
    } catch (const std::exception&) {
    }
  }

  return entries;
}

void AdbClient::syncStartRecv(int syncFd, const std::string& remotePath)
{
  if (remotePath.empty() || remotePath.size() > kSyncPathMax)
    throw std::invalid_argument("Invalid ADB sync RECV path length");

  uint8_t req[8];
  std::memcpy(req, "RECV", 4);
  putLe32(req + 4, static_cast<uint32_t>(remotePath.size()));
  writeAll(syncFd, req, sizeof(req));
  writeAll(syncFd, reinterpret_cast<const uint8_t*>(remotePath.data()),
           remotePath.size());
}

int AdbClient::syncReadRecvChunk(int syncFd,
                                 std::vector<uint8_t>* chunkOut,
                                 std::string* errOut)
{
  uint8_t hdr[8];
  readAll(syncFd, hdr, sizeof(hdr));
  uint32_t len = getLe32(hdr + 4);

  if (std::memcmp(hdr, "DATA", 4) == 0) {
    if (len > kChunkMax)
      throw std::runtime_error("Invalid ADB sync DATA length");
    if (len > 0 && chunkOut != nullptr) {
      size_t old = chunkOut->size();
      chunkOut->resize(old + len);
      readAll(syncFd, chunkOut->data() + old, len);
    } else if (len > 0) {
      std::vector<uint8_t> discard(len);
      readAll(syncFd, discard.data(), len);
    }
    return 1;
  }

  if (std::memcmp(hdr, "DONE", 4) == 0)
    return 0;

  if (std::memcmp(hdr, "FAIL", 4) == 0) {
    if (len > kChunkMax)
      throw std::runtime_error("Invalid ADB sync FAIL length");
    std::string msg(len, '\0');
    if (len > 0)
      readAll(syncFd, reinterpret_cast<uint8_t*>(&msg[0]), len);
    if (errOut != nullptr)
      *errOut = msg;
    return -1;
  }

  throw std::runtime_error("Unexpected ADB sync RECV response");
}

void AdbClient::syncStartSend(int syncFd, const std::string& remotePath,
                              unsigned int mode)
{
  std::string remoteSpec =
    core::format("%s,%u", remotePath.c_str(), mode);
  if (remoteSpec.size() > kSyncPathMax)
    throw std::invalid_argument("Remote path specification too long");

  uint8_t header[8];
  std::memcpy(header, "SEND", 4);
  putLe32(header + 4, static_cast<uint32_t>(remoteSpec.size()));
  writeAll(syncFd, header, sizeof(header));
  writeAll(syncFd, reinterpret_cast<const uint8_t*>(remoteSpec.data()),
           remoteSpec.size());
}

bool AdbClient::syncCheckImmediateError(int syncFd, int timeoutMs,
                                        std::string* errOut)
{
  struct pollfd pfd;
  pfd.fd = syncFd;
  pfd.events = POLLIN;
  pfd.revents = 0;

  int pr;
  do {
    pr = ::poll(&pfd, 1, timeoutMs);
  } while (pr < 0 && errno == EINTR);

  if (pr <= 0)
    return false;

  uint8_t hdr[8];
  ssize_t first = ::recv(syncFd, hdr, sizeof(hdr), MSG_PEEK);
  if (first == 0) {
    if (errOut != nullptr)
      *errOut = "ADB sync connection closed";
    return true;
  }
  if (first < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return false;
    if (errOut != nullptr)
      *errOut = std::strerror(errno);
    return true;
  }
  if (first < 4)
    return false;
  if (std::memcmp(hdr, "FAIL", 4) != 0)
    return false;

  readAll(syncFd, hdr, sizeof(hdr));
  uint32_t respLen = getLe32(hdr + 4);
  if (respLen > kChunkMax)
    throw std::runtime_error("Invalid ADB sync FAIL length");
  std::string msg(respLen, '\0');
  if (respLen > 0)
    readAll(syncFd, reinterpret_cast<uint8_t*>(&msg[0]), respLen);
  if (errOut != nullptr)
    *errOut = msg;
  return true;
}

void AdbClient::syncWriteSendData(int syncFd, const uint8_t* data, size_t len)
{
  if (!data || len == 0)
    return;

  std::vector<uint8_t> buf(8 + kChunkMax);
  std::memcpy(buf.data(), "DATA", 4);

  size_t off = 0;
  while (off < len) {
    size_t chunk = std::min(kChunkMax, len - off);
    putLe32(buf.data() + 4, static_cast<uint32_t>(chunk));
    std::memcpy(buf.data() + 8, data + off, chunk);
    writeAll(syncFd, buf.data(), 8 + chunk);
    off += chunk;
  }
}

bool AdbClient::syncFinishSend(int syncFd, uint32_t mtimeSec,
                               std::string* errOut)
{
  uint8_t header[8];
  std::memcpy(header, "DONE", 4);
  putLe32(header + 4, mtimeSec);
  writeAll(syncFd, header, sizeof(header));

  uint8_t resp[8];
  readAll(syncFd, resp, sizeof(resp));
  uint32_t respLen = getLe32(resp + 4);

  if (std::memcmp(resp, "FAIL", 4) == 0) {
    if (respLen > kChunkMax)
      throw std::runtime_error("Invalid ADB sync error message length");
    std::string failMsg(respLen, '\0');
    if (respLen > 0)
      readAll(syncFd, reinterpret_cast<uint8_t*>(&failMsg[0]), respLen);
    if (errOut != nullptr)
      *errOut = failMsg;
    return false;
  }
  if (std::memcmp(resp, "OKAY", 4) != 0)
    throw std::runtime_error("Unexpected ADB sync response");

  syncQuit(syncFd);
  return true;
}

void AdbClient::syncQuit(int syncFd)
{
  if (syncFd < 0)
    return;
  uint8_t header[8];
  std::memcpy(header, "QUIT", 4);
  putLe32(header + 4, 0);
  try {
    writeAll(syncFd, header, sizeof(header));
  } catch (const std::exception&) {
  }
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

  return fd.release();
}

int AdbClient::openShellV2(const std::string& serial,
                           const std::string& command,
                           int timeoutMs) const
{
  FdGuard fd(connectDaemon(timeoutMs));
  selectTransport(fd.get(), serial);

  sendCommand(fd.get(), "shell,v2:" + command);
  std::string err;
  if (!readStatus(fd.get(), &err))
    throw std::runtime_error(
      core::format("ADB shell,v2 command failed: %s", err.c_str()));

  return fd.release();
}

void AdbClient::shellV2WriteStdin(int fd, const uint8_t* data, size_t len)
{
  if (!data || len == 0)
    return;

  std::vector<uint8_t> pkt(5 + kShellV2ChunkMax);
  pkt[0] = kShellV2IdStdin;
  size_t off = 0;
  while (off < len) {
    size_t chunk = std::min(kShellV2ChunkMax, len - off);
    putLe32(pkt.data() + 1, static_cast<uint32_t>(chunk));
    std::memcpy(pkt.data() + 5, data + off, chunk);
    writeAll(fd, pkt.data(), 5 + chunk);
    off += chunk;
  }
}

void AdbClient::shellV2CloseStdin(int fd)
{
  uint8_t pkt[5];
  pkt[0] = kShellV2IdCloseStdin;
  putLe32(pkt + 1, 0);
  writeAll(fd, pkt, sizeof(pkt));
}

bool AdbClient::shellV2ReadPacket(int fd, uint8_t* idOut,
                                  std::vector<uint8_t>* payloadOut)
{
  uint8_t hdr[5];
  size_t off = 0;
  while (off < sizeof(hdr)) {
    ssize_t n = ::recv(fd, hdr + off, sizeof(hdr) - off, 0);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      throw core::socket_error("ADB shell,v2 recv failed", errno);
    }
    if (n == 0) {
      if (off == 0)
        return false;
      throw std::runtime_error("ADB shell,v2 connection closed mid-packet");
    }
    off += static_cast<size_t>(n);
  }

  uint32_t len = getLe32(hdr + 1);
  if (len > 16u * 1024u * 1024u)
    throw std::runtime_error("ADB shell,v2 packet too large");

  if (idOut != nullptr)
    *idOut = hdr[0];
  if (payloadOut != nullptr) {
    payloadOut->resize(len);
    if (len > 0)
      readAll(fd, payloadOut->data(), len);
  } else if (len > 0) {
    std::vector<uint8_t> discard(len);
    readAll(fd, discard.data(), len);
  }
  return true;
}

AdbShellResult AdbClient::runShell(const std::string& serial,
                                   const std::string& command,
                                   bool useShellV2,
                                   int timeoutMs) const
{
  AdbShellResult res;
  if (useShellV2) {
    FdGuard fd(openShellV2(serial, command, timeoutMs));
    shellV2CloseStdin(fd.get());
    uint8_t id = 0;
    std::vector<uint8_t> payload;
    while (shellV2ReadPacket(fd.get(), &id, &payload)) {
      if (id == kShellV2IdStdout) {
        res.stdoutData.append(reinterpret_cast<const char*>(payload.data()),
                              payload.size());
      } else if (id == kShellV2IdStderr) {
        res.stderrData.append(reinterpret_cast<const char*>(payload.data()),
                              payload.size());
      } else if (id == kShellV2IdExit) {
        if (!payload.empty())
          res.exitCode = static_cast<int>(payload[0]);
        break;
      }
    }
    return res;
  }

  static const char kSentinel[] = "__SCRCPY_ADB_RC__";
  std::string wrapped =
    command + "; printf '\\n" + kSentinel + "%d\\n' $?";
  FdGuard fd(connectDaemon(timeoutMs));
  selectTransport(fd.get(), serial);
  sendCommand(fd.get(), "shell:" + wrapped);
  std::string err;
  if (!readStatus(fd.get(), &err)) {
    throw std::runtime_error(
      core::format("ADB shell command failed: %s", err.c_str()));
  }

  std::string output;
  char buf[4096];
  while (true) {
    ssize_t n = ::recv(fd.get(), buf, sizeof(buf), 0);
    if (n > 0) {
      output.append(buf, static_cast<size_t>(n));
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else if (n == 0) {
      break;
    } else {
      throw core::socket_error("ADB shell recv failed", errno);
    }
  }

  size_t pos = output.rfind(kSentinel);
  if (pos != std::string::npos) {
    size_t codeStart = pos + sizeof(kSentinel) - 1;
    res.exitCode = std::atoi(output.c_str() + codeStart);
    if (pos > 0 && output[pos - 1] == '\n')
      pos--;
    if (pos > 0 && output[pos - 1] == '\r')
      pos--;
    res.stdoutData = output.substr(0, pos);
  } else {
    res.stdoutData = std::move(output);
  }
  return res;
}

std::string AdbClient::quoteShellArg(const std::string& value)
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
