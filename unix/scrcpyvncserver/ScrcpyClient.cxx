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

#include "ScrcpyClient.h"
#include "ScrcpyServer.h"
#include "parameters.h"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <core/LogWriter.h>
#include <core/i18n.h>
#include <core/time.h>
#include <network/Socket.h>
#include <rdr/FdInStream.h>
#include <rdr/InStream.h>
#include <rfb/Exception.h>
#include <rfb/PixelFormat.h>
#include <rfb/SMsgWriter.h>
#include <rfb/ServerCore.h>
#include <rfb/encodings.h>
#include <rfb/fenceTypes.h>
#include <rfb/fileTransferTypes.h>
#include <rfb/msgTypes.h>
#include <rfb/screenTypes.h>

static core::LogWriter vlog("ScrcpyClient");

namespace {

void writeU16Be(uint8_t* p, uint16_t v)
{
  p[0] = static_cast<uint8_t>((v >> 8) & 0xff);
  p[1] = static_cast<uint8_t>(v & 0xff);
}

void writeU32Be(uint8_t* p, uint32_t v)
{
  p[0] = static_cast<uint8_t>((v >> 24) & 0xff);
  p[1] = static_cast<uint8_t>((v >> 16) & 0xff);
  p[2] = static_cast<uint8_t>((v >> 8) & 0xff);
  p[3] = static_cast<uint8_t>(v & 0xff);
}

rfb::TightCapability makeTightCap(int32_t code, const char* signature)
{
  rfb::TightCapability cap;
  cap.code = code;
  std::memcpy(cap.vendor, rfb::tightVendorTight, 4);
  std::memcpy(cap.signature, signature, 8);
  return cap;
}

} // namespace

ScrcpyClient::ScrcpyClient(ScrcpyServer* server, network::Socket* s,
                           rfb::AccessRights ar)
  : rfb::SConnection(s, ar),
    server_(server),
    zeroCopyOs_(s->getFd()),
    tight13DownloadPendingWrite_(false),
    inProcessMessages_(false),
    receivedEncodings_(false),
    continuousUpdates_(false),
    updateRequested_(false),
    pendingKeyFrameRequest_(false),
    retryVideoOnWritable_(false),
    needsKeyFrame_(true),
    needsReset_(true),
    needsResetAll_(false),
    idleTimer_(&idleCb_),
    idleCb_(this)
{
  int fd = s->getFd();
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0)
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  setStreams(&s->inStream(), &zeroCopyOs_);

  if (fileTransferEnabled()) {
    rfb::SecurityServer* sec = getSecurity();
    bool hasTightAuth =
      !sec->GetEnabledExtSecTypes().empty() ||
      sec->IsSupported(rfb::secTypeNone) ||
      sec->IsSupported(rfb::secTypeVncAuth) ||
      sec->IsSupported(rfb::secTypePlain);
    if (hasTightAuth)
      sec->EnableSecType(rfb::secTypeTight);
  }
}

ScrcpyClient::~ScrcpyClient()
{
  if (ftSession_)
    ftSession_->stop();
  if (!closeReason_.empty()) {
    vlog.info(_("Closing %s: %s"), getPeerEndpoint(),
              closeReason_.c_str());
  }
}

void ScrcpyClient::init()
{
  try {
    initialiseProtocol();
  } catch (std::exception& e) {
    close(e.what());
  }
}

bool ScrcpyClient::accessCheck(rfb::AccessRights ar) const
{
  return rfb::SConnection::accessCheck(ar);
}

void ScrcpyClient::close(const char* reason)
{
  if (state() == RFBSTATE_CLOSING)
    return;

  closeReason_ = reason ? reason : "";
  idleTimer_.stop();
  tight13DownloadPendingWrite_ = false;
  if (ftSession_) {
    ftSession_->stop();
    ftSession_.reset();
  }
  for (const auto& key : pressedKeys_)
    server_->clientKeyEvent(key.second, 0, false);
  pressedKeys_.clear();

  // Drain any pending data in ZeroCopyOutStream before SConnection::close()
  // shuts down the write half of the socket.
  try {
    if (getOutStream())
      getOutStream()->cork(false);
    zeroCopyOs_.cork(false);
  } catch (std::exception&) {
  }

  rfb::SConnection::close(reason);
}

bool ScrcpyClient::hasBufferedWriteData()
{
  return state() != RFBSTATE_CLOSING &&
         (zeroCopyOs_.hasBufferedData() ||
          (retryVideoOnWritable_ && readyForVideo()));
}

void ScrcpyClient::processSocketReadEvent()
{
  if (state() == RFBSTATE_CLOSING) {
    if (getSock()->isShutdownRead())
      return;

    rdr::InStream& in = getSock()->inStream();
    while (true) {
      try {
        in.skip(in.avail());
        if (!in.hasData(1))
          break;
      } catch (std::exception&) {
        getSock()->shutdownRead();
        break;
      }
    }
    return;
  }

  try {
    inProcessMessages_ = true;
    getOutStream()->cork(true);

    while (state() != RFBSTATE_CLOSING && processMsg())
      continue;

    inProcessMessages_ = false;
    if (state() == RFBSTATE_CLOSING)
      return;

    getOutStream()->cork(false);
    flushNoDataUpdateIfReady();
    requestPendingKeyFrame();
  } catch (rdr::end_of_stream&) {
    inProcessMessages_ = false;
    close(_("Clean disconnection"));
  } catch (std::exception& e) {
    inProcessMessages_ = false;
    close(e.what());
  }
}

void ScrcpyClient::processSocketWriteEvent()
{
  if (state() == RFBSTATE_CLOSING)
    return;

  try {
    getOutStream()->flush();
    if (getOutStream() != &zeroCopyOs_)
      zeroCopyOs_.flush();
    flushNoDataUpdateIfReady();
    if (tight13DownloadPendingWrite_ && !zeroCopyOs_.hasBufferedData()) {
      tight13DownloadPendingWrite_ = false;
      if (ftSession_)
        ftSession_->enqueueTight13DownloadNextChunk();
    }
    if (retryVideoOnWritable_ && !zeroCopyOs_.hasBufferedData()) {
      retryVideoOnWritable_ = false;
      if (readyForVideo() && needsKeyFrame_)
        server_->clientRequestsKeyFrame(this);
    }
  } catch (std::exception& e) {
    close(e.what());
  }
}

void ScrcpyClient::onIdleTimeout(core::Timer* /*t*/)
{
  close(_("Idle for too long"));
}

void ScrcpyClient::authSuccess()
{
  server_->clearBlacklist(getSock()->getPeerAddress());
  if (rfb::Server::idleTimeout)
    idleTimer_.start(core::secsToMillis(rfb::Server::idleTimeout));
}

void ScrcpyClient::queryConnection()
{
  bool needsApproval = getSock()->requiresQuery() &&
                       !accessCheck(rfb::AccessNoQuery);
  approveConnection(!needsApproval,
                    _("This server cannot approve connection queries"));
}

void ScrcpyClient::clientReady(bool shared)
{
  if (rfb::Server::idleTimeout)
    idleTimer_.start(core::secsToMillis(rfb::Server::idleTimeout));

  if (rfb::Server::alwaysShared)
    shared = true;
  if (!accessCheck(rfb::AccessNonShared))
    shared = true;
  if (rfb::Server::neverShared)
    shared = false;

  server_->clientReady(this, shared);
  if (state() == RFBSTATE_CLOSING || state() == RFBSTATE_INVALID)
    return;

  client.setDimensions(server_->width(), server_->height());
  client.setName(server_->desktopName().c_str());
  client.setPF(rfb::PixelFormat(32, 24, false, true, 255, 255, 255, 16, 8, 0));

  desktopReady();
}

void ScrcpyClient::setEncodings(int nEncodings, const int32_t* encodings)
{
  if (nEncodings == 0 ||
      std::find(encodings, encodings + nEncodings, rfb::encodingH264) ==
      encodings + nEncodings) {
    throw rfb::protocol_error(_("Client does not support Open H.264 encoding (50)"));
  }

  rfb::SConnection::setEncodings(nEncodings, encodings);
  receivedEncodings_ = true;
  if (!client.supportsContinuousUpdates() && continuousUpdates_) {
    continuousUpdates_ = false;
    requireKeyFrame();
  }
}

void ScrcpyClient::supportsFence()
{
  uint8_t type = 0;
  writer()->writeFence(rfb::fenceFlagRequest, sizeof(type), &type);
}

void ScrcpyClient::supportsContinuousUpdates()
{
  writer()->writeEndOfContinuousUpdates();
}

void ScrcpyClient::framebufferUpdateRequest(const core::Rect& r,
                                            bool incremental)
{
  if (!accessCheck(rfb::AccessView))
    return;

  rfb::SConnection::framebufferUpdateRequest(r, incremental);

  if (!incremental || !continuousUpdates_)
    updateRequested_ = true;

  if (!incremental)
    requireKeyFrame();
  if (needsKeyFrame_)
    pendingKeyFrameRequest_ = true;
}

void ScrcpyClient::setDesktopSize(int /*fb_width*/, int /*fb_height*/,
                                  const rfb::ScreenSet& /*layout*/)
{
  writer()->writeDesktopSize(rfb::reasonClient, rfb::resultProhibited);
  flushNoDataUpdateIfReady();
}

void ScrcpyClient::fence(uint32_t flags, unsigned len, const uint8_t data[])
{
  if (!(flags & rfb::fenceFlagRequest))
    return;

  flags &= (rfb::fenceFlagBlockBefore | rfb::fenceFlagBlockAfter);
  if (ftSession_ && (flags & rfb::fenceFlagBlockBefore)) {
    ftSession_->enqueueFenceBarrier(flags, data, len);
    return;
  }

  writer()->writeFence(flags, len, data);
}

void ScrcpyClient::enableContinuousUpdates(bool enable, int /*x*/, int /*y*/,
                                           int /*w*/, int /*h*/)
{
  if (!accessCheck(rfb::AccessView))
    return;

  if (!client.supportsContinuousUpdates()) {
    throw rfb::protocol_error(
      _("Client tried to enable continuous updates when not allowed"));
  }

  continuousUpdates_ = enable;
  if (enable) {
    updateRequested_ = false;
    if (needsKeyFrame_)
      pendingKeyFrameRequest_ = true;
  } else {
    requireKeyFrame();
    writer()->writeEndOfContinuousUpdates();
  }
}

void ScrcpyClient::pointerEvent(const core::Point& pos, uint16_t buttonMask)
{
  if (rfb::Server::idleTimeout)
    idleTimer_.start(core::secsToMillis(rfb::Server::idleTimeout));
  if (!accessCheck(rfb::AccessPtrEvents))
    return;
  server_->clientPointerEvent(this, pos.x, pos.y, buttonMask);
}

void ScrcpyClient::keyEvent(uint32_t keysym, uint32_t keycode, bool down)
{
  if (rfb::Server::idleTimeout)
    idleTimer_.start(core::secsToMillis(rfb::Server::idleTimeout));
  if (!accessCheck(rfb::AccessKeyEvents) || !rfb::Server::acceptKeyEvents)
    return;

  auto lookup = std::make_pair(keycode != 0, keycode ? keycode : keysym);
  auto key = pressedKeys_.find(lookup);
  if (down) {
    if (key != pressedKeys_.end())
      keysym = key->second;
    else if (pressedKeys_.size() < 256)
      pressedKeys_.emplace(lookup, keysym);
    else
      return;
  } else {
    if (key == pressedKeys_.end())
      return;
    keysym = key->second;
    pressedKeys_.erase(key);
  }

  server_->clientKeyEvent(keysym, keycode, down);
}

void ScrcpyClient::handleClipboardRequest()
{
  server_->clientClipboardRequest(this);
}

void ScrcpyClient::handleClipboardAnnounce(bool available)
{
  server_->clientClipboardAnnounce(this, available);
}

void ScrcpyClient::handleClipboardData(const char* data)
{
  server_->clientClipboardData(this, data);
}

int ScrcpyClient::fileTransferWakeupFd() const
{
  return ftSession_ ? ftSession_->wakeupFd() : -1;
}

bool ScrcpyClient::fileTransferEnabled() const
{
  return static_cast<bool>(fileTransfer) && !server_->deviceSerial().empty();
}

void ScrcpyClient::noteActivity()
{
  if (rfb::Server::idleTimeout)
    idleTimer_.start(core::secsToMillis(rfb::Server::idleTimeout));
  server_->noteClientActivity();
}

void ScrcpyClient::getTightCapabilities(
  std::vector<rfb::TightCapability>* serverMsgs,
  std::vector<rfb::TightCapability>* clientMsgs,
  std::vector<rfb::TightCapability>* /*encodings*/)
{
  if (!fileTransferEnabled() || !accessCheck(rfb::AccessFileTransfer))
    return;

  if (serverMsgs) {
    // TightVNC 2.x server-to-client messages
    serverMsgs->push_back(makeTightCap(
      static_cast<int32_t>(rfb::tight2CompressionSupportReply),
      rfb::sigTight2CompressionSupportReply));
    serverMsgs->push_back(makeTightCap(
      static_cast<int32_t>(rfb::tight2FileListReply),
      rfb::sigTight2FileListReply));
    serverMsgs->push_back(makeTightCap(
      static_cast<int32_t>(rfb::tight2Md5Reply),
      rfb::sigTight2Md5Reply));
    serverMsgs->push_back(makeTightCap(
      static_cast<int32_t>(rfb::tight2UploadStartReply),
      rfb::sigTight2UploadStartReply));
    serverMsgs->push_back(makeTightCap(
      static_cast<int32_t>(rfb::tight2UploadDataReply),
      rfb::sigTight2UploadDataReply));
    serverMsgs->push_back(makeTightCap(
      static_cast<int32_t>(rfb::tight2UploadEndReply),
      rfb::sigTight2UploadEndReply));
    serverMsgs->push_back(makeTightCap(
      static_cast<int32_t>(rfb::tight2DownloadStartReply),
      rfb::sigTight2DownloadStartReply));
    serverMsgs->push_back(makeTightCap(
      static_cast<int32_t>(rfb::tight2DownloadDataReply),
      rfb::sigTight2DownloadDataReply));
    serverMsgs->push_back(makeTightCap(
      static_cast<int32_t>(rfb::tight2DownloadEndReply),
      rfb::sigTight2DownloadEndReply));
    serverMsgs->push_back(makeTightCap(
      static_cast<int32_t>(rfb::tight2MkdirReply),
      rfb::sigTight2MkdirReply));
    serverMsgs->push_back(makeTightCap(
      static_cast<int32_t>(rfb::tight2RemoveReply),
      rfb::sigTight2RemoveReply));
    serverMsgs->push_back(makeTightCap(
      static_cast<int32_t>(rfb::tight2RenameReply),
      rfb::sigTight2RenameReply));
    serverMsgs->push_back(makeTightCap(
      static_cast<int32_t>(rfb::tight2DirSizeReply),
      rfb::sigTight2DirSizeReply));
    serverMsgs->push_back(makeTightCap(
      static_cast<int32_t>(rfb::tight2LastRequestFailedReply),
      rfb::sigTight2LastRequestFailedReply));

    // TightVNC 1.3.x server-to-client messages
    serverMsgs->push_back(makeTightCap(
      rfb::tight13CapFileListData, rfb::sigTight13FileListData));
    serverMsgs->push_back(makeTightCap(
      rfb::tight13CapFileDownloadData, rfb::sigTight13FileDownloadData));
    serverMsgs->push_back(makeTightCap(
      rfb::tight13CapFileUploadCancel, rfb::sigTight13FileUploadCancel));
    serverMsgs->push_back(makeTightCap(
      rfb::tight13CapFileDownloadFailed, rfb::sigTight13FileDownloadFailed));
  }

  if (clientMsgs) {
    // TightVNC 2.x client-to-server messages
    clientMsgs->push_back(makeTightCap(
      static_cast<int32_t>(rfb::tight2CompressionSupportReq),
      rfb::sigTight2CompressionSupportReq));
    clientMsgs->push_back(makeTightCap(
      static_cast<int32_t>(rfb::tight2FileListReq),
      rfb::sigTight2FileListReq));
    clientMsgs->push_back(makeTightCap(
      static_cast<int32_t>(rfb::tight2Md5Req),
      rfb::sigTight2Md5Req));
    clientMsgs->push_back(makeTightCap(
      static_cast<int32_t>(rfb::tight2UploadStartReq),
      rfb::sigTight2UploadStartReq));
    clientMsgs->push_back(makeTightCap(
      static_cast<int32_t>(rfb::tight2UploadDataReq),
      rfb::sigTight2UploadDataReq));
    clientMsgs->push_back(makeTightCap(
      static_cast<int32_t>(rfb::tight2UploadEndReq),
      rfb::sigTight2UploadEndReq));
    clientMsgs->push_back(makeTightCap(
      static_cast<int32_t>(rfb::tight2DownloadStartReq),
      rfb::sigTight2DownloadStartReq));
    clientMsgs->push_back(makeTightCap(
      static_cast<int32_t>(rfb::tight2DownloadDataReq),
      rfb::sigTight2DownloadDataReq));
    clientMsgs->push_back(makeTightCap(
      static_cast<int32_t>(rfb::tight2MkdirReq),
      rfb::sigTight2MkdirReq));
    clientMsgs->push_back(makeTightCap(
      static_cast<int32_t>(rfb::tight2RemoveReq),
      rfb::sigTight2RemoveReq));
    clientMsgs->push_back(makeTightCap(
      static_cast<int32_t>(rfb::tight2RenameReq),
      rfb::sigTight2RenameReq));
    clientMsgs->push_back(makeTightCap(
      static_cast<int32_t>(rfb::tight2DirSizeReq),
      rfb::sigTight2DirSizeReq));

    // TightVNC 1.3.x client-to-server messages
    clientMsgs->push_back(makeTightCap(
      rfb::tight13CapFileListReq, rfb::sigTight13FileListReq));
    clientMsgs->push_back(makeTightCap(
      rfb::tight13CapFileDownloadReq, rfb::sigTight13FileDownloadReq));
    clientMsgs->push_back(makeTightCap(
      rfb::tight13CapFileUploadReq, rfb::sigTight13FileUploadReq));
    clientMsgs->push_back(makeTightCap(
      rfb::tight13CapFileUploadData, rfb::sigTight13FileUploadData));
    clientMsgs->push_back(makeTightCap(
      rfb::tight13CapFileDownloadCancel, rfb::sigTight13FileDownloadCancel));
    clientMsgs->push_back(makeTightCap(
      rfb::tight13CapFileUploadFailed, rfb::sigTight13FileUploadFailed));
    clientMsgs->push_back(makeTightCap(
      rfb::tight13CapFileCreateDirReq, rfb::sigTight13FileCreateDirReq));
  }
}

bool ScrcpyClient::ensureFileTransferReady(bool isTight13,
                                           uint8_t tight13Flags,
                                           bool isDownload,
                                           bool isUpload)
{
  noteActivity();

  if (!fileTransferEnabled() || !accessCheck(rfb::AccessFileTransfer)) {
    const char* msg = _("File transfer is not permitted");
    if (!isTight13) {
      writer()->writeTight2LastRequestFailedReply(msg);
    } else if (isDownload) {
      writer()->writeTight13FileDownloadFailed(msg);
    } else if (isUpload) {
      writer()->writeTight13FileUploadCancel(msg);
    } else {
      writer()->writeTight13FileListData(tight13Flags | rfb::tight13FlagError, {});
    }
    return false;
  }

  if (!ftSession_) {
    ftSession_.reset(new AdbFileTransferSession(
      server_->adbHost(), server_->adbPort(), server_->deviceSerial(),
      static_cast<const char*>(fileTransferRoot)));
  }
  return true;
}

void ScrcpyClient::tight2CompressionSupportRequest()
{
  if (!ensureFileTransferReady(false))
    return;
  ftSession_->enqueueTight2CompressionSupport();
}

void ScrcpyClient::tight2FileListRequest(uint8_t compressLevel,
                                         const std::string& path)
{
  if (!ensureFileTransferReady(false))
    return;
  ftSession_->enqueueTight2FileList(compressLevel, path);
}

void ScrcpyClient::tight2Md5Request(const std::string& path,
                                    uint64_t offset, uint64_t dataLen)
{
  if (!ensureFileTransferReady(false))
    return;
  ftSession_->enqueueTight2Md5(path, offset, dataLen);
}

void ScrcpyClient::tight2UploadStartRequest(const std::string& path,
                                            uint8_t flags, uint64_t offset)
{
  if (!ensureFileTransferReady(false))
    return;
  ftSession_->enqueueTight2UploadStart(path, flags, offset);
}

void ScrcpyClient::tight2UploadDataRequest(const uint8_t* data, size_t len)
{
  if (!ensureFileTransferReady(false))
    return;
  ftSession_->enqueueTight2UploadData(data, len);
}

void ScrcpyClient::tight2UploadEndRequest(uint16_t fileFlags,
                                          uint64_t modTimeMs)
{
  if (!ensureFileTransferReady(false))
    return;
  ftSession_->enqueueTight2UploadEnd(fileFlags, modTimeMs);
}

void ScrcpyClient::tight2DownloadStartRequest(const std::string& path,
                                              uint64_t offset)
{
  if (!ensureFileTransferReady(false))
    return;
  ftSession_->enqueueTight2DownloadStart(path, offset);
}

void ScrcpyClient::tight2DownloadDataRequest(uint8_t compressLevel,
                                             uint32_t reqSize)
{
  if (!ensureFileTransferReady(false))
    return;
  ftSession_->enqueueTight2DownloadData(compressLevel, reqSize);
}

void ScrcpyClient::tight2MkdirRequest(const std::string& path)
{
  if (!ensureFileTransferReady(false))
    return;
  ftSession_->enqueueTight2Mkdir(path);
}

void ScrcpyClient::tight2RemoveRequest(const std::string& path)
{
  if (!ensureFileTransferReady(false))
    return;
  ftSession_->enqueueTight2Remove(path);
}

void ScrcpyClient::tight2RenameRequest(const std::string& oldPath,
                                       const std::string& newPath)
{
  if (!ensureFileTransferReady(false))
    return;
  ftSession_->enqueueTight2Rename(oldPath, newPath);
}

void ScrcpyClient::tight2DirSizeRequest(const std::string& path)
{
  if (!ensureFileTransferReady(false))
    return;
  ftSession_->enqueueTight2DirSize(path);
}

void ScrcpyClient::tight13FileListRequest(uint8_t flags,
                                          const std::string& dir)
{
  if (!ensureFileTransferReady(true, flags))
    return;
  ftSession_->enqueueTight13FileList(flags, dir);
}

void ScrcpyClient::tight13FileDownloadRequest(uint8_t compressLevel,
                                              uint32_t position,
                                              const std::string& path)
{
  if (!ensureFileTransferReady(true, 0, true, false))
    return;
  tight13DownloadPendingWrite_ = false;
  ftSession_->enqueueTight13DownloadStart(compressLevel, position, path);
}

void ScrcpyClient::tight13FileUploadRequest(uint8_t /*compressLevel*/,
                                            uint32_t position,
                                            const std::string& path)
{
  if (!ensureFileTransferReady(true, 0, false, true))
    return;
  ftSession_->enqueueTight13UploadStart(position, path);
}

void ScrcpyClient::tight13FileUploadData(const uint8_t* data, size_t len)
{
  noteActivity();
  if (!fileTransferEnabled() || !accessCheck(rfb::AccessFileTransfer) ||
      !ftSession_)
    return;
  ftSession_->enqueueTight13UploadData(data, len);
}

void ScrcpyClient::tight13FileUploadEnd(uint32_t modTime)
{
  noteActivity();
  if (!fileTransferEnabled() || !accessCheck(rfb::AccessFileTransfer) ||
      !ftSession_)
    return;
  ftSession_->enqueueTight13UploadEnd(modTime);
}

void ScrcpyClient::tight13FileDownloadCancel(const std::string& /*reason*/)
{
  noteActivity();
  tight13DownloadPendingWrite_ = false;
  if (ftSession_)
    ftSession_->enqueueTight13DownloadCancel();
}

void ScrcpyClient::tight13FileUploadFailed(const std::string& /*reason*/)
{
  noteActivity();
  if (ftSession_)
    ftSession_->enqueueTight13UploadFailed();
}

void ScrcpyClient::tight13FileCreateDirRequest(const std::string& dir)
{
  noteActivity();
  if (!fileTransferEnabled() || !accessCheck(rfb::AccessFileTransfer))
    return;
  if (!ftSession_) {
    ftSession_.reset(new AdbFileTransferSession(
      server_->adbHost(), server_->adbPort(), server_->deviceSerial(),
      static_cast<const char*>(fileTransferRoot)));
  }
  ftSession_->enqueueTight13CreateDir(dir);
}

void ScrcpyClient::processFileTransferEvents()
{
  if (!ftSession_ || state() != RFBSTATE_NORMAL)
    return;

  std::vector<AdbFileTransferSession::Reply> replies = ftSession_->drainReplies();
  if (replies.empty())
    return;

  try {
    using Kind = AdbFileTransferSession::Reply::Kind;
    for (const auto& reply : replies) {
      if (state() == RFBSTATE_CLOSING)
        break;
      switch (reply.kind) {
      case Kind::Tight2CompressionSupport:
        writer()->writeTight2CompressionSupportReply(reply.boolVal);
        break;
      case Kind::Tight2FileList:
        writer()->writeTight2FileListReply(reply.compressLevel, reply.entries);
        break;
      case Kind::Tight2Md5:
        writer()->writeTight2Md5Reply(reply.md5);
        break;
      case Kind::Tight2UploadStart:
        writer()->writeTight2UploadStartReply();
        break;
      case Kind::Tight2UploadData:
        writer()->writeTight2UploadDataReply();
        break;
      case Kind::Tight2UploadEnd:
        writer()->writeTight2UploadEndReply();
        break;
      case Kind::Tight2DownloadStart:
        writer()->writeTight2DownloadStartReply();
        break;
      case Kind::Tight2DownloadData:
        writer()->writeTight2DownloadDataReply(
          reply.compressLevel,
          reply.data.empty() ? nullptr : reply.data.data(),
          reply.data.size());
        break;
      case Kind::Tight2DownloadEnd:
        writer()->writeTight2DownloadEndReply(reply.flags, reply.u64Val);
        break;
      case Kind::Tight2Mkdir:
        writer()->writeTight2MkdirReply();
        break;
      case Kind::Tight2Remove:
        writer()->writeTight2RemoveReply();
        break;
      case Kind::Tight2Rename:
        writer()->writeTight2RenameReply();
        break;
      case Kind::Tight2DirSize:
        writer()->writeTight2DirSizeReply(reply.u64Val);
        break;
      case Kind::Tight2Failed:
        writer()->writeTight2LastRequestFailedReply(reply.text);
        break;
      case Kind::Tight13FileList:
        writer()->writeTight13FileListData(reply.flags, reply.entries);
        break;
      case Kind::Tight13DownloadData:
        writer()->writeTight13FileDownloadData(
          reply.compressLevel,
          reply.data.empty() ? nullptr : reply.data.data(),
          static_cast<uint16_t>(reply.data.size()));
        if (zeroCopyOs_.hasBufferedData()) {
          tight13DownloadPendingWrite_ = true;
        } else if (ftSession_) {
          ftSession_->enqueueTight13DownloadNextChunk();
        }
        break;
      case Kind::Tight13DownloadEnd:
        tight13DownloadPendingWrite_ = false;
        writer()->writeTight13FileDownloadEnd(reply.u32Val);
        break;
      case Kind::Tight13DownloadFailed:
        tight13DownloadPendingWrite_ = false;
        writer()->writeTight13FileDownloadFailed(reply.text);
        break;
      case Kind::Tight13UploadCancel:
        writer()->writeTight13FileUploadCancel(reply.text);
        break;
      case Kind::FenceReply:
        if (client.supportsFence()) {
          writer()->writeFence(reply.u32Val,
                               static_cast<unsigned>(reply.data.size()),
                               reply.data.empty() ? nullptr : reply.data.data());
        }
        break;
      }
    }
  } catch (std::exception& e) {
    close(e.what());
  }
}

bool ScrcpyClient::readyForVideo()
{
  if (state() != RFBSTATE_NORMAL)
    return false;
  if (!receivedEncodings_ || !client.supportsEncoding(rfb::encodingH264))
    return false;
  return continuousUpdates_ || updateRequested_;
}

void ScrcpyClient::requireKeyFrame()
{
  needsKeyFrame_ = true;
  needsReset_ = true;
}

void ScrcpyClient::requestPendingKeyFrame()
{
  if (!pendingKeyFrameRequest_)
    return;

  pendingKeyFrameRequest_ = false;
  if (readyForVideo())
    server_->clientRequestsKeyFrame(this);
}

void ScrcpyClient::flushNoDataUpdateIfReady()
{
  if (inProcessMessages_ || state() != RFBSTATE_NORMAL)
    return;
  if (!writer()->needFakeUpdate())
    return;
  if (!continuousUpdates_ && !updateRequested_)
    return;
  if (zeroCopyOs_.hasBufferedData())
    return;

  if (writer()->needNoDataUpdate()) {
    writer()->writeNoDataUpdate();
  } else {
    writer()->writeFramebufferUpdateStart(0);
    writer()->writeFramebufferUpdateEnd();
  }
  if (!continuousUpdates_)
    updateRequested_ = false;
}

void ScrcpyClient::onDesktopResize(int width, int height)
{
  try {
    if (state() != RFBSTATE_NORMAL)
      return;

    if (client.width() == width && client.height() == height)
      return;

    client.setDimensions(width, height);
    if (!client.supportsDesktopSize()) {
      close(_("Client does not support resizing the desktop"));
      return;
    }

    writer()->writeDesktopSize(rfb::reasonServer);
    requireKeyFrame();
    needsResetAll_ = true;

    flushNoDataUpdateIfReady();
  } catch (std::exception& e) {
    close(e.what());
  }
}

void ScrcpyClient::onConfigUpdated()
{
  requireKeyFrame();
}

void ScrcpyClient::announceClipboardOrClose(bool available)
{
  try {
    if (state() != RFBSTATE_NORMAL)
      return;
    announceClipboard(available);
  } catch (std::exception& e) {
    close(e.what());
  }
}

bool ScrcpyClient::sendH264Frame(int width, int height,
                                 const std::vector<uint8_t>& spsPps,
                                 const uint8_t* nalData, size_t nalLen,
                                 bool isKeyFrame, bool hasInlineSpsPps)
{
  if (!readyForVideo()) {
    requireKeyFrame();
    return false;
  }
  if (!nalData || nalLen == 0)
    return false;

  try {
    if (writer()->needFakeUpdate()) {
      if (zeroCopyOs_.hasBufferedData()) {
        requireKeyFrame();
        retryVideoOnWritable_ = true;
        return false;
      }
      flushNoDataUpdateIfReady();
      if (!continuousUpdates_ && !updateRequested_) {
        requireKeyFrame();
        return false;
      }
    }

    if (needsKeyFrame_ && !isKeyFrame)
      return false;

    bool prependSpsPps = needsReset_ && !hasInlineSpsPps;
    if (prependSpsPps && spsPps.empty())
      return false;

    size_t payloadLen = nalLen + (prependSpsPps ? spsPps.size() : 0);
    if (payloadLen > 0xffffffffu)
      return false;

    uint32_t resetFlags = 0;
    if (needsResetAll_)
      resetFlags = 0x3u;
    else if (needsReset_)
      resetFlags = 0x1u;

    // 4B FramebufferUpdate + 12B Rect header + 8B Open H.264 header = 24 bytes
    uint8_t hdr[24];
    hdr[0] = rfb::msgTypeFramebufferUpdate;
    hdr[1] = 0;
    writeU16Be(hdr + 2, 1);

    writeU16Be(hdr + 4, 0);
    writeU16Be(hdr + 6, 0);
    writeU16Be(hdr + 8, static_cast<uint16_t>(width));
    writeU16Be(hdr + 10, static_cast<uint16_t>(height));
    writeU32Be(hdr + 12, static_cast<uint32_t>(rfb::encodingH264));

    writeU32Be(hdr + 16, static_cast<uint32_t>(payloadLen));
    writeU32Be(hdr + 20, resetFlags);

    if (getOutStream() == &zeroCopyOs_) {
      struct iovec iov[3];
      int iovcnt = 0;

      iov[iovcnt].iov_base = hdr;
      iov[iovcnt].iov_len = sizeof(hdr);
      iovcnt++;

      if (prependSpsPps) {
        iov[iovcnt].iov_base = const_cast<uint8_t*>(spsPps.data());
        iov[iovcnt].iov_len = spsPps.size();
        iovcnt++;
      }

      iov[iovcnt].iov_base = const_cast<uint8_t*>(nalData);
      iov[iovcnt].iov_len = nalLen;
      iovcnt++;

      if (!zeroCopyOs_.writevFrame(iov, iovcnt)) {
        // Socket is congested: drop frame cleanly and resync at next IDR.
        requireKeyFrame();
        retryVideoOnWritable_ = true;
        return false;
      }
    } else {
      // Encrypted stream (TLS / RSA-AES): check socket congestion first.
      zeroCopyOs_.flush();
      if (zeroCopyOs_.hasBufferedData()) {
        requireKeyFrame();
        retryVideoOnWritable_ = true;
        return false;
      }
      rdr::OutStream* out = getOutStream();
      out->writeBytes(hdr, sizeof(hdr));
      if (prependSpsPps)
        out->writeBytes(spsPps.data(), spsPps.size());
      out->writeBytes(nalData, nalLen);
      out->flush();
    }

    needsKeyFrame_ = false;
    needsReset_ = false;
    needsResetAll_ = false;
    retryVideoOnWritable_ = false;
    if (!continuousUpdates_)
      updateRequested_ = false;

    return true;
  } catch (std::exception& e) {
    close(e.what());
    return false;
  }
}
