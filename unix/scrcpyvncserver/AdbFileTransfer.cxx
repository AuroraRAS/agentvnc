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

#include "AdbFileTransfer.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <functional>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <core/Exception.h>
#include <core/LogWriter.h>
#include <core/string.h>

static core::LogWriter vlog("AdbFileTransfer");

namespace {

static const size_t kMaxDownloadChunk = 1024u * 1024u;
static const size_t kTight13DownloadChunk = 32768u;
static const int kMaxDirSizeDepth = 64;
static const int kAdbTransferTimeoutMs = 30000;

std::string trimTrailingNuls(const std::string& s)
{
  size_t end = s.size();
  while (end > 0 && s[end - 1] == '\0')
    end--;
  return s.substr(0, end);
}

bool normalizeVirtualSegments(const std::string& input,
                              bool allowEmptyAsRoot,
                              std::string* out,
                              std::string* errorOut)
{
  std::string cleaned = trimTrailingNuls(input);
  if (cleaned.find('\0') != std::string::npos) {
    if (errorOut)
      *errorOut = "Invalid null byte in path";
    return false;
  }

  for (char& c : cleaned) {
    if (c == '\\')
      c = '/';
  }

  if (cleaned.empty()) {
    if (!allowEmptyAsRoot) {
      if (errorOut)
        *errorOut = "Path must not be empty";
      return false;
    }
    *out = "/";
    return true;
  }

  std::vector<std::string> parts;
  size_t i = 0;
  while (i < cleaned.size()) {
    while (i < cleaned.size() && cleaned[i] == '/')
      i++;
    if (i >= cleaned.size())
      break;
    size_t j = cleaned.find('/', i);
    if (j == std::string::npos)
      j = cleaned.size();
    std::string seg = cleaned.substr(i, j - i);
    if (seg == "..") {
      if (!parts.empty())
        parts.pop_back();
    } else if (!seg.empty() && seg != ".") {
      parts.push_back(std::move(seg));
    }
    i = j + 1;
  }

  if (parts.empty()) {
    *out = "/";
    return true;
  }

  std::string res;
  for (const std::string& seg : parts) {
    res.push_back('/');
    res.append(seg);
  }
  *out = std::move(res);
  return true;
}

std::string parentDirectory(const std::string& path)
{
  if (path.empty() || path == "/")
    return "/";
  size_t slash = path.rfind('/');
  if (slash == std::string::npos || slash == 0)
    return "/";
  return path.substr(0, slash);
}

bool parseHexDigit(char c, uint8_t* val)
{
  if (c >= '0' && c <= '9') {
    *val = static_cast<uint8_t>(c - '0');
    return true;
  }
  if (c >= 'a' && c <= 'f') {
    *val = static_cast<uint8_t>(c - 'a' + 10);
    return true;
  }
  if (c >= 'A' && c <= 'F') {
    *val = static_cast<uint8_t>(c - 'A' + 10);
    return true;
  }
  return false;
}

bool parseMd5Hex(const std::string& text, uint8_t digest[16])
{
  size_t pos = 0;
  while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
    pos++;
  if (text.size() - pos < 32)
    return false;
  for (int i = 0; i < 16; ++i) {
    uint8_t hi = 0;
    uint8_t lo = 0;
    if (!parseHexDigit(text[pos + static_cast<size_t>(i * 2)], &hi) ||
        !parseHexDigit(text[pos + static_cast<size_t>(i * 2) + 1], &lo))
      return false;
    digest[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

std::string formatTouchDateUtc(time_t t)
{
  struct tm tmVal;
  std::memset(&tmVal, 0, sizeof(tmVal));
  gmtime_r(&t, &tmVal);
  char buf[80];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                tmVal.tm_year + 1900, tmVal.tm_mon + 1, tmVal.tm_mday,
                tmVal.tm_hour, tmVal.tm_min, tmVal.tm_sec);
  return std::string(buf);
}

std::string extractShellError(const AdbShellResult& res,
                              const std::string& fallback)
{
  std::string msg = !res.stderrData.empty() ? res.stderrData : res.stdoutData;
  while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' ||
                          msg.back() == ' ')) {
    msg.pop_back();
  }
  if (msg.empty())
    return fallback;
  return msg;
}

class IoFdScope {
public:
  IoFdScope(std::function<void(int)> setFn, int fd)
    : setFn_(std::move(setFn))
  {
    setFn_(fd);
  }
  ~IoFdScope()
  {
    setFn_(-1);
  }
private:
  std::function<void(int)> setFn_;
};

} // namespace

AdbFileTransferSession::AdbFileTransferSession(std::string adbHost,
                                               int adbPort,
                                               std::string serial,
                                               std::string rootDir)
  : adb_(std::move(adbHost), adbPort),
    serial_(std::move(serial)),
    rootDir_(std::move(rootDir)),
    stopping_(false),
    workerStarted_(false),
    activeIoFd_(-1),
    featuresQueried_(false),
    downloadSyncFd_(-1),
    downloadFlags_(0),
    downloadMtimeMs_(0),
    downloadCompressLevel_(0),
    downloadEof_(false),
    tight13DownloadActive_(false),
    uploadMode_(UploadMode::None),
    uploadFd_(-1),
    uploadTempFd_(-1),
    tight13UploadActive_(false)
{
  wakeupPipe_[0] = -1;
  wakeupPipe_[1] = -1;
  if (::pipe2(wakeupPipe_, O_NONBLOCK | O_CLOEXEC) != 0)
    throw core::posix_error("Failed to create file transfer wakeup pipe", errno);
}

AdbFileTransferSession::~AdbFileTransferSession()
{
  stop();
  for (int i = 0; i < 2; ++i) {
    if (wakeupPipe_[i] >= 0) {
      ::close(wakeupPipe_[i]);
      wakeupPipe_[i] = -1;
    }
  }
}

void AdbFileTransferSession::stop()
{
  {
    std::lock_guard<std::mutex> lk(reqMu_);
    stopping_ = true;
    reqQueue_.clear();
  }
  reqCv_.notify_all();

  {
    std::lock_guard<std::mutex> lk(ioFdMu_);
    if (activeIoFd_ >= 0)
      ::shutdown(activeIoFd_, SHUT_RDWR);
  }

  if (workerStarted_ && worker_.joinable())
    worker_.join();
  workerStarted_ = false;
}

bool AdbFileTransferSession::normalizePath(const std::string& rawPath,
                                           const std::string& rootDir,
                                           bool allowEmptyAsRoot,
                                           std::string* devicePathOut,
                                           std::string* virtualPathOut,
                                           std::string* errorOut)
{
  std::string vpath;
  if (!normalizeVirtualSegments(rawPath, allowEmptyAsRoot, &vpath, errorOut))
    return false;

  std::string normRoot = "/";
  if (!rootDir.empty()) {
    std::string rootErr;
    if (!normalizeVirtualSegments(rootDir, true, &normRoot, &rootErr)) {
      if (errorOut)
        *errorOut = "Invalid FileTransferRoot: " + rootErr;
      return false;
    }
  }

  std::string dpath;
  if (normRoot == "/") {
    dpath = vpath;
  } else if (vpath == "/") {
    dpath = normRoot;
  } else {
    dpath = normRoot + vpath;
  }

  if (virtualPathOut)
    *virtualPathOut = std::move(vpath);
  if (devicePathOut)
    *devicePathOut = std::move(dpath);
  return true;
}

void AdbFileTransferSession::enqueue(Request req)
{
  {
    std::lock_guard<std::mutex> lk(reqMu_);
    if (stopping_)
      return;
    if (!workerStarted_) {
      workerStarted_ = true;
      worker_ = std::thread(&AdbFileTransferSession::workerMain, this);
    }
    reqQueue_.push_back(std::move(req));
  }
  reqCv_.notify_one();
}

void AdbFileTransferSession::notifyMainThread()
{
  uint8_t byte = 1;
  ssize_t n;
  do {
    n = ::write(wakeupPipe_[1], &byte, 1);
  } while (n < 0 && errno == EINTR);
}

void AdbFileTransferSession::postReply(Reply reply)
{
  {
    std::lock_guard<std::mutex> lk(replyMu_);
    replyQueue_.push_back(std::move(reply));
  }
  notifyMainThread();
}

std::vector<AdbFileTransferSession::Reply> AdbFileTransferSession::drainReplies()
{
  uint8_t drainBuf[256];
  while (true) {
    ssize_t n = ::read(wakeupPipe_[0], drainBuf, sizeof(drainBuf));
    if (n > 0)
      continue;
    if (n < 0 && errno == EINTR)
      continue;
    break;
  }

  std::vector<Reply> out;
  std::lock_guard<std::mutex> lk(replyMu_);
  while (!replyQueue_.empty()) {
    out.push_back(std::move(replyQueue_.front()));
    replyQueue_.pop_front();
  }
  return out;
}

void AdbFileTransferSession::setActiveIoFd(int fd)
{
  std::lock_guard<std::mutex> lk(ioFdMu_);
  activeIoFd_ = fd;
}

void AdbFileTransferSession::closeManagedFd(int* fd)
{
  if (fd && *fd >= 0) {
    setActiveIoFd(-1);
    ::close(*fd);
    *fd = -1;
  }
}

void AdbFileTransferSession::closeDownload()
{
  if (downloadSyncFd_ >= 0) {
    setActiveIoFd(downloadSyncFd_);
    AdbClient::syncQuit(downloadSyncFd_);
    closeManagedFd(&downloadSyncFd_);
  }
  downloadPath_.clear();
  downloadFlags_ = 0;
  downloadMtimeMs_ = 0;
  downloadCompressLevel_ = 0;
  downloadEof_ = false;
  tight13DownloadActive_ = false;
  downloadPrefetch_.clear();
}

void AdbFileTransferSession::abortUpload()
{
  if (uploadMode_ == UploadMode::SyncSend && uploadFd_ >= 0) {
    setActiveIoFd(uploadFd_);
    AdbClient::syncQuit(uploadFd_);
    closeManagedFd(&uploadFd_);
  } else if (uploadMode_ == UploadMode::ShellV2Append && uploadFd_ >= 0) {
    setActiveIoFd(uploadFd_);
    try {
      AdbClient::shellV2CloseStdin(uploadFd_);
    } catch (const std::exception&) {
    }
    closeManagedFd(&uploadFd_);
  }
  if (uploadTempFd_ >= 0) {
    ::close(uploadTempFd_);
    uploadTempFd_ = -1;
  }
  if (!uploadTempPath_.empty()) {
    ::unlink(uploadTempPath_.c_str());
    uploadTempPath_.clear();
  }
  uploadMode_ = UploadMode::None;
  uploadPath_.clear();
  tight13UploadActive_ = false;
}

bool AdbFileTransferSession::isPathInUse(const std::string& devicePath) const
{
  auto matchesOrChild = [&devicePath](const std::string& active) {
    if (active.empty())
      return false;
    if (active == devicePath)
      return true;
    std::string prefix = (devicePath == "/") ? "/" : (devicePath + "/");
    return active.compare(0, prefix.size(), prefix) == 0;
  };
  return matchesOrChild(downloadPath_) || matchesOrChild(uploadPath_);
}

void AdbFileTransferSession::ensureFeatures()
{
  if (featuresQueried_)
    return;
  features_ = adb_.queryFeatures(serial_);
  featuresQueried_ = true;
}

void AdbFileTransferSession::workerMain()
{
  while (true) {
    Request req;
    {
      std::unique_lock<std::mutex> lk(reqMu_);
      reqCv_.wait(lk, [this]() { return stopping_ || !reqQueue_.empty(); });
      if (stopping_)
        break;
      req = std::move(reqQueue_.front());
      reqQueue_.pop_front();
    }

    handleRequest(req);
  }

  closeDownload();
  abortUpload();
}

void AdbFileTransferSession::handleRequest(const Request& req)
{
  switch (req.kind) {
  case Request::Kind::Tight2CompressionSupport: {
    Reply r;
    r.kind = Reply::Kind::Tight2CompressionSupport;
    r.boolVal = true;
    postReply(std::move(r));
    break;
  }
  case Request::Kind::Tight2FileList:
    handleTight2FileList(req.u8Val, req.path);
    break;
  case Request::Kind::Tight2Md5:
    handleTight2Md5(req.path, req.u64Val, req.u64Val2);
    break;
  case Request::Kind::Tight2UploadStart:
    handleTight2UploadStart(req.path, req.u8Val, req.u64Val);
    break;
  case Request::Kind::Tight2UploadData:
    handleTight2UploadData(req.data);
    break;
  case Request::Kind::Tight2UploadEnd:
    handleTight2UploadEnd(req.u16Val, req.u64Val);
    break;
  case Request::Kind::Tight2DownloadStart:
    handleTight2DownloadStart(req.path, req.u64Val);
    break;
  case Request::Kind::Tight2DownloadData:
    handleTight2DownloadData(req.u8Val, req.u32Val);
    break;
  case Request::Kind::Tight2Mkdir:
    handleTight2Mkdir(req.path);
    break;
  case Request::Kind::Tight2Remove:
    handleTight2Remove(req.path);
    break;
  case Request::Kind::Tight2Rename:
    handleTight2Rename(req.path, req.path2);
    break;
  case Request::Kind::Tight2DirSize:
    handleTight2DirSize(req.path);
    break;
  case Request::Kind::Tight13FileList:
    handleTight13FileList(req.u8Val, req.path);
    break;
  case Request::Kind::Tight13DownloadStart:
    handleTight13DownloadStart(req.u8Val, req.u32Val, req.path);
    break;
  case Request::Kind::Tight13DownloadNextChunk:
    handleTight13DownloadNextChunk();
    break;
  case Request::Kind::Tight13DownloadCancel:
    closeDownload();
    break;
  case Request::Kind::Tight13UploadStart:
    handleTight13UploadStart(req.u32Val, req.path);
    break;
  case Request::Kind::Tight13UploadData:
    handleTight13UploadData(req.data);
    break;
  case Request::Kind::Tight13UploadEnd:
    handleTight13UploadEnd(req.u32Val);
    break;
  case Request::Kind::Tight13UploadFailed:
    abortUpload();
    break;
  case Request::Kind::Tight13CreateDir:
    handleTight13CreateDir(req.path);
    break;
  case Request::Kind::FenceBarrier: {
    Reply r;
    r.kind = Reply::Kind::FenceReply;
    r.u32Val = req.u32Val;
    r.data = req.data;
    postReply(std::move(r));
    break;
  }
  }
}

void AdbFileTransferSession::handleTight2FileList(uint8_t compressLevel,
                                                  const std::string& rawPath)
{
  std::string devicePath;
  std::string err;
  if (!normalizePath(rawPath, rootDir_, true, &devicePath, nullptr, &err)) {
    Reply r;
    r.kind = Reply::Kind::Tight2Failed;
    r.text = err;
    postReply(std::move(r));
    return;
  }

  int syncFd = -1;
  try {
    ensureFeatures();
    syncFd = adb_.openSync(serial_, kAdbTransferTimeoutMs);
    IoFdScope scope([this](int f) { setActiveIoFd(f); }, syncFd);

    AdbStat st;
    if (!AdbClient::syncStat(syncFd, devicePath, features_.statV2, true, &st) ||
        !S_ISDIR(st.mode)) {
      AdbClient::syncQuit(syncFd);
      ::close(syncFd);
      Reply r;
      r.kind = Reply::Kind::Tight2Failed;
      r.text = (st.error != 0) ? std::strerror(static_cast<int>(st.error))
                               : "Path is not a directory";
      postReply(std::move(r));
      return;
    }

    std::vector<AdbDirEntry> raw =
      AdbClient::syncList(syncFd, devicePath, features_.lsV2, features_.statV2);
    AdbClient::syncQuit(syncFd);
    ::close(syncFd);
    syncFd = -1;

    Reply r;
    r.kind = Reply::Kind::Tight2FileList;
    r.compressLevel = compressLevel;
    r.entries.reserve(raw.size());
    for (const AdbDirEntry& e : raw) {
      rfb::FileEntry fe;
      fe.name = e.name;
      fe.isDir = S_ISDIR(e.mode);
      fe.size = fe.isDir ? 0 : e.size;
      fe.mtime = e.mtime;
      fe.isExecutable = !fe.isDir && ((e.mode & 0111u) != 0);
      r.entries.push_back(std::move(fe));
    }
    postReply(std::move(r));
  } catch (const std::exception& e) {
    if (syncFd >= 0) {
      setActiveIoFd(-1);
      ::close(syncFd);
    }
    Reply r;
    r.kind = Reply::Kind::Tight2Failed;
    r.text = e.what();
    postReply(std::move(r));
  }
}

void AdbFileTransferSession::handleTight2Md5(const std::string& rawPath,
                                             uint64_t offset,
                                             uint64_t dataLen)
{
  std::string devicePath;
  std::string err;
  if (!normalizePath(rawPath, rootDir_, false, &devicePath, nullptr, &err)) {
    Reply r;
    r.kind = Reply::Kind::Tight2Failed;
    r.text = err;
    postReply(std::move(r));
    return;
  }

  int syncFd = -1;
  try {
    ensureFeatures();
    syncFd = adb_.openSync(serial_, kAdbTransferTimeoutMs);
    AdbStat st;
    bool okStat = false;
    {
      IoFdScope scope([this](int f) { setActiveIoFd(f); }, syncFd);
      okStat = AdbClient::syncStat(syncFd, devicePath, features_.statV2, true, &st);
      AdbClient::syncQuit(syncFd);
    }
    ::close(syncFd);
    syncFd = -1;

    if (!okStat || S_ISDIR(st.mode)) {
      Reply r;
      r.kind = Reply::Kind::Tight2Failed;
      r.text = S_ISDIR(st.mode)
        ? "Cannot compute MD5 of a directory"
        : (st.error ? std::strerror(static_cast<int>(st.error))
                    : "File not found");
      postReply(std::move(r));
      return;
    }
    if (offset > st.size) {
      Reply r;
      r.kind = Reply::Kind::Tight2Failed;
      r.text = "MD5 offset exceeds file size";
      postReply(std::move(r));
      return;
    }

    uint64_t avail = st.size - offset;
    uint64_t len = std::min(dataLen, avail);
    if (len == 0) {
      static const uint8_t kEmptyMd5[16] = {
        0xd4, 0x1d, 0x8c, 0xd9, 0x8f, 0x00, 0xb2, 0x04,
        0xe9, 0x80, 0x09, 0x98, 0xec, 0xf8, 0x42, 0x7e
      };
      Reply r;
      r.kind = Reply::Kind::Tight2Md5;
      std::memcpy(r.md5, kEmptyMd5, 16);
      postReply(std::move(r));
      return;
    }

    std::string qpath = AdbClient::quoteShellArg(devicePath);
    std::string cmd;
    bool partial = !(offset == 0 && len == st.size);
    if (!partial) {
      cmd = "md5sum -- " + qpath;
    } else {
      cmd = core::format(
        "(dd if=%s bs=65536 iflag=skip_bytes,count_bytes skip=%llu count=%llu status=none"
        " || printf '__SCRCPY_DD_FAIL__' >&2) | md5sum",
        qpath.c_str(),
        static_cast<unsigned long long>(offset),
        static_cast<unsigned long long>(len));
    }

    AdbShellResult res =
      adb_.runShell(serial_, cmd, features_.shellV2, kAdbTransferTimeoutMs);
    if (res.exitCode != 0 ||
        res.stderrData.find("__SCRCPY_DD_FAIL__") != std::string::npos) {
      Reply r;
      r.kind = Reply::Kind::Tight2Failed;
      r.text = extractShellError(res, "Failed to compute MD5");
      postReply(std::move(r));
      return;
    }

    Reply r;
    if (!parseMd5Hex(res.stdoutData, r.md5)) {
      r.kind = Reply::Kind::Tight2Failed;
      r.text = "Unexpected md5sum output";
    } else {
      r.kind = Reply::Kind::Tight2Md5;
    }
    postReply(std::move(r));
  } catch (const std::exception& e) {
    if (syncFd >= 0) {
      setActiveIoFd(-1);
      ::close(syncFd);
    }
    Reply r;
    r.kind = Reply::Kind::Tight2Failed;
    r.text = e.what();
    postReply(std::move(r));
  }
}

bool AdbFileTransferSession::openDownloadInternal(const std::string& rawPath,
                                                  uint64_t offset,
                                                  bool allowEmptyCancel,
                                                  std::string* errOut)
{
  closeDownload();

  std::string trimmed = trimTrailingNuls(rawPath);
  if (allowEmptyCancel && trimmed.empty()) {
    if (errOut)
      *errOut = "Download cancelled";
    return false;
  }

  std::string devicePath;
  if (!normalizePath(trimmed, rootDir_, false, &devicePath, nullptr, errOut))
    return false;

  try {
    ensureFeatures();
    downloadSyncFd_ = adb_.openSync(serial_, kAdbTransferTimeoutMs);
    IoFdScope scope([this](int f) { setActiveIoFd(f); }, downloadSyncFd_);

    AdbStat st;
    if (!AdbClient::syncStat(downloadSyncFd_, devicePath, features_.statV2,
                             true, &st) ||
        S_ISDIR(st.mode)) {
      if (errOut) {
        *errOut = S_ISDIR(st.mode)
          ? "Cannot download a directory"
          : (st.error ? std::strerror(static_cast<int>(st.error))
                      : "File not found");
      }
      closeDownload();
      return false;
    }

    if (offset > st.size) {
      if (errOut)
        *errOut = "Specified file pointer position is more than file length";
      closeDownload();
      return false;
    }

    AdbClient::syncStartRecv(downloadSyncFd_, devicePath);

    uint64_t remainingSkip = offset;
    std::vector<uint8_t> buf;
    bool gotInitial = false;
    while (!gotInitial) {
      buf.clear();
      std::string recvErr;
      int rc = AdbClient::syncReadRecvChunk(downloadSyncFd_, &buf, &recvErr);
      if (rc < 0) {
        if (errOut)
          *errOut = recvErr.empty() ? "ADB download failed" : recvErr;
        closeDownload();
        return false;
      }
      if (rc == 0) {
        downloadEof_ = true;
        gotInitial = true;
        break;
      }
      if (remainingSkip >= buf.size()) {
        remainingSkip -= buf.size();
        // If we skipped exactly to st.size, loop once more to detect EOF/error
        if (remainingSkip == 0 && offset == st.size)
          continue;
        if (remainingSkip == 0)
          gotInitial = true;
      } else {
        downloadPrefetch_.assign(
          buf.begin() + static_cast<ptrdiff_t>(remainingSkip), buf.end());
        remainingSkip = 0;
        gotInitial = true;
      }
    }

    downloadPath_ = devicePath;
    downloadFlags_ = ((st.mode & 0111u) != 0) ? rfb::tight2FlagExec : 0;
    downloadMtimeMs_ = (st.mtime > 0)
      ? static_cast<uint64_t>(st.mtime) * 1000ULL : 0ULL;
    return true;
  } catch (const std::exception& e) {
    if (errOut)
      *errOut = e.what();
    closeDownload();
    return false;
  }
}

bool AdbFileTransferSession::readDownloadChunk(size_t maxBytes,
                                               std::vector<uint8_t>* out,
                                               bool* eofOut,
                                               std::string* errOut)
{
  out->clear();
  *eofOut = false;

  if (downloadSyncFd_ < 0) {
    if (errOut)
      *errOut = "No active download";
    return false;
  }

  try {
    IoFdScope scope([this](int f) { setActiveIoFd(f); }, downloadSyncFd_);

    if (!downloadPrefetch_.empty()) {
      size_t take = std::min(maxBytes, downloadPrefetch_.size());
      out->assign(downloadPrefetch_.begin(),
                  downloadPrefetch_.begin() + static_cast<ptrdiff_t>(take));
      downloadPrefetch_.erase(downloadPrefetch_.begin(),
                              downloadPrefetch_.begin() + static_cast<ptrdiff_t>(take));
    }

    while (out->size() < maxBytes && !downloadEof_) {
      std::vector<uint8_t> pkt;
      std::string recvErr;
      int rc = AdbClient::syncReadRecvChunk(downloadSyncFd_, &pkt, &recvErr);
      if (rc < 0) {
        if (errOut)
          *errOut = recvErr.empty() ? "ADB download failed" : recvErr;
        return false;
      }
      if (rc == 0) {
        downloadEof_ = true;
        break;
      }
      size_t need = maxBytes - out->size();
      if (pkt.size() <= need) {
        out->insert(out->end(), pkt.begin(), pkt.end());
      } else {
        out->insert(out->end(), pkt.begin(),
                    pkt.begin() + static_cast<ptrdiff_t>(need));
        downloadPrefetch_.assign(pkt.begin() + static_cast<ptrdiff_t>(need),
                                 pkt.end());
      }
    }

    *eofOut = (out->empty() && downloadEof_);
    return true;
  } catch (const std::exception& e) {
    if (errOut)
      *errOut = e.what();
    return false;
  }
}

void AdbFileTransferSession::handleTight2DownloadStart(const std::string& rawPath,
                                                       uint64_t offset)
{
  std::string err;
  if (!openDownloadInternal(rawPath, offset, true, &err)) {
    Reply r;
    r.kind = Reply::Kind::Tight2Failed;
    r.text = err;
    postReply(std::move(r));
    return;
  }

  Reply r;
  r.kind = Reply::Kind::Tight2DownloadStart;
  postReply(std::move(r));
}

void AdbFileTransferSession::handleTight2DownloadData(uint8_t compressLevel,
                                                      uint32_t reqSize)
{
  size_t clamped = std::max<size_t>(1u, std::min<size_t>(reqSize, kMaxDownloadChunk));
  std::vector<uint8_t> chunk;
  bool eof = false;
  std::string err;

  if (!readDownloadChunk(clamped, &chunk, &eof, &err)) {
    closeDownload();
    Reply r;
    r.kind = Reply::Kind::Tight2Failed;
    r.text = err;
    postReply(std::move(r));
    return;
  }

  if (eof) {
    uint8_t flags = downloadFlags_;
    uint64_t mtimeMs = downloadMtimeMs_;
    closeDownload();
    Reply r;
    r.kind = Reply::Kind::Tight2DownloadEnd;
    r.flags = flags;
    r.u64Val = mtimeMs;
    postReply(std::move(r));
    return;
  }

  Reply r;
  r.kind = Reply::Kind::Tight2DownloadData;
  r.compressLevel = compressLevel;
  r.data = std::move(chunk);
  postReply(std::move(r));
}

bool AdbFileTransferSession::openUploadInternal(const std::string& rawPath,
                                                bool overwrite,
                                                uint64_t offset,
                                                std::string* errOut)
{
  abortUpload();

  std::string devicePath;
  if (!normalizePath(rawPath, rootDir_, false, &devicePath, nullptr, errOut))
    return false;

  try {
    ensureFeatures();

    int statFd = adb_.openSync(serial_, kAdbTransferTimeoutMs);
    AdbStat st;
    bool exists = false;
    AdbStat parentSt;
    bool parentExists = false;
    {
      IoFdScope scope([this](int f) { setActiveIoFd(f); }, statFd);
      exists = AdbClient::syncStat(statFd, devicePath, features_.statV2,
                                   true, &st);
      if (overwrite) {
        std::string pdir = parentDirectory(devicePath);
        parentExists = AdbClient::syncStat(statFd, pdir, features_.statV2,
                                           true, &parentSt);
      }
      AdbClient::syncQuit(statFd);
    }
    ::close(statFd);

    if (overwrite) {
      if (offset > 0) {
        if (errOut)
          *errOut = "Specified file pointer position is more than file length";
        return false;
      }
      if (exists && S_ISDIR(st.mode)) {
        if (errOut)
          *errOut = "Cannot overwrite a directory";
        return false;
      }
      if (!parentExists || !S_ISDIR(parentSt.mode)) {
        if (errOut)
          *errOut = "Parent directory does not exist";
        return false;
      }
    } else {
      if (!exists || S_ISDIR(st.mode)) {
        if (errOut)
          *errOut = exists ? "Cannot upload over a directory"
                           : "The system cannot find the file specified";
        return false;
      }
      if (offset > st.size) {
        if (errOut)
          *errOut = "Specified file pointer position is more than file length";
        return false;
      }
    }

    if (offset == 0) {
      uploadFd_ = adb_.openSync(serial_, kAdbTransferTimeoutMs);
      IoFdScope scope([this](int f) { setActiveIoFd(f); }, uploadFd_);
      AdbClient::syncStartSend(uploadFd_, devicePath, 0100644);
      std::string sendErr;
      if (AdbClient::syncCheckImmediateError(uploadFd_, 50, &sendErr)) {
        if (errOut)
          *errOut = sendErr.empty() ? "ADB SEND rejected" : sendErr;
        closeManagedFd(&uploadFd_);
        return false;
      }
      uploadMode_ = UploadMode::SyncSend;
      uploadPath_ = devicePath;
      return true;
    }

    if (features_.shellV2) {
      std::string qpath = AdbClient::quoteShellArg(devicePath);
      std::string cmd = core::format(
        "exec 4>&1; truncate -s %llu -- %s && exec >>%s && printf 'OK' >&4 && exec 4>&- && exec cat",
        static_cast<unsigned long long>(offset),
        qpath.c_str(), qpath.c_str());
      uploadFd_ = adb_.openShellV2(serial_, cmd, kAdbTransferTimeoutMs);
      IoFdScope scope([this](int f) { setActiveIoFd(f); }, uploadFd_);

      std::string stdoutAccum;
      std::string stderrAccum;
      while (stdoutAccum.size() < 2) {
        uint8_t id = 0;
        std::vector<uint8_t> payload;
        if (!AdbClient::shellV2ReadPacket(uploadFd_, &id, &payload)) {
          if (errOut)
            *errOut = stderrAccum.empty() ? "Upload resume failed" : stderrAccum;
          closeManagedFd(&uploadFd_);
          return false;
        }
        if (id == 1) {
          stdoutAccum.append(reinterpret_cast<const char*>(payload.data()),
                             payload.size());
        } else if (id == 2) {
          stderrAccum.append(reinterpret_cast<const char*>(payload.data()),
                             payload.size());
        } else if (id == 3) {
          while (!stderrAccum.empty() &&
                 (stderrAccum.back() == '\n' || stderrAccum.back() == '\r'))
            stderrAccum.pop_back();
          if (errOut)
            *errOut = stderrAccum.empty() ? "Upload resume failed" : stderrAccum;
          closeManagedFd(&uploadFd_);
          return false;
        }
      }
      if (stdoutAccum != "OK") {
        if (errOut)
          *errOut = "Unexpected upload resume handshake";
        closeManagedFd(&uploadFd_);
        return false;
      }

      uploadMode_ = UploadMode::ShellV2Append;
      uploadPath_ = devicePath;
      return true;
    }

    // Fallback when shell_v2 is unavailable: pull prefix [0..offset) locally.
    char tmpl[] = "/tmp/scrcpy-ft-up-XXXXXX";
    uploadTempFd_ = ::mkstemp(tmpl);
    if (uploadTempFd_ < 0) {
      if (errOut)
        *errOut = std::strerror(errno);
      return false;
    }
    uploadTempPath_ = tmpl;

    int pullFd = adb_.openSync(serial_, kAdbTransferTimeoutMs);
    {
      IoFdScope scope([this](int f) { setActiveIoFd(f); }, pullFd);
      AdbClient::syncStartRecv(pullFd, devicePath);
      uint64_t copied = 0;
      while (copied < offset) {
        std::vector<uint8_t> chunk;
        std::string recvErr;
        int rc = AdbClient::syncReadRecvChunk(pullFd, &chunk, &recvErr);
        if (rc < 0) {
          if (errOut)
            *errOut = recvErr.empty() ? "Failed to read existing file prefix"
                                      : recvErr;
          AdbClient::syncQuit(pullFd);
          ::close(pullFd);
          abortUpload();
          return false;
        }
        if (rc == 0)
          break;
        size_t take = static_cast<size_t>(
          std::min<uint64_t>(chunk.size(), offset - copied));
        size_t off = 0;
        while (off < take) {
          ssize_t wn = ::write(uploadTempFd_, chunk.data() + off, take - off);
          if (wn < 0 && errno == EINTR)
            continue;
          if (wn <= 0) {
            if (errOut)
              *errOut = std::strerror(errno);
            AdbClient::syncQuit(pullFd);
            ::close(pullFd);
            abortUpload();
            return false;
          }
          off += static_cast<size_t>(wn);
        }
        copied += take;
      }
      AdbClient::syncQuit(pullFd);
    }
    ::close(pullFd);

    uploadMode_ = UploadMode::TempFileFallback;
    uploadPath_ = devicePath;
    return true;
  } catch (const std::exception& e) {
    if (errOut)
      *errOut = e.what();
    abortUpload();
    return false;
  }
}

bool AdbFileTransferSession::writeUploadChunkInternal(
  const std::vector<uint8_t>& data, std::string* errOut)
{
  if (uploadMode_ == UploadMode::None) {
    if (errOut)
      *errOut = "No active upload";
    return false;
  }

  try {
    if (uploadMode_ == UploadMode::SyncSend) {
      IoFdScope scope([this](int f) { setActiveIoFd(f); }, uploadFd_);
      std::string sendErr;
      if (AdbClient::syncCheckImmediateError(uploadFd_, 0, &sendErr)) {
        if (errOut)
          *errOut = sendErr.empty() ? "ADB SEND failed" : sendErr;
        return false;
      }
      if (!data.empty())
        AdbClient::syncWriteSendData(uploadFd_, data.data(), data.size());
      return true;
    }

    if (uploadMode_ == UploadMode::ShellV2Append) {
      IoFdScope scope([this](int f) { setActiveIoFd(f); }, uploadFd_);
      struct pollfd pfd = { uploadFd_, POLLIN, 0 };
      if (::poll(&pfd, 1, 0) > 0) {
        uint8_t id = 0;
        std::vector<uint8_t> payload;
        if (!AdbClient::shellV2ReadPacket(uploadFd_, &id, &payload)) {
          if (errOut)
            *errOut = "Upload stream closed unexpectedly";
          return false;
        }
        if (id == 2 || id == 3) {
          std::string msg(reinterpret_cast<const char*>(payload.data()),
                          payload.size());
          while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
            msg.pop_back();
          if (errOut)
            *errOut = msg.empty() ? "Upload append failed" : msg;
          return false;
        }
      }
      if (!data.empty())
        AdbClient::shellV2WriteStdin(uploadFd_, data.data(), data.size());
      return true;
    }

    if (uploadMode_ == UploadMode::TempFileFallback) {
      size_t off = 0;
      while (off < data.size()) {
        ssize_t n = ::write(uploadTempFd_, data.data() + off, data.size() - off);
        if (n < 0 && errno == EINTR)
          continue;
        if (n <= 0) {
          if (errOut)
            *errOut = std::strerror(errno);
          return false;
        }
        off += static_cast<size_t>(n);
      }
      return true;
    }
  } catch (const std::exception& e) {
    if (errOut)
      *errOut = e.what();
    return false;
  }

  return false;
}

bool AdbFileTransferSession::finishUploadInternal(uint16_t fileFlags,
                                                  uint64_t modTimeMs,
                                                  std::string* errOut)
{
  if (uploadMode_ == UploadMode::None)
    return true;

  std::string targetPath = uploadPath_;
  uint32_t mtimeSec = static_cast<uint32_t>(modTimeMs / 1000ULL);
  if (mtimeSec == 0)
    mtimeSec = static_cast<uint32_t>(::time(nullptr));

  try {
    if (uploadMode_ == UploadMode::SyncSend) {
      IoFdScope scope([this](int f) { setActiveIoFd(f); }, uploadFd_);
      std::string sendErr;
      bool ok = AdbClient::syncFinishSend(uploadFd_, mtimeSec, &sendErr);
      closeManagedFd(&uploadFd_);
      uploadMode_ = UploadMode::None;
      uploadPath_.clear();
      if (!ok) {
        if (errOut)
          *errOut = sendErr.empty() ? "ADB SEND failed" : sendErr;
        return false;
      }
    } else if (uploadMode_ == UploadMode::ShellV2Append) {
      std::string stderrAccum;
      int exitCode = -1;
      {
        IoFdScope scope([this](int f) { setActiveIoFd(f); }, uploadFd_);
        AdbClient::shellV2CloseStdin(uploadFd_);
        uint8_t id = 0;
        std::vector<uint8_t> payload;
        while (AdbClient::shellV2ReadPacket(uploadFd_, &id, &payload)) {
          if (id == 2) {
            stderrAccum.append(reinterpret_cast<const char*>(payload.data()),
                               payload.size());
          } else if (id == 3) {
            if (!payload.empty())
              exitCode = static_cast<int>(payload[0]);
            break;
          }
        }
        closeManagedFd(&uploadFd_);
      }
      uploadMode_ = UploadMode::None;
      uploadPath_.clear();

      if (exitCode != 0) {
        while (!stderrAccum.empty() &&
               (stderrAccum.back() == '\n' || stderrAccum.back() == '\r'))
          stderrAccum.pop_back();
        if (errOut)
          *errOut = stderrAccum.empty() ? "Upload append failed" : stderrAccum;
        return false;
      }

      if (modTimeMs > 0) {
        std::string cmd = "touch -c -d " +
                          AdbClient::quoteShellArg(
                            formatTouchDateUtc(static_cast<time_t>(mtimeSec))) +
                          " -- " + AdbClient::quoteShellArg(targetPath);
        try {
          adb_.runShell(serial_, cmd, features_.shellV2, kAdbTransferTimeoutMs);
        } catch (const std::exception&) {
        }
      }
    } else if (uploadMode_ == UploadMode::TempFileFallback) {
      ::close(uploadTempFd_);
      uploadTempFd_ = -1;
      std::string tmpPath = uploadTempPath_;
      uploadTempPath_.clear();
      uploadMode_ = UploadMode::None;
      uploadPath_.clear();

      int syncFd = adb_.openSync(serial_, kAdbTransferTimeoutMs);
      try {
        IoFdScope scope([this](int f) { setActiveIoFd(f); }, syncFd);
        AdbClient::syncStartSend(syncFd, targetPath, 0100644);
        int inFd = ::open(tmpPath.c_str(), O_RDONLY | O_CLOEXEC);
        if (inFd < 0)
          throw core::posix_error("Failed to reopen temp upload file", errno);
        uint8_t buf[65536];
        while (true) {
          ssize_t n = ::read(inFd, buf, sizeof(buf));
          if (n < 0 && errno == EINTR)
            continue;
          if (n < 0) {
            int saved = errno;
            ::close(inFd);
            throw core::posix_error("Failed reading temp upload file", saved);
          }
          if (n == 0)
            break;
          AdbClient::syncWriteSendData(syncFd, buf, static_cast<size_t>(n));
        }
        ::close(inFd);
        std::string sendErr;
        bool ok = AdbClient::syncFinishSend(syncFd, mtimeSec, &sendErr);
        ::close(syncFd);
        ::unlink(tmpPath.c_str());
        if (!ok) {
          if (errOut)
            *errOut = sendErr.empty() ? "ADB SEND failed" : sendErr;
          return false;
        }
      } catch (...) {
        setActiveIoFd(-1);
        ::close(syncFd);
        ::unlink(tmpPath.c_str());
        throw;
      }
    }

    if ((fileFlags & rfb::tight2FlagExec) != 0 && !targetPath.empty()) {
      try {
        adb_.runShell(serial_,
                      "chmod 755 -- " + AdbClient::quoteShellArg(targetPath),
                      features_.shellV2, kAdbTransferTimeoutMs);
      } catch (const std::exception&) {
      }
    }
    return true;
  } catch (const std::exception& e) {
    if (errOut)
      *errOut = e.what();
    abortUpload();
    return false;
  }
}

void AdbFileTransferSession::handleTight2UploadStart(const std::string& rawPath,
                                                     uint8_t flags,
                                                     uint64_t offset)
{
  std::string err;
  bool overwrite = (flags & 0x01u) != 0;
  if (!openUploadInternal(rawPath, overwrite, offset, &err)) {
    Reply r;
    r.kind = Reply::Kind::Tight2Failed;
    r.text = err;
    postReply(std::move(r));
    return;
  }

  Reply r;
  r.kind = Reply::Kind::Tight2UploadStart;
  postReply(std::move(r));
}

void AdbFileTransferSession::handleTight2UploadData(
  const std::vector<uint8_t>& data)
{
  std::string err;
  if (!writeUploadChunkInternal(data, &err)) {
    abortUpload();
    Reply r;
    r.kind = Reply::Kind::Tight2Failed;
    r.text = err;
    postReply(std::move(r));
    return;
  }

  Reply r;
  r.kind = Reply::Kind::Tight2UploadData;
  postReply(std::move(r));
}

void AdbFileTransferSession::handleTight2UploadEnd(uint16_t fileFlags,
                                                   uint64_t modTimeMs)
{
  std::string err;
  if (!finishUploadInternal(fileFlags, modTimeMs, &err)) {
    Reply r;
    r.kind = Reply::Kind::Tight2Failed;
    r.text = err;
    postReply(std::move(r));
    return;
  }

  Reply r;
  r.kind = Reply::Kind::Tight2UploadEnd;
  postReply(std::move(r));
}

void AdbFileTransferSession::handleTight2Mkdir(const std::string& rawPath)
{
  std::string devicePath;
  std::string virtualPath;
  std::string err;
  if (!normalizePath(rawPath, rootDir_, false, &devicePath, &virtualPath, &err)) {
    Reply r;
    r.kind = Reply::Kind::Tight2Failed;
    r.text = err;
    postReply(std::move(r));
    return;
  }
  if (virtualPath == "/") {
    Reply r;
    r.kind = Reply::Kind::Tight2Failed;
    r.text = "Cannot create root directory";
    postReply(std::move(r));
    return;
  }

  try {
    ensureFeatures();
    AdbShellResult res = adb_.runShell(
      serial_, "mkdir -- " + AdbClient::quoteShellArg(devicePath),
      features_.shellV2, kAdbTransferTimeoutMs);
    Reply r;
    if (res.exitCode != 0) {
      r.kind = Reply::Kind::Tight2Failed;
      r.text = extractShellError(res, "mkdir failed");
    } else {
      r.kind = Reply::Kind::Tight2Mkdir;
    }
    postReply(std::move(r));
  } catch (const std::exception& e) {
    Reply r;
    r.kind = Reply::Kind::Tight2Failed;
    r.text = e.what();
    postReply(std::move(r));
  }
}

void AdbFileTransferSession::handleTight2Remove(const std::string& rawPath)
{
  std::string devicePath;
  std::string virtualPath;
  std::string err;
  if (!normalizePath(rawPath, rootDir_, false, &devicePath, &virtualPath, &err)) {
    Reply r;
    r.kind = Reply::Kind::Tight2Failed;
    r.text = err;
    postReply(std::move(r));
    return;
  }
  if (virtualPath == "/") {
    Reply r;
    r.kind = Reply::Kind::Tight2Failed;
    r.text = "Cannot remove root directory";
    postReply(std::move(r));
    return;
  }
  if (isPathInUse(devicePath)) {
    Reply r;
    r.kind = Reply::Kind::Tight2Failed;
    r.text = "File is currently open for transfer";
    postReply(std::move(r));
    return;
  }

  try {
    ensureFeatures();
    AdbShellResult res = adb_.runShell(
      serial_, "rm -r -- " + AdbClient::quoteShellArg(devicePath),
      features_.shellV2, kAdbTransferTimeoutMs);
    Reply r;
    if (res.exitCode != 0) {
      r.kind = Reply::Kind::Tight2Failed;
      r.text = extractShellError(res, "rm failed");
    } else {
      r.kind = Reply::Kind::Tight2Remove;
    }
    postReply(std::move(r));
  } catch (const std::exception& e) {
    Reply r;
    r.kind = Reply::Kind::Tight2Failed;
    r.text = e.what();
    postReply(std::move(r));
  }
}

void AdbFileTransferSession::handleTight2Rename(const std::string& oldRaw,
                                                const std::string& newRaw)
{
  std::string oldDevice;
  std::string oldVirtual;
  std::string newDevice;
  std::string newVirtual;
  std::string err;

  if (!normalizePath(oldRaw, rootDir_, false, &oldDevice, &oldVirtual, &err) ||
      !normalizePath(newRaw, rootDir_, false, &newDevice, &newVirtual, &err)) {
    Reply r;
    r.kind = Reply::Kind::Tight2Failed;
    r.text = err;
    postReply(std::move(r));
    return;
  }
  if (oldVirtual == "/" || newVirtual == "/") {
    Reply r;
    r.kind = Reply::Kind::Tight2Failed;
    r.text = "Cannot rename root directory";
    postReply(std::move(r));
    return;
  }
  if (isPathInUse(oldDevice) || isPathInUse(newDevice)) {
    Reply r;
    r.kind = Reply::Kind::Tight2Failed;
    r.text = "File is currently open for transfer";
    postReply(std::move(r));
    return;
  }
  if (oldDevice == newDevice) {
    Reply r;
    r.kind = Reply::Kind::Tight2Rename;
    postReply(std::move(r));
    return;
  }

  int syncFd = -1;
  try {
    ensureFeatures();
    syncFd = adb_.openSync(serial_, kAdbTransferTimeoutMs);
    AdbStat srcSt;
    AdbStat dstSt;
    bool srcOk = false;
    bool dstOk = false;
    {
      IoFdScope scope([this](int f) { setActiveIoFd(f); }, syncFd);
      srcOk = AdbClient::syncStat(syncFd, oldDevice, features_.statV2,
                                  false, &srcSt);
      dstOk = AdbClient::syncStat(syncFd, newDevice, features_.statV2,
                                  true, &dstSt);
      AdbClient::syncQuit(syncFd);
    }
    ::close(syncFd);
    syncFd = -1;

    if (!srcOk) {
      Reply r;
      r.kind = Reply::Kind::Tight2Failed;
      r.text = srcSt.error ? std::strerror(static_cast<int>(srcSt.error))
                           : "Source path not found";
      postReply(std::move(r));
      return;
    }
    if (dstOk && S_ISDIR(dstSt.mode)) {
      Reply r;
      r.kind = Reply::Kind::Tight2Failed;
      r.text = "Destination is an existing directory";
      postReply(std::move(r));
      return;
    }

    AdbShellResult res = adb_.runShell(
      serial_,
      "mv -- " + AdbClient::quoteShellArg(oldDevice) + " " +
      AdbClient::quoteShellArg(newDevice),
      features_.shellV2, kAdbTransferTimeoutMs);
    Reply r;
    if (res.exitCode != 0) {
      r.kind = Reply::Kind::Tight2Failed;
      r.text = extractShellError(res, "mv failed");
    } else {
      r.kind = Reply::Kind::Tight2Rename;
    }
    postReply(std::move(r));
  } catch (const std::exception& e) {
    if (syncFd >= 0) {
      setActiveIoFd(-1);
      ::close(syncFd);
    }
    Reply r;
    r.kind = Reply::Kind::Tight2Failed;
    r.text = e.what();
    postReply(std::move(r));
  }
}

bool AdbFileTransferSession::computeDirSizeFallback(int syncFd,
                                                    const std::string& dirPath,
                                                    int depth,
                                                    uint64_t* totalOut)
{
  if (depth > kMaxDirSizeDepth)
    return false;
  std::vector<AdbDirEntry> entries =
    AdbClient::syncList(syncFd, dirPath, features_.lsV2, features_.statV2);
  std::string prefix = (dirPath == "/") ? "/" : (dirPath + "/");
  for (const AdbDirEntry& e : entries) {
    if (S_ISDIR(e.mode)) {
      if (!computeDirSizeFallback(syncFd, prefix + e.name, depth + 1, totalOut))
        return false;
    } else {
      *totalOut += e.size;
    }
  }
  return true;
}

void AdbFileTransferSession::handleTight2DirSize(const std::string& rawPath)
{
  std::string devicePath;
  std::string err;
  if (!normalizePath(rawPath, rootDir_, true, &devicePath, nullptr, &err)) {
    Reply r;
    r.kind = Reply::Kind::Tight2Failed;
    r.text = err;
    postReply(std::move(r));
    return;
  }

  int syncFd = -1;
  try {
    ensureFeatures();
    syncFd = adb_.openSync(serial_, kAdbTransferTimeoutMs);
    AdbStat st;
    bool okDir = false;
    {
      IoFdScope scope([this](int f) { setActiveIoFd(f); }, syncFd);
      okDir = AdbClient::syncStat(syncFd, devicePath, features_.statV2, true, &st) &&
              S_ISDIR(st.mode);
      AdbClient::syncQuit(syncFd);
    }
    ::close(syncFd);
    syncFd = -1;

    if (!okDir) {
      Reply r;
      r.kind = Reply::Kind::Tight2Failed;
      r.text = (st.error != 0) ? std::strerror(static_cast<int>(st.error))
                               : "Path is not a directory";
      postReply(std::move(r));
      return;
    }

    AdbShellResult res = adb_.runShell(
      serial_,
      "find -H " + AdbClient::quoteShellArg(devicePath) +
        " -type f -printf '%s\\n'",
      features_.shellV2, kAdbTransferTimeoutMs);
    if (res.exitCode == 0) {
      uint64_t total = 0;
      bool parsedOk = true;
      size_t pos = 0;
      while (pos < res.stdoutData.size()) {
        size_t eol = res.stdoutData.find('\n', pos);
        if (eol == std::string::npos)
          eol = res.stdoutData.size();
        std::string line = res.stdoutData.substr(pos, eol - pos);
        pos = eol + 1;

        while (!line.empty() &&
               (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
          line.pop_back();
        size_t lead = 0;
        while (lead < line.size() &&
               (line[lead] == ' ' || line[lead] == '\t'))
          lead++;
        if (lead == line.size())
          continue;

        char* endPtr = nullptr;
        errno = 0;
        unsigned long long val =
          std::strtoull(line.c_str() + lead, &endPtr, 10);
        if (errno != 0 || endPtr == line.c_str() + lead || *endPtr != '\0') {
          parsedOk = false;
          break;
        }
        if (val > UINT64_MAX - total)
          total = UINT64_MAX;
        else
          total += static_cast<uint64_t>(val);
      }
      if (parsedOk) {
        Reply r;
        r.kind = Reply::Kind::Tight2DirSize;
        r.u64Val = total;
        postReply(std::move(r));
        return;
      }
    }

    // Fallback: recursive syncList sum
    syncFd = adb_.openSync(serial_, kAdbTransferTimeoutMs);
    uint64_t total = 0;
    {
      IoFdScope scope([this](int f) { setActiveIoFd(f); }, syncFd);
      computeDirSizeFallback(syncFd, devicePath, 0, &total);
      AdbClient::syncQuit(syncFd);
    }
    ::close(syncFd);
    syncFd = -1;

    Reply r;
    r.kind = Reply::Kind::Tight2DirSize;
    r.u64Val = total;
    postReply(std::move(r));
  } catch (const std::exception& e) {
    if (syncFd >= 0) {
      setActiveIoFd(-1);
      ::close(syncFd);
    }
    Reply r;
    r.kind = Reply::Kind::Tight2Failed;
    r.text = e.what();
    postReply(std::move(r));
  }
}

void AdbFileTransferSession::handleTight13FileList(uint8_t flags,
                                                   const std::string& rawPath)
{
  std::string devicePath;
  std::string err;
  if (!normalizePath(rawPath, rootDir_, true, &devicePath, nullptr, &err)) {
    Reply r;
    r.kind = Reply::Kind::Tight13FileList;
    r.flags = flags | rfb::tight13FlagError;
    postReply(std::move(r));
    return;
  }

  int syncFd = -1;
  try {
    ensureFeatures();
    syncFd = adb_.openSync(serial_, kAdbTransferTimeoutMs);
    IoFdScope scope([this](int f) { setActiveIoFd(f); }, syncFd);

    AdbStat st;
    if (!AdbClient::syncStat(syncFd, devicePath, features_.statV2, true, &st) ||
        !S_ISDIR(st.mode)) {
      AdbClient::syncQuit(syncFd);
      ::close(syncFd);
      Reply r;
      r.kind = Reply::Kind::Tight13FileList;
      r.flags = flags | rfb::tight13FlagError;
      postReply(std::move(r));
      return;
    }

    std::vector<AdbDirEntry> raw =
      AdbClient::syncList(syncFd, devicePath, features_.lsV2, features_.statV2);
    AdbClient::syncQuit(syncFd);
    ::close(syncFd);
    syncFd = -1;

    Reply r;
    r.kind = Reply::Kind::Tight13FileList;
    r.flags = flags & static_cast<uint8_t>(~rfb::tight13FlagError);
    r.entries.reserve(raw.size());
    for (const AdbDirEntry& e : raw) {
      bool isDir = S_ISDIR(e.mode);
      if ((flags & rfb::tight13FlagDirOnly) != 0 && !isDir)
        continue;
      rfb::FileEntry fe;
      fe.name = e.name;
      fe.isDir = isDir;
      fe.size = isDir ? 0 : e.size;
      fe.mtime = e.mtime;
      fe.isExecutable = !isDir && ((e.mode & 0111u) != 0);
      r.entries.push_back(std::move(fe));
    }
    postReply(std::move(r));
  } catch (const std::exception&) {
    if (syncFd >= 0) {
      setActiveIoFd(-1);
      ::close(syncFd);
    }
    Reply r;
    r.kind = Reply::Kind::Tight13FileList;
    r.flags = flags | rfb::tight13FlagError;
    postReply(std::move(r));
  }
}

void AdbFileTransferSession::handleTight13DownloadStart(
  uint8_t compressLevel, uint32_t position, const std::string& rawPath)
{
  std::string err;
  if (!openDownloadInternal(rawPath, position, false, &err)) {
    Reply r;
    r.kind = Reply::Kind::Tight13DownloadFailed;
    r.text = err;
    postReply(std::move(r));
    return;
  }

  tight13DownloadActive_ = true;
  downloadCompressLevel_ = compressLevel;
  handleTight13DownloadNextChunk();
}

void AdbFileTransferSession::handleTight13DownloadNextChunk()
{
  if (!tight13DownloadActive_ || downloadSyncFd_ < 0)
    return;

  std::vector<uint8_t> chunk;
  bool eof = false;
  std::string err;
  if (!readDownloadChunk(kTight13DownloadChunk, &chunk, &eof, &err)) {
    closeDownload();
    Reply r;
    r.kind = Reply::Kind::Tight13DownloadFailed;
    r.text = err;
    postReply(std::move(r));
    return;
  }

  if (eof) {
    uint32_t mtimeSec = static_cast<uint32_t>(downloadMtimeMs_ / 1000ULL);
    closeDownload();
    Reply r;
    r.kind = Reply::Kind::Tight13DownloadEnd;
    r.u32Val = mtimeSec;
    postReply(std::move(r));
    return;
  }

  Reply r;
  r.kind = Reply::Kind::Tight13DownloadData;
  r.compressLevel = downloadCompressLevel_;
  r.data = std::move(chunk);
  postReply(std::move(r));
}

void AdbFileTransferSession::handleTight13UploadStart(uint32_t position,
                                                    const std::string& rawPath)
{
  std::string err;
  bool overwrite = (position == 0);
  if (!openUploadInternal(rawPath, overwrite, position, &err)) {
    Reply r;
    r.kind = Reply::Kind::Tight13UploadCancel;
    r.text = err;
    postReply(std::move(r));
    return;
  }
  tight13UploadActive_ = true;
}

void AdbFileTransferSession::handleTight13UploadData(
  const std::vector<uint8_t>& data)
{
  if (!tight13UploadActive_)
    return;
  std::string err;
  if (!writeUploadChunkInternal(data, &err)) {
    abortUpload();
    Reply r;
    r.kind = Reply::Kind::Tight13UploadCancel;
    r.text = err;
    postReply(std::move(r));
  }
}

void AdbFileTransferSession::handleTight13UploadEnd(uint32_t modTimeSec)
{
  if (!tight13UploadActive_)
    return;
  std::string err;
  if (!finishUploadInternal(0, static_cast<uint64_t>(modTimeSec) * 1000ULL,
                            &err)) {
    Reply r;
    r.kind = Reply::Kind::Tight13UploadCancel;
    r.text = err;
    postReply(std::move(r));
    return;
  }
  tight13UploadActive_ = false;
}

void AdbFileTransferSession::handleTight13CreateDir(const std::string& rawPath)
{
  std::string devicePath;
  std::string virtualPath;
  std::string err;
  if (!normalizePath(rawPath, rootDir_, false, &devicePath, &virtualPath, &err) ||
      virtualPath == "/") {
    vlog.error("Tight 1.3 mkdir rejected '%s': %s", rawPath.c_str(),
               err.empty() ? "invalid path" : err.c_str());
    return;
  }

  try {
    ensureFeatures();
    AdbShellResult res = adb_.runShell(
      serial_, "mkdir -- " + AdbClient::quoteShellArg(devicePath),
      features_.shellV2, kAdbTransferTimeoutMs);
    if (res.exitCode != 0) {
      vlog.error("Tight 1.3 mkdir '%s' failed: %s", devicePath.c_str(),
                 extractShellError(res, "mkdir failed").c_str());
    }
  } catch (const std::exception& e) {
    vlog.error("Tight 1.3 mkdir '%s' failed: %s", devicePath.c_str(), e.what());
  }
}

void AdbFileTransferSession::enqueueTight2CompressionSupport()
{
  Request req;
  req.kind = Request::Kind::Tight2CompressionSupport;
  enqueue(std::move(req));
}

void AdbFileTransferSession::enqueueTight2FileList(uint8_t compressLevel,
                                                   const std::string& path)
{
  Request req;
  req.kind = Request::Kind::Tight2FileList;
  req.u8Val = compressLevel;
  req.path = path;
  enqueue(std::move(req));
}

void AdbFileTransferSession::enqueueTight2Md5(const std::string& path,
                                              uint64_t offset,
                                              uint64_t dataLen)
{
  Request req;
  req.kind = Request::Kind::Tight2Md5;
  req.path = path;
  req.u64Val = offset;
  req.u64Val2 = dataLen;
  enqueue(std::move(req));
}

void AdbFileTransferSession::enqueueTight2UploadStart(const std::string& path,
                                                      uint8_t flags,
                                                      uint64_t offset)
{
  Request req;
  req.kind = Request::Kind::Tight2UploadStart;
  req.path = path;
  req.u8Val = flags;
  req.u64Val = offset;
  enqueue(std::move(req));
}

void AdbFileTransferSession::enqueueTight2UploadData(const uint8_t* data,
                                                     size_t len)
{
  Request req;
  req.kind = Request::Kind::Tight2UploadData;
  if (data && len > 0)
    req.data.assign(data, data + len);
  enqueue(std::move(req));
}

void AdbFileTransferSession::enqueueTight2UploadEnd(uint16_t fileFlags,
                                                    uint64_t modTimeMs)
{
  Request req;
  req.kind = Request::Kind::Tight2UploadEnd;
  req.u16Val = fileFlags;
  req.u64Val = modTimeMs;
  enqueue(std::move(req));
}

void AdbFileTransferSession::enqueueTight2DownloadStart(const std::string& path,
                                                        uint64_t offset)
{
  Request req;
  req.kind = Request::Kind::Tight2DownloadStart;
  req.path = path;
  req.u64Val = offset;
  enqueue(std::move(req));
}

void AdbFileTransferSession::enqueueTight2DownloadData(uint8_t compressLevel,
                                                       uint32_t reqSize)
{
  Request req;
  req.kind = Request::Kind::Tight2DownloadData;
  req.u8Val = compressLevel;
  req.u32Val = reqSize;
  enqueue(std::move(req));
}

void AdbFileTransferSession::enqueueTight2Mkdir(const std::string& path)
{
  Request req;
  req.kind = Request::Kind::Tight2Mkdir;
  req.path = path;
  enqueue(std::move(req));
}

void AdbFileTransferSession::enqueueTight2Remove(const std::string& path)
{
  Request req;
  req.kind = Request::Kind::Tight2Remove;
  req.path = path;
  enqueue(std::move(req));
}

void AdbFileTransferSession::enqueueTight2Rename(const std::string& oldPath,
                                                 const std::string& newPath)
{
  Request req;
  req.kind = Request::Kind::Tight2Rename;
  req.path = oldPath;
  req.path2 = newPath;
  enqueue(std::move(req));
}

void AdbFileTransferSession::enqueueTight2DirSize(const std::string& path)
{
  Request req;
  req.kind = Request::Kind::Tight2DirSize;
  req.path = path;
  enqueue(std::move(req));
}

void AdbFileTransferSession::enqueueTight13FileList(uint8_t flags,
                                                    const std::string& dir)
{
  Request req;
  req.kind = Request::Kind::Tight13FileList;
  req.u8Val = flags;
  req.path = dir;
  enqueue(std::move(req));
}

void AdbFileTransferSession::enqueueTight13DownloadStart(uint8_t compressLevel,
                                                         uint32_t position,
                                                         const std::string& path)
{
  Request req;
  req.kind = Request::Kind::Tight13DownloadStart;
  req.u8Val = compressLevel;
  req.u32Val = position;
  req.path = path;
  enqueue(std::move(req));
}

void AdbFileTransferSession::enqueueTight13DownloadNextChunk()
{
  Request req;
  req.kind = Request::Kind::Tight13DownloadNextChunk;
  enqueue(std::move(req));
}

void AdbFileTransferSession::enqueueTight13DownloadCancel()
{
  Request req;
  req.kind = Request::Kind::Tight13DownloadCancel;
  enqueue(std::move(req));
}

void AdbFileTransferSession::enqueueTight13UploadStart(uint32_t position,
                                                       const std::string& path)
{
  Request req;
  req.kind = Request::Kind::Tight13UploadStart;
  req.u32Val = position;
  req.path = path;
  enqueue(std::move(req));
}

void AdbFileTransferSession::enqueueTight13UploadData(const uint8_t* data,
                                                      size_t len)
{
  Request req;
  req.kind = Request::Kind::Tight13UploadData;
  if (data && len > 0)
    req.data.assign(data, data + len);
  enqueue(std::move(req));
}

void AdbFileTransferSession::enqueueTight13UploadEnd(uint32_t modTimeSec)
{
  Request req;
  req.kind = Request::Kind::Tight13UploadEnd;
  req.u32Val = modTimeSec;
  enqueue(std::move(req));
}

void AdbFileTransferSession::enqueueTight13UploadFailed()
{
  Request req;
  req.kind = Request::Kind::Tight13UploadFailed;
  enqueue(std::move(req));
}

void AdbFileTransferSession::enqueueTight13CreateDir(const std::string& dir)
{
  Request req;
  req.kind = Request::Kind::Tight13CreateDir;
  req.path = dir;
  enqueue(std::move(req));
}

void AdbFileTransferSession::enqueueFenceBarrier(uint32_t flags,
                                                 const uint8_t* data,
                                                 size_t len)
{
  Request req;
  req.kind = Request::Kind::FenceBarrier;
  req.u32Val = flags;
  if (data && len > 0)
    req.data.assign(data, data + len);
  enqueue(std::move(req));
}
