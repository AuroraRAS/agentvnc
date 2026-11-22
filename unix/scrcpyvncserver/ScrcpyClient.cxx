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

#include <cassert>
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

} // namespace

ScrcpyClient::ScrcpyClient(ScrcpyServer* server, network::Socket* s,
                           rfb::AccessRights ar)
  : rfb::SConnection(s, ar),
    server_(server),
    zeroCopyOs_(s->getFd()),
    inProcessMessages_(false),
    receivedEncodings_(false),
    continuousUpdates_(false),
    updateRequested_(false),
    pendingKeyFrameRequest_(false),
    needsKeyFrame_(true),
    needsReset_(true),
    needsResetAll_(false),
    needsSpsPps_(true),
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
}

ScrcpyClient::~ScrcpyClient()
{
  if (!closeReason_.empty()) {
    vlog.info(_("Closing %s: %s"), getPeerEndpoint(),
              closeReason_.c_str());
  }
}

bool ScrcpyClient::init()
{
  try {
    initialiseProtocol();
  } catch (std::exception& e) {
    close(e.what());
    return false;
  }
  return true;
}

bool ScrcpyClient::accessCheck(rfb::AccessRights ar) const
{
  return rfb::SConnection::accessCheck(ar);
}

void ScrcpyClient::close(const char* reason)
{
  if (closeReason_.empty())
    closeReason_ = reason ? reason : "";
  else
    vlog.debug("Second close: %s (%s)", getPeerEndpoint(),
               reason ? reason : "");

  // Drain any pending data in ZeroCopyOutStream before SConnection::close()
  // shuts down the write half of the socket.
  try {
    if (getOutStream())
      getOutStream()->flush();
    zeroCopyOs_.flush();
  } catch (std::exception&) {
  }

  rfb::SConnection::close(reason);
}

bool ScrcpyClient::hasBufferedWriteData()
{
  return zeroCopyOs_.hasBufferedData();
}

void ScrcpyClient::processSocketReadEvent()
{
  if (state() == RFBSTATE_CLOSING) {
    assert(getSock()->isShutdownWrite());

    if (getSock()->isShutdownRead())
      return;

    while (true) {
      try {
        getInStream()->skip(getInStream()->avail());
        if (!getInStream()->hasData(1))
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

    while (processMsg())
      continue;

    getOutStream()->cork(false);
    inProcessMessages_ = false;

    flushNoDataUpdateIfReady();

    if (pendingKeyFrameRequest_) {
      pendingKeyFrameRequest_ = false;
      server_->clientRequestsKeyFrame(this);
    }
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
  approveConnection(true);
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
  rfb::SConnection::setEncodings(nEncodings, encodings);
  receivedEncodings_ = true;

  if (!client.supportsEncoding(rfb::encodingH264)) {
    vlog.error("Client %s does not advertise Open H.264 encoding (50)",
               getPeerEndpoint());
    close(_("Client does not support Open H.264 encoding (50)"));
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

void ScrcpyClient::setPixelFormat(const rfb::PixelFormat& pf)
{
  rfb::SConnection::setPixelFormat(pf);
}

void ScrcpyClient::framebufferUpdateRequest(const core::Rect& r,
                                            bool incremental)
{
  if (!accessCheck(rfb::AccessView))
    return;

  rfb::SConnection::framebufferUpdateRequest(r, incremental);

  if (!incremental || !continuousUpdates_)
    updateRequested_ = true;

  if (!incremental) {
    needsKeyFrame_ = true;
    needsReset_ = true;
    needsSpsPps_ = true;
    pendingKeyFrameRequest_ = true;
  } else if (needsKeyFrame_) {
    pendingKeyFrameRequest_ = true;
  }
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
  if (!accessCheck(rfb::AccessKeyEvents))
    return;
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

bool ScrcpyClient::readyForVideo()
{
  if (state() != RFBSTATE_NORMAL)
    return false;
  if (!receivedEncodings_ || !client.supportsEncoding(rfb::encodingH264))
    return false;
  return continuousUpdates_ || updateRequested_;
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
    if (!continuousUpdates_)
      updateRequested_ = false;
  } else {
    writer()->writeFramebufferUpdateStart(0);
    writer()->writeFramebufferUpdateEnd();
  }
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
    needsKeyFrame_ = true;
    needsReset_ = true;
    needsResetAll_ = true;
    needsSpsPps_ = true;

    flushNoDataUpdateIfReady();
  } catch (std::exception& e) {
    close(e.what());
  }
}

void ScrcpyClient::onConfigUpdated()
{
  needsSpsPps_ = true;
  needsReset_ = true;
}

void ScrcpyClient::setDesktopNameOrClose(const char* name)
{
  try {
    client.setName(name);
    if (state() == RFBSTATE_NORMAL &&
        client.supportsEncoding(rfb::pseudoEncodingDesktopName)) {
      writer()->writeSetDesktopName();
      flushNoDataUpdateIfReady();
    }
  } catch (std::exception& e) {
    close(e.what());
  }
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

void ScrcpyClient::sendClipboardDataOrClose(const char* data)
{
  try {
    if (state() != RFBSTATE_NORMAL)
      return;
    sendClipboardData(data);
  } catch (std::exception& e) {
    close(e.what());
  }
}

bool ScrcpyClient::sendH264Frame(int width, int height,
                                 const std::vector<uint8_t>& spsPps,
                                 const uint8_t* nalData, size_t nalLen,
                                 bool isKeyFrame, bool hasInlineSpsPps)
{
  if (!readyForVideo() || !nalData || nalLen == 0)
    return false;

  try {
    if (writer()->needFakeUpdate()) {
      if (zeroCopyOs_.hasBufferedData()) {
        needsKeyFrame_ = true;
        needsReset_ = true;
        needsSpsPps_ = true;
        return false;
      }
      flushNoDataUpdateIfReady();
      if (!continuousUpdates_ && !updateRequested_)
        return false;
    }

    if (needsKeyFrame_ && !isKeyFrame)
      return false;

    bool prependSpsPps = (needsSpsPps_ || needsReset_) && !hasInlineSpsPps;
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
        needsKeyFrame_ = true;
        needsReset_ = true;
        needsSpsPps_ = true;
        return false;
      }
    } else {
      // Encrypted stream (TLS / RSA-AES): check socket congestion first.
      zeroCopyOs_.flush();
      if (zeroCopyOs_.hasBufferedData()) {
        needsKeyFrame_ = true;
        needsReset_ = true;
        needsSpsPps_ = true;
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
    needsSpsPps_ = false;
    if (!continuousUpdates_)
      updateRequested_ = false;

    return true;
  } catch (std::exception& e) {
    close(e.what());
    return false;
  }
}
