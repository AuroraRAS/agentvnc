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

#include "RDPDesktop.h"

#include <algorithm>
#include <chrono>

#include <core/LogWriter.h>
#include <rfb/screenTypes.h>

namespace rdpvnc {

static core::LogWriter vlog("RDPDesktop");

namespace {

uint64_t desktopNowMs()
{
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

int clampDebounceMs(int ms)
{
  return std::max(300, std::min(ms, 500));
}

} // namespace

RDPDesktop::RDPDesktop(const RdpConfig& rdpConfig,
                       const DesktopPolicyConfig& policy,
                       std::shared_ptr<RdpCredentialProvider> credProvider)
  : rdpConfig_(rdpConfig),
    policy_(policy),
    credProvider_(std::move(credProvider)),
    resizeTimer_(this),
    idleTimer_(this),
    reconnectTimer_(this)
{
  policy_.resizeDebounceMs = clampDebounceMs(policy_.resizeDebounceMs);
}

RDPDesktop::~RDPDesktop()
{
  teardownUpstreamSession();
}

void RDPDesktop::init(rfb::VNCServer* vs)
{
  server_ = vs;
  // Provide an initial pixel buffer matching configured resolution so
  // ServerInit is valid even before post-connect completes if queried early.
  pixelBuffer_.rebind(AlignedBuffer::create(
      rdpConfig_.initialWidth, rdpConfig_.initialHeight, 0));
  if (server_) {
    server_->setPixelBuffer(&pixelBuffer_);
  }
}

void RDPDesktop::startUpstreamSession()
{
  if (session_ && session_->isRunning())
    return;

  eventQueue_.clear();
  session_.reset(new RdpSession(rdpConfig_, eventQueue_, credProvider_));
  state_ = State::Connecting;
  prevButtonMask_ = 0;
  hasPendingResize_ = false;
  resizeTimer_.stop();

  if (!session_->start()) {
    finishReconnectFailure("Failed to start RDP session worker");
  }
}

void RDPDesktop::teardownUpstreamSession()
{
  resizeTimer_.stop();
  idleTimer_.stop();
  reconnectTimer_.stop();
  hasPendingResize_ = false;

  if (session_) {
    session_->stop();
    session_.reset();
  }
  eventQueue_.clear();

  state_ = State::Idle;
}

void RDPDesktop::start()
{
  started_ = true;
  idleTimer_.stop();
  reconnectAttempts_ = 0;

  if (state_ == State::IdleGrace && session_ && session_->isRunning()) {
    state_ = session_->isConnected() ? State::Connected : State::Connecting;
    clientEncodingsChanged();
    if (session_->isConnected())
      session_->requestRefresh();
    return;
  }

  if (state_ == State::Idle) {
    startUpstreamSession();
  }
}

void RDPDesktop::stop()
{
  started_ = false;
  resizeTimer_.stop();
  hasPendingResize_ = false;
  prevButtonMask_ = 0;

  if (policy_.idleDisconnectSec > 0 && session_ && session_->isRunning() &&
      (state_ == State::Connected || state_ == State::Connecting)) {
    state_ = State::IdleGrace;
    idleTimer_.start(policy_.idleDisconnectSec * 1000);
    vlog.info("Last VNC client disconnected; keeping upstream RDP alive for %d s",
              policy_.idleDisconnectSec);
    return;
  }

  teardownUpstreamSession();
  if (server_) {
    server_->setPixelBuffer(nullptr);
    pixelBuffer_.detach();
  }
}

void RDPDesktop::queryConnection(rfb::SConnection* conn)
{
  if (server_ && conn) {
    server_->approveConnection(
        conn, false, "Interactive QueryConnect is not supported in rdpvncserver");
  }
}

void RDPDesktop::terminate()
{
  started_ = false;
  teardownUpstreamSession();
  if (server_) {
    server_->closeClients("rdpvncserver terminating");
  }
}

void RDPDesktop::pointerEvent(const core::Point& pos, uint16_t buttonMask)
{
  if (session_ && state_ == State::Connected && pixelBuffer_.hasValidBuffer()) {
    session_->sendPointerEvent(pos.x, pos.y, buttonMask, prevButtonMask_,
                               pixelBuffer_.width(), pixelBuffer_.height());
  }
  prevButtonMask_ = buttonMask;
}

void RDPDesktop::keyEvent(uint32_t keysym, uint32_t keycode, bool down)
{
  if (session_ && state_ == State::Connected) {
    session_->sendKeyEvent(keysym, keycode, down);
  }
}

void RDPDesktop::handleClipboardRequest()
{
  if (session_ && state_ == State::Connected && rdpConfig_.enableClipboard) {
    session_->sendClipboardRequest();
  }
}

void RDPDesktop::handleClipboardAnnounce(bool available)
{
  if (!session_ || state_ != State::Connected || !rdpConfig_.enableClipboard)
    return;

  uint64_t now = desktopNowMs();
  // Anti-loop guard: ignore immediate ownership echo after RDP announced.
  if (available && lastClipboardOrigin_ == ClipboardOrigin::RemoteRdp &&
      (now - lastRemoteAnnounceMs_) < 300) {
    return;
  }

  lastClipboardOrigin_ = ClipboardOrigin::LocalVnc;
  session_->sendClipboardAnnounce(available);
}

void RDPDesktop::handleClipboardData(const char* data)
{
  if (!session_ || state_ != State::Connected || !rdpConfig_.enableClipboard)
    return;

  std::string text = data ? data : "";
  uint64_t now = desktopNowMs();

  // Anti-loop guard: do not reflect RDP-originated text right back to RDP.
  if (!awaitingVncClipboardForRdp_ &&
      lastClipboardOrigin_ == ClipboardOrigin::RemoteRdp &&
      text == lastRemoteClipboardText_ &&
      (now - lastRemoteDataMs_) < 1000) {
    return;
  }

  awaitingVncClipboardForRdp_ = false;
  lastLocalClipboardText_ = text;
  lastLocalDataSentMs_ = now;
  lastClipboardOrigin_ = ClipboardOrigin::LocalVnc;
  session_->sendClipboardData(text);
}

unsigned int RDPDesktop::setScreenLayout(int fb_width, int fb_height,
                                         const rfb::ScreenSet& layout)
{
  if (!rdpConfig_.enableDynamicResize || !session_ ||
      state_ != State::Connected) {
    return rfb::resultProhibited;
  }

  if (fb_width <= 0 || fb_height <= 0 ||
      fb_width > AlignedBuffer::kMaxDimension ||
      fb_height > AlignedBuffer::kMaxDimension) {
    return rfb::resultInvalid;
  }

  if (pixelBuffer_.hasValidBuffer() &&
      pixelBuffer_.width() == fb_width &&
      pixelBuffer_.height() == fb_height) {
    return rfb::resultSuccess;
  }

  pendingResizeWidth_ = fb_width;
  pendingResizeHeight_ = fb_height;
  pendingResizeLayout_ = layout;
  hasPendingResize_ = true;

  // Mandatory 300-500 ms debounce timer before emitting MS-RDPEDISP PDUs.
  resizeTimer_.start(policy_.resizeDebounceMs);
  return rfb::resultPending;
}

void RDPDesktop::clientEncodingsChanged()
{
  if (!server_ || !session_)
    return;

  int total = 0;
  int h264 = 0;
  int synced = 0;
  server_->getClientCapabilities(&total, &h264, &synced, nullptr);
  bool anyH264 = rdpConfig_.enableH264Passthrough && (h264 > 0);
  bool allH264Synced = anyH264 && (total > 0) && (synced == total);
  session_->setClientH264Caps(anyH264, allH264Synced);
}

void RDPDesktop::flushDebouncedResize()
{
  if (!hasPendingResize_)
    return;
  hasPendingResize_ = false;

  if (!session_ || state_ != State::Connected)
    return;

  std::vector<DISPLAY_CONTROL_MONITOR_LAYOUT> monitors =
      KeyMap::buildMonitorLayout(pendingResizeWidth_, pendingResizeHeight_,
                                 pendingResizeLayout_);
  vlog.info("Sending debounced MS-RDPEDISP layout for %dx%d (%zu monitor(s))",
            pendingResizeWidth_, pendingResizeHeight_, monitors.size());
  session_->sendMonitorLayout(monitors);
}

void RDPDesktop::finishReconnectFailure(const std::string& reason)
{
  teardownUpstreamSession();
  if (server_ && started_) {
    server_->closeClients(reason.c_str());
  }
}

void RDPDesktop::handleTimeout(core::Timer* t)
{
  if (t == &resizeTimer_) {
    flushDebouncedResize();
    return;
  }

  if (t == &idleTimer_) {
    if (!started_ && state_ == State::IdleGrace) {
      vlog.info("Idle grace timer expired; disconnecting upstream RDP");
      teardownUpstreamSession();
    }
    return;
  }

  if (t == &reconnectTimer_) {
    if (started_ && state_ == State::Reconnecting) {
      vlog.info("Attempting upstream RDP reconnect (%d/%d)",
                reconnectAttempts_, policy_.maxReconnectAttempts);
      startUpstreamSession();
    }
    return;
  }
}

void RDPDesktop::processEvents()
{
  std::vector<RdpEvent> events;
  eventQueue_.drain(events);

  for (RdpEvent& ev : events) {
    handleSingleEvent(ev);
  }
}

void RDPDesktop::handleSingleEvent(RdpEvent& ev)
{
  switch (ev.type) {
  case RdpEvent::Type::Connected:
    if (state_ == State::Connecting || state_ == State::IdleGrace) {
      if (started_)
        state_ = State::Connected;
      reconnectAttempts_ = 0;
      lastDisconnectReason_.clear();
      clientEncodingsChanged();
      vlog.info("Upstream RDP session ready");
    }
    break;

  case RdpEvent::Type::Resize:
    if (state_ == State::Idle)
      break;
    hasPendingResize_ = false;
    resizeTimer_.stop();
    pixelBuffer_.rebind(std::move(ev.framebuffer));
    if (server_) {
      server_->setPixelBuffer(&pixelBuffer_, ev.layout);
      server_->resetH264Contexts();
    }
    break;

  case RdpEvent::Type::FrameUpdate:
    if (!pixelBuffer_.hasValidBuffer() ||
        ev.fbGeneration != pixelBuffer_.generation()) {
      break;
    }
    if (server_ && !ev.dirty.is_empty())
      server_->add_changed(ev.dirty);
    break;

  case RdpEvent::Type::CopyRect:
    if (!pixelBuffer_.hasValidBuffer() ||
        ev.fbGeneration != pixelBuffer_.generation()) {
      break;
    }
    if (server_ && !ev.rect.is_empty()) {
      core::Point delta(ev.rect.tl.x - ev.srcX, ev.rect.tl.y - ev.srcY);
      server_->add_copied(core::Region(ev.rect), delta);
    }
    break;

  case RdpEvent::Type::SolidFill:
    if (!pixelBuffer_.hasValidBuffer() ||
        ev.fbGeneration != pixelBuffer_.generation()) {
      break;
    }
    if (server_ && !ev.rect.is_empty()) {
      uint8_t bgrx[4] = { ev.b, ev.g, ev.r, 0 };
      server_->add_solid_fill(ev.rect, bgrx);
    }
    break;

  case RdpEvent::Type::RawRect:
    if (!pixelBuffer_.hasValidBuffer() ||
        ev.fbGeneration != pixelBuffer_.generation()) {
      break;
    }
    if (server_ && !ev.rect.is_empty())
      server_->add_raw_passthrough(ev.rect);
    break;

  case RdpEvent::Type::JpegRect:
    if (!pixelBuffer_.hasValidBuffer() ||
        ev.fbGeneration != pixelBuffer_.generation()) {
      break;
    }
    if (server_ && !ev.rect.is_empty() && !ev.payload.empty()) {
      server_->add_jpeg_passthrough(ev.rect, ev.payload.data(),
                                    ev.payload.size());
    }
    break;

  case RdpEvent::Type::H264Frame:
    if (state_ != State::Connected ||
        ev.fbGeneration != pixelBuffer_.generation()) {
      break;
    }
    if (server_ && !ev.rect.is_empty() && !ev.payload.empty()) {
      server_->add_h264(ev.rect, ev.payload.data(), ev.payload.size(),
                        ev.h264Flags, ev.h264IsIdr, ev.fallbackDirty);
    }
    break;

  case RdpEvent::Type::CursorShape:
    if (server_ && ev.width > 0 && ev.height > 0 && !ev.payload.empty()) {
      server_->setCursor(ev.width, ev.height,
                         core::Point(ev.hotX, ev.hotY), ev.payload.data());
    }
    break;

  case RdpEvent::Type::CursorNull:
    if (server_) {
      server_->setCursor(0, 0, core::Point(0, 0), nullptr);
    }
    break;

  case RdpEvent::Type::CursorPosition:
    if (server_ && state_ == State::Connected)
      server_->setCursorPos(core::Point(ev.x, ev.y), true);
    break;

  case RdpEvent::Type::ClipboardAnnounce:
    if (server_ && state_ == State::Connected && rdpConfig_.enableClipboard) {
      uint64_t now = desktopNowMs();
      if (ev.clipboardAvailable &&
          lastClipboardOrigin_ == ClipboardOrigin::LocalVnc &&
          (now - lastLocalDataSentMs_) < 300) {
        break;
      }
      lastClipboardOrigin_ = ClipboardOrigin::RemoteRdp;
      lastRemoteAnnounceMs_ = now;
      server_->announceClipboard(ev.clipboardAvailable);
    }
    break;

  case RdpEvent::Type::ClipboardRequest:
    if (server_ && state_ == State::Connected && rdpConfig_.enableClipboard) {
      awaitingVncClipboardForRdp_ = true;
      server_->requestClipboard();
    }
    break;

  case RdpEvent::Type::ClipboardData:
    if (server_ && state_ == State::Connected && rdpConfig_.enableClipboard) {
      uint64_t now = desktopNowMs();
      if (lastClipboardOrigin_ == ClipboardOrigin::LocalVnc &&
          ev.text == lastLocalClipboardText_ &&
          (now - lastLocalDataSentMs_) < 1000) {
        break;
      }
      lastClipboardOrigin_ = ClipboardOrigin::RemoteRdp;
      lastRemoteDataMs_ = now;
      lastRemoteClipboardText_ = ev.text;
      server_->sendClipboardData(ev.text.c_str());
    }
    break;

  case RdpEvent::Type::Disconnect:
    lastDisconnectReason_ =
        ev.text.empty() ? "Upstream RDP session closed" : ev.text;
    vlog.info("Handling upstream RDP disconnect event: %s",
              lastDisconnectReason_.c_str());
    if (!started_) {
      teardownUpstreamSession();
      break;
    }

    if (policy_.maxReconnectAttempts > 0 &&
        reconnectAttempts_ < policy_.maxReconnectAttempts) {
      ++reconnectAttempts_;
      int backoffMs = 1000 << std::min(reconnectAttempts_ - 1, 3);
      if (session_) {
        session_->stop();
        session_.reset();
      }
      if (server_)
        server_->resetH264Contexts();
      state_ = State::Reconnecting;
      vlog.info("Scheduling upstream RDP reconnect in %d ms", backoffMs);
      reconnectTimer_.start(backoffMs);
    } else {
      finishReconnectFailure(lastDisconnectReason_);
    }
    break;
  }
}

} // namespace rdpvnc
