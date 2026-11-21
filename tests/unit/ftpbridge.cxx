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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <network/TcpSocket.h>
#include <rdr/BufferedOutStream.h>
#include <rdr/FdInStream.h>
#include <rdr/FdOutStream.h>
#include <rfb/CConnection.h>
#include <rfb/FileTransferClient.h>
#include <rfb/PixelBuffer.h>
#include <rfb/Security.h>
#include <rfb/TightCapability.h>
#include <rfb/encodings.h>
#include <rfb/fenceTypes.h>
#include <rfb/fileTransferTypes.h>
#include <rfb/msgTypes.h>

#include "../../vncviewer/FtpProtocol.h"
#include "../../vncviewer/FtpServer.h"

namespace {

enum class MockServerMode {
  Tight2,
  Tight13,
  Ultra,
  NoFt
};

static uint16_t readBE16(const uint8_t* p)
{
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

static uint32_t readBE32(const uint8_t* p)
{
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) |
         static_cast<uint32_t>(p[3]);
}

static uint64_t readBE64(const uint8_t* p)
{
  return (static_cast<uint64_t>(readBE32(p)) << 32) |
         static_cast<uint64_t>(readBE32(p + 4));
}

static void writeBE16(std::vector<uint8_t>* out, uint16_t v)
{
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out->push_back(static_cast<uint8_t>(v & 0xFF));
}

static void writeLE16(std::vector<uint8_t>* out, uint16_t v)
{
  out->push_back(static_cast<uint8_t>(v & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

static void writeBE32(std::vector<uint8_t>* out, uint32_t v)
{
  out->push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out->push_back(static_cast<uint8_t>(v & 0xFF));
}

static void writeBE64(std::vector<uint8_t>* out, uint64_t v)
{
  writeBE32(out, static_cast<uint32_t>(v >> 32));
  writeBE32(out, static_cast<uint32_t>(v & 0xFFFFFFFFU));
}

static void writeCap(std::vector<uint8_t>* out, int32_t code,
                     const char* vendor, const char* sig)
{
  writeBE32(out, static_cast<uint32_t>(code));
  out->insert(out->end(), vendor, vendor + 4);
  out->insert(out->end(), sig, sig + 8);
}

static std::vector<uint8_t> compressSync(const uint8_t* data, size_t len)
{
  if (len == 0)
    return {};
  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
               Z_DEFAULT_STRATEGY);
  std::vector<uint8_t> out(deflateBound(&zs, static_cast<uLong>(len)) + 32);
  zs.next_in = const_cast<Bytef*>(data);
  zs.avail_in = static_cast<uInt>(len);
  zs.next_out = out.data();
  zs.avail_out = static_cast<uInt>(out.size());
  deflate(&zs, Z_SYNC_FLUSH);
  size_t total = zs.total_out;
  deflateEnd(&zs);
  out.resize(total);
  return out;
}

static std::vector<uint8_t> decompressSync(const uint8_t* data, size_t len,
                                           size_t outLen)
{
  if (outLen == 0)
    return {};
  std::vector<uint8_t> out(outLen);
  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  inflateInit2(&zs, 15);
  zs.next_in = const_cast<Bytef*>(data);
  zs.avail_in = static_cast<uInt>(len);
  zs.next_out = out.data();
  zs.avail_out = static_cast<uInt>(outLen);
  int guard = 0;
  int rc = Z_OK;
  while ((rc == Z_OK || rc == Z_BUF_ERROR) &&
         zs.total_out < outLen && guard++ < 64) {
    rc = inflate(&zs, Z_SYNC_FLUSH);
    if (rc == Z_STREAM_END)
      break;
  }
  inflateEnd(&zs);
  return out;
}

static std::string normPath(const std::string& p)
{
  std::string out = p;
  for (size_t i = 0; i < out.size(); i++) {
    if (out[i] == '\\')
      out[i] = '/';
  }
  return ftp::normalizeFtpPath("/", out);
}

class MockVncFileServer {
public:
  struct Node {
    bool isDir = false;
    std::vector<uint8_t> data;
    int64_t mtime = 1700000000LL;
  };

  explicit MockVncFileServer(MockServerMode mode)
    : mode_(mode), listenFd_(-1), clientFd_(-1), listenPort_(0),
      state_(State::Version)
  {
    nodes_["/"] = {true, {}, 1700000000LL};
    if (mode_ == MockServerMode::Ultra) {
      nodes_["/C:"] = {true, {}, 1700000000LL};
      nodes_["/C:/sub"] = {true, {}, 1700000000LL};
      std::string hello = "Hello from UltraVNC server!";
      nodes_["/C:/hello.txt"] = {
        false, std::vector<uint8_t>(hello.begin(), hello.end()), 1700000500LL
      };
    } else {
      nodes_["/sub"] = {true, {}, 1700000000LL};
      std::string hello = "Hello from TightVNC server!";
      nodes_["/hello.txt"] = {
        false, std::vector<uint8_t>(hello.begin(), hello.end()), 1700000500LL
      };
      std::string nested = "Nested content 1234567890";
      nodes_["/sub/nested.txt"] = {
        false, std::vector<uint8_t>(nested.begin(), nested.end()), 1700000600LL
      };
      nodes_["/empty.txt"] = {false, {}, 1700000100LL};
      std::vector<uint8_t> big(260000);
      for (size_t i = 0; i < big.size(); i++)
        big[i] = static_cast<uint8_t>((i * 37 + 13) & 0xFF);
      nodes_["/big.bin"] = {false, big, 1700000700LL};
    }
  }

  ~MockVncFileServer()
  {
    if (clientFd_ >= 0)
      ::close(clientFd_);
    if (listenFd_ >= 0)
      ::close(listenFd_);
  }

  bool start()
  {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0)
      return false;
    int one = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    int fl = ::fcntl(listenFd_, F_GETFL, 0);
    ::fcntl(listenFd_, F_SETFL, fl | O_NONBLOCK);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (::bind(listenFd_, reinterpret_cast<struct sockaddr*>(&sa),
               sizeof(sa)) != 0 ||
        ::listen(listenFd_, 4) != 0) {
      return false;
    }
    socklen_t len = sizeof(sa);
    if (::getsockname(listenFd_, reinterpret_cast<struct sockaddr*>(&sa),
                      &len) != 0)
      return false;
    listenPort_ = ntohs(sa.sin_port);
    return true;
  }

  int listenFd() const { return listenFd_; }
  int clientFd() const { return clientFd_; }
  int port() const { return listenPort_; }
  bool hasPendingWrite() const { return !outBuf_.empty(); }

  void acceptIfNeeded()
  {
    if (clientFd_ >= 0)
      return;
    int cfd = ::accept(listenFd_, nullptr, nullptr);
    if (cfd < 0)
      return;
    int fl = ::fcntl(cfd, F_GETFL, 0);
    ::fcntl(cfd, F_SETFL, fl | O_NONBLOCK);
    int one = 1;
    ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    clientFd_ = cfd;
    const char ver[] = "RFB 003.008\n";
    outBuf_.insert(outBuf_.end(), ver, ver + 12);
    state_ = State::Version;
  }

  void handleIo(bool canRead, bool canWrite)
  {
    if (clientFd_ < 0)
      return;
    if (canRead) {
      uint8_t buf[65536];
      for (;;) {
        ssize_t n = ::recv(clientFd_, buf, sizeof(buf), 0);
        if (n > 0) {
          inBuf_.insert(inBuf_.end(), buf, buf + n);
        } else {
          break;
        }
      }
      processInput();
    }
    if (canWrite && !outBuf_.empty()) {
      ssize_t n = ::send(clientFd_, outBuf_.data(), outBuf_.size(),
                         MSG_NOSIGNAL);
      if (n > 0)
        outBuf_.erase(outBuf_.begin(), outBuf_.begin() + n);
    }
  }

  bool getFile(const std::string& path, std::vector<uint8_t>* out) const
  {
    std::map<std::string, Node>::const_iterator it = nodes_.find(path);
    if (it == nodes_.end() || it->second.isDir)
      return false;
    *out = it->second.data;
    return true;
  }

  bool hasPath(const std::string& path) const
  {
    return nodes_.find(path) != nodes_.end();
  }

  int listReqCount() const { return listReqCount_; }

private:
  enum class State {
    Version,
    SecChoice,
    TightTunnel,
    TightAuth,
    ClientInit,
    Normal
  };

  std::vector<std::pair<std::string, Node>> listChildren(const std::string& dir)
  {
    std::vector<std::pair<std::string, Node>> res;
    std::string prefix = (dir == "/") ? "/" : (dir + "/");
    for (auto& kv : nodes_) {
      if (kv.first == dir)
        continue;
      if (kv.first.compare(0, prefix.size(), prefix) != 0)
        continue;
      std::string rest = kv.first.substr(prefix.size());
      if (rest.find('/') != std::string::npos)
        continue;
      res.push_back({rest, kv.second});
    }
    return res;
  }

  void sendUltra(uint8_t type, uint16_t param, uint32_t size,
                 const uint8_t* payload, uint32_t len,
                 bool hasSizeH = false, uint32_t sizeH = 0)
  {
    outBuf_.push_back(rfb::msgTypeFileTransfer);
    outBuf_.push_back(type);
    writeLE16(&outBuf_, param);
    writeBE32(&outBuf_, size);
    writeBE32(&outBuf_, len);
    if (len > 0 && payload != nullptr)
      outBuf_.insert(outBuf_.end(), payload, payload + len);
    if (hasSizeH)
      writeBE32(&outBuf_, sizeH);
  }

  void sendTight2Failed(const std::string& msg)
  {
    writeBE32(&outBuf_, rfb::tight2LastRequestFailedReply);
    writeBE32(&outBuf_, static_cast<uint32_t>(msg.size()));
    outBuf_.insert(outBuf_.end(), msg.begin(), msg.end());
  }

  void sendServerInit()
  {
    writeBE16(&outBuf_, 64);
    writeBE16(&outBuf_, 64);
    // 16-byte PixelFormat: 32bpp RGBA
    outBuf_.push_back(32); outBuf_.push_back(24);
    outBuf_.push_back(0); outBuf_.push_back(1);
    writeBE16(&outBuf_, 255); writeBE16(&outBuf_, 255); writeBE16(&outBuf_, 255);
    outBuf_.push_back(16); outBuf_.push_back(8); outBuf_.push_back(0);
    outBuf_.push_back(0); outBuf_.push_back(0); outBuf_.push_back(0);
    const char* name = "MockVNC";
    writeBE32(&outBuf_, static_cast<uint32_t>(strlen(name)));
    outBuf_.insert(outBuf_.end(), name, name + strlen(name));

    if (mode_ == MockServerMode::Tight2) {
      writeBE16(&outBuf_, 14);
      writeBE16(&outBuf_, 13);
      writeBE16(&outBuf_, 0);
      writeBE16(&outBuf_, 0);
      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2CompressionSupportReply), rfb::tightVendorTight, rfb::sigTight2CompressionSupportReply);
      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2FileListReply), rfb::tightVendorTight, rfb::sigTight2FileListReply);
      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2Md5Reply), rfb::tightVendorTight, rfb::sigTight2Md5Reply);
      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2UploadStartReply), rfb::tightVendorTight, rfb::sigTight2UploadStartReply);
      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2UploadDataReply), rfb::tightVendorTight, rfb::sigTight2UploadDataReply);
      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2UploadEndReply), rfb::tightVendorTight, rfb::sigTight2UploadEndReply);
      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2DownloadStartReply), rfb::tightVendorTight, rfb::sigTight2DownloadStartReply);
      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2DownloadDataReply), rfb::tightVendorTight, rfb::sigTight2DownloadDataReply);
      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2DownloadEndReply), rfb::tightVendorTight, rfb::sigTight2DownloadEndReply);
      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2MkdirReply), rfb::tightVendorTight, rfb::sigTight2MkdirReply);
      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2RemoveReply), rfb::tightVendorTight, rfb::sigTight2RemoveReply);
      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2RenameReply), rfb::tightVendorTight, rfb::sigTight2RenameReply);
      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2DirSizeReply), rfb::tightVendorTight, rfb::sigTight2DirSizeReply);
      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2LastRequestFailedReply), rfb::tightVendorTight, rfb::sigTight2LastRequestFailedReply);

      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2CompressionSupportReq), rfb::tightVendorTight, rfb::sigTight2CompressionSupportReq);
      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2FileListReq), rfb::tightVendorTight, rfb::sigTight2FileListReq);
      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2Md5Req), rfb::tightVendorTight, rfb::sigTight2Md5Req);
      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2UploadStartReq), rfb::tightVendorTight, rfb::sigTight2UploadStartReq);
      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2UploadDataReq), rfb::tightVendorTight, rfb::sigTight2UploadDataReq);
      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2UploadEndReq), rfb::tightVendorTight, rfb::sigTight2UploadEndReq);
      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2DownloadStartReq), rfb::tightVendorTight, rfb::sigTight2DownloadStartReq);
      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2DownloadDataReq), rfb::tightVendorTight, rfb::sigTight2DownloadDataReq);
      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2MkdirReq), rfb::tightVendorTight, rfb::sigTight2MkdirReq);
      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2RemoveReq), rfb::tightVendorTight, rfb::sigTight2RemoveReq);
      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2RenameReq), rfb::tightVendorTight, rfb::sigTight2RenameReq);
      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2DirSizeReq), rfb::tightVendorTight, rfb::sigTight2DirSizeReq);
      writeCap(&outBuf_, static_cast<int32_t>(rfb::tight2LastRequestFailedReply), rfb::tightVendorTight, rfb::sigTight2LastRequestFailedReply);
    } else if (mode_ == MockServerMode::Tight13) {
      writeBE16(&outBuf_, 4);
      writeBE16(&outBuf_, 6);
      writeBE16(&outBuf_, 0);
      writeBE16(&outBuf_, 0);
      writeCap(&outBuf_, rfb::tight13CapFileListData, rfb::tightVendorTight, rfb::sigTight13FileListData);
      writeCap(&outBuf_, rfb::tight13CapFileDownloadData, rfb::tightVendorTight, rfb::sigTight13FileDownloadData);
      writeCap(&outBuf_, rfb::tight13CapFileUploadCancel, rfb::tightVendorTight, rfb::sigTight13FileUploadCancel);
      writeCap(&outBuf_, rfb::tight13CapFileDownloadFailed, rfb::tightVendorTight, rfb::sigTight13FileDownloadFailed);

      writeCap(&outBuf_, rfb::tight13CapFileListReq, rfb::tightVendorTight, rfb::sigTight13FileListReq);
      writeCap(&outBuf_, rfb::tight13CapFileDownloadReq, rfb::tightVendorTight, rfb::sigTight13FileDownloadReq);
      writeCap(&outBuf_, rfb::tight13CapFileUploadReq, rfb::tightVendorTight, rfb::sigTight13FileUploadReq);
      writeCap(&outBuf_, rfb::tight13CapFileUploadData, rfb::tightVendorTight, rfb::sigTight13FileUploadData);
      writeCap(&outBuf_, rfb::tight13CapFileDownloadCancel, rfb::tightVendorTight, rfb::sigTight13FileDownloadCancel);
      writeCap(&outBuf_, rfb::tight13CapFileUploadFailed, rfb::tightVendorTight, rfb::sigTight13FileUploadFailed);
    }
  }

  void processInput()
  {
    for (;;) {
      if (state_ == State::Version) {
        if (inBuf_.size() < 12)
          return;
        inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 12);
        if (mode_ == MockServerMode::Tight2 || mode_ == MockServerMode::Tight13) {
          outBuf_.push_back(3);
          outBuf_.push_back(rfb::secTypeNone);
          outBuf_.push_back(rfb::secTypeVncAuth);
          outBuf_.push_back(rfb::secTypeTight);
        } else {
          outBuf_.push_back(1);
          outBuf_.push_back(rfb::secTypeNone);
        }
        state_ = State::SecChoice;
      } else if (state_ == State::SecChoice) {
        if (inBuf_.size() < 1)
          return;
        uint8_t sec = inBuf_[0];
        inBuf_.erase(inBuf_.begin());
        if (sec == rfb::secTypeTight) {
          writeBE32(&outBuf_, 1);
          writeCap(&outBuf_, rfb::tightTunnelNone, rfb::tightVendorTight, rfb::sigTightTunnelNone);
          state_ = State::TightTunnel;
        } else {
          writeBE32(&outBuf_, 0); // SecurityResult OK
          state_ = State::ClientInit;
        }
      } else if (state_ == State::TightTunnel) {
        if (inBuf_.size() < 4)
          return;
        inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 4);
        writeBE32(&outBuf_, 1);
        writeCap(&outBuf_, rfb::tightAuthNone, rfb::tightVendorStandard, rfb::sigTightAuthNone);
        state_ = State::TightAuth;
      } else if (state_ == State::TightAuth) {
        if (inBuf_.size() < 4)
          return;
        inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 4);
        writeBE32(&outBuf_, 0); // SecurityResult OK
        state_ = State::ClientInit;
      } else if (state_ == State::ClientInit) {
        if (inBuf_.size() < 1)
          return;
        inBuf_.erase(inBuf_.begin());
        sendServerInit();
        state_ = State::Normal;
      } else {
        if (!processNormalMsg())
          return;
      }
    }
  }

  static std::string parseTight2Str(const uint8_t* buf, size_t avail, size_t* consumed)
  {
    if (avail < 4)
      return "";
    uint32_t len = readBE32(buf);
    if (avail < 4 + len)
      return "";
    std::string res(reinterpret_cast<const char*>(buf + 4), len);
    while (!res.empty() && res.back() == '\0')
      res.pop_back();
    *consumed = 4 + len;
    return res;
  }

  std::string ultraWireToPath(const std::string& wire)
  {
    std::string p = wire;
    for (size_t i = 0; i < p.size(); i++) {
      if (p[i] == '\\')
        p[i] = '/';
    }
    return normPath("/" + p);
  }

  bool processNormalMsg()
  {
    if (inBuf_.empty())
      return false;
    uint8_t msg = inBuf_[0];

    switch (msg) {
    case rfb::msgTypeSetPixelFormat:
      if (inBuf_.size() < 20)
        return false;
      inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 20);
      return true;

    case rfb::msgTypeSetEncodings: {
      if (inBuf_.size() < 4)
        return false;
      uint16_t count = readBE16(inBuf_.data() + 2);
      if (inBuf_.size() < 4 + count * 4U)
        return false;
      bool sawUltra = false;
      for (uint16_t i = 0; i < count; i++) {
        uint32_t enc = readBE32(inBuf_.data() + 4 + i * 4U);
        if (enc == static_cast<uint32_t>(rfb::pseudoEncodingFTProtocolVersion))
          sawUltra = true;
      }
      inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 4 + count * 4U);
      if (mode_ == MockServerMode::Ultra && sawUltra && !sentUltraVer_) {
        sentUltraVer_ = true;
        sendUltra(rfb::ultraFileTransferProtocolVersion, 4, 8192, nullptr, 0);
      }
      return true;
    }

    case rfb::msgTypeFramebufferUpdateRequest:
      if (inBuf_.size() < 10)
        return false;
      inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 10);
      return true;

    case rfb::msgTypeKeyEvent:
      if (inBuf_.size() < 8)
        return false;
      inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 8);
      return true;

    case rfb::msgTypePointerEvent:
      if (inBuf_.size() < 6)
        return false;
      inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 6);
      return true;

    case rfb::msgTypeClientCutText: {
      if (inBuf_.size() < 8)
        return false;
      uint32_t len = readBE32(inBuf_.data() + 4);
      if (inBuf_.size() < 8 + len)
        return false;
      inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 8 + len);
      return true;
    }

    case rfb::msgTypeClientFence: {
      if (inBuf_.size() < 9)
        return false;
      uint32_t flags = readBE32(inBuf_.data() + 4);
      uint8_t plen = inBuf_[8];
      if (inBuf_.size() < 9U + plen)
        return false;
      if (flags & rfb::fenceFlagRequest) {
        outBuf_.push_back(rfb::msgTypeServerFence);
        outBuf_.push_back(0); outBuf_.push_back(0); outBuf_.push_back(0);
        writeBE32(&outBuf_, flags & ~rfb::fenceFlagRequest);
        outBuf_.push_back(plen);
        outBuf_.insert(outBuf_.end(), inBuf_.data() + 9,
                       inBuf_.data() + 9 + plen);
      }
      inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 9 + plen);
      return true;
    }

    case rfb::msgTypeTightExt:
      return processTight2Msg();

    case rfb::msgTypeTightFileListRequest: {
      if (inBuf_.size() < 4)
        return false;
      uint16_t dlen = readBE16(inBuf_.data() + 2);
      if (inBuf_.size() < 4U + dlen)
        return false;
      listReqCount_++;
      std::string dir(reinterpret_cast<const char*>(inBuf_.data() + 4), dlen);
      inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 4 + dlen);
      dir = normPath(dir);
      auto it = nodes_.find(dir);
      if (it == nodes_.end() || !it->second.isDir) {
        outBuf_.push_back(rfb::msgTypeTightFileListData);
        outBuf_.push_back(rfb::tight13FlagError);
        writeBE16(&outBuf_, 0);
        writeBE16(&outBuf_, 0);
        writeBE16(&outBuf_, 0);
        return true;
      }
      auto children = listChildren(dir);
      std::vector<uint8_t> names;
      for (auto& ch : children) {
        names.insert(names.end(), ch.first.begin(), ch.first.end());
        names.push_back('\0');
      }
      outBuf_.push_back(rfb::msgTypeTightFileListData);
      outBuf_.push_back(0);
      writeBE16(&outBuf_, static_cast<uint16_t>(children.size()));
      writeBE16(&outBuf_, static_cast<uint16_t>(names.size()));
      writeBE16(&outBuf_, static_cast<uint16_t>(names.size()));
      for (auto& ch : children) {
        writeBE32(&outBuf_, ch.second.isDir ? rfb::tight13DirSize
                                            : static_cast<uint32_t>(ch.second.data.size()));
        writeBE32(&outBuf_, static_cast<uint32_t>(ch.second.mtime));
      }
      outBuf_.insert(outBuf_.end(), names.begin(), names.end());
      return true;
    }

    case rfb::msgTypeTightFileDownloadRequest: {
      if (inBuf_.size() < 8)
        return false;
      uint16_t plen = readBE16(inBuf_.data() + 2);
      if (inBuf_.size() < 8U + plen)
        return false;
      uint32_t pos = readBE32(inBuf_.data() + 4);
      std::string path(reinterpret_cast<const char*>(inBuf_.data() + 8), plen);
      inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 8 + plen);
      path = normPath(path);
      auto it = nodes_.find(path);
      if (it == nodes_.end() || it->second.isDir) {
        const char* msgErr = "File not found";
        outBuf_.push_back(rfb::msgTypeTightFileDownloadFailed);
        outBuf_.push_back(0);
        writeBE16(&outBuf_, static_cast<uint16_t>(strlen(msgErr)));
        outBuf_.insert(outBuf_.end(), msgErr, msgErr + strlen(msgErr));
        return true;
      }
      const std::vector<uint8_t>& d = it->second.data;
      size_t off = std::min<size_t>(pos, d.size());
      while (off < d.size()) {
        size_t chunk = std::min<size_t>(d.size() - off, 32768);
        outBuf_.push_back(rfb::msgTypeTightFileDownloadData);
        outBuf_.push_back(0);
        writeBE16(&outBuf_, static_cast<uint16_t>(chunk));
        writeBE16(&outBuf_, static_cast<uint16_t>(chunk));
        outBuf_.insert(outBuf_.end(), d.data() + off, d.data() + off + chunk);
        off += chunk;
      }
      outBuf_.push_back(rfb::msgTypeTightFileDownloadData);
      outBuf_.push_back(0);
      writeBE16(&outBuf_, 0);
      writeBE16(&outBuf_, 0);
      writeBE32(&outBuf_, static_cast<uint32_t>(it->second.mtime));
      return true;
    }

    case rfb::msgTypeTightFileUploadRequest: {
      if (inBuf_.size() < 8)
        return false;
      uint16_t plen = readBE16(inBuf_.data() + 2);
      if (inBuf_.size() < 8U + plen)
        return false;
      std::string path(reinterpret_cast<const char*>(inBuf_.data() + 8), plen);
      inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 8 + plen);
      t13UploadPath_ = normPath(path);
      t13UploadBuf_.clear();
      return true;
    }

    case rfb::msgTypeTightFileUploadData: {
      if (inBuf_.size() < 6)
        return false;
      uint16_t realSz = readBE16(inBuf_.data() + 2);
      uint16_t compSz = readBE16(inBuf_.data() + 4);
      if (realSz == 0 && compSz == 0) {
        if (inBuf_.size() < 10)
          return false;
        inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 10);
        if (!t13UploadPath_.empty()) {
          nodes_[t13UploadPath_] = {false, t13UploadBuf_, 1700001000LL};
          t13UploadPath_.clear();
          t13UploadBuf_.clear();
        }
        return true;
      }
      if (inBuf_.size() < 6U + compSz)
        return false;
      t13UploadBuf_.insert(t13UploadBuf_.end(), inBuf_.data() + 6,
                           inBuf_.data() + 6 + compSz);
      inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 6 + compSz);
      return true;
    }

    case rfb::msgTypeTightFileDownloadCancel:
    case rfb::msgTypeTightFileUploadFailed: {
      if (inBuf_.size() < 4)
        return false;
      uint16_t rlen = readBE16(inBuf_.data() + 2);
      if (inBuf_.size() < 4U + rlen)
        return false;
      inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 4 + rlen);
      t13UploadPath_.clear();
      t13UploadBuf_.clear();
      return true;
    }

    case rfb::msgTypeTightFileCreateDirRequest: {
      if (inBuf_.size() < 4)
        return false;
      uint16_t plen = readBE16(inBuf_.data() + 2);
      if (inBuf_.size() < 4U + plen)
        return false;
      std::string dir(reinterpret_cast<const char*>(inBuf_.data() + 4), plen);
      inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 4 + plen);
      dir = normPath(dir);
      if (nodes_.find(dir) == nodes_.end())
        nodes_[dir] = {true, {}, 1700001000LL};
      return true;
    }

    case rfb::msgTypeFileTransfer:
      return processUltraMsg();

    default:
      inBuf_.erase(inBuf_.begin());
      return true;
    }
  }

  bool processTight2Msg()
  {
    if (inBuf_.size() < 4)
      return false;
    uint32_t msgId = readBE32(inBuf_.data());

    switch (msgId) {
    case rfb::tight2CompressionSupportReq:
      inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 4);
      writeBE32(&outBuf_, rfb::tight2CompressionSupportReply);
      outBuf_.push_back(1);
      return true;

    case rfb::tight2FileListReq: {
      if (inBuf_.size() < 9)
        return false;
      size_t consumed = 0;
      std::string dir = parseTight2Str(inBuf_.data() + 5, inBuf_.size() - 5, &consumed);
      if (consumed == 0)
        return false;
      uint8_t compLevel = inBuf_[4];
      inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 5 + consumed);
      listReqCount_++;
      dir = normPath(dir);
      auto it = nodes_.find(dir);
      if (it == nodes_.end() || !it->second.isDir) {
        sendTight2Failed("No such directory");
        return true;
      }
      auto children = listChildren(dir);
      std::vector<uint8_t> raw;
      writeBE32(&raw, static_cast<uint32_t>(children.size()));
      for (auto& ch : children) {
        writeBE64(&raw, ch.second.isDir ? 0 : ch.second.data.size());
        writeBE64(&raw, static_cast<uint64_t>(ch.second.mtime) * 1000ULL);
        writeBE16(&raw, ch.second.isDir ? rfb::tight2FlagDir : 0);
        writeBE32(&raw, static_cast<uint32_t>(ch.first.size()));
        raw.insert(raw.end(), ch.first.begin(), ch.first.end());
      }
      std::vector<uint8_t> packed = compLevel ? compressSync(raw.data(), raw.size()) : raw;
      writeBE32(&outBuf_, rfb::tight2FileListReply);
      outBuf_.push_back(compLevel ? 1 : 0);
      writeBE32(&outBuf_, static_cast<uint32_t>(packed.size()));
      writeBE32(&outBuf_, static_cast<uint32_t>(raw.size()));
      outBuf_.insert(outBuf_.end(), packed.begin(), packed.end());
      return true;
    }

    case rfb::tight2DownloadStartReq: {
      if (inBuf_.size() < 16)
        return false;
      size_t consumed = 0;
      std::string path = parseTight2Str(inBuf_.data() + 4, inBuf_.size() - 4, &consumed);
      if (consumed == 0 || inBuf_.size() < 4 + consumed + 8)
        return false;
      uint64_t offset = readBE64(inBuf_.data() + 4 + consumed);
      inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 4 + consumed + 8);
      path = normPath(path);
      auto it = nodes_.find(path);
      if (it == nodes_.end() || it->second.isDir) {
        sendTight2Failed("File not found");
        return true;
      }
      t2DlPath_ = path;
      t2DlOffset_ = static_cast<size_t>(std::min<uint64_t>(offset, it->second.data.size()));
      writeBE32(&outBuf_, rfb::tight2DownloadStartReply);
      return true;
    }

    case rfb::tight2DownloadDataReq: {
      if (inBuf_.size() < 9)
        return false;
      uint8_t compLevel = inBuf_[4];
      uint32_t reqSize = readBE32(inBuf_.data() + 5);
      inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 9);
      auto it = nodes_.find(t2DlPath_);
      if (it == nodes_.end()) {
        sendTight2Failed("Missing download file");
        return true;
      }
      const std::vector<uint8_t>& d = it->second.data;
      if (t2DlOffset_ >= d.size()) {
        writeBE32(&outBuf_, rfb::tight2DownloadEndReply);
        outBuf_.push_back(0);
        writeBE64(&outBuf_, static_cast<uint64_t>(it->second.mtime) * 1000ULL);
        return true;
      }
      size_t chunk = std::min<size_t>(reqSize, d.size() - t2DlOffset_);
      std::vector<uint8_t> raw(d.data() + t2DlOffset_, d.data() + t2DlOffset_ + chunk);
      t2DlOffset_ += chunk;
      std::vector<uint8_t> packed = compLevel ? compressSync(raw.data(), raw.size()) : raw;
      writeBE32(&outBuf_, rfb::tight2DownloadDataReply);
      outBuf_.push_back(compLevel ? 1 : 0);
      writeBE32(&outBuf_, static_cast<uint32_t>(packed.size()));
      writeBE32(&outBuf_, static_cast<uint32_t>(raw.size()));
      outBuf_.insert(outBuf_.end(), packed.begin(), packed.end());
      return true;
    }

    case rfb::tight2UploadStartReq: {
      if (inBuf_.size() < 17)
        return false;
      size_t consumed = 0;
      std::string path = parseTight2Str(inBuf_.data() + 4, inBuf_.size() - 4, &consumed);
      if (consumed == 0 || inBuf_.size() < 4 + consumed + 9)
        return false;
      uint64_t offset = readBE64(inBuf_.data() + 4 + consumed + 1);
      inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 4 + consumed + 9);
      path = normPath(path);
      t2UpPath_ = path;
      t2UpBuf_.clear();
      auto it = nodes_.find(path);
      if (offset > 0 && it != nodes_.end() && !it->second.isDir &&
          offset <= it->second.data.size()) {
        t2UpBuf_.assign(it->second.data.begin(),
                        it->second.data.begin() + static_cast<size_t>(offset));
      }
      writeBE32(&outBuf_, rfb::tight2UploadStartReply);
      return true;
    }

    case rfb::tight2UploadDataReq: {
      if (inBuf_.size() < 13)
        return false;
      uint8_t compLevel = inBuf_[4];
      uint32_t csize = readBE32(inBuf_.data() + 5);
      uint32_t usize = readBE32(inBuf_.data() + 9);
      if (inBuf_.size() < 13U + csize)
        return false;
      if (compLevel != 0 && csize != usize) {
        std::vector<uint8_t> dec = decompressSync(inBuf_.data() + 13, csize, usize);
        t2UpBuf_.insert(t2UpBuf_.end(), dec.begin(), dec.end());
      } else {
        t2UpBuf_.insert(t2UpBuf_.end(), inBuf_.data() + 13, inBuf_.data() + 13 + csize);
      }
      inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 13 + csize);
      writeBE32(&outBuf_, rfb::tight2UploadDataReply);
      return true;
    }

    case rfb::tight2UploadEndReq:
      if (inBuf_.size() < 14)
        return false;
      inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 14);
      nodes_[t2UpPath_] = {false, t2UpBuf_, 1700001000LL};
      t2UpPath_.clear();
      t2UpBuf_.clear();
      writeBE32(&outBuf_, rfb::tight2UploadEndReply);
      return true;

    case rfb::tight2MkdirReq: {
      if (inBuf_.size() < 8)
        return false;
      size_t consumed = 0;
      std::string dir = parseTight2Str(inBuf_.data() + 4, inBuf_.size() - 4, &consumed);
      if (consumed == 0)
        return false;
      inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 4 + consumed);
      dir = normPath(dir);
      if (nodes_.find(dir) != nodes_.end()) {
        sendTight2Failed("Already exists");
      } else {
        nodes_[dir] = {true, {}, 1700001000LL};
        writeBE32(&outBuf_, rfb::tight2MkdirReply);
      }
      return true;
    }

    case rfb::tight2RemoveReq: {
      if (inBuf_.size() < 8)
        return false;
      size_t consumed = 0;
      std::string path = parseTight2Str(inBuf_.data() + 4, inBuf_.size() - 4, &consumed);
      if (consumed == 0)
        return false;
      inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 4 + consumed);
      path = normPath(path);
      if (nodes_.erase(path) == 0)
        sendTight2Failed("Not found");
      else
        writeBE32(&outBuf_, rfb::tight2RemoveReply);
      return true;
    }

    case rfb::tight2RenameReq: {
      if (inBuf_.size() < 12)
        return false;
      size_t c1 = 0, c2 = 0;
      std::string fromP = parseTight2Str(inBuf_.data() + 4, inBuf_.size() - 4, &c1);
      if (c1 == 0)
        return false;
      std::string toP = parseTight2Str(inBuf_.data() + 4 + c1, inBuf_.size() - 4 - c1, &c2);
      if (c2 == 0)
        return false;
      inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 4 + c1 + c2);
      fromP = normPath(fromP);
      toP = normPath(toP);
      auto it = nodes_.find(fromP);
      if (it == nodes_.end()) {
        sendTight2Failed("Source not found");
      } else {
        nodes_[toP] = it->second;
        nodes_.erase(it);
        writeBE32(&outBuf_, rfb::tight2RenameReply);
      }
      return true;
    }

    case rfb::tight2Md5Req: {
      if (inBuf_.size() < 8)
        return false;
      size_t consumed = 0;
      parseTight2Str(inBuf_.data() + 4, inBuf_.size() - 4, &consumed);
      if (consumed == 0)
        return false;
      inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 4 + consumed);
      writeBE32(&outBuf_, rfb::tight2Md5Reply);
      for (int i = 0; i < 16; i++)
        outBuf_.push_back(0);
      return true;
    }

    case rfb::tight2DirSizeReq: {
      if (inBuf_.size() < 8)
        return false;
      size_t consumed = 0;
      parseTight2Str(inBuf_.data() + 4, inBuf_.size() - 4, &consumed);
      if (consumed == 0)
        return false;
      inBuf_.erase(inBuf_.begin(), inBuf_.begin() + 4 + consumed);
      writeBE32(&outBuf_, rfb::tight2DirSizeReply);
      writeBE64(&outBuf_, 0);
      return true;
    }

    default:
      inBuf_.erase(inBuf_.begin());
      return true;
    }
  }

  bool processUltraMsg()
  {
    if (inBuf_.size() < 12)
      return false;
    uint8_t contentType = inBuf_[1];
    uint16_t param = static_cast<uint16_t>(inBuf_[2] | (inBuf_[3] << 8));
    uint32_t length = readBE32(inBuf_.data() + 8);
    bool hasSizeH = (contentType == rfb::ultraFileTransferOffer);
    size_t total = 12U + length + (hasSizeH ? 4U : 0U);
    if (inBuf_.size() < total)
      return false;

    std::string payload(reinterpret_cast<const char*>(inBuf_.data() + 12), length);
    inBuf_.erase(inBuf_.begin(), inBuf_.begin() + total);

    switch (contentType) {
    case rfb::ultraAbortFileTransfer:
      if (!ultraProbeDone_) {
        ultraProbeDone_ = true;
        sendUltra(rfb::ultraFileTransferAccess, 0, 1, nullptr, 0);
      }
      return true;

    case rfb::ultraDirContentRequest: {
      listReqCount_++;
      if ((param & 0xFF) == rfb::ultraRDrivesList) {
        const char drives[] = { 'C', ':', '\\', '\0', '\0' };
        sendUltra(rfb::ultraDirPacket, rfb::ultraADrivesList, 0,
                  reinterpret_cast<const uint8_t*>(drives), sizeof(drives));
        return true;
      }
      std::string dir = ultraWireToPath(payload);
      auto it = nodes_.find(dir);
      if (it == nodes_.end() || !it->second.isDir) {
        sendUltra(rfb::ultraDirPacket, rfb::ultraADirectory, 0, nullptr, 0);
        return true;
      }
      sendUltra(rfb::ultraDirPacket, rfb::ultraADirectory, 0,
                reinterpret_cast<const uint8_t*>(payload.data()),
                static_cast<uint32_t>(payload.size()));
      auto children = listChildren(dir);
      for (auto& ch : children) {
        std::vector<uint8_t> wfd(44, 0);
        if (ch.second.isDir)
          wfd[0] = rfb::ultraFileAttrDir;
        uint32_t szLo = static_cast<uint32_t>(ch.second.data.size() & 0xFFFFFFFFU);
        wfd[32] = static_cast<uint8_t>(szLo & 0xFF);
        wfd[33] = static_cast<uint8_t>((szLo >> 8) & 0xFF);
        wfd[34] = static_cast<uint8_t>((szLo >> 16) & 0xFF);
        wfd[35] = static_cast<uint8_t>((szLo >> 24) & 0xFF);
        uint32_t tLo = static_cast<uint32_t>(ch.second.mtime);
        wfd[20] = static_cast<uint8_t>(tLo & 0xFF);
        wfd[21] = static_cast<uint8_t>((tLo >> 8) & 0xFF);
        wfd[22] = static_cast<uint8_t>((tLo >> 16) & 0xFF);
        wfd[23] = static_cast<uint8_t>((tLo >> 24) & 0xFF);
        for (size_t i = 0; i < ch.first.size(); i++)
          writeLE16(&wfd, static_cast<uint8_t>(ch.first[i]));
        writeLE16(&wfd, 0);
        uint16_t p = rfb::ultraADirUnicode |
                     (ch.second.isDir ? rfb::ultraADirectory : rfb::ultraAFile);
        sendUltra(rfb::ultraDirPacket, p, 0, wfd.data(),
                  static_cast<uint32_t>(wfd.size()));
      }
      sendUltra(rfb::ultraDirPacket, 0, 0, nullptr, 0);
      return true;
    }

    case rfb::ultraFileTransferRequest: {
      std::string path = ultraWireToPath(payload);
      auto it = nodes_.find(path);
      if (it == nodes_.end() || it->second.isDir) {
        sendUltra(rfb::ultraFileHeader, 0, rfb::ultraError, nullptr, 0,
                  true, rfb::ultraError);
        return true;
      }
      ultraDlPath_ = path;
      sendUltra(rfb::ultraFileHeader, 0,
                static_cast<uint32_t>(it->second.data.size()),
                nullptr, 0, true, 0);
      return true;
    }

    case rfb::ultraFileHeader: {
      auto it = nodes_.find(ultraDlPath_);
      if (it != nodes_.end()) {
        const std::vector<uint8_t>& d = it->second.data;
        if (!d.empty()) {
          sendUltra(rfb::ultraFilePacket, 0, 0, d.data(),
                    static_cast<uint32_t>(d.size()));
        }
        sendUltra(rfb::ultraEndOfFile, 0, 0, nullptr, 0);
      }
      return true;
    }

    case rfb::ultraFileTransferOffer: {
      size_t comma = payload.find(',');
      std::string filePart = (comma == std::string::npos) ? payload : payload.substr(0, comma);
      ultraUpPath_ = ultraWireToPath(filePart);
      ultraUpBuf_.clear();
      sendUltra(rfb::ultraFileAcceptHeader, 0, 0, nullptr, 0);
      return true;
    }

    case rfb::ultraFilePacket:
      ultraUpBuf_.insert(ultraUpBuf_.end(), payload.begin(), payload.end());
      return true;

    case rfb::ultraEndOfFile:
      nodes_[ultraUpPath_] = {false, ultraUpBuf_, 1700001000LL};
      ultraUpPath_.clear();
      ultraUpBuf_.clear();
      return true;

    case rfb::ultraCommand: {
      bool ok = true;
      if (param == rfb::ultraCDirCreate) {
        std::string p = ultraWireToPath(payload);
        if (nodes_.find(p) != nodes_.end())
          ok = false;
        else
          nodes_[p] = {true, {}, 1700001000LL};
      } else if (param == rfb::ultraCFileDelete || param == rfb::ultraCDirDelete) {
        std::string p = ultraWireToPath(payload);
        ok = (nodes_.erase(p) > 0);
      } else if (param == rfb::ultraCFileRename || param == rfb::ultraCDirRename) {
        size_t star = payload.find('*');
        if (star == std::string::npos) {
          ok = false;
        } else {
          std::string src = ultraWireToPath(payload.substr(0, star));
          std::string dst = ultraWireToPath(payload.substr(star + 1));
          auto it = nodes_.find(src);
          if (it == nodes_.end()) {
            ok = false;
          } else {
            nodes_[dst] = it->second;
            nodes_.erase(it);
          }
        }
      }
      sendUltra(rfb::ultraCommandReturn, 0, ok ? 0 : rfb::ultraError,
                reinterpret_cast<const uint8_t*>(payload.data()),
                static_cast<uint32_t>(payload.size()));
      return true;
    }

    default:
      return true;
    }
  }

  MockServerMode mode_;
  int listenFd_;
  int clientFd_;
  int listenPort_;
  State state_;
  std::vector<uint8_t> inBuf_;
  std::vector<uint8_t> outBuf_;
  std::map<std::string, Node> nodes_;

  std::string t2DlPath_;
  size_t t2DlOffset_ = 0;
  std::string t2UpPath_;
  std::vector<uint8_t> t2UpBuf_;

  std::string t13UploadPath_;
  std::vector<uint8_t> t13UploadBuf_;

  bool sentUltraVer_ = false;
  bool ultraProbeDone_ = false;
  std::string ultraDlPath_;
  std::string ultraUpPath_;
  std::vector<uint8_t> ultraUpBuf_;

  int listReqCount_ = 0;
};

class PollFtpLoop : public FtpEventLoop {
public:
  struct Watch {
    bool read;
    bool write;
    FdCallback cb;
    void* data;
  };

  struct TimerEntry {
    std::chrono::steady_clock::time_point due;
    TimerCallback cb;
    void* data;
  };

  void setFdWatch(int fd, bool read, bool write,
                  FdCallback cb, void* data) override
  {
    if (!read && !write) {
      watches_.erase(fd);
      return;
    }
    watches_[fd] = {read, write, cb, data};
  }

  void removeFd(int fd) override
  {
    watches_.erase(fd);
  }

  void addTimer(int delayMs, TimerCallback cb, void* data) override
  {
    removeTimer(cb, data);
    timers_.push_back({std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(delayMs), cb, data});
  }

  void removeTimer(TimerCallback cb, void* data) override
  {
    timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                                 [&](const TimerEntry& t) {
                                   return t.cb == cb && t.data == data;
                                 }),
                  timers_.end());
  }

  const std::map<int, Watch>& watches() const { return watches_; }

  void fireDueTimers()
  {
    auto now = std::chrono::steady_clock::now();
    std::vector<TimerEntry> due;
    for (auto it = timers_.begin(); it != timers_.end(); ) {
      if (it->due <= now) {
        due.push_back(*it);
        it = timers_.erase(it);
      } else {
        ++it;
      }
    }
    for (auto& t : due)
      t.cb(t.data);
  }

private:
  std::map<int, Watch> watches_;
  std::vector<TimerEntry> timers_;
};

class BridgeCConn : public rfb::CConnection, public FtpBackend {
public:
  BridgeCConn()
    : viewOnly_(false), sock_(nullptr)
  {
    supportsFileTransfer = true;
  }

  void attachSocket(network::TcpSocket* sock)
  {
    sock_ = sock;
    setStreams(&sock->inStream(), &sock->outStream());
  }

  void setViewOnly(bool vo) { viewOnly_ = vo; }

  void initDone() override
  {
    setFramebuffer(new rfb::ManagedPixelBuffer(rfb::PixelFormat(),
                                               server.width(),
                                               server.height()));
  }
  void bell() override {}
  void getUserPasswd(std::string* u, std::string* p) override
  {
    if (u != nullptr) *u = "u";
    if (p != nullptr) *p = "p";
  }
  bool verifyCertificate(unsigned int, const uint8_t*, size_t) override
  {
    return true;
  }
  bool verifyHostKey(const uint8_t*, size_t, const char*) override
  {
    return true;
  }

  bool isViewOnly() const override { return viewOnly_.load(); }
  bool isConnected() const override
  {
    return const_cast<BridgeCConn*>(this)->state() ==
           rfb::CConnection::RFBSTATE_NORMAL;
  }
  rfb::FileTransferClient* fileTransfer() override
  {
    return &fileTransferClient();
  }
  void flushOutput() override
  {
    if (sock_ != nullptr)
      sock_->outStream().flush();
  }
  bool isOutputCongested() const override
  {
    if (sock_ == nullptr)
      return false;
    rdr::BufferedOutStream* bos =
      dynamic_cast<rdr::BufferedOutStream*>(&sock_->outStream());
    return bos != nullptr && bos->bufferedSize() >= 262144;
  }

private:
  std::atomic<bool> viewOnly_;
  network::TcpSocket* sock_;
};

class BridgeHarness {
public:
  explicit BridgeHarness(MockServerMode mode,
                         const std::string& user = "",
                         const std::string& pass = "")
    : mode_(mode), mock_(mode), user_(user), pass_(pass), stop_(false),
      ftpPort_(0)
  {
    EXPECT_EQ(::pipe(wakePipe_), 0);
    for (int i = 0; i < 2; i++) {
      int fl = ::fcntl(wakePipe_[i], F_GETFL, 0);
      ::fcntl(wakePipe_[i], F_SETFL, fl | O_NONBLOCK);
    }
  }

  ~BridgeHarness()
  {
    stop_.store(true);
    wake();
    if (worker_.joinable())
      worker_.join();
    ftp_.reset();
    if (conn_ != nullptr) {
      conn_->close();
      conn_.reset();
    }
    sock_.reset();
    ::close(wakePipe_[0]);
    ::close(wakePipe_[1]);
  }

  bool start()
  {
    if (!mock_.start())
      return false;

    sock_.reset(new network::TcpSocket("127.0.0.1", mock_.port()));
    conn_.reset(new BridgeCConn());
    conn_->setServerName("127.0.0.1");
    conn_->attachSocket(sock_.get());
    conn_->initialiseProtocol();

    ftp_.reset(new FtpServer(&loop_, conn_.get()));
    FtpServerConfig cfg;
    cfg.bindAddress = "127.0.0.1";
    cfg.port = 0;
    cfg.username = user_;
    cfg.password = pass_;
    std::string err;
    if (!ftp_->start(cfg, &err))
      return false;
    ftpPort_ = ftp_->listenPort();

    worker_ = std::thread([this]() { runLoop(); });

    std::unique_lock<std::mutex> lk(mu_);
    bool ok = readyCv_.wait_for(lk, std::chrono::seconds(5), [this]() {
      if (mode_ == MockServerMode::NoFt)
        return conn_->isConnected();
      return conn_->isConnected() && conn_->fileTransfer()->isSupported();
    });
    return ok;
  }

  int ftpPort() const { return ftpPort_; }
  void setViewOnly(bool vo) { conn_->setViewOnly(vo); }

  bool getMockFile(const std::string& path, std::vector<uint8_t>* out)
  {
    std::lock_guard<std::mutex> lk(mu_);
    return mock_.getFile(path, out);
  }

  bool hasMockPath(const std::string& path)
  {
    std::lock_guard<std::mutex> lk(mu_);
    return mock_.hasPath(path);
  }

  int mockListCount()
  {
    std::lock_guard<std::mutex> lk(mu_);
    return mock_.listReqCount();
  }

private:
  void wake()
  {
    char b = 1;
    ssize_t unused = ::write(wakePipe_[1], &b, 1);
    (void)unused;
  }

  void runLoop()
  {
    while (!stop_.load()) {
      std::vector<struct pollfd> pfds;
      std::vector<int> ftpWatchedFds;
      {
        std::lock_guard<std::mutex> lk(mu_);
        pfds.push_back({wakePipe_[0], POLLIN, 0});
        pfds.push_back({mock_.listenFd(), POLLIN, 0});
        if (mock_.clientFd() >= 0) {
          short ev = POLLIN;
          if (mock_.hasPendingWrite())
            ev |= POLLOUT;
          pfds.push_back({mock_.clientFd(), ev, 0});
        }
        if (sock_ && sock_->getFd() >= 0) {
          short ev = POLLIN;
          if (sock_->outStream().hasBufferedData())
            ev |= POLLOUT;
          pfds.push_back({sock_->getFd(), ev, 0});
        }
        for (const auto& kv : loop_.watches()) {
          short ev = 0;
          if (kv.second.read) ev |= POLLIN;
          if (kv.second.write) ev |= POLLOUT;
          if (ev != 0) {
            pfds.push_back({kv.first, ev, 0});
            ftpWatchedFds.push_back(kv.first);
          }
        }
      }

      int rc = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 10);
      if (stop_.load())
        break;

      std::lock_guard<std::mutex> lk(mu_);
      if (rc > 0) {
        if (pfds[0].revents & POLLIN) {
          char dump[64];
          while (::read(wakePipe_[0], dump, sizeof(dump)) > 0) {}
        }
        if (pfds[1].revents & POLLIN)
          mock_.acceptIfNeeded();

        size_t idx = 2;
        if (mock_.clientFd() >= 0 && idx < pfds.size() &&
            pfds[idx].fd == mock_.clientFd()) {
          mock_.handleIo((pfds[idx].revents & (POLLIN | POLLHUP | POLLERR)) != 0,
                         (pfds[idx].revents & POLLOUT) != 0);
          idx++;
        }

        if (sock_ && idx < pfds.size() && pfds[idx].fd == sock_->getFd()) {
          if (pfds[idx].revents & POLLOUT) {
            sock_->outStream().flush();
            ftp_->onBackendOutputDrained();
          }
          if (pfds[idx].revents & (POLLIN | POLLHUP)) {
            int guard = 0;
            while (guard++ < 32 && conn_->processMsg()) {}
            sock_->outStream().flush();
          }
          idx++;
        }

        for (; idx < pfds.size(); idx++) {
          short rev = pfds[idx].revents;
          if (rev == 0)
            continue;
          int fd = pfds[idx].fd;
          auto it = loop_.watches().find(fd);
          if (it != loop_.watches().end()) {
            PollFtpLoop::Watch w = it->second;
            w.cb(fd,
                 (rev & (POLLIN | POLLHUP | POLLERR)) != 0,
                 (rev & (POLLOUT | POLLERR)) != 0,
                 w.data);
          }
        }
      }

      loop_.fireDueTimers();
      mock_.acceptIfNeeded();
      if (mock_.clientFd() >= 0 && mock_.hasPendingWrite())
        mock_.handleIo(false, true);
      if (sock_ && sock_->outStream().hasBufferedData()) {
        sock_->outStream().flush();
        ftp_->onBackendOutputDrained();
      }
      if ((mode_ == MockServerMode::NoFt && conn_->isConnected()) ||
          (conn_->isConnected() && conn_->fileTransfer()->isSupported())) {
        readyCv_.notify_all();
      }
    }
  }

  MockServerMode mode_;
  MockVncFileServer mock_;
  std::string user_;
  std::string pass_;
  int wakePipe_[2];
  std::atomic<bool> stop_;
  int ftpPort_;

  std::mutex mu_;
  std::condition_variable readyCv_;
  PollFtpLoop loop_;
  std::unique_ptr<network::TcpSocket> sock_;
  std::unique_ptr<BridgeCConn> conn_;
  std::unique_ptr<FtpServer> ftp_;
  std::thread worker_;
};

class FtpClientHelper {
public:
  FtpClientHelper() : ctrlFd_(-1) {}
  ~FtpClientHelper() { close(); }

  bool connect(int port)
  {
    close();
    ctrlFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (ctrlFd_ < 0)
      return false;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(static_cast<uint16_t>(port));
    if (::connect(ctrlFd_, reinterpret_cast<struct sockaddr*>(&sa),
                  sizeof(sa)) != 0) {
      close();
      return false;
    }
    return true;
  }

  void close()
  {
    if (ctrlFd_ >= 0) {
      ::close(ctrlFd_);
      ctrlFd_ = -1;
    }
  }

  std::string readLine()
  {
    for (;;) {
      size_t nl = buf_.find('\n');
      if (nl != std::string::npos) {
        std::string line = buf_.substr(0, nl);
        buf_.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        return line;
      }
      char tmp[1024];
      ssize_t n = ::recv(ctrlFd_, tmp, sizeof(tmp), 0);
      if (n <= 0)
        return "";
      buf_.append(tmp, static_cast<size_t>(n));
    }
  }

  std::pair<int, std::string> readReply()
  {
    std::string first = readLine();
    if (first.size() < 4)
      return {0, first};
    int code = std::atoi(first.substr(0, 3).c_str());
    if (first[3] != '-')
      return {code, first};
    std::string prefix = first.substr(0, 3) + " ";
    std::string full = first + "\n";
    for (;;) {
      std::string next = readLine();
      full += next + "\n";
      if (next.empty() || next.compare(0, 4, prefix) == 0)
        break;
    }
    return {code, full};
  }

  std::pair<int, std::string> cmd(const std::string& line)
  {
    sendRaw(line + "\r\n");
    return readReply();
  }

  void sendRaw(const std::string& line)
  {
    size_t off = 0;
    while (off < line.size()) {
      ssize_t n = ::send(ctrlFd_, line.data() + off, line.size() - off, 0);
      if (n <= 0)
        break;
      off += static_cast<size_t>(n);
    }
  }

  int openPasv()
  {
    auto rep = cmd("PASV");
    if (rep.first != 227)
      return -1;
    size_t lp = rep.second.find('(');
    size_t rp = rep.second.find(')', lp);
    if (lp == std::string::npos || rp == std::string::npos)
      return -1;
    std::string host;
    int port = 0;
    if (!ftp::parsePortArg(rep.second.substr(lp + 1, rp - lp - 1), &host, &port))
      return -1;
    int dfd = ::socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(static_cast<uint16_t>(port));
    if (::connect(dfd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) != 0) {
      ::close(dfd);
      return -1;
    }
    return dfd;
  }

  int openEpsv()
  {
    auto rep = cmd("EPSV");
    if (rep.first != 229)
      return -1;
    size_t p1 = rep.second.find("|||");
    if (p1 == std::string::npos)
      return -1;
    p1 += 3;
    size_t p2 = rep.second.find('|', p1);
    if (p2 == std::string::npos)
      return -1;
    int port = std::atoi(rep.second.substr(p1, p2 - p1).c_str());
    int dfd = ::socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(static_cast<uint16_t>(port));
    if (::connect(dfd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) != 0) {
      ::close(dfd);
      return -1;
    }
    return dfd;
  }

  int setupPortListener(bool useEprt)
  {
    int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    EXPECT_EQ(::bind(lfd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)), 0);
    EXPECT_EQ(::listen(lfd, 1), 0);
    socklen_t len = sizeof(sa);
    EXPECT_EQ(::getsockname(lfd, reinterpret_cast<struct sockaddr*>(&sa), &len), 0);
    int port = ntohs(sa.sin_port);
    if (useEprt) {
      auto rep = cmd("EPRT |1|127.0.0.1|" + std::to_string(port) + "|");
      EXPECT_EQ(rep.first, 200);
    } else {
      auto rep = cmd("PORT 127,0,0,1," + std::to_string(port >> 8) + "," +
                     std::to_string(port & 0xFF));
      EXPECT_EQ(rep.first, 200);
    }
    return lfd;
  }

  static std::vector<uint8_t> readAllData(int dfd)
  {
    std::vector<uint8_t> out;
    uint8_t buf[16384];
    for (;;) {
      ssize_t n = ::recv(dfd, buf, sizeof(buf), 0);
      if (n <= 0)
        break;
      out.insert(out.end(), buf, buf + n);
    }
    ::close(dfd);
    return out;
  }

  static void sendAllData(int dfd, const uint8_t* data, size_t len)
  {
    size_t off = 0;
    while (off < len) {
      ssize_t n = ::send(dfd, data + off, len - off, 0);
      if (n <= 0)
        break;
      off += static_cast<size_t>(n);
    }
    ::close(dfd);
  }

private:
  int ctrlFd_;
  std::string buf_;
};

} // namespace

TEST(FtpBridgeIntegration, Tight2_FullWorkflowAndConcurrency)
{
  BridgeHarness h(MockServerMode::Tight2, "admin", "pw123");
  ASSERT_TRUE(h.start());

  FtpClientHelper cli;
  ASSERT_TRUE(cli.connect(h.ftpPort()));
  EXPECT_EQ(cli.readReply().first, 220);

  // Auth enforcement
  EXPECT_EQ(cli.cmd("PWD").first, 530);
  EXPECT_EQ(cli.cmd("USER wrong").first, 331);
  EXPECT_EQ(cli.cmd("PASS wrong").first, 530);
  EXPECT_EQ(cli.cmd("USER admin").first, 331);
  EXPECT_EQ(cli.cmd("PASS pw123").first, 230);

  // Session commands
  EXPECT_EQ(cli.cmd("AUTH TLS").first, 502);
  EXPECT_EQ(cli.cmd("SYST").first, 215);
  auto feat = cli.cmd("FEAT");
  EXPECT_EQ(feat.first, 211);
  EXPECT_NE(feat.second.find("MLSD"), std::string::npos);
  EXPECT_EQ(cli.cmd("CLNT FileZilla").first, 200);
  EXPECT_EQ(cli.cmd("OPTS UTF8 ON").first, 200);
  EXPECT_NE(cli.cmd("PWD").second.find("\"/\""), std::string::npos);
  EXPECT_EQ(cli.cmd("TYPE I").first, 200);

  // PASV + MLSD
  {
    int dfd = cli.openPasv();
    ASSERT_GE(dfd, 0);
    EXPECT_EQ(cli.cmd("MLSD").first, 150);
    auto data = FtpClientHelper::readAllData(dfd);
    EXPECT_EQ(cli.readReply().first, 226);
    std::string listing(data.begin(), data.end());
    EXPECT_NE(listing.find("type=dir;"), std::string::npos);
    EXPECT_NE(listing.find("hello.txt"), std::string::npos);
    EXPECT_NE(listing.find("big.bin"), std::string::npos);
  }

  // CWD cache hit check + MLST + SIZE + MDTM
  int listBefore = h.mockListCount();
  EXPECT_EQ(cli.cmd("CWD /sub").first, 250);
  EXPECT_NE(cli.cmd("PWD").second.find("\"/sub\""), std::string::npos);
  EXPECT_EQ(cli.cmd("CDUP").first, 250);
  EXPECT_NE(cli.cmd("PWD").second.find("\"/\""), std::string::npos);
  EXPECT_EQ(cli.cmd("CWD /no_such_dir").first, 550);

  auto sz = cli.cmd("SIZE /hello.txt");
  EXPECT_EQ(sz.first, 213);
  EXPECT_EQ(sz.second, "213 27");
  auto md = cli.cmd("MDTM /hello.txt");
  EXPECT_EQ(md.first, 213);
  auto ml = cli.cmd("MLST /hello.txt");
  EXPECT_EQ(ml.first, 250);
  EXPECT_NE(ml.second.find("type=file;size=27;"), std::string::npos);
  EXPECT_EQ(h.mockListCount(), listBefore); // All hit cached "/"
  EXPECT_EQ(cli.cmd("CWD /sub/no_such_dir").first, 550);
  EXPECT_EQ(h.mockListCount(), listBefore + 1); // Uncached "/sub" missed cache

  // EPSV + LIST -la
  {
    int dfd = cli.openEpsv();
    ASSERT_GE(dfd, 0);
    EXPECT_EQ(cli.cmd("LIST -la /sub").first, 150);
    auto data = FtpClientHelper::readAllData(dfd);
    EXPECT_EQ(cli.readReply().first, 226);
    std::string listing(data.begin(), data.end());
    EXPECT_NE(listing.find("nested.txt"), std::string::npos);
  }

  // PORT + NLST & EPRT + MLSD
  {
    int lfd = cli.setupPortListener(false);
    EXPECT_EQ(cli.cmd("NLST /").first, 150);
    int dfd = ::accept(lfd, nullptr, nullptr);
    ::close(lfd);
    ASSERT_GE(dfd, 0);
    auto data = FtpClientHelper::readAllData(dfd);
    EXPECT_EQ(cli.readReply().first, 226);
    std::string names(data.begin(), data.end());
    EXPECT_NE(names.find("hello.txt"), std::string::npos);

    int lfd2 = cli.setupPortListener(true);
    EXPECT_EQ(cli.cmd("MLSD /sub").first, 150);
    int dfd2 = ::accept(lfd2, nullptr, nullptr);
    ::close(lfd2);
    ASSERT_GE(dfd2, 0);
    FtpClientHelper::readAllData(dfd2);
    EXPECT_EQ(cli.readReply().first, 226);
  }

  // MKD / STOR (> 200 KiB) / RETR / REST / APPE / RNFR-RNTO / DELE / RMD
  EXPECT_EQ(cli.cmd("MKD /work").first, 257);

  std::vector<uint8_t> payload(220000);
  for (size_t i = 0; i < payload.size(); i++)
    payload[i] = static_cast<uint8_t>((i * 19 + 5) & 0xFF);
  {
    int dfd = cli.openPasv();
    ASSERT_GE(dfd, 0);
    EXPECT_EQ(cli.cmd("STOR /work/up.bin").first, 150);
    FtpClientHelper::sendAllData(dfd, payload.data(), payload.size());
    EXPECT_EQ(cli.readReply().first, 226);
  }

  std::vector<uint8_t> stored;
  ASSERT_TRUE(h.getMockFile("/work/up.bin", &stored));
  EXPECT_EQ(stored, payload);

  // RETR full
  {
    int dfd = cli.openPasv();
    ASSERT_GE(dfd, 0);
    EXPECT_EQ(cli.cmd("RETR /work/up.bin").first, 150);
    auto got = FtpClientHelper::readAllData(dfd);
    EXPECT_EQ(cli.readReply().first, 226);
    EXPECT_EQ(got, payload);
  }

  // REST + RETR
  {
    EXPECT_EQ(cli.cmd("REST 1000").first, 350);
    int dfd = cli.openPasv();
    ASSERT_GE(dfd, 0);
    EXPECT_EQ(cli.cmd("RETR /work/up.bin").first, 150);
    auto tail = FtpClientHelper::readAllData(dfd);
    EXPECT_EQ(cli.readReply().first, 226);
    std::vector<uint8_t> expected(payload.begin() + 1000, payload.end());
    EXPECT_EQ(tail, expected);
  }

  // APPE append
  {
    const std::string extra = "APPENDED";
    int dfd = cli.openPasv();
    ASSERT_GE(dfd, 0);
    EXPECT_EQ(cli.cmd("APPE /sub/nested.txt").first, 150);
    FtpClientHelper::sendAllData(dfd,
                                 reinterpret_cast<const uint8_t*>(extra.data()),
                                 extra.size());
    EXPECT_EQ(cli.readReply().first, 226);
    std::vector<uint8_t> updated;
    ASSERT_TRUE(h.getMockFile("/sub/nested.txt", &updated));
    EXPECT_EQ(std::string(updated.begin(), updated.end()),
              "Nested content 1234567890APPENDED");
  }

  // Zero-byte STOR & RETR
  {
    int dfd = cli.openPasv();
    ASSERT_GE(dfd, 0);
    EXPECT_EQ(cli.cmd("STOR /work/zero.txt").first, 150);
    FtpClientHelper::sendAllData(dfd, nullptr, 0);
    EXPECT_EQ(cli.readReply().first, 226);

    int dfd2 = cli.openPasv();
    ASSERT_GE(dfd2, 0);
    EXPECT_EQ(cli.cmd("RETR /work/zero.txt").first, 150);
    auto z = FtpClientHelper::readAllData(dfd2);
    EXPECT_EQ(cli.readReply().first, 226);
    EXPECT_TRUE(z.empty());
  }

  // Rename / Delete / Rmdir
  EXPECT_EQ(cli.cmd("RNFR /work/zero.txt").first, 350);
  EXPECT_EQ(cli.cmd("RNTO /work/renamed.txt").first, 250);
  EXPECT_FALSE(h.hasMockPath("/work/zero.txt"));
  EXPECT_TRUE(h.hasMockPath("/work/renamed.txt"));
  EXPECT_EQ(cli.cmd("DELE /work/renamed.txt").first, 250);
  EXPECT_EQ(cli.cmd("DELE /work/up.bin").first, 250);
  EXPECT_EQ(cli.cmd("RMD /work").first, 250);

  // Concurrent second session (FileZilla browsing + transfer queuing)
  FtpClientHelper cli2;
  ASSERT_TRUE(cli2.connect(h.ftpPort()));
  EXPECT_EQ(cli2.readReply().first, 220);
  EXPECT_EQ(cli2.cmd("USER admin").first, 331);
  EXPECT_EQ(cli2.cmd("PASS pw123").first, 230);

  {
    int d1 = cli.openPasv();
    int d2 = cli2.openPasv();
    ASSERT_GE(d1, 0);
    ASSERT_GE(d2, 0);
    cli.sendRaw("RETR /big.bin\r\n");
    cli2.sendRaw("LIST /\r\n");

    EXPECT_EQ(cli.readReply().first, 150);
    auto bigGot = FtpClientHelper::readAllData(d1);
    EXPECT_EQ(cli.readReply().first, 226);
    std::vector<uint8_t> bigExpected;
    ASSERT_TRUE(h.getMockFile("/big.bin", &bigExpected));
    EXPECT_EQ(bigGot, bigExpected);

    EXPECT_EQ(cli2.readReply().first, 150);
    auto list2 = FtpClientHelper::readAllData(d2);
    EXPECT_EQ(cli2.readReply().first, 226);
    std::string s2(list2.begin(), list2.end());
    EXPECT_NE(s2.find("big.bin"), std::string::npos);
  }

  // ABOR mid-download then clean reuse
  {
    int dfd = cli.openPasv();
    ASSERT_GE(dfd, 0);
    EXPECT_EQ(cli.cmd("RETR /big.bin").first, 150);
    char firstChunk[1024];
    EXPECT_GT(::recv(dfd, firstChunk, sizeof(firstChunk), 0), 0);
    cli.sendRaw("ABOR\r\n");
    auto r1 = cli.readReply();
    EXPECT_TRUE(r1.first == 426 || r1.first == 226);
    if (r1.first == 426) {
      auto r2 = cli.readReply();
      EXPECT_EQ(r2.first, 226);
    }
    ::close(dfd);
    EXPECT_EQ(cli.cmd("PWD").first, 257);
  }

  // ViewOnly blocks writes while allowing reads
  h.setViewOnly(true);
  EXPECT_EQ(cli.cmd("MKD /blocked").first, 550);
  EXPECT_EQ(cli.cmd("DELE /hello.txt").first, 550);
  EXPECT_EQ(cli.cmd("RNFR /hello.txt").first, 550);
  {
    int dfd = cli.openPasv();
    EXPECT_EQ(cli.cmd("STOR /blocked.txt").first, 550);
    FtpClientHelper::readAllData(dfd);
  }
  {
    int dfd = cli.openPasv();
    EXPECT_EQ(cli.cmd("RETR /hello.txt").first, 150);
    auto got = FtpClientHelper::readAllData(dfd);
    EXPECT_EQ(cli.readReply().first, 226);
    EXPECT_EQ(std::string(got.begin(), got.end()), "Hello from TightVNC server!");
  }

  EXPECT_EQ(cli2.cmd("QUIT").first, 221);
  EXPECT_EQ(cli.cmd("QUIT").first, 221);
}

TEST(FtpBridgeIntegration, Tight13_EndToEnd)
{
  BridgeHarness h(MockServerMode::Tight13);
  ASSERT_TRUE(h.start());

  FtpClientHelper cli;
  ASSERT_TRUE(cli.connect(h.ftpPort()));
  EXPECT_EQ(cli.readReply().first, 220);
  EXPECT_EQ(cli.cmd("USER any").first, 331);
  EXPECT_EQ(cli.cmd("PASS any").first, 230);

  // Directory creation succeeds even with FTC_FCDR omitted from InteractionCaps
  EXPECT_EQ(cli.cmd("MKD /t13dir").first, 257);
  EXPECT_TRUE(h.hasMockPath("/t13dir"));

  // Upload > 100 KiB synced via ClientFence("FT13")
  std::vector<uint8_t> upData(110000);
  for (size_t i = 0; i < upData.size(); i++)
    upData[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
  {
    int dfd = cli.openPasv();
    ASSERT_GE(dfd, 0);
    EXPECT_EQ(cli.cmd("STOR /t13dir/up.dat").first, 150);
    FtpClientHelper::sendAllData(dfd, upData.data(), upData.size());
    EXPECT_EQ(cli.readReply().first, 226);
  }
  std::vector<uint8_t> stored;
  ASSERT_TRUE(h.getMockFile("/t13dir/up.dat", &stored));
  EXPECT_EQ(stored, upData);

  // Full download & REST resume
  {
    EXPECT_EQ(cli.cmd("REST 50000").first, 350);
    int dfd = cli.openPasv();
    ASSERT_GE(dfd, 0);
    EXPECT_EQ(cli.cmd("RETR /t13dir/up.dat").first, 150);
    auto tail = FtpClientHelper::readAllData(dfd);
    EXPECT_EQ(cli.readReply().first, 226);
    std::vector<uint8_t> exp(upData.begin() + 50000, upData.end());
    EXPECT_EQ(tail, exp);
  }

  // Zero-byte download
  {
    int dfd = cli.openPasv();
    ASSERT_GE(dfd, 0);
    EXPECT_EQ(cli.cmd("RETR /empty.txt").first, 150);
    auto z = FtpClientHelper::readAllData(dfd);
    EXPECT_EQ(cli.readReply().first, 226);
    EXPECT_TRUE(z.empty());
  }

  // Unsupported commands return 550 cleanly
  EXPECT_EQ(cli.cmd("DELE /t13dir/up.dat").first, 550);
  EXPECT_EQ(cli.cmd("RNFR /t13dir/up.dat").first, 550);
  EXPECT_EQ(cli.cmd("QUIT").first, 221);
}

TEST(FtpBridgeIntegration, UltraVNC_EndToEnd)
{
  BridgeHarness h(MockServerMode::Ultra);
  ASSERT_TRUE(h.start());

  FtpClientHelper cli;
  ASSERT_TRUE(cli.connect(h.ftpPort()));
  EXPECT_EQ(cli.readReply().first, 220);
  EXPECT_EQ(cli.cmd("USER any").first, 331);
  EXPECT_EQ(cli.cmd("PASS any").first, 230);

  // Root lists drives ("C:")
  {
    int dfd = cli.openPasv();
    ASSERT_GE(dfd, 0);
    EXPECT_EQ(cli.cmd("MLSD /").first, 150);
    auto d = FtpClientHelper::readAllData(dfd);
    EXPECT_EQ(cli.readReply().first, 226);
    std::string s(d.begin(), d.end());
    EXPECT_NE(s.find("C:"), std::string::npos);
  }

  EXPECT_EQ(cli.cmd("CWD /C:").first, 250);
  EXPECT_EQ(cli.cmd("MKD /C:/udir").first, 257);

  // Upload and download with REST resume
  const std::string body = "0123456789UltraPayload";
  {
    int dfd = cli.openPasv();
    ASSERT_GE(dfd, 0);
    EXPECT_EQ(cli.cmd("STOR /C:/udir/file.bin").first, 150);
    FtpClientHelper::sendAllData(dfd,
                                 reinterpret_cast<const uint8_t*>(body.data()),
                                 body.size());
    EXPECT_EQ(cli.readReply().first, 226);
  }
  {
    EXPECT_EQ(cli.cmd("REST 10").first, 350);
    int dfd = cli.openPasv();
    ASSERT_GE(dfd, 0);
    EXPECT_EQ(cli.cmd("RETR /C:/udir/file.bin").first, 150);
    auto d = FtpClientHelper::readAllData(dfd);
    EXPECT_EQ(cli.readReply().first, 226);
    EXPECT_EQ(std::string(d.begin(), d.end()), "UltraPayload");
  }

  // Rename, Delete, Rmdir
  EXPECT_EQ(cli.cmd("RNFR /C:/udir/file.bin").first, 350);
  EXPECT_EQ(cli.cmd("RNTO /C:/udir/file2.bin").first, 250);
  EXPECT_EQ(cli.cmd("DELE /C:/udir/file2.bin").first, 250);
  EXPECT_EQ(cli.cmd("RMD /C:/udir").first, 250);
  EXPECT_FALSE(h.hasMockPath("/C:/udir"));
  EXPECT_EQ(cli.cmd("QUIT").first, 221);
}

TEST(FtpBridgeIntegration, NoFileTransfer_Graceful550)
{
  BridgeHarness h(MockServerMode::NoFt);
  ASSERT_TRUE(h.start());

  FtpClientHelper cli;
  ASSERT_TRUE(cli.connect(h.ftpPort()));
  EXPECT_EQ(cli.readReply().first, 220);
  EXPECT_EQ(cli.cmd("USER any").first, 331);
  EXPECT_EQ(cli.cmd("PASS any").first, 230);
  EXPECT_EQ(cli.cmd("PWD").first, 257);

  int dfd = cli.openPasv();
  ASSERT_GE(dfd, 0);
  EXPECT_EQ(cli.cmd("LIST /").first, 550);
  ::close(dfd);

  EXPECT_EQ(cli.cmd("RETR /file").first, 550);
  EXPECT_EQ(cli.cmd("MKD /dir").first, 550);
  EXPECT_EQ(cli.cmd("QUIT").first, 221);
}
