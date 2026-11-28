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

#include <assert.h>
#include <ctype.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <stdexcept>

#include <zlib.h>

#include <core/LogWriter.h>
#include <core/string.h>

#include <rfb/CMsgWriter.h>
#include <rfb/FileTransferClient.h>
#include <rfb/ServerParams.h>
#include <rfb/fenceTypes.h>
#include <rfb/msgTypes.h>

using namespace rfb;

static core::LogWriter vlog("FileTransferClient");

static const size_t maxUltraUncompressedChunk = 16 * 1024 * 1024;

static uint32_t readLE32(const uint8_t* p)
{
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint16_t readLE16(const uint8_t* p)
{
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static std::vector<uint8_t> inflateUltraPacket(const uint8_t* data, size_t len)
{
  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  if (inflateInit(&zs) != Z_OK)
    throw std::runtime_error("zlib inflateInit failed");

  zs.next_in = const_cast<Bytef*>(data);
  zs.avail_in = (uInt)len;

  std::vector<uint8_t> out;
  uint8_t buf[16384];
  int rc = Z_OK;
  while (rc == Z_OK) {
    zs.next_out = buf;
    zs.avail_out = sizeof(buf);
    rc = inflate(&zs, Z_NO_FLUSH);
    size_t produced = sizeof(buf) - zs.avail_out;
    if (out.size() + produced > maxUltraUncompressedChunk) {
      inflateEnd(&zs);
      throw std::runtime_error("Compressed UltraVNC packet exceeds size limit");
    }
    out.insert(out.end(), buf, buf + produced);
  }
  inflateEnd(&zs);
  if (rc != Z_STREAM_END)
    throw std::runtime_error("Corrupt compressed UltraVNC file packet");

  return out;
}

FileTransferClient::FileTransferClient(ServerParams* server)
  : server_(server),
    writer_(nullptr),
    handler_(nullptr),
    preferredProtocol_(FileTransferProtocol::Unsupported),
    ready_(false),
    serverInitDone_(false),
    opState_(OpState::Idle),
    commandKind_(CommandKind::NoneOp),
    tight2Available_(false),
    tight2CompressionProbeCap_(false),
    tight2ReplyPending_(false),
    tight2CancelAction_(Tight2CancelAction::NoCancel),
    tight13Available_(false),
    tight13SyncPending_(false),
    downloadStarted_(false),
    uploadReady_(false),
    tight13StaleListReplies_(0),
    ultraProbeSent_(false),
    ultraProbeInFlight_(false),
    ultraSessionStarted_(false),
    ultraDrivesListing_(false),
    ultraDirEchoReceived_(false),
    ultraDownloadSkipBytes_(0)
{
}

void FileTransferClient::reset()
{
  caps_ = FileTransferCaps();
  ready_ = false;
  serverInitDone_ = false;
  opState_ = OpState::Idle;
  commandKind_ = CommandKind::NoneOp;
  tight2Available_ = false;
  tight2CompressionProbeCap_ = false;
  tight2ReplyPending_ = false;
  tight2CancelAction_ = Tight2CancelAction::NoCancel;
  tight13Available_ = false;
  tight13SyncPending_ = false;
  downloadStarted_ = false;
  uploadReady_ = false;
  tight13StaleListReplies_ = 0;
  tight13ExpectedMkdirBase_.clear();
  ultraProbeSent_ = false;
  ultraProbeInFlight_ = false;
  ultraSessionStarted_ = false;
  ultraDrivesListing_ = false;
  ultraDirEchoReceived_ = false;
  ultraDownloadSkipBytes_ = 0;
  pendingEntries_.clear();
  if (server_ != nullptr)
    server_->fileTransferCaps = caps_;
}

std::string FileTransferClient::toTight2Path(const std::string& ftpPath)
{
  if (ftpPath.empty() || ftpPath == "/")
    return "/";
  std::string out = ftpPath;
  if (out[0] != '/')
    out.insert(out.begin(), '/');
  while (out.size() > 1 && out.back() == '/')
    out.pop_back();
  return out;
}

std::string FileTransferClient::toTight13Path(const std::string& ftpPath)
{
  if (ftpPath.empty() || ftpPath == "/")
    return "/";
  std::string out = ftpPath;
  if (out[0] != '/')
    out.insert(out.begin(), '/');
  while (out.size() > 1 && out.back() == '/')
    out.pop_back();
  return out;
}

std::string FileTransferClient::toUltraPath(const std::string& ftpPath, bool isDir)
{
  std::string p = ftpPath;
  if (p.empty())
    p = "/";
  if (p[0] == '/')
    p.erase(p.begin());
  while (!p.empty() && p.back() == '/')
    p.pop_back();

  std::string drive;
  std::string rest;
  if (p.size() >= 2 && isalpha((unsigned char)p[0]) && p[1] == ':' &&
      (p.size() == 2 || p[2] == '/')) {
    drive = p.substr(0, 2);
    if (p.size() > 2)
      rest = p.substr(3);
  } else {
    drive = "C:";
    rest = p;
  }

  for (size_t i = 0; i < rest.size(); i++) {
    if (rest[i] == '/')
      rest[i] = '\\';
  }

  std::string out = drive + "\\";
  if (!rest.empty()) {
    out += rest;
    if (isDir)
      out += "\\";
  }
  return out;
}

std::string FileTransferClient::decodeUltraFileName(const uint8_t* buf,
                                                    size_t len,
                                                    bool isUnicode)
{
  if (!isUnicode) {
    size_t slen = strnlen((const char*)buf, len);
    std::string raw((const char*)buf, slen);
    if (core::isValidUTF8(raw.c_str(), raw.size()))
      return raw;
    return core::latin1ToUTF8(raw.c_str(), raw.size());
  }

  std::string out;
  size_t i = 0;
  while (i + 1 < len) {
    uint16_t u1 = readLE16(buf + i);
    i += 2;
    if (u1 == 0)
      break;
    unsigned codePoint = u1;
    if (u1 >= 0xD800 && u1 <= 0xDBFF && i + 1 < len) {
      uint16_t u2 = readLE16(buf + i);
      if (u2 >= 0xDC00 && u2 <= 0xDFFF) {
        i += 2;
        codePoint = 0x10000U + (((unsigned)(u1 - 0xD800) << 10) |
                                (unsigned)(u2 - 0xDC00));
      }
    }
    char utf8[5];
    size_t n = core::ucs4ToUTF8(codePoint, utf8);
    out.append(utf8, n);
  }
  return out;
}

int64_t FileTransferClient::decodeUltraFileTime(uint32_t low, uint32_t high)
{
  if (high == 0)
    return (int64_t)low;

  uint64_t ft64 = ((uint64_t)high << 32) | (uint64_t)low;
  const uint64_t epochDiff = 116444736000000000ULL;
  if (ft64 < epochDiff)
    return 0;
  return (int64_t)((ft64 - epochDiff) / 10000000ULL);
}

std::string FileTransferClient::formatUltraTimeSuffix(time_t t)
{
  struct tm tmVal;
  memset(&tmVal, 0, sizeof(tmVal));
#ifdef _WIN32
  gmtime_s(&tmVal, &t);
#else
  gmtime_r(&t, &tmVal);
#endif
  char buf[32];
  strftime(buf, sizeof(buf), "%m/%d/%Y %H:%M", &tmVal);
  return std::string(buf);
}

void FileTransferClient::onServerTightCaps(
  const std::vector<TightCapability>& serverMsgs,
  const std::vector<TightCapability>& clientMsgs,
  const std::vector<TightCapability>& encodings)
{
  server_->serverMsgCaps = serverMsgs;
  server_->clientMsgCaps = clientMsgs;
  server_->encodingCaps = encodings;

  tight2Available_ =
    server_->hasServerMsgCap((int32_t)tight2FileListReply, tightVendorTight,
                             sigTight2FileListReply) &&
    server_->hasClientMsgCap((int32_t)tight2FileListReq, tightVendorTight,
                             sigTight2FileListReq);

  tight2CompressionProbeCap_ =
    server_->hasClientMsgCap((int32_t)tight2CompressionSupportReq,
                             tightVendorTight, sigTight2CompressionSupportReq) &&
    server_->hasServerMsgCap((int32_t)tight2CompressionSupportReply,
                             tightVendorTight, sigTight2CompressionSupportReply);

  tight13Available_ =
    server_->hasServerMsgCap(tight13CapFileListData, tightVendorTight,
                             sigTight13FileListData) &&
    server_->hasClientMsgCap(tight13CapFileListReq, tightVendorTight,
                             sigTight13FileListReq);

  FileTransferProtocol chosen = FileTransferProtocol::Unsupported;
  if (preferredProtocol_ == FileTransferProtocol::Tight2 && tight2Available_)
    chosen = FileTransferProtocol::Tight2;
  else if (preferredProtocol_ == FileTransferProtocol::Tight13 && tight13Available_)
    chosen = FileTransferProtocol::Tight13;
  else if (preferredProtocol_ == FileTransferProtocol::Unsupported) {
    if (tight2Available_)
      chosen = FileTransferProtocol::Tight2;
    else if (tight13Available_)
      chosen = FileTransferProtocol::Tight13;
  }

  if (chosen == FileTransferProtocol::Tight2) {
    caps_ = FileTransferCaps();
    caps_.protocol = FileTransferProtocol::Tight2;
    caps_.canList = true;
    caps_.canDownload =
      server_->hasClientMsgCap((int32_t)tight2DownloadStartReq, tightVendorTight,
                               sigTight2DownloadStartReq) &&
      server_->hasClientMsgCap((int32_t)tight2DownloadDataReq, tightVendorTight,
                               sigTight2DownloadDataReq);
    caps_.canUpload =
      server_->hasClientMsgCap((int32_t)tight2UploadStartReq, tightVendorTight,
                               sigTight2UploadStartReq) &&
      server_->hasClientMsgCap((int32_t)tight2UploadDataReq, tightVendorTight,
                               sigTight2UploadDataReq) &&
      server_->hasClientMsgCap((int32_t)tight2UploadEndReq, tightVendorTight,
                               sigTight2UploadEndReq);
    caps_.canResumeDownload = caps_.canDownload;
    caps_.canResumeUpload = caps_.canUpload;
    caps_.canMkdir =
      server_->hasClientMsgCap((int32_t)tight2MkdirReq, tightVendorTight,
                               sigTight2MkdirReq);
    caps_.canDelete =
      server_->hasClientMsgCap((int32_t)tight2RemoveReq, tightVendorTight,
                               sigTight2RemoveReq);
    caps_.canRename =
      server_->hasClientMsgCap((int32_t)tight2RenameReq, tightVendorTight,
                               sigTight2RenameReq);
    caps_.canCompress = false;
    server_->fileTransferCaps = caps_;
    vlog.info("Detected TightVNC 2.x file transfer capabilities");
  } else if (chosen == FileTransferProtocol::Tight13) {
    caps_ = FileTransferCaps();
    caps_.protocol = FileTransferProtocol::Tight13;
    caps_.canList = true;
    caps_.canDownload =
      server_->hasClientMsgCap(tight13CapFileDownloadReq, tightVendorTight,
                               sigTight13FileDownloadReq);
    caps_.canUpload =
      server_->hasClientMsgCap(tight13CapFileUploadReq, tightVendorTight,
                               sigTight13FileUploadReq) &&
      server_->hasClientMsgCap(tight13CapFileUploadData, tightVendorTight,
                               sigTight13FileUploadData);
    caps_.canResumeDownload = caps_.canDownload;
    caps_.canResumeUpload = caps_.canUpload;
    // LibVNCServer handles FTC_FCDR (136) even though it omits it from
    // InteractionCaps client message list.
    caps_.canMkdir = true;
    caps_.canDelete = false;
    caps_.canRename = false;
    caps_.canCompress = false;
    server_->fileTransferCaps = caps_;
    vlog.info("Detected TightVNC 1.3 file transfer capabilities");
  }
}

void FileTransferClient::onServerInit()
{
  serverInitDone_ = true;

  if (caps_.protocol == FileTransferProtocol::Tight2) {
    if (tight2CompressionProbeCap_ && writer_ != nullptr) {
      opState_ = OpState::Probing;
      tight2ReplyPending_ = true;
      writer_->writeTight2CompressionSupportRequest();
    } else {
      ready_ = true;
      if (handler_ != nullptr)
        handler_->onFileTransferReady(caps_);
    }
    return;
  }

  if (caps_.protocol == FileTransferProtocol::Tight13) {
    ready_ = true;
    if (handler_ != nullptr)
      handler_->onFileTransferReady(caps_);
    return;
  }

  if (preferredProtocol_ == FileTransferProtocol::Ultra &&
      !ultraProbeSent_ && writer_ != nullptr) {
    probeUltraVnc();
  }
}

void FileTransferClient::onSupportedMessages(const uint8_t client2server[32],
                                             const uint8_t /*server2client*/[32])
{
  bool hasFileTransfer = (client2server[msgTypeFileTransfer / 8] &
                          (1 << (msgTypeFileTransfer % 8))) != 0;
  if (!hasFileTransfer)
    return;

  if (caps_.protocol == FileTransferProtocol::Unsupported &&
      (preferredProtocol_ == FileTransferProtocol::Unsupported ||
       preferredProtocol_ == FileTransferProtocol::Ultra)) {
    caps_ = FileTransferCaps();
    caps_.protocol = FileTransferProtocol::Ultra;
    if (caps_.ultraVersion == 0)
      caps_.ultraVersion = 2;
    server_->fileTransferCaps = caps_;
    vlog.info("Server advertised UltraVNC FileTransfer via SupportedMessages");
    if (serverInitDone_ && !ultraProbeSent_)
      probeUltraVnc();
  }
}

void FileTransferClient::probeUltraVnc()
{
  if (writer_ == nullptr || ultraProbeSent_)
    return;
  if (preferredProtocol_ != FileTransferProtocol::Unsupported &&
      preferredProtocol_ != FileTransferProtocol::Ultra)
    return;

  ultraProbeSent_ = true;
  ultraProbeInFlight_ = true;
  if (caps_.protocol == FileTransferProtocol::Unsupported) {
    caps_.protocol = FileTransferProtocol::Ultra;
    if (caps_.ultraVersion == 0)
      caps_.ultraVersion = 2;
    server_->fileTransferCaps = caps_;
  }
  if (opState_ == OpState::Idle)
    opState_ = OpState::Probing;

  // UltraVNC permission probe: AbortFileTransfer with contentParam >= 1
  writer_->writeUltraFileTransfer(ultraAbortFileTransfer, 3, 0, nullptr, 0);
}

void FileTransferClient::ensureReadyForOp()
{
  if (!isSupported())
    throw std::runtime_error("RFB file transfer is not supported");
  if (writer_ == nullptr)
    throw std::runtime_error("RFB writer is not initialized");
  if (opState_ != OpState::Idle)
    throw std::logic_error("Another RFB file transfer operation is already active");
}

void FileTransferClient::failActiveOp(const std::string& error)
{
  OpState prev = opState_;
  opState_ = OpState::Idle;
  commandKind_ = CommandKind::NoneOp;

  if (handler_ == nullptr)
    return;

  switch (prev) {
  case OpState::Listing:
    handler_->onListComplete(false, std::vector<FileEntry>(), error);
    break;
  case OpState::Downloading:
    if (!downloadStarted_)
      handler_->onDownloadStarted(false, 0, false, error);
    else
      handler_->onDownloadComplete(false, error);
    break;
  case OpState::Uploading:
    if (!uploadReady_)
      handler_->onUploadReady(false, error);
    else
      handler_->onUploadComplete(false, error);
    break;
  case OpState::Command:
    handler_->onCommandComplete(false, error);
    break;
  case OpState::DrainingCancel:
    handler_->onCancelComplete();
    break;
  case OpState::Idle:
  case OpState::Probing:
    break;
  }
}

void FileTransferClient::listDirectory(const std::string& path)
{
  ensureReadyForOp();
  pendingEntries_.clear();
  opState_ = OpState::Listing;

  switch (caps_.protocol) {
  case FileTransferProtocol::Tight2:
    tight2ReplyPending_ = true;
    writer_->writeTight2FileListRequest(caps_.canCompress ? 1 : 0,
                                        toTight2Path(path));
    break;

  case FileTransferProtocol::Tight13:
    writer_->writeTight13FileListRequest(0, toTight13Path(path));
    break;

  case FileTransferProtocol::Ultra: {
    std::string norm = toTight2Path(path);
    if (norm == "/") {
      ultraDrivesListing_ = true;
      writer_->writeUltraFileTransfer(ultraDirContentRequest,
                                      ultraRDrivesList, 0, nullptr, 0);
    } else {
      ultraDrivesListing_ = false;
      ultraDirEchoReceived_ = false;
      std::string wire = toUltraPath(norm, true);
      uint16_t param = ultraRDirContent;
      if (caps_.ultraUnicode)
        param |= ultraRDirContentUnicode;
      writer_->writeUltraFileTransfer(ultraDirContentRequest, param, 0,
                                      (const uint8_t*)wire.data(),
                                      (uint32_t)wire.size());
    }
    break;
  }

  case FileTransferProtocol::Unsupported:
    opState_ = OpState::Idle;
    break;
  }
}

void FileTransferClient::startDownload(const std::string& path, uint64_t offset)
{
  ensureReadyForOp();
  opState_ = OpState::Downloading;
  downloadStarted_ = false;
  ultraDownloadSkipBytes_ = offset;

  switch (caps_.protocol) {
  case FileTransferProtocol::Tight2:
    tight2ReplyPending_ = true;
    writer_->writeTight2DownloadStartRequest(toTight2Path(path), offset);
    break;

  case FileTransferProtocol::Tight13:
    writer_->writeTight13FileDownloadRequest(0, (uint32_t)offset,
                                            toTight13Path(path));
    break;

  case FileTransferProtocol::Ultra: {
    std::string wire = toUltraPath(path, false);
    writer_->writeUltraFileTransfer(ultraFileTransferRequest, 0,
                                    caps_.canCompress ? 1U : 0U,
                                    (const uint8_t*)wire.data(),
                                    (uint32_t)wire.size());
    break;
  }

  case FileTransferProtocol::Unsupported:
    opState_ = OpState::Idle;
    break;
  }
}

void FileTransferClient::requestMoreDownloadData()
{
  if (opState_ != OpState::Downloading)
    return;
  if (caps_.protocol == FileTransferProtocol::Tight2 && !tight2ReplyPending_) {
    tight2ReplyPending_ = true;
    writer_->writeTight2DownloadDataRequest(caps_.canCompress ? 1 : 0, 65536);
  }
}

void FileTransferClient::cancelDownload()
{
  if (opState_ != OpState::Downloading)
    return;

  switch (caps_.protocol) {
  case FileTransferProtocol::Tight2:
    if (tight2ReplyPending_) {
      opState_ = OpState::DrainingCancel;
      tight2CancelAction_ = Tight2CancelAction::CloseDownload;
    } else if (downloadStarted_ && writer_ != nullptr) {
      opState_ = OpState::DrainingCancel;
      tight2CancelAction_ = Tight2CancelAction::NoCancel;
      tight2ReplyPending_ = true;
      writer_->writeTight2DownloadStartRequest("", 0);
    } else {
      tight2CancelAction_ = Tight2CancelAction::NoCancel;
      opState_ = OpState::Idle;
      downloadStarted_ = false;
      if (handler_ != nullptr)
        handler_->onCancelComplete();
    }
    break;

  case FileTransferProtocol::Tight13:
    writer_->writeTight13FileDownloadCancel("Cancelled by client");
    if (server_->supportsFence) {
      // Discard any in-flight data before starting a new download.
      static const uint8_t tag[4] = { 'F', 'C', '1', '3' };
      opState_ = OpState::DrainingCancel;
      writer_->writeFence(fenceFlagRequest | fenceFlagBlockBefore, 4, tag);
      break;
    }
    opState_ = OpState::Idle;
    if (handler_ != nullptr)
      handler_->onCancelComplete();
    break;

  case FileTransferProtocol::Ultra:
    writer_->writeUltraFileTransfer(ultraAbortFileTransfer, 0, 0, nullptr, 0);
    opState_ = OpState::Idle;
    if (handler_ != nullptr)
      handler_->onCancelComplete();
    break;

  case FileTransferProtocol::Unsupported:
    opState_ = OpState::Idle;
    break;
  }
}

void FileTransferClient::startUpload(const std::string& path,
                                     bool overwrite, uint64_t offset)
{
  ensureReadyForOp();
  opState_ = OpState::Uploading;
  uploadReady_ = false;
  tight13SyncPending_ = false;

  switch (caps_.protocol) {
  case FileTransferProtocol::Tight2:
    tight2ReplyPending_ = true;
    writer_->writeTight2UploadStartRequest(toTight2Path(path),
                                           overwrite && (offset == 0), offset);
    break;

  case FileTransferProtocol::Tight13:
    writer_->writeTight13FileUploadRequest(0, (uint32_t)offset,
                                          toTight13Path(path));
    // Tight 1.3 has no upload start reply; immediately ready for chunks
    uploadReady_ = true;
    if (handler_ != nullptr)
      handler_->onUploadReady(true, "");
    break;

  case FileTransferProtocol::Ultra: {
    if (offset > 0) {
      opState_ = OpState::Idle;
      if (handler_ != nullptr)
        handler_->onUploadReady(false, "Resume upload is not supported");
      return;
    }
    std::string wire = toUltraPath(path, false) + "," +
                       formatUltraTimeSuffix(time(nullptr));
    writer_->writeUltraFileTransfer(ultraFileTransferOffer, 0, 0,
                                    (const uint8_t*)wire.data(),
                                    (uint32_t)wire.size(),
                                    caps_.ultraVersion != 1, 0);
    break;
  }

  case FileTransferProtocol::Unsupported:
    opState_ = OpState::Idle;
    break;
  }
}

void FileTransferClient::sendUploadData(const uint8_t* data, size_t len)
{
  if (opState_ != OpState::Uploading || writer_ == nullptr)
    return;

  switch (caps_.protocol) {
  case FileTransferProtocol::Tight2:
    tight2ReplyPending_ = true;
    writer_->writeTight2UploadDataRequest(data, (uint32_t)len);
    break;

  case FileTransferProtocol::Tight13: {
    size_t offset = 0;
    while (offset < len) {
      size_t chunk = std::min<size_t>(len - offset, 32768);
      writer_->writeTight13FileUploadData(0, data + offset, (uint16_t)chunk);
      offset += chunk;
    }
    if (handler_ != nullptr)
      handler_->onUploadChunkAck();
    break;
  }

  case FileTransferProtocol::Ultra: {
    size_t blockSize = caps_.ultraBlockSize > 0 ? caps_.ultraBlockSize : 8192;
    size_t offset = 0;
    while (offset < len) {
      size_t chunk = std::min<size_t>(len - offset, blockSize);
      writer_->writeUltraFileTransfer(ultraFilePacket, 0, 0,
                                      data + offset, (uint32_t)chunk);
      offset += chunk;
    }
    if (handler_ != nullptr)
      handler_->onUploadChunkAck();
    break;
  }

  case FileTransferProtocol::Unsupported:
    break;
  }
}

void FileTransferClient::finishUpload(uint64_t modTimeSec)
{
  if (opState_ != OpState::Uploading || writer_ == nullptr)
    return;

  switch (caps_.protocol) {
  case FileTransferProtocol::Tight2:
    tight2ReplyPending_ = true;
    writer_->writeTight2UploadEndRequest(0, modTimeSec * 1000ULL);
    break;

  case FileTransferProtocol::Tight13:
    writer_->writeTight13FileUploadEnd((uint32_t)modTimeSec);
    tight13SyncPending_ = true;
    if (server_->supportsFence) {
      static const uint8_t tag[4] = { 'F', 'T', '1', '3' };
      writer_->writeFence(fenceFlagRequest | fenceFlagBlockBefore, 4, tag);
    } else {
      tight13StaleListReplies_++;
      writer_->writeTight13FileListRequest(0, "/");
    }
    break;

  case FileTransferProtocol::Ultra:
    writer_->writeUltraFileTransfer(ultraEndOfFile, 0, 0, nullptr, 0);
    opState_ = OpState::Idle;
    if (handler_ != nullptr)
      handler_->onUploadComplete(true, "");
    break;

  case FileTransferProtocol::Unsupported:
    break;
  }
}

void FileTransferClient::cancelUpload()
{
  if (opState_ != OpState::Uploading)
    return;

  switch (caps_.protocol) {
  case FileTransferProtocol::Tight2:
    if (tight2ReplyPending_) {
      opState_ = OpState::DrainingCancel;
      tight2CancelAction_ = Tight2CancelAction::CloseUpload;
    } else if (uploadReady_ && writer_ != nullptr) {
      opState_ = OpState::DrainingCancel;
      tight2CancelAction_ = Tight2CancelAction::NoCancel;
      tight2ReplyPending_ = true;
      writer_->writeTight2UploadEndRequest(0, 0);
    } else {
      tight2CancelAction_ = Tight2CancelAction::NoCancel;
      opState_ = OpState::Idle;
      uploadReady_ = false;
      if (handler_ != nullptr)
        handler_->onCancelComplete();
    }
    break;

  case FileTransferProtocol::Tight13:
    writer_->writeTight13FileUploadFailed("Cancelled by client");
    opState_ = OpState::Idle;
    if (handler_ != nullptr)
      handler_->onCancelComplete();
    break;

  case FileTransferProtocol::Ultra:
    writer_->writeUltraFileTransfer(ultraAbortFileTransfer, 0, 0, nullptr, 0);
    opState_ = OpState::Idle;
    if (handler_ != nullptr)
      handler_->onCancelComplete();
    break;

  case FileTransferProtocol::Unsupported:
    opState_ = OpState::Idle;
    break;
  }
}

void FileTransferClient::abortPendingOp()
{
  if (caps_.protocol == FileTransferProtocol::Tight13) {
    if (opState_ == OpState::Downloading && server_->supportsFence) {
      cancelDownload();
      return;
    }
    if (opState_ == OpState::DrainingCancel)
      return;
  }

  if (caps_.protocol == FileTransferProtocol::Tight2) {
    // Tight replies have no request ID. Keep the connection busy until the
    // old reply is consumed and any open remote handle has been released.
    if (opState_ == OpState::Downloading) {
      cancelDownload();
      return;
    }
    if (opState_ == OpState::Uploading) {
      cancelUpload();
      return;
    }
    if (opState_ == OpState::DrainingCancel)
      return;
    if (tight2ReplyPending_ &&
        (opState_ == OpState::Listing || opState_ == OpState::Command)) {
      opState_ = OpState::DrainingCancel;
      commandKind_ = CommandKind::NoneOp;
      return;
    }
  }

  if (opState_ == OpState::Downloading && writer_ != nullptr) {
    if (caps_.protocol == FileTransferProtocol::Tight13)
      writer_->writeTight13FileDownloadCancel("Operation aborted");
    else if (caps_.protocol == FileTransferProtocol::Ultra)
      writer_->writeUltraFileTransfer(ultraAbortFileTransfer, 0, 0, nullptr, 0);
  } else if (opState_ == OpState::Uploading && writer_ != nullptr) {
    if (caps_.protocol == FileTransferProtocol::Tight13)
      writer_->writeTight13FileUploadFailed("Operation aborted");
    else if (caps_.protocol == FileTransferProtocol::Ultra)
      writer_->writeUltraFileTransfer(ultraAbortFileTransfer, 0, 0, nullptr, 0);
  }

  opState_ = OpState::Idle;
  commandKind_ = CommandKind::NoneOp;
  tight2ReplyPending_ = false;
  tight2CancelAction_ = Tight2CancelAction::NoCancel;
  tight13SyncPending_ = false;
  downloadStarted_ = false;
  uploadReady_ = false;
  tight13ExpectedMkdirBase_.clear();
  ultraDrivesListing_ = false;
  ultraDirEchoReceived_ = false;
  ultraDownloadSkipBytes_ = 0;
  pendingEntries_.clear();
}

void FileTransferClient::createDirectory(const std::string& path)
{
  ensureReadyForOp();
  opState_ = OpState::Command;
  commandKind_ = CommandKind::Mkdir;

  switch (caps_.protocol) {
  case FileTransferProtocol::Tight2:
    tight2ReplyPending_ = true;
    writer_->writeTight2MkdirRequest(toTight2Path(path));
    break;

  case FileTransferProtocol::Tight13: {
    std::string norm = toTight13Path(path);
    writer_->writeTight13FileCreateDirRequest(norm);
    size_t slash = norm.rfind('/');
    std::string parent = (slash == 0 || slash == std::string::npos) ? "/" : norm.substr(0, slash);
    tight13ExpectedMkdirBase_ = (slash == std::string::npos) ? norm : norm.substr(slash + 1);
    tight13StaleListReplies_++;
    writer_->writeTight13FileListRequest(0, parent);
    break;
  }

  case FileTransferProtocol::Ultra: {
    std::string wire = toUltraPath(path, false);
    writer_->writeUltraFileTransfer(ultraCommand, ultraCDirCreate, 0,
                                    (const uint8_t*)wire.data(),
                                    (uint32_t)wire.size());
    break;
  }

  case FileTransferProtocol::Unsupported:
    opState_ = OpState::Idle;
    break;
  }
}

void FileTransferClient::removeFileOrDir(const std::string& path, bool /*isDir*/)
{
  ensureReadyForOp();
  if (!caps_.canDelete) {
    if (handler_ != nullptr)
      handler_->onCommandComplete(false, "Delete is not supported by server");
    return;
  }

  opState_ = OpState::Command;
  commandKind_ = CommandKind::Remove;

  switch (caps_.protocol) {
  case FileTransferProtocol::Tight2:
    tight2ReplyPending_ = true;
    writer_->writeTight2RemoveRequest(toTight2Path(path));
    break;

  case FileTransferProtocol::Ultra: {
    std::string wire = toUltraPath(path, false);
    writer_->writeUltraFileTransfer(ultraCommand, ultraCFileDelete, 0,
                                    (const uint8_t*)wire.data(),
                                    (uint32_t)wire.size());
    break;
  }

  case FileTransferProtocol::Tight13:
  case FileTransferProtocol::Unsupported:
    opState_ = OpState::Idle;
    if (handler_ != nullptr)
      handler_->onCommandComplete(false, "Delete is not supported by server");
    break;
  }
}

void FileTransferClient::renameFileOrDir(const std::string& fromPath,
                                         const std::string& toPath,
                                         bool /*isDir*/)
{
  ensureReadyForOp();
  if (!caps_.canRename) {
    if (handler_ != nullptr)
      handler_->onCommandComplete(false, "Rename is not supported by server");
    return;
  }

  opState_ = OpState::Command;
  commandKind_ = CommandKind::Rename;

  switch (caps_.protocol) {
  case FileTransferProtocol::Tight2:
    tight2ReplyPending_ = true;
    writer_->writeTight2RenameRequest(toTight2Path(fromPath),
                                      toTight2Path(toPath));
    break;

  case FileTransferProtocol::Ultra: {
    std::string wire = toUltraPath(fromPath, false) + "*" +
                       toUltraPath(toPath, false);
    writer_->writeUltraFileTransfer(ultraCommand, ultraCFileRename, 0,
                                    (const uint8_t*)wire.data(),
                                    (uint32_t)wire.size());
    break;
  }

  case FileTransferProtocol::Tight13:
  case FileTransferProtocol::Unsupported:
    opState_ = OpState::Idle;
    if (handler_ != nullptr)
      handler_->onCommandComplete(false, "Rename is not supported by server");
    break;
  }
}

void FileTransferClient::onFenceReply()
{
  if (caps_.protocol == FileTransferProtocol::Tight13 &&
      opState_ == OpState::Uploading && tight13SyncPending_) {
    tight13SyncPending_ = false;
    opState_ = OpState::Idle;
    if (handler_ != nullptr)
      handler_->onUploadComplete(true, "");
  }
}

void FileTransferClient::onTight13CancelFenceReply()
{
  if (caps_.protocol != FileTransferProtocol::Tight13 ||
      opState_ != OpState::DrainingCancel)
    return;

  opState_ = OpState::Idle;
  downloadStarted_ = false;
  if (handler_ != nullptr)
    handler_->onCancelComplete();
}

void FileTransferClient::onTight13FileListData(
  uint8_t flags, const std::vector<FileEntry>& entries)
{
  if (caps_.protocol != FileTransferProtocol::Tight13)
    return;

  if (opState_ == OpState::Uploading && tight13SyncPending_) {
    if (tight13StaleListReplies_ > 0)
      tight13StaleListReplies_--;
    tight13SyncPending_ = false;
    opState_ = OpState::Idle;
    if (handler_ != nullptr)
      handler_->onUploadComplete(true, "");
    return;
  }

  if (opState_ == OpState::Command && commandKind_ == CommandKind::Mkdir) {
    if (tight13StaleListReplies_ > 0)
      tight13StaleListReplies_--;
    bool ok = false;
    if ((flags & tight13FlagError) == 0) {
      for (const FileEntry& e : entries) {
        if (e.isDir && e.name == tight13ExpectedMkdirBase_) {
          ok = true;
          break;
        }
      }
    }
    tight13ExpectedMkdirBase_.clear();
    opState_ = OpState::Idle;
    commandKind_ = CommandKind::NoneOp;
    if (handler_ != nullptr)
      handler_->onCommandComplete(ok, ok ? "" : "Failed to create directory");
    return;
  }

  if (opState_ != OpState::Listing) {
    if (tight13StaleListReplies_ > 0)
      tight13StaleListReplies_--;
    return;
  }

  opState_ = OpState::Idle;
  if ((flags & tight13FlagError) != 0) {
    if (handler_ != nullptr)
      handler_->onListComplete(false, std::vector<FileEntry>(),
                               "Cannot list directory");
    return;
  }

  std::vector<FileEntry> filtered;
  filtered.reserve(entries.size());
  for (const FileEntry& e : entries) {
    if (e.name == "." || e.name == "..")
      continue;
    filtered.push_back(e);
  }

  if (handler_ != nullptr)
    handler_->onListComplete(true, filtered, "");
}

void FileTransferClient::onTight13FileDownloadData(const uint8_t* data,
                                                   size_t len)
{
  if (caps_.protocol != FileTransferProtocol::Tight13 ||
      opState_ != OpState::Downloading)
    return;

  if (!downloadStarted_) {
    downloadStarted_ = true;
    if (handler_ != nullptr)
      handler_->onDownloadStarted(true, 0, false, "");
    if (opState_ != OpState::Downloading)
      return;
  }

  if (len > 0 && handler_ != nullptr)
    handler_->onDownloadData(data, len);
}

void FileTransferClient::onTight13FileDownloadEnd(uint32_t /*modTime*/)
{
  if (caps_.protocol != FileTransferProtocol::Tight13 ||
      opState_ != OpState::Downloading)
    return;

  if (!downloadStarted_) {
    downloadStarted_ = true;
    if (handler_ != nullptr)
      handler_->onDownloadStarted(true, 0, true, "");
    if (opState_ != OpState::Downloading)
      return;
  }

  opState_ = OpState::Idle;
  if (handler_ != nullptr)
    handler_->onDownloadComplete(true, "");
}

void FileTransferClient::onTight13FileUploadCancel(const std::string& reason)
{
  if (caps_.protocol != FileTransferProtocol::Tight13 ||
      opState_ != OpState::Uploading)
    return;

  tight13SyncPending_ = false;
  opState_ = OpState::Idle;
  if (handler_ != nullptr)
    handler_->onUploadComplete(false,
                               reason.empty() ? "Upload cancelled by server" : reason);
}

void FileTransferClient::onTight13FileDownloadFailed(const std::string& reason)
{
  if (caps_.protocol != FileTransferProtocol::Tight13 ||
      opState_ != OpState::Downloading)
    return;

  bool started = downloadStarted_;
  opState_ = OpState::Idle;
  if (handler_ != nullptr) {
    std::string msg = reason.empty() ? "Download failed" : reason;
    if (!started)
      handler_->onDownloadStarted(false, 0, false, msg);
    else
      handler_->onDownloadComplete(false, msg);
  }
}

void FileTransferClient::finishTight2DrainingCancel(bool remoteClosedHandle)
{
  if (!remoteClosedHandle && writer_ != nullptr) {
    if (tight2CancelAction_ == Tight2CancelAction::CloseDownload) {
      tight2CancelAction_ = Tight2CancelAction::NoCancel;
      tight2ReplyPending_ = true;
      writer_->writeTight2DownloadStartRequest("", 0);
      return;
    }
    if (tight2CancelAction_ == Tight2CancelAction::CloseUpload) {
      tight2CancelAction_ = Tight2CancelAction::NoCancel;
      tight2ReplyPending_ = true;
      writer_->writeTight2UploadEndRequest(0, 0);
      return;
    }
  }

  tight2CancelAction_ = Tight2CancelAction::NoCancel;
  opState_ = OpState::Idle;
  downloadStarted_ = false;
  uploadReady_ = false;
  if (handler_ != nullptr)
    handler_->onCancelComplete();
}

void FileTransferClient::onTight2CompressionSupportReply(bool supported)
{
  tight2ReplyPending_ = false;
  if (caps_.protocol != FileTransferProtocol::Tight2)
    return;

  caps_.canCompress = supported;
  server_->fileTransferCaps = caps_;
  if (opState_ == OpState::Probing)
    opState_ = OpState::Idle;

  if (!ready_) {
    ready_ = true;
    if (handler_ != nullptr)
      handler_->onFileTransferReady(caps_);
  }
}

void FileTransferClient::onTight2FileListReply(
  const std::vector<FileEntry>& entries)
{
  tight2ReplyPending_ = false;
  if (opState_ == OpState::DrainingCancel) {
    finishTight2DrainingCancel(true);
    return;
  }
  if (opState_ != OpState::Listing)
    return;

  opState_ = OpState::Idle;
  std::vector<FileEntry> filtered;
  filtered.reserve(entries.size());
  for (const FileEntry& e : entries) {
    if (e.name == "." || e.name == "..")
      continue;
    filtered.push_back(e);
  }
  if (handler_ != nullptr)
    handler_->onListComplete(true, filtered, "");
}

void FileTransferClient::onTight2Md5Reply(const uint8_t /*digest*/[16])
{
  tight2ReplyPending_ = false;
  if (opState_ == OpState::DrainingCancel) {
    finishTight2DrainingCancel(true);
  }
}

void FileTransferClient::onTight2UploadStartReply()
{
  tight2ReplyPending_ = false;
  if (opState_ == OpState::DrainingCancel) {
    finishTight2DrainingCancel(false);
    return;
  }
  if (opState_ != OpState::Uploading)
    return;

  uploadReady_ = true;
  if (handler_ != nullptr)
    handler_->onUploadReady(true, "");
}

void FileTransferClient::onTight2UploadDataReply()
{
  tight2ReplyPending_ = false;
  if (opState_ == OpState::DrainingCancel) {
    finishTight2DrainingCancel(false);
    return;
  }
  if (opState_ != OpState::Uploading)
    return;

  if (handler_ != nullptr)
    handler_->onUploadChunkAck();
}

void FileTransferClient::onTight2UploadEndReply()
{
  tight2ReplyPending_ = false;
  if (opState_ == OpState::DrainingCancel) {
    finishTight2DrainingCancel(true);
    return;
  }
  if (opState_ != OpState::Uploading)
    return;

  opState_ = OpState::Idle;
  if (handler_ != nullptr)
    handler_->onUploadComplete(true, "");
}

void FileTransferClient::onTight2DownloadStartReply()
{
  tight2ReplyPending_ = false;
  if (opState_ == OpState::DrainingCancel) {
    finishTight2DrainingCancel(false);
    return;
  }
  if (opState_ != OpState::Downloading)
    return;

  downloadStarted_ = true;
  if (handler_ != nullptr)
    handler_->onDownloadStarted(true, 0, false, "");
}

void FileTransferClient::onTight2DownloadDataReply(const uint8_t* data,
                                                   size_t len)
{
  tight2ReplyPending_ = false;
  if (opState_ == OpState::DrainingCancel) {
    finishTight2DrainingCancel(false);
    return;
  }
  if (opState_ != OpState::Downloading)
    return;

  if (len > 0 && handler_ != nullptr)
    handler_->onDownloadData(data, len);
}

void FileTransferClient::onTight2DownloadEndReply(uint8_t /*flags*/,
                                                  uint64_t /*modTimeMs*/)
{
  tight2ReplyPending_ = false;
  if (opState_ == OpState::DrainingCancel) {
    finishTight2DrainingCancel(true);
    return;
  }
  if (opState_ != OpState::Downloading)
    return;

  opState_ = OpState::Idle;
  if (handler_ != nullptr)
    handler_->onDownloadComplete(true, "");
}

void FileTransferClient::onTight2MkdirReply()
{
  tight2ReplyPending_ = false;
  if (opState_ == OpState::DrainingCancel) {
    finishTight2DrainingCancel(true);
    return;
  }
  if (opState_ != OpState::Command)
    return;

  opState_ = OpState::Idle;
  commandKind_ = CommandKind::NoneOp;
  if (handler_ != nullptr)
    handler_->onCommandComplete(true, "");
}

void FileTransferClient::onTight2RemoveReply()
{
  onTight2MkdirReply();
}

void FileTransferClient::onTight2RenameReply()
{
  onTight2MkdirReply();
}

void FileTransferClient::onTight2DirSizeReply(uint64_t /*size*/)
{
  tight2ReplyPending_ = false;
  if (opState_ == OpState::DrainingCancel) {
    finishTight2DrainingCancel(true);
  }
}

void FileTransferClient::onTight2LastRequestFailedReply(
  const std::string& message)
{
  tight2ReplyPending_ = false;
  if (opState_ == OpState::Probing) {
    opState_ = OpState::Idle;
    caps_.canCompress = false;
    ready_ = true;
    if (handler_ != nullptr)
      handler_->onFileTransferReady(caps_);
    return;
  }

  if (opState_ == OpState::DrainingCancel) {
    finishTight2DrainingCancel(true);
    return;
  }

  std::string err = message.empty() ? "Remote operation failed" : message;
  if (opState_ == OpState::Downloading && !downloadStarted_) {
    opState_ = OpState::Idle;
    if (handler_ != nullptr)
      handler_->onDownloadStarted(false, 0, false, err);
    return;
  }

  if (opState_ == OpState::Uploading && !uploadReady_) {
    opState_ = OpState::Idle;
    if (handler_ != nullptr)
      handler_->onUploadReady(false, err);
    return;
  }

  failActiveOp(err);
}

void FileTransferClient::handleUltraDirPacket(uint16_t contentParam,
                                              const uint8_t* payload,
                                              size_t length)
{
  if (opState_ != OpState::Listing)
    return;

  bool isUnicode = (contentParam & ultraADirUnicode) != 0;
  uint16_t baseParam = contentParam & (uint16_t)(~ultraADirUnicode);

  if (ultraDrivesListing_) {
    if (baseParam == ultraADrivesList) {
      std::vector<FileEntry> drives;
      size_t off = 0;
      while (off < length) {
        if (payload[off] == '\0')
          break;
        size_t slen = strnlen((const char*)payload + off, length - off);
        if (slen >= 2) {
          FileEntry e;
          e.name.assign((const char*)payload + off, 2);
          e.isDir = true;
          drives.push_back(e);
        }
        off += slen + 1;
      }
      opState_ = OpState::Idle;
      if (handler_ != nullptr)
        handler_->onListComplete(true, drives, "");
    }
    return;
  }

  if (baseParam == ultraADirInaccessible) {
    opState_ = OpState::Idle;
    pendingEntries_.clear();
    if (handler_ != nullptr)
      handler_->onListComplete(false, std::vector<FileEntry>(),
                               "Directory inaccessible");
    return;
  }

  if (baseParam == 0 && length == 0) {
    opState_ = OpState::Idle;
    std::vector<FileEntry> out;
    out.swap(pendingEntries_);
    if (handler_ != nullptr)
      handler_->onListComplete(true, out, "");
    return;
  }

  if (!ultraDirEchoReceived_ && baseParam == ultraADirectory) {
    if (length == 0) {
      opState_ = OpState::Idle;
      pendingEntries_.clear();
      if (handler_ != nullptr)
        handler_->onListComplete(false, std::vector<FileEntry>(),
                                 "Cannot open directory");
      return;
    }
    ultraDirEchoReceived_ = true;
    return;
  }

  if ((baseParam == ultraADirectory || baseParam == ultraAFile) &&
      length >= 44 && payload != nullptr) {
    uint32_t attrs = readLE32(payload);
    uint32_t writeLow = readLE32(payload + 20);
    uint32_t writeHigh = readLE32(payload + 24);
    uint32_t sizeHigh = readLE32(payload + 28);
    uint32_t sizeLow = readLE32(payload + 32);

    std::string name = decodeUltraFileName(payload + 44, length - 44, isUnicode);
    if (name.empty() || name == "." || name == "..")
      return;

    FileEntry entry;
    entry.name = name;
    entry.isDir = (attrs & ultraFileAttrDir) != 0;
    entry.size = entry.isDir ? 0 : (((uint64_t)sizeHigh << 32) | (uint64_t)sizeLow);
    entry.mtime = decodeUltraFileTime(writeLow, writeHigh);
    pendingEntries_.push_back(entry);
  }
}

void FileTransferClient::handleUltraFilePacket(uint32_t size,
                                               const uint8_t* payload,
                                               size_t length)
{
  if (opState_ != OpState::Downloading || !downloadStarted_)
    return;

  std::vector<uint8_t> inflated;
  const uint8_t* outPtr = payload;
  size_t outLen = length;

  if (size == 1 && length > 0 && payload != nullptr) {
    try {
      inflated = inflateUltraPacket(payload, length);
    } catch (std::exception& e) {
      writer_->writeUltraFileTransfer(ultraAbortFileTransfer, 0, 0, nullptr, 0);
      opState_ = OpState::Idle;
      if (handler_ != nullptr)
        handler_->onDownloadComplete(false, e.what());
      return;
    }
    outPtr = inflated.empty() ? nullptr : inflated.data();
    outLen = inflated.size();
  }

  if (ultraDownloadSkipBytes_ > 0) {
    if (ultraDownloadSkipBytes_ >= outLen) {
      ultraDownloadSkipBytes_ -= outLen;
      return;
    }
    outPtr += ultraDownloadSkipBytes_;
    outLen -= (size_t)ultraDownloadSkipBytes_;
    ultraDownloadSkipBytes_ = 0;
  }

  if (outLen > 0 && handler_ != nullptr)
    handler_->onDownloadData(outPtr, outLen);
}

void FileTransferClient::onUltraFileTransferMessage(
  uint8_t contentType, uint16_t contentParam,
  uint32_t size, uint32_t sizeH,
  const uint8_t* payload, size_t length)
{
  switch (contentType) {
  case ultraFileTransferProtocolVersion: {
    int ver = contentParam & 0xFF;
    caps_.ultraVersion = ver;
    caps_.ultraUnicode = (ver >= 4);
    if (ver >= 3 && size >= 4096 && size <= 1048576)
      caps_.ultraBlockSize = size;
    server_->fileTransferCaps = caps_;
    vlog.info("UltraVNC FT protocol version %d (blockSize=%u, unicode=%d)",
              ver, caps_.ultraBlockSize, (int)caps_.ultraUnicode);

    if ((caps_.protocol == FileTransferProtocol::Unsupported ||
         caps_.protocol == FileTransferProtocol::Ultra) &&
        (preferredProtocol_ == FileTransferProtocol::Unsupported ||
         preferredProtocol_ == FileTransferProtocol::Ultra)) {
      caps_.protocol = FileTransferProtocol::Ultra;
      server_->fileTransferCaps = caps_;
      if (serverInitDone_ && !ultraProbeSent_)
        probeUltraVnc();
    }
    break;
  }

  case ultraFileTransferSessionStart:
  case ultraFileTransferSessionEnd:
  case ultraFileChecksums:
    break;

  case ultraFileTransferAccess:
    ultraProbeInFlight_ = false;
    if (opState_ == OpState::Probing)
      opState_ = OpState::Idle;
    if (size == 1) {
      caps_.protocol = FileTransferProtocol::Ultra;
      caps_.canList = true;
      caps_.canDownload = true;
      caps_.canUpload = true;
      caps_.canResumeDownload = true;
      caps_.canResumeUpload = false;
      caps_.canMkdir = true;
      caps_.canDelete = true;
      caps_.canRename = true;
      caps_.canCompress = true;
      server_->fileTransferCaps = caps_;
      ready_ = true;
      if (caps_.ultraVersion >= 3 && !ultraSessionStarted_ && writer_ != nullptr) {
        ultraSessionStarted_ = true;
        writer_->writeUltraFileTransfer(ultraFileTransferSessionStart, 0, 0,
                                        nullptr, 0);
      }
      if (handler_ != nullptr)
        handler_->onFileTransferReady(caps_);
    } else {
      caps_.protocol = FileTransferProtocol::Unsupported;
      ready_ = false;
      server_->fileTransferCaps = caps_;
      vlog.info("UltraVNC file transfer access denied by server");
    }
    break;

  case ultraDirPacket:
    handleUltraDirPacket(contentParam, payload, length);
    break;

  case ultraFileHeader: {
    if (opState_ != OpState::Downloading || downloadStarted_)
      break;
    bool isErr = (caps_.ultraVersion == 1 && size == ultraError) ||
                 (caps_.ultraVersion != 1 && size == ultraError && sizeH == ultraError);
    if (isErr) {
      opState_ = OpState::Idle;
      if (handler_ != nullptr)
        handler_->onDownloadStarted(false, 0, false,
                                    "Cannot open remote file");
      break;
    }
    downloadStarted_ = true;
    uint64_t totalSize = (caps_.ultraVersion == 1)
      ? (uint64_t)size
      : (((uint64_t)sizeH << 32) | (uint64_t)size);
    writer_->writeUltraFileTransfer(ultraFileHeader, 0, 0, nullptr, 0);
    uint64_t remaining = (totalSize > ultraDownloadSkipBytes_)
      ? (totalSize - ultraDownloadSkipBytes_) : 0;
    if (handler_ != nullptr)
      handler_->onDownloadStarted(true, remaining, true, "");
    break;
  }

  case ultraFilePacket:
    handleUltraFilePacket(size, payload, length);
    break;

  case ultraEndOfFile:
    if (opState_ == OpState::Downloading && downloadStarted_) {
      opState_ = OpState::Idle;
      if (handler_ != nullptr)
        handler_->onDownloadComplete(true, "");
    }
    break;

  case ultraFileAcceptHeader:
    if (opState_ != OpState::Uploading || uploadReady_)
      break;
    if (size == ultraError) {
      opState_ = OpState::Idle;
      if (handler_ != nullptr)
        handler_->onUploadReady(false, "Server rejected file upload");
    } else {
      uploadReady_ = true;
      if (handler_ != nullptr)
        handler_->onUploadReady(true, "");
    }
    break;

  case ultraCommandReturn:
    if (opState_ == OpState::Command) {
      opState_ = OpState::Idle;
      commandKind_ = CommandKind::NoneOp;
      bool ok = (size != ultraError);
      if (handler_ != nullptr)
        handler_->onCommandComplete(ok, ok ? "" : "Remote command failed");
    }
    break;

  case ultraAbortFileTransfer:
    if (ultraProbeInFlight_) {
      ultraProbeInFlight_ = false;
      caps_.ultraVersion = 1;
      server_->fileTransferCaps = caps_;
      if (opState_ == OpState::Probing)
        opState_ = OpState::Idle;
      if (size == 1) {
        caps_.protocol = FileTransferProtocol::Ultra;
        caps_.canList = true;
        caps_.canDownload = true;
        caps_.canUpload = true;
        caps_.canResumeDownload = true;
        caps_.canResumeUpload = false;
        caps_.canMkdir = true;
        caps_.canDelete = true;
        caps_.canRename = true;
        caps_.canCompress = true;
        server_->fileTransferCaps = caps_;
        ready_ = true;
        if (handler_ != nullptr)
          handler_->onFileTransferReady(caps_);
      } else {
        caps_.protocol = FileTransferProtocol::Unsupported;
        ready_ = false;
        server_->fileTransferCaps = caps_;
      }
      break;
    }

    if (opState_ == OpState::Downloading) {
      bool hadHeader = downloadStarted_;
      opState_ = OpState::Idle;
      if (handler_ != nullptr) {
        if (!hadHeader)
          handler_->onDownloadStarted(false, 0, false,
                                      "Transfer aborted by server");
        else
          handler_->onDownloadComplete(false, "Transfer aborted by server");
      }
    } else if (opState_ == OpState::Uploading) {
      bool wasReady = uploadReady_;
      opState_ = OpState::Idle;
      if (handler_ != nullptr) {
        if (!wasReady)
          handler_->onUploadReady(false, "Transfer aborted by server");
        else
          handler_->onUploadComplete(false, "Transfer aborted by server");
      }
    }
    break;

  default:
    break;
  }
}
