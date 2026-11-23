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

#include "RdpSession.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <system_error>

#include <core/LogWriter.h>

#include <freerdp/addin.h>
#include <freerdp/channels/channels.h>
#include <freerdp/channels/cliprdr.h>
#include <freerdp/channels/disp.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/client/channels.h>
#include <freerdp/codec/color.h>
#include <freerdp/constants.h>
#include <freerdp/error.h>
#include <freerdp/gdi/gfx.h>
#include <freerdp/settings_keys.h>
#include <winpr/crt.h>
#include <winpr/user.h>

namespace rdpvnc {

static core::LogWriter vlog("RdpSession");

namespace {

std::once_flag gFreeRdpAddinsOnce;

void ensureStaticAddinProviderRegistered()
{
  std::call_once(gFreeRdpAddinsOnce, []() {
    freerdp_register_addin_provider(&freerdp_channels_load_static_addin_entry,
                                    0);
  });
}

struct RdpClientContextWrapper {
  rdpClientContext common;
  RdpSession* session;
};

struct RdpPointerWrapper {
  rdpPointer base;
  std::vector<uint8_t>* rgba;
};

uint64_t monotonicNowMs()
{
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

void removeRectFromHints(std::vector<RdpSession::PendingHintOp>& hints,
                         const core::Rect& r)
{
  // Avoid MSVC/C++ iterator issues: erase any hint overlapping r so stale
  // SolidFill/Raw/JPEG hints cannot overwrite later updates.
  for (auto it = hints.begin(); it != hints.end();) {
    if (!it->rect.intersect(r).is_empty()) {
      it = hints.erase(it);
    } else {
      ++it;
    }
  }
}

char* replaceHeapString(char* existing, const std::string& val)
{
  std::free(existing);
  return _strdup(val.c_str());
}

rfb::ScreenSet buildScreenSetFromDims(int width, int height)
{
  rfb::ScreenSet set;
  set.add_screen(rfb::Screen(0, 0, 0, width, height, 0));
  return set;
}

} // namespace

StaticCredentialProvider::StaticCredentialProvider(std::string username,
                                                   std::string password,
                                                   std::string domain)
  : username_(std::move(username)),
    password_(std::move(password)),
    domain_(std::move(domain))
{
}

bool StaticCredentialProvider::getCredentials(const std::string& /*host*/,
                                              int /*port*/,
                                              std::string& username,
                                              std::string& password,
                                              std::string& domain,
                                              bool isRetry)
{
  if (isRetry || username_.empty() || password_.empty())
    return false;
  username = username_;
  password = password_;
  domain = domain_;
  return true;
}

RdpSession::RdpSession(const RdpConfig& config, RdpEventQueue& eventQueue,
                       std::shared_ptr<RdpCredentialProvider> credProvider)
  : config_(config),
    eventQueue_(eventQueue),
    credProvider_(std::move(credProvider))
{
  if (!credProvider_) {
    credProvider_ = std::make_shared<StaticCredentialProvider>(
        config_.username, config_.password, config_.domain);
  }
  wakeEvent_ = CreateEvent(nullptr, TRUE, FALSE, nullptr);
}

RdpSession::~RdpSession()
{
  stop();
  if (wakeEvent_) {
    CloseHandle(wakeEvent_);
    wakeEvent_ = nullptr;
  }
}

bool RdpSession::start()
{
  if (running_.load())
    return true;
  if (!wakeEvent_)
    return false;
  if (workerThread_.joinable())
    workerThread_.join();

  stopRequested_.store(false);
  connected_.store(false);
  authAttempted_ = false;

  {
    std::lock_guard<std::mutex> lock(outboundMutex_);
    outboundQueue_.clear();
  }
  if (wakeEvent_)
    ResetEvent(wakeEvent_);

  running_.store(true);
  try {
    workerThread_ = std::thread(&RdpSession::workerMain, this);
  } catch (const std::system_error& e) {
    vlog.error("Could not start RDP worker: %s", e.what());
    running_.store(false);
    return false;
  }
  return true;
}

void RdpSession::stop()
{
  if (!running_.load() && !workerThread_.joinable())
    return;

  stopRequested_.store(true);
  eventQueue_.cancel();
  SetEvent(wakeEvent_);
  {
    std::lock_guard<std::mutex> lock(connectionMutex_);
    if (connectAbortEvent_)
      SetEvent(connectAbortEvent_);
  }

  if (workerThread_.joinable())
    workerThread_.join();

  running_.store(false);
  connected_.store(false);
}

void RdpSession::sendKeyEvent(uint32_t keysym, uint32_t keycode, bool down)
{
  KeyAction action = KeyMap::translateKey(keysym, keycode, down);
  if (action.kind == KeyAction::Kind::None)
    return;

  OutboundCommand cmd;
  cmd.type = OutboundCommand::Type::Key;
  cmd.key = action;
  enqueueOutbound(std::move(cmd));
}

void RdpSession::sendPointerEvent(int x, int y, uint16_t buttonMask,
                                  uint16_t prevButtonMask, int fbWidth,
                                  int fbHeight)
{
  std::vector<PointerAction> actions = KeyMap::translatePointer(
      x, y, buttonMask, prevButtonMask, fbWidth, fbHeight);
  if (actions.empty())
    return;

  OutboundCommand cmd;
  cmd.type = OutboundCommand::Type::Pointer;
  cmd.pointerActions = std::move(actions);
  enqueueOutbound(std::move(cmd));
}

void RdpSession::sendMonitorLayout(uint64_t resizeId,
    const std::vector<DISPLAY_CONTROL_MONITOR_LAYOUT>& monitors)
{
  if (monitors.empty())
    return;

  OutboundCommand cmd;
  cmd.type = OutboundCommand::Type::MonitorLayout;
  cmd.monitors = monitors;
  cmd.resizeId = resizeId;
  enqueueOutbound(std::move(cmd));
}

void RdpSession::sendClipboardAnnounce(bool available)
{
  OutboundCommand cmd;
  cmd.type = OutboundCommand::Type::ClipboardAnnounce;
  cmd.clipboardAvailable = available;
  enqueueOutbound(std::move(cmd));
}

void RdpSession::sendClipboardRequest()
{
  OutboundCommand cmd;
  cmd.type = OutboundCommand::Type::ClipboardRequest;
  enqueueOutbound(std::move(cmd));
}

void RdpSession::sendClipboardData(const std::string& utf8Text)
{
  OutboundCommand cmd;
  cmd.type = OutboundCommand::Type::ClipboardData;
  cmd.text = utf8Text;
  enqueueOutbound(std::move(cmd));
}

void RdpSession::requestRefresh()
{
  OutboundCommand cmd;
  cmd.type = OutboundCommand::Type::RefreshScreen;
  enqueueOutbound(std::move(cmd));
}

void RdpSession::setClientH264Caps(bool anyH264Client, bool allH264Synced)
{
  bool prevAny = anyH264Client_.exchange(anyH264Client);
  bool prevAll = allH264Synced_;
  allH264Synced_ = allH264Synced;
  if ((anyH264Client && !prevAny) || (anyH264Client && prevAll && !allH264Synced)) {
    requestRefresh();
  }
}

void RdpSession::enqueueOutbound(OutboundCommand&& cmd)
{
  if (!running_.load())
    return;
  {
    std::lock_guard<std::mutex> lock(outboundMutex_);
    outboundQueue_.push_back(std::move(cmd));
  }
  if (wakeEvent_)
    SetEvent(wakeEvent_);
}

RdpSession* RdpSession::fromContext(rdpContext* ctx)
{
  if (!ctx)
    return nullptr;
  auto* wrapper = reinterpret_cast<RdpClientContextWrapper*>(ctx);
  return wrapper->session;
}

RdpSession* RdpSession::fromGfx(RdpgfxClientContext* gfx)
{
  if (!gfx || !gfx->custom)
    return nullptr;
  auto* gdi = static_cast<rdpGdi*>(gfx->custom);
  return fromContext(gdi->context);
}

RdpSession* RdpSession::fromCliprdr(CliprdrClientContext* cliprdr)
{
  if (!cliprdr)
    return nullptr;
  return static_cast<RdpSession*>(cliprdr->custom);
}

core::Rect RdpSession::fbBounds() const
{
  if (!currentFb_)
    return core::Rect();
  return core::Rect(0, 0, currentFb_->width(), currentFb_->height());
}

uint64_t RdpSession::nextGeneration()
{
  return ++fbGeneration_;
}

void RdpSession::noopGdiFree(void* /*ptr*/)
{
}

void RdpSession::pushDisconnect(uint32_t code, const std::string& reason)
{
  connected_.store(false);
  RdpEvent ev;
  ev.type = RdpEvent::Type::Disconnect;
  ev.reasonCode = code;
  ev.text = reason;
  eventQueue_.push(std::move(ev));
}

void RdpSession::capturePixels(const core::Region& region,
                               std::vector<uint8_t>& pixels) const
{
  std::vector<core::Rect> rects;
  region.get_rects(&rects);
  size_t size = 0;
  for (const core::Rect& rect : rects)
    size += static_cast<size_t>(rect.area()) * 4;
  pixels.resize(size);

  uint8_t* dst = pixels.data();
  for (const core::Rect& rect : rects) {
    size_t rowBytes = static_cast<size_t>(rect.width()) * 4;
    for (int y = rect.tl.y; y < rect.br.y; ++y) {
      const uint8_t* src = currentFb_->data() +
          static_cast<size_t>(y) * currentFb_->strideBytes() + rect.tl.x * 4;
      std::memcpy(dst, src, rowBytes);
      dst += rowBytes;
    }
  }
}

void RdpSession::pushFrameUpdate(const core::Region& dirty)
{
  if (dirty.is_empty())
    return;
  RdpEvent ev;
  ev.type = RdpEvent::Type::FrameUpdate;
  ev.fbGeneration = fbGeneration_;
  ev.dirty = dirty;
  capturePixels(dirty, ev.pixels);
  eventQueue_.push(std::move(ev));
}

void RdpSession::pushCopyRect(const PendingCopyOp& copy)
{
  RdpEvent ev;
  ev.type = RdpEvent::Type::CopyRect;
  ev.fbGeneration = fbGeneration_;
  ev.rect = copy.dstRect;
  ev.srcX = copy.srcX;
  ev.srcY = copy.srcY;
  capturePixels(core::Region(ev.rect), ev.pixels);
  eventQueue_.push(std::move(ev));
}

bool RdpSession::reallocateFramebuffer(rdpContext* ctx, int width, int height,
                                       const rfb::ScreenSet* explicitLayout)
{
  if (width <= 0 || height <= 0 ||
      width > AlignedBuffer::kMaxDimension ||
      height > AlignedBuffer::kMaxDimension) {
    vlog.error("Invalid RDP framebuffer size %dx%d", width, height);
    return false;
  }

  if (currentFb_ && currentFb_->width() == width && currentFb_->height() == height) {
    if (explicitLayout && explicitLayout->validate(width, height) &&
        *explicitLayout != currentLayout_) {
      currentLayout_ = *explicitLayout;
      RdpEvent ev;
      ev.type = RdpEvent::Type::Resize;
      ev.fbGeneration = fbGeneration_;
      ev.width = width;
      ev.height = height;
      ev.layout = currentLayout_;
      eventQueue_.push(std::move(ev));
    }
    return true;
  }

  uint64_t gen = nextGeneration();
  std::shared_ptr<AlignedBuffer> nextFb = AlignedBuffer::create(width, height, gen);
  auto publishedFb = AlignedBuffer::create(width, height, gen);

  if (!gdiInitialized_) {
    if (!gdi_init_ex(ctx->instance, PIXEL_FORMAT_BGRX32,
                     static_cast<UINT32>(nextFb->strideBytes()),
                     nextFb->data(), &RdpSession::noopGdiFree)) {
      vlog.error("gdi_init_ex failed for %dx%d", width, height);
      return false;
    }
    gdiInitialized_ = true;
  } else {
    if (!gdi_resize_ex(ctx->gdi, static_cast<UINT32>(width),
                       static_cast<UINT32>(height),
                       static_cast<UINT32>(nextFb->strideBytes()),
                       PIXEL_FORMAT_BGRX32, nextFb->data(),
                       &RdpSession::noopGdiFree)) {
      vlog.error("gdi_resize_ex failed for %dx%d", width, height);
      return false;
    }
  }

  currentFb_ = nextFb;
  h264State_.reset();
  gfxPaintDirty_.clear();
  dirtyInGfxFrame_.clear();
  suppressedGfxCopies_.clear();
  suppressedLegacyCopies_.clear();
  pendingGfxCopies_.clear();
  pendingGfxHints_.clear();
  pendingGfxH264_.clear();
  pendingLegacyCopies_.clear();
  pendingLegacyHints_.clear();

  RdpEvent ev;
  ev.type = RdpEvent::Type::Resize;
  ev.fbGeneration = gen;
  ev.framebuffer = std::move(publishedFb);
  ev.width = width;
  ev.height = height;
  currentLayout_ = explicitLayout && explicitLayout->validate(width, height)
                      ? *explicitLayout : buildScreenSetFromDims(width, height);
  ev.layout = currentLayout_;
  eventQueue_.push(std::move(ev));
  return true;
}

void RdpSession::workerMain()
{
  ensureStaticAddinProviderRegistered();

  freerdp* instance = freerdp_new();
  if (!instance) {
    pushDisconnect(0, "Failed to allocate FreeRDP instance");
    running_.store(false);
    return;
  }

  instance->ContextSize = sizeof(RdpClientContextWrapper);
  instance->PreConnect = &RdpSession::cbPreConnect;
  instance->PostConnect = &RdpSession::cbPostConnect;
  instance->PostDisconnect = &RdpSession::cbPostDisconnect;
  instance->AuthenticateEx = &RdpSession::cbAuthenticateEx;
  instance->VerifyCertificateEx = &RdpSession::cbVerifyCertificateEx;
  instance->VerifyChangedCertificateEx = &RdpSession::cbVerifyChangedCertificateEx;
  instance->LoadChannels = freerdp_client_load_channels;

  if (!freerdp_context_new(instance)) {
    freerdp_free(instance);
    pushDisconnect(0, "Failed to allocate FreeRDP context");
    running_.store(false);
    return;
  }

  rdpContext* ctx = instance->context;
  auto* wrapper = reinterpret_cast<RdpClientContextWrapper*>(ctx);
  wrapper->session = this;

  rdpSettings* settings = ctx->settings;
  bool ok = true;
  ok &= freerdp_settings_set_string(settings, FreeRDP_ServerHostname,
                                    config_.host.c_str()) != FALSE;
  ok &= freerdp_settings_set_uint32(settings, FreeRDP_ServerPort,
                                    static_cast<UINT32>(config_.port)) != FALSE;
  if (!config_.username.empty()) {
    ok &= freerdp_settings_set_string(settings, FreeRDP_Username,
                                      config_.username.c_str()) != FALSE;
  }
  if (!config_.password.empty()) {
    ok &= freerdp_settings_set_string(settings, FreeRDP_Password,
                                      config_.password.c_str()) != FALSE;
  }
  if (!config_.domain.empty()) {
    ok &= freerdp_settings_set_string(settings, FreeRDP_Domain,
                                      config_.domain.c_str()) != FALSE;
  }

  ok &= freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity,
                                  config_.nla ? TRUE : FALSE) != FALSE;
  ok &= freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity,
                                  config_.tls ? TRUE : FALSE) != FALSE;
  ok &= freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity,
                                  config_.rdpSecurity ? TRUE : FALSE) != FALSE;
  ok &= freerdp_settings_set_bool(settings, FreeRDP_IgnoreCertificate,
                                  config_.ignoreCert ? TRUE : FALSE) != FALSE;

  ok &= freerdp_settings_set_bool(settings, FreeRDP_SoftwareGdi, TRUE) != FALSE;
  ok &= freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32) != FALSE;
  ok &= freerdp_settings_set_uint32(
      settings, FreeRDP_DesktopWidth,
      static_cast<UINT32>(config_.initialWidth)) != FALSE;
  ok &= freerdp_settings_set_uint32(
      settings, FreeRDP_DesktopHeight,
      static_cast<UINT32>(config_.initialHeight)) != FALSE;
  ok &= freerdp_settings_set_uint32(
      settings, FreeRDP_TcpConnectTimeout,
      static_cast<UINT32>(config_.connectTimeoutMs)) != FALSE;

  ok &= freerdp_settings_set_bool(settings, FreeRDP_AudioPlayback, FALSE) != FALSE;
  ok &= freerdp_settings_set_bool(
      settings, FreeRDP_SupportDynamicChannels,
      (config_.enableGfx || config_.enableDynamicResize) ? TRUE : FALSE) != FALSE;
  // Graphics callbacks and the outbound channel commands share worker state.
  ok &= freerdp_settings_set_bool(
      settings, FreeRDP_SynchronousDynamicChannels, TRUE) != FALSE;
  ok &= freerdp_settings_set_bool(
      settings, FreeRDP_SupportGraphicsPipeline,
      config_.enableGfx ? TRUE : FALSE) != FALSE;
  ok &= freerdp_settings_set_bool(
      settings, FreeRDP_GfxH264, config_.enableGfx ? TRUE : FALSE) != FALSE;

  // FreeRDP omits RDPGFX 10.x capabilities when H.264 is enabled but
  // AVC444 is disabled. GDI decodes AVC444 for the pixel fallback.
  ok &= freerdp_settings_set_bool(
      settings, FreeRDP_GfxAVC444, config_.enableGfx ? TRUE : FALSE) != FALSE;
  ok &= freerdp_settings_set_bool(
      settings, FreeRDP_GfxAVC444v2, config_.enableGfx ? TRUE : FALSE) != FALSE;
  ok &= freerdp_settings_set_bool(
      settings, FreeRDP_SupportDisplayControl,
      config_.enableDynamicResize ? TRUE : FALSE) != FALSE;
  ok &= freerdp_settings_set_bool(
      settings, FreeRDP_DynamicResolutionUpdate,
      config_.enableDynamicResize ? TRUE : FALSE) != FALSE;
  ok &= freerdp_settings_set_bool(
      settings, FreeRDP_RedirectClipboard,
      config_.enableClipboard ? TRUE : FALSE) != FALSE;

  if (!ok) {
    freerdp_context_free(instance);
    freerdp_free(instance);
    pushDisconnect(0, "Failed to apply FreeRDP connection settings");
    running_.store(false);
    return;
  }

  vlog.info("Connecting to RDP server %s:%d", config_.host.c_str(), config_.port);
  bool connected = !stopRequested_.load() && freerdp_connect(instance);
  {
    std::lock_guard<std::mutex> lock(connectionMutex_);
    connectAbortEvent_ = nullptr;
  }
  if (!connected) {
    UINT32 err = freerdp_get_last_error(ctx);
    const char* errStr = freerdp_get_last_error_string(err);
    std::string msg = std::string("RDP connect failed: ") +
                      (errStr ? errStr : "unknown error");
    vlog.error("%s (0x%08x)", msg.c_str(), err);
    freerdp_context_free(instance);
    freerdp_free(instance);
    pushDisconnect(err, msg);
    running_.store(false);
    return;
  }

  HANDLE handles[MAXIMUM_WAIT_OBJECTS];
  std::string disconnectReason = "RDP connection closed";
  UINT32 disconnectCode = 0;

  while (!stopRequested_.load() && !freerdp_shall_disconnect_context(ctx)) {
    drainOutboundCommands(ctx);
    if (stopRequested_.load())
      break;

    {
      std::lock_guard<std::mutex> lock(clipboardMutex_);
      if (waitingLocalClipboardData_ &&
          (monotonicNowMs() - localClipboardReqStartMs_ > 1500)) {
        waitingLocalClipboardData_ = false;
        sendCliprdrDataResponse(false, std::string());
      }
    }

    DWORD count = freerdp_get_event_handles(ctx, handles,
                                            MAXIMUM_WAIT_OBJECTS - 1);
    if (count == 0) {
      disconnectCode = freerdp_get_last_error(ctx);
      disconnectReason = "Failed to query FreeRDP event handles";
      break;
    }
    if (wakeEvent_)
      handles[count++] = wakeEvent_;

    DWORD waitStatus = WaitForMultipleObjects(count, handles, FALSE, 100);
    if (waitStatus == WAIT_FAILED) {
      disconnectCode = freerdp_get_last_error(ctx);
      disconnectReason = "WaitForMultipleObjects failed in RDP worker";
      break;
    }

    if (wakeEvent_)
      ResetEvent(wakeEvent_);
    drainOutboundCommands(ctx);
    if (stopRequested_.load())
      break;

    if (!freerdp_check_event_handles(ctx)) {
      disconnectCode = freerdp_get_last_error(ctx);
      const char* errStr = freerdp_get_last_error_string(disconnectCode);
      disconnectReason = errStr && *errStr ? errStr : "RDP socket EOF or error";
      vlog.info("FreeRDP event loop terminated: %s (0x%08x)",
                disconnectReason.c_str(), disconnectCode);
      break;
    }
  }

  if (freerdp_shall_disconnect_context(ctx) && disconnectCode == 0) {
    disconnectCode = freerdp_get_last_error(ctx);
    if (disconnectCode != 0) {
      const char* errStr = freerdp_get_last_error_string(disconnectCode);
      if (errStr && *errStr)
        disconnectReason = errStr;
    }
  }

  bool stoppedByMain = stopRequested_.load();
  freerdp_disconnect(instance);
  freerdp_context_free(instance);
  freerdp_free(instance);

  currentFb_.reset();
  gdiInitialized_ = false;

  if (!stoppedByMain) {
    pushDisconnect(disconnectCode, disconnectReason);
  } else {
    connected_.store(false);
  }
  running_.store(false);
}

void RdpSession::drainOutboundCommands(rdpContext* ctx)
{
  std::deque<OutboundCommand> batch;
  {
    std::lock_guard<std::mutex> lock(outboundMutex_);
    if (outboundQueue_.empty())
      return;
    batch.swap(outboundQueue_);
  }

  for (const OutboundCommand& cmd : batch) {
    if (stopRequested_.load())
      break;
    executeOutboundCommand(ctx, cmd);
  }
}

void RdpSession::executeOutboundCommand(rdpContext* ctx,
                                        const OutboundCommand& cmd)
{
  if (!ctx || !ctx->input)
    return;

  switch (cmd.type) {
  case OutboundCommand::Type::Key:
    if (cmd.key.kind == KeyAction::Kind::Scancode) {
      freerdp_input_send_keyboard_event(ctx->input, cmd.key.flags,
                                        cmd.key.code);
    } else if (cmd.key.kind == KeyAction::Kind::Pause) {
      freerdp_input_send_keyboard_pause_event(ctx->input);
    } else if (cmd.key.kind == KeyAction::Kind::Unicode) {
      freerdp_input_send_unicode_keyboard_event(ctx->input, cmd.key.flags,
                                                cmd.key.code);
      if (cmd.key.code2 != 0) {
        freerdp_input_send_unicode_keyboard_event(ctx->input, cmd.key.flags,
                                                  cmd.key.code2);
      }
    }
    break;

  case OutboundCommand::Type::Pointer:
    for (const PointerAction& act : cmd.pointerActions) {
      if (act.isExtended) {
        freerdp_input_send_extended_mouse_event(ctx->input, act.flags,
                                                act.x, act.y);
      } else {
        freerdp_input_send_mouse_event(ctx->input, act.flags, act.x, act.y);
      }
    }
    break;

  case OutboundCommand::Type::MonitorLayout: {
    uint64_t area = 0;
    for (const auto& monitor : cmd.monitors)
      area += static_cast<uint64_t>(monitor.Width) * monitor.Height;
    if (disp_ && disp_->SendMonitorLayout &&
        cmd.monitors.size() <= dispMaxMonitors_ && area <= dispMaxArea_) {
      std::vector<DISPLAY_CONTROL_MONITOR_LAYOUT> copy = cmd.monitors;
      if (disp_->SendMonitorLayout(disp_, copy.size(), copy.data()) == CHANNEL_RC_OK)
        break;
    }
    vlog.info("Upstream display control could not accept the requested layout");
    RdpEvent ev;
    ev.type = RdpEvent::Type::ResizeFailed;
    ev.resizeId = cmd.resizeId;
    eventQueue_.push(std::move(ev));
    break;
  }

  case OutboundCommand::Type::ClipboardAnnounce: {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    pendingLocalClipboardAnnounce_ = true;
    pendingLocalClipboardAvailable_ = cmd.clipboardAvailable;
    if (cliprdrReady_)
      sendCliprdrFormatList(cmd.clipboardAvailable);
    break;
  }

  case OutboundCommand::Type::ClipboardRequest: {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    if (cliprdrReady_ && cliprdr_ && cliprdr_->ClientFormatDataRequest) {
      CLIPRDR_FORMAT_DATA_REQUEST req{};
      req.common.msgType = CB_FORMAT_DATA_REQUEST;
      req.common.dataLen = 4;
      req.requestedFormatId = CF_UNICODETEXT;
      cliprdr_->ClientFormatDataRequest(cliprdr_, &req);
    }
    break;
  }

  case OutboundCommand::Type::ClipboardData: {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    if (waitingLocalClipboardData_) {
      waitingLocalClipboardData_ = false;
      sendCliprdrDataResponse(true, cmd.text);
    }
    break;
  }

  case OutboundCommand::Type::RefreshScreen:
    if (ctx->update && ctx->update->RefreshRect && currentFb_) {
      RECTANGLE_16 area{};
      area.left = 0;
      area.top = 0;
      area.right = static_cast<UINT16>(currentFb_->width() - 1);
      area.bottom = static_cast<UINT16>(currentFb_->height() - 1);
      ctx->update->RefreshRect(ctx, 1, &area);
    }
    break;

  }
}

BOOL RdpSession::cbPreConnect(freerdp* instance)
{
  if (!instance || !instance->context)
    return FALSE;
  rdpContext* ctx = instance->context;
  RdpSession* self = fromContext(ctx);
  {
    std::lock_guard<std::mutex> lock(self->connectionMutex_);
    if (self->stopRequested_.load())
      return FALSE;
    self->connectAbortEvent_ = freerdp_abort_event(ctx);
  }
  if (PubSub_SubscribeChannelConnected(
          ctx->pubSub, &RdpSession::cbOnChannelConnected) < 0 ||
      PubSub_SubscribeChannelDisconnected(
          ctx->pubSub, &RdpSession::cbOnChannelDisconnected) < 0) {
    return FALSE;
  }
  return TRUE;
}

BOOL RdpSession::cbPostConnect(freerdp* instance)
{
  if (!instance || !instance->context)
    return FALSE;
  rdpContext* ctx = instance->context;
  RdpSession* self = fromContext(ctx);
  if (!self)
    return FALSE;

  int w = static_cast<int>(freerdp_settings_get_uint32(
      ctx->settings, FreeRDP_DesktopWidth));
  int h = static_cast<int>(freerdp_settings_get_uint32(
      ctx->settings, FreeRDP_DesktopHeight));
  if (w <= 0)
    w = self->config_.initialWidth;
  if (h <= 0)
    h = self->config_.initialHeight;

  self->h264State_.reset();
  if (!self->reallocateFramebuffer(ctx, w, h))
    return FALSE;

  if (ctx->update) {
    self->origUpdate_ = *ctx->update;
    ctx->update->BeginPaint = &RdpSession::cbBeginPaint;
    ctx->update->EndPaint = &RdpSession::cbEndPaint;
    ctx->update->DesktopResize = &RdpSession::cbDesktopResize;
    ctx->update->SurfaceBits = &RdpSession::cbSurfaceBits;

    if (ctx->update->primary) {
      self->origPrimary_ = *ctx->update->primary;
      ctx->update->primary->ScrBlt = &RdpSession::cbScrBlt;
      ctx->update->primary->PatBlt = &RdpSession::cbPatBlt;
      ctx->update->primary->OpaqueRect = &RdpSession::cbOpaqueRect;
    }
  }

  if (ctx->graphics) {
    rdpPointer pointer{};
    pointer.size = sizeof(RdpPointerWrapper);
    pointer.New = &RdpSession::cbPointerNew;
    pointer.Free = &RdpSession::cbPointerFree;
    pointer.Set = &RdpSession::cbPointerSet;
    pointer.SetNull = &RdpSession::cbPointerSetNull;
    pointer.SetDefault = &RdpSession::cbPointerSetDefault;
    pointer.SetPosition = &RdpSession::cbPointerSetPosition;
    graphics_register_pointer(ctx->graphics, &pointer);
  }

  self->connected_.store(true);
  RdpEvent ev;
  ev.type = RdpEvent::Type::Connected;
  ev.fbGeneration = self->fbGeneration_;
  self->eventQueue_.push(std::move(ev));
  return TRUE;
}

void RdpSession::cbPostDisconnect(freerdp* instance)
{
  if (!instance || !instance->context)
    return;
  rdpContext* ctx = instance->context;
  RdpSession* self = fromContext(ctx);
  if (!self)
    return;

  if (ctx->pubSub) {
    PubSub_UnsubscribeChannelConnected(ctx->pubSub,
                                       &RdpSession::cbOnChannelConnected);
    PubSub_UnsubscribeChannelDisconnected(ctx->pubSub,
                                          &RdpSession::cbOnChannelDisconnected);
  }

  if (self->gdiInitialized_) {
    gdi_free(instance);
    self->gdiInitialized_ = false;
  }
  self->gfx_ = nullptr;
  self->gfxHooked_ = false;
  self->disp_ = nullptr;
  std::lock_guard<std::mutex> lock(self->clipboardMutex_);
  self->cliprdr_ = nullptr;
  self->cliprdrReady_ = false;
}

BOOL RdpSession::cbAuthenticateEx(freerdp* instance, char** username,
                                  char** password, char** domain,
                                  rdp_auth_reason /*reason*/)
{
  if (!instance || !instance->context)
    return FALSE;
  RdpSession* self = fromContext(instance->context);
  if (!self || !self->credProvider_)
    return FALSE;

  std::string u, p, d;
  bool isRetry = self->authAttempted_;
  self->authAttempted_ = true;

  if (!self->credProvider_->getCredentials(
          self->config_.host, self->config_.port, u, p, d, isRetry)) {
    vlog.error("RDP authentication required or rejected (non-interactive)");
    return FALSE;
  }

  *username = replaceHeapString(*username, u);
  *password = replaceHeapString(*password, p);
  *domain = replaceHeapString(*domain, d);
  return (*username != nullptr && *password != nullptr) ? TRUE : FALSE;
}

DWORD RdpSession::cbVerifyCertificateEx(freerdp* instance, const char* host,
                                        UINT16 port, const char* commonName,
                                        const char* /*subject*/,
                                        const char* /*issuer*/,
                                        const char* fingerprint, DWORD flags)
{
  RdpSession* self = instance ? fromContext(instance->context) : nullptr;
  if (self && self->config_.ignoreCert) {
    vlog.info("Accepting TLS certificate for %s:%u (CN=%s, fp=%s) per IgnoreCert",
              host ? host : "", port, commonName ? commonName : "",
              fingerprint ? fingerprint : "");
    return 1;
  }
  vlog.error("Untrusted RDP TLS certificate for %s:%u (CN=%s, fp=%s, flags=0x%x); "
             "pass -RdpIgnoreCert or install trusted CA",
             host ? host : "", port, commonName ? commonName : "",
             fingerprint ? fingerprint : "", static_cast<unsigned>(flags));
  return 0;
}

DWORD RdpSession::cbVerifyChangedCertificateEx(
    freerdp* instance, const char* host, UINT16 port, const char* commonName,
    const char* /*subject*/, const char* /*issuer*/, const char* newFingerprint,
    const char* /*oldSubject*/, const char* /*oldIssuer*/,
    const char* oldFingerprint, DWORD flags)
{
  RdpSession* self = instance ? fromContext(instance->context) : nullptr;
  if (self && self->config_.ignoreCert) {
    vlog.info("Accepting changed TLS certificate for %s:%u per IgnoreCert",
              host ? host : "", port);
    return 1;
  }
  vlog.error("Changed RDP TLS certificate for %s:%u (CN=%s, old=%s, new=%s, flags=0x%x)",
             host ? host : "", port, commonName ? commonName : "",
             oldFingerprint ? oldFingerprint : "",
             newFingerprint ? newFingerprint : "",
             static_cast<unsigned>(flags));
  return 0;
}

void RdpSession::cbOnChannelConnected(void* context,
                                      const ChannelConnectedEventArgs* e)
{
  auto* ctx = static_cast<rdpContext*>(context);
  RdpSession* self = fromContext(ctx);
  if (!self || !e || !e->name)
    return;

  if (std::strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) == 0 && e->pInterface) {
    auto* gfx = static_cast<RdpgfxClientContext*>(e->pInterface);
    if (ctx->gdi && gdi_graphics_pipeline_init(ctx->gdi, gfx)) {
      self->gfx_ = gfx;
      self->origGfx_ = *gfx;
      self->gfxHooked_ = true;

      gfx->ResetGraphics = &RdpSession::cbGfxResetGraphics;
      gfx->StartFrame = &RdpSession::cbGfxStartFrame;
      gfx->EndFrame = &RdpSession::cbGfxEndFrame;
      gfx->SurfaceCommand = &RdpSession::cbGfxSurfaceCommand;
      gfx->SolidFill = &RdpSession::cbGfxSolidFill;
      gfx->SurfaceToSurface = &RdpSession::cbGfxSurfaceToSurface;
      gfx->DeleteSurface = &RdpSession::cbGfxDeleteSurface;
      vlog.info("MS-RDPEGFX channel connected and hooks installed");
    }
  } else if (std::strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0 && e->pInterface) {
    auto* disp = static_cast<DispClientContext*>(e->pInterface);
    disp->custom = self;
    disp->DisplayControlCaps = &RdpSession::cbDispCaps;
    self->disp_ = disp;
    self->dispMaxArea_ = 0;
    vlog.info("MS-RDPEDISP channel connected");
  } else if (std::strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0 &&
             e->pInterface) {
    std::lock_guard<std::mutex> lock(self->clipboardMutex_);
    auto* cliprdr = static_cast<CliprdrClientContext*>(e->pInterface);
    cliprdr->custom = self;
    cliprdr->ServerCapabilities = &RdpSession::cbCliprdrServerCaps;
    cliprdr->MonitorReady = &RdpSession::cbCliprdrMonitorReady;
    cliprdr->ServerFormatList = &RdpSession::cbCliprdrServerFormatList;
    cliprdr->ServerFormatListResponse =
        &RdpSession::cbCliprdrServerFormatListResponse;
    cliprdr->ServerFormatDataRequest =
        &RdpSession::cbCliprdrServerFormatDataRequest;
    cliprdr->ServerFormatDataResponse =
        &RdpSession::cbCliprdrServerFormatDataResponse;
    self->cliprdr_ = cliprdr;
    vlog.info("CLIPRDR channel connected");
  }
}

void RdpSession::cbOnChannelDisconnected(void* context,
                                         const ChannelDisconnectedEventArgs* e)
{
  auto* ctx = static_cast<rdpContext*>(context);
  RdpSession* self = fromContext(ctx);
  if (!self || !e || !e->name)
    return;

  if (std::strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) == 0 && e->pInterface) {
    auto* gfx = static_cast<RdpgfxClientContext*>(e->pInterface);
    if (self->gfxHooked_ && self->gfx_ == gfx) {
      gfx->ResetGraphics = self->origGfx_.ResetGraphics;
      gfx->StartFrame = self->origGfx_.StartFrame;
      gfx->EndFrame = self->origGfx_.EndFrame;
      gfx->SurfaceCommand = self->origGfx_.SurfaceCommand;
      gfx->SolidFill = self->origGfx_.SolidFill;
      gfx->SurfaceToSurface = self->origGfx_.SurfaceToSurface;
      gfx->DeleteSurface = self->origGfx_.DeleteSurface;
      self->gfxHooked_ = false;
      self->gfx_ = nullptr;
    }
    if (ctx->gdi)
      gdi_graphics_pipeline_uninit(ctx->gdi, gfx);
  } else if (std::strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0) {
    self->disp_ = nullptr;
  } else if (std::strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0) {
    std::lock_guard<std::mutex> lock(self->clipboardMutex_);
    self->cliprdr_ = nullptr;
    self->cliprdrReady_ = false;
    self->waitingLocalClipboardData_ = false;
  }
}

BOOL RdpSession::cbBeginPaint(rdpContext* ctx)
{
  RdpSession* self = fromContext(ctx);
  if (!self)
    return FALSE;

  self->inPaint_ = true;
  if (!self->inGfxEndFrame_) {
    self->pendingLegacyCopies_.clear();
    self->pendingLegacyHints_.clear();
    self->suppressedLegacyCopies_.clear();
  }

  if (self->origUpdate_.BeginPaint)
    return self->origUpdate_.BeginPaint(ctx);
  return TRUE;
}

void RdpSession::emitPendingCopiesAndDirty(const core::Region& hwndDirty,
                                           std::vector<PendingCopyOp>& copies,
                                           core::Region& suppressedCopies)
{
  // Emit CopyRect operations first before remaining dirty pixel rects.
  for (const PendingCopyOp& cp : copies) {
    pushCopyRect(cp);
  }
  copies.clear();

  core::Region effectiveDirty = hwndDirty.intersect(fbBounds());
  effectiveDirty.assign_subtract(suppressedCopies);
  suppressedCopies.clear();

  pushFrameUpdate(effectiveDirty);
}

void RdpSession::emitPendingHints(std::vector<PendingHintOp>& hints)
{
  for (PendingHintOp& op : hints) {
    RdpEvent ev;
    ev.fbGeneration = fbGeneration_;
    ev.rect = op.rect;
    if (op.kind == PendingHintOp::Kind::SolidFill) {
      // GDI may have clipped a fill or painted over it with a later order.
      bool solid = true;
      for (int y = op.rect.tl.y; y < op.rect.br.y && solid; ++y) {
        const uint8_t* px = currentFb_->data() +
            static_cast<size_t>(y) * currentFb_->strideBytes() + op.rect.tl.x * 4;
        for (int x = op.rect.tl.x; x < op.rect.br.x; ++x, px += 4) {
          if (px[0] != op.b || px[1] != op.g || px[2] != op.r) {
            solid = false;
            break;
          }
        }
      }
      if (!solid)
        continue;
      ev.type = RdpEvent::Type::SolidFill;
      ev.r = op.r;
      ev.g = op.g;
      ev.b = op.b;
    } else if (op.kind == PendingHintOp::Kind::RawRect) {
      ev.type = RdpEvent::Type::RawRect;
    } else {
      ev.type = RdpEvent::Type::JpegRect;
      ev.qualityLevel = op.qualityLevel;
      ev.payload = std::move(op.payload);
    }
    eventQueue_.push(std::move(ev));
  }
  hints.clear();
}

BOOL RdpSession::cbEndPaint(rdpContext* ctx)
{
  RdpSession* self = fromContext(ctx);
  if (!self)
    return FALSE;

  core::Region dirty;
  HGDI_WND hwnd = (ctx->gdi && ctx->gdi->primary && ctx->gdi->primary->hdc)
                      ? ctx->gdi->primary->hdc->hwnd
                      : nullptr;
  if (hwnd) {
    if (hwnd->ninvalid > 0 && hwnd->cinvalid) {
      for (INT32 i = 0; i < hwnd->ninvalid; ++i) {
        const GDI_RGN& rgn = hwnd->cinvalid[i];
        if (!rgn.null && rgn.w > 0 && rgn.h > 0) {
          dirty.assign_union(core::Region(
              core::Rect(rgn.x, rgn.y, rgn.x + rgn.w, rgn.y + rgn.h)));
        }
      }
    } else if (hwnd->invalid && !hwnd->invalid->null &&
               hwnd->invalid->w > 0 && hwnd->invalid->h > 0) {
      dirty.assign_union(core::Region(core::Rect(
          hwnd->invalid->x, hwnd->invalid->y,
          hwnd->invalid->x + hwnd->invalid->w,
          hwnd->invalid->y + hwnd->invalid->h)));
    }
  }

  BOOL ok = TRUE;
  if (self->origUpdate_.EndPaint)
    ok = self->origUpdate_.EndPaint(ctx);

  if (hwnd) {
    if (hwnd->invalid)
      hwnd->invalid->null = TRUE;
    hwnd->ninvalid = 0;
  }

  self->inPaint_ = false;

  if (self->inGfxEndFrame_) {
    self->gfxPaintDirty_.assign_union(dirty);
  } else {
    self->emitPendingCopiesAndDirty(dirty, self->pendingLegacyCopies_,
                                    self->suppressedLegacyCopies_);
    self->emitPendingHints(self->pendingLegacyHints_);
  }
  return ok;
}

BOOL RdpSession::cbDesktopResize(rdpContext* ctx)
{
  RdpSession* self = fromContext(ctx);
  if (!self || !ctx->settings)
    return FALSE;

  int w = static_cast<int>(freerdp_settings_get_uint32(
      ctx->settings, FreeRDP_DesktopWidth));
  int h = static_cast<int>(freerdp_settings_get_uint32(
      ctx->settings, FreeRDP_DesktopHeight));

  vlog.info("RDP upstream DesktopResize to %dx%d", w, h);
  if (!self->reallocateFramebuffer(ctx, w, h, self->gfxResizeLayout_))
    return FALSE;
  return TRUE;
}

void RdpSession::invalidatePendingHintsOverlapping(const core::Rect& rect)
{
  if (inGfxFrame_) {
    dirtyInGfxFrame_.assign_union(core::Region(rect));
    suppressedGfxCopies_.assign_subtract(core::Region(rect));
    removeRectFromHints(pendingGfxHints_, rect);
    for (auto it = pendingGfxH264_.begin(); it != pendingGfxH264_.end();) {
      if (!it->rect.intersect(rect).is_empty()) {
        h264State_.markAllContextsReset();
        it = pendingGfxH264_.erase(it);
      } else
        ++it;
    }
  } else {
    suppressedLegacyCopies_.assign_subtract(core::Region(rect));
    removeRectFromHints(pendingLegacyHints_, rect);
  }
}

BOOL RdpSession::cbSurfaceBits(rdpContext* ctx,
                               const SURFACE_BITS_COMMAND* cmd)
{
  RdpSession* self = fromContext(ctx);
  if (!self || !cmd)
    return FALSE;

  core::Rect r(static_cast<int>(cmd->destLeft), static_cast<int>(cmd->destTop),
               static_cast<int>(cmd->destRight),
               static_cast<int>(cmd->destBottom));
  r = r.intersect(self->fbBounds());

  bool validJpeg = self->config_.enableJpegPassthrough &&
                   cmd->bmp.codecID == RDP_CODEC_ID_JPEG &&
                   !r.is_empty() &&
                   r.width() == static_cast<int>(cmd->bmp.width) &&
                   r.height() == static_cast<int>(cmd->bmp.height) &&
                   RdpH264StreamState::isValidJfif(
                       cmd->bmp.bitmapData, cmd->bmp.bitmapDataLength);

  BOOL ok = TRUE;
  if (self->origUpdate_.SurfaceBits)
    ok = self->origUpdate_.SurfaceBits(ctx, cmd);

  if (ok && validJpeg) {
    self->invalidatePendingHintsOverlapping(r);
    PendingHintOp op;
    op.kind = PendingHintOp::Kind::JpegRect;
    op.rect = r;
    op.qualityLevel = self->config_.jpegQuality;
    op.payload.assign(cmd->bmp.bitmapData,
                      cmd->bmp.bitmapData + cmd->bmp.bitmapDataLength);

    if (self->inPaint_) {
      self->pendingLegacyHints_.push_back(std::move(op));
    } else if (self->inGfxFrame_) {
      self->pendingGfxHints_.push_back(std::move(op));
    } else {
      self->pushFrameUpdate(core::Region(r));

      std::vector<PendingHintOp> immediate;
      immediate.push_back(std::move(op));
      self->emitPendingHints(immediate);
    }
  } else if (!r.is_empty()) {
    self->invalidatePendingHintsOverlapping(r);
  }

  return ok;
}

BOOL RdpSession::cbScrBlt(rdpContext* ctx, const SCRBLT_ORDER* scrBlt)
{
  RdpSession* self = fromContext(ctx);
  if (!self || !scrBlt)
    return FALSE;

  if (!self->origPrimary_.ScrBlt || !self->origPrimary_.ScrBlt(ctx, scrBlt))
    return FALSE;

  if (gdi_rop3_code(static_cast<BYTE>(scrBlt->bRop)) != GDI_SRCCOPY)
    return TRUE;

  core::Rect bounds = self->fbBounds();
  core::Rect dstRect(scrBlt->nLeftRect, scrBlt->nTopRect,
                     scrBlt->nLeftRect + scrBlt->nWidth,
                     scrBlt->nTopRect + scrBlt->nHeight);
  core::Rect srcRect(scrBlt->nXSrc, scrBlt->nYSrc,
                     scrBlt->nXSrc + scrBlt->nWidth,
                     scrBlt->nYSrc + scrBlt->nHeight);

  if (!dstRect.is_empty() &&
      dstRect.enclosed_by(bounds) && srcRect.enclosed_by(bounds)) {
    removeRectFromHints(self->pendingLegacyHints_, dstRect);
    if (self->inPaint_) {
      self->pendingLegacyCopies_.emplace_back(dstRect, srcRect.tl.x, srcRect.tl.y);
      self->suppressedLegacyCopies_.assign_union(core::Region(dstRect));
    } else {
      self->pushCopyRect(PendingCopyOp(dstRect, srcRect.tl.x, srcRect.tl.y));
    }
  }
  return TRUE;
}

BOOL RdpSession::cbPatBlt(rdpContext* ctx, PATBLT_ORDER* patBlt)
{
  RdpSession* self = fromContext(ctx);
  if (!self || !patBlt)
    return FALSE;

  if (!self->origPrimary_.PatBlt || !self->origPrimary_.PatBlt(ctx, patBlt))
    return FALSE;

  DWORD rop = gdi_rop3_code(static_cast<BYTE>(patBlt->bRop));
  bool isSolidPat = (rop == GDI_BLACKNESS) || (rop == GDI_WHITENESS) ||
                    (rop == GDI_PATCOPY && patBlt->brush.style == GDI_BS_SOLID);
  core::Rect r(patBlt->nLeftRect, patBlt->nTopRect,
               patBlt->nLeftRect + patBlt->nWidth,
               patBlt->nTopRect + patBlt->nHeight);
  r = r.intersect(self->fbBounds());

  if (r.is_empty())
    return TRUE;

  self->invalidatePendingHintsOverlapping(r);

  if (isSolidPat && self->currentFb_) {
    // Sample rendered BGRX32 pixel written by FreeRDP GDI to match fb palette/depth.
    size_t offset = static_cast<size_t>(r.tl.y) * self->currentFb_->strideBytes() +
                    static_cast<size_t>(r.tl.x) * 4U;
    const uint8_t* px = self->currentFb_->data() + offset;

    PendingHintOp op;
    op.kind = PendingHintOp::Kind::SolidFill;
    op.rect = r;
    op.b = px[0];
    op.g = px[1];
    op.r = px[2];
    if (self->inPaint_) {
      self->pendingLegacyHints_.push_back(std::move(op));
    } else {
      self->pushFrameUpdate(core::Region(r));

      std::vector<PendingHintOp> immediate;
      immediate.push_back(std::move(op));
      self->emitPendingHints(immediate);
    }
  }
  return TRUE;
}

BOOL RdpSession::cbOpaqueRect(rdpContext* ctx,
                              const OPAQUE_RECT_ORDER* opaqueRect)
{
  RdpSession* self = fromContext(ctx);
  if (!self || !opaqueRect)
    return FALSE;

  if (!self->origPrimary_.OpaqueRect ||
      !self->origPrimary_.OpaqueRect(ctx, opaqueRect)) {
    return FALSE;
  }

  core::Rect r(opaqueRect->nLeftRect, opaqueRect->nTopRect,
               opaqueRect->nLeftRect + opaqueRect->nWidth,
               opaqueRect->nTopRect + opaqueRect->nHeight);
  r = r.intersect(self->fbBounds());
  if (r.is_empty())
    return TRUE;

  self->invalidatePendingHintsOverlapping(r);

  if (self->currentFb_) {
    size_t offset = static_cast<size_t>(r.tl.y) * self->currentFb_->strideBytes() +
                    static_cast<size_t>(r.tl.x) * 4U;
    const uint8_t* px = self->currentFb_->data() + offset;

    PendingHintOp op;
    op.kind = PendingHintOp::Kind::SolidFill;
    op.rect = r;
    op.b = px[0];
    op.g = px[1];
    op.r = px[2];
    if (self->inPaint_) {
      self->pendingLegacyHints_.push_back(std::move(op));
    } else {
      self->pushFrameUpdate(core::Region(r));

      std::vector<PendingHintOp> immediate;
      immediate.push_back(std::move(op));
      self->emitPendingHints(immediate);
    }
  }
  return TRUE;
}

BOOL RdpSession::cbPointerNew(rdpContext* /*ctx*/, rdpPointer* pointer)
{
  auto* wrapper = reinterpret_cast<RdpPointerWrapper*>(pointer);
  if (!wrapper || pointer->width == 0 || pointer->height == 0)
    return FALSE;

  size_t byteLen =
      static_cast<size_t>(pointer->width) * static_cast<size_t>(pointer->height) * 4U;
  auto* rgba = new std::vector<uint8_t>(byteLen, 0);

  if (!freerdp_image_copy_from_pointer_data(
          rgba->data(), PIXEL_FORMAT_RGBA32, pointer->width * 4U, 0, 0,
          pointer->width, pointer->height, pointer->xorMaskData,
          pointer->lengthXorMask, pointer->andMaskData,
          pointer->lengthAndMask, pointer->xorBpp, nullptr)) {
    delete rgba;
    return FALSE;
  }

  wrapper->rgba = rgba;
  return TRUE;
}

void RdpSession::cbPointerFree(rdpContext* /*ctx*/, rdpPointer* pointer)
{
  auto* wrapper = reinterpret_cast<RdpPointerWrapper*>(pointer);
  if (wrapper && wrapper->rgba) {
    delete wrapper->rgba;
    wrapper->rgba = nullptr;
  }
}

BOOL RdpSession::cbPointerSet(rdpContext* ctx, rdpPointer* pointer)
{
  RdpSession* self = fromContext(ctx);
  auto* wrapper = reinterpret_cast<RdpPointerWrapper*>(pointer);
  if (!self || !wrapper || !wrapper->rgba)
    return FALSE;

  RdpEvent ev;
  ev.type = RdpEvent::Type::CursorShape;
  ev.width = static_cast<int>(pointer->width);
  ev.height = static_cast<int>(pointer->height);
  ev.hotX = static_cast<int>(pointer->xPos);
  ev.hotY = static_cast<int>(pointer->yPos);
  ev.payload = *wrapper->rgba;
  self->eventQueue_.push(std::move(ev));
  return TRUE;
}

BOOL RdpSession::cbPointerSetNull(rdpContext* ctx)
{
  RdpSession* self = fromContext(ctx);
  if (!self)
    return FALSE;
  RdpEvent ev;
  ev.type = RdpEvent::Type::CursorNull;
  self->eventQueue_.push(std::move(ev));
  return TRUE;
}

BOOL RdpSession::cbPointerSetDefault(rdpContext* ctx)
{
  return cbPointerSetNull(ctx);
}

BOOL RdpSession::cbPointerSetPosition(rdpContext* ctx, UINT32 x, UINT32 y)
{
  RdpSession* self = fromContext(ctx);
  if (!self)
    return FALSE;
  RdpEvent ev;
  ev.type = RdpEvent::Type::CursorPosition;
  ev.x = static_cast<int>(x);
  ev.y = static_cast<int>(y);
  self->eventQueue_.push(std::move(ev));
  return TRUE;
}

bool RdpSession::tryGetMappedSurfaceOffset(RdpgfxClientContext* gfx,
                                           UINT16 surfaceId,
                                           int& originX, int& originY) const
{
  if (!gfx || !gfx->GetSurfaceData)
    return false;
  auto* surf = static_cast<gdiGfxSurface*>(gfx->GetSurfaceData(gfx, surfaceId));
  if (!surf || !surf->outputMapped || surf->windowMapped)
    return false;
  if (surf->outputOriginX > AlignedBuffer::kMaxDimension ||
      surf->outputOriginY > AlignedBuffer::kMaxDimension)
    return false;
  if ((surf->outputTargetWidth != 0 && surf->outputTargetWidth != surf->width) ||
      (surf->outputTargetHeight != 0 &&
       surf->outputTargetHeight != surf->height)) {
    return false;
  }
  originX = static_cast<int>(surf->outputOriginX);
  originY = static_cast<int>(surf->outputOriginY);
  return true;
}

UINT RdpSession::cbGfxResetGraphics(RdpgfxClientContext* gfx,
                                    const RDPGFX_RESET_GRAPHICS_PDU* resetGraphics)
{
  RdpSession* self = fromGfx(gfx);
  if (!self || !self->origGfx_.ResetGraphics || !resetGraphics)
    return ERROR_INTERNAL_ERROR;

  rfb::ScreenSet layout;
  int64_t left = 0;
  int64_t top = 0;
  if (resetGraphics->monitorCount > 0) {
    left = resetGraphics->monitorDefArray[0].left;
    top = resetGraphics->monitorDefArray[0].top;
    for (UINT32 i = 1; i < resetGraphics->monitorCount; ++i) {
      const MONITOR_DEF& m = resetGraphics->monitorDefArray[i];
      left = std::min(left, static_cast<int64_t>(m.left));
      top = std::min(top, static_cast<int64_t>(m.top));
    }
  }
  for (UINT32 i = 0; i < resetGraphics->monitorCount; ++i) {
    const MONITOR_DEF& m = resetGraphics->monitorDefArray[i];
    int64_t width = static_cast<int64_t>(m.right) - m.left + 1;
    int64_t height = static_cast<int64_t>(m.bottom) - m.top + 1;
    if (m.right - left >= AlignedBuffer::kMaxDimension ||
        m.bottom - top >= AlignedBuffer::kMaxDimension ||
        width <= 0 || height <= 0)
      break;
    layout.add_screen(rfb::Screen(i, m.left - left, m.top - top, width, height, 0));
  }
  if (static_cast<UINT32>(layout.num_screens()) != resetGraphics->monitorCount)
    layout = rfb::ScreenSet();
  self->gfxResizeLayout_ = &layout;
  UINT rc = self->origGfx_.ResetGraphics(gfx, resetGraphics);
  self->gfxResizeLayout_ = nullptr;
  self->h264State_.reset();
  // Some FreeRDP versions skip DesktopResize if only the layout changed.
  auto* gdi = static_cast<rdpGdi*>(gfx->custom);
  if (rc == CHANNEL_RC_OK &&
      !self->reallocateFramebuffer(gdi->context, resetGraphics->width,
                                   resetGraphics->height, &layout))
    return ERROR_INTERNAL_ERROR;
  return rc;
}

UINT RdpSession::cbGfxStartFrame(RdpgfxClientContext* gfx,
                                 const RDPGFX_START_FRAME_PDU* startFrame)
{
  RdpSession* self = fromGfx(gfx);
  if (!self || !self->origGfx_.StartFrame)
    return ERROR_INTERNAL_ERROR;

  self->inGfxFrame_ = true;
  self->dirtyInGfxFrame_.clear();
  self->gfxPaintDirty_.clear();
  self->suppressedGfxCopies_.clear();
  self->pendingGfxCopies_.clear();
  self->pendingGfxHints_.clear();
  if (!self->pendingGfxH264_.empty()) {
    self->pendingGfxH264_.clear();
    self->h264State_.reset();
  }

  return self->origGfx_.StartFrame(gfx, startFrame);
}

void RdpSession::flushPostGfxHints()
{
  for (const PendingH264Op& frame : pendingGfxH264_)
    suppressedGfxCopies_.assign_union(core::Region(frame.rect));
  emitPendingCopiesAndDirty(gfxPaintDirty_, pendingGfxCopies_,
                            suppressedGfxCopies_);
  emitPendingHints(pendingGfxHints_);
  for (PendingH264Op& h : pendingGfxH264_) {
    RdpEvent ev;
    ev.type = RdpEvent::Type::H264Frame;
    ev.fbGeneration = fbGeneration_;
    ev.rect = h.rect;
    ev.payload = std::move(h.annexB);
    ev.h264Flags = h.flags;
    ev.h264IsIdr = h.isIdr;
    ev.fallbackDirty = std::move(h.fallbackDirty);
    capturePixels(core::Region(ev.rect), ev.pixels);
    eventQueue_.push(std::move(ev));
  }
  pendingGfxH264_.clear();
  gfxPaintDirty_.clear();
  dirtyInGfxFrame_.clear();
  suppressedGfxCopies_.clear();
  pendingGfxCopies_.clear();
}

UINT RdpSession::cbGfxEndFrame(RdpgfxClientContext* gfx,
                               const RDPGFX_END_FRAME_PDU* endFrame)
{
  RdpSession* self = fromGfx(gfx);
  if (!self || !self->origGfx_.EndFrame)
    return ERROR_INTERNAL_ERROR;

  self->inGfxEndFrame_ = true;
  UINT rc = self->origGfx_.EndFrame(gfx, endFrame);
  self->inGfxEndFrame_ = false;
  self->inGfxFrame_ = false;

  if (rc == CHANNEL_RC_OK)
    self->flushPostGfxHints();
  return rc;
}

UINT RdpSession::cbGfxDeleteSurface(RdpgfxClientContext* gfx,
                                    const RDPGFX_DELETE_SURFACE_PDU* deleteSurface)
{
  RdpSession* self = fromGfx(gfx);
  if (!self || !self->origGfx_.DeleteSurface)
    return ERROR_INTERNAL_ERROR;

  if (deleteSurface) {
    int ox = 0;
    int oy = 0;
    if (self->tryGetMappedSurfaceOffset(gfx, deleteSurface->surfaceId, ox, oy)) {
      self->h264State_.reset();
      self->pendingGfxH264_.clear();
    }
  }
  return self->origGfx_.DeleteSurface(gfx, deleteSurface);
}

UINT RdpSession::cbGfxSurfaceCommand(RdpgfxClientContext* gfx,
                                     const RDPGFX_SURFACE_COMMAND* cmd)
{
  RdpSession* self = fromGfx(gfx);
  if (!self || !self->origGfx_.SurfaceCommand || !cmd)
    return ERROR_INTERNAL_ERROR;

  int ox = 0;
  int oy = 0;
  bool isMapped = self->tryGetMappedSurfaceOffset(gfx, cmd->surfaceId, ox, oy);
  core::Rect rawRect;
  if (isMapped && cmd->left <= AlignedBuffer::kMaxDimension &&
      cmd->top <= AlignedBuffer::kMaxDimension &&
      cmd->right <= AlignedBuffer::kMaxDimension &&
      cmd->bottom <= AlignedBuffer::kMaxDimension) {
    rawRect = core::Rect(ox + cmd->left, oy + cmd->top,
                         ox + cmd->right, oy + cmd->bottom);
  }
  core::Rect r = rawRect.intersect(self->fbBounds());

  UINT rc = self->origGfx_.SurfaceCommand(gfx, cmd);
  if (rc != CHANNEL_RC_OK)
    return rc;

  if (!r.is_empty())
    self->invalidatePendingHintsOverlapping(r);
  else if (!self->pendingGfxH264_.empty()) {
    // Some codecs update a surface without a destination rectangle.
    self->pendingGfxH264_.clear();
    self->h264State_.reset();
  }

  if (cmd->codecId == RDPGFX_CODECID_AVC444 ||
      cmd->codecId == RDPGFX_CODECID_AVC444v2) {
    self->h264State_.reset();
  } else if (cmd->codecId == RDPGFX_CODECID_AVC420) {
    // RFB paints the entire H.264 rectangle. RDP can instead apply
    // selected regions of a decoded surface, which need pixel fallback.
    auto* avc = static_cast<const RDPGFX_AVC420_BITMAP_STREAM*>(cmd->extra);
    core::Region coverage;
    if (avc && avc->meta.regionRects) {
      for (UINT32 i = 0; i < avc->meta.numRegionRects; ++i) {
        const auto& rect = avc->meta.regionRects[i];
        coverage.assign_union(core::Rect(rect.left, rect.top,
                                        rect.right, rect.bottom));
      }
    }

    ProcessedH264Frame processed;
    if (self->config_.enableH264Passthrough && self->anyH264Client_.load() &&
        self->inGfxFrame_ && !r.is_empty() && r == rawRect &&
        r == self->fbBounds() && coverage == core::Region(r) &&
        avc && self->h264State_.processAccessUnit(avc->data, avc->length, processed)) {
      if (!processed.hasIdr && processed.flags != 0)
        self->h264State_.markAllContextsReset();
      else {
        PendingH264Op op;
        op.rect = r;
        op.annexB = std::move(processed.annexB);
        op.flags = processed.flags;
        op.isIdr = processed.hasIdr && processed.hasSps && processed.hasPps;
        op.fallbackDirty = core::Region(r);
        self->pendingGfxH264_.push_back(std::move(op));
      }
    } else {
      self->h264State_.reset();
    }
  }

  if (!isMapped || r.is_empty())
    return rc;

  // P3: Opt-in zero-copy Raw passthrough on bit-for-bit PIXEL_FORMAT_BGRX32 match
  if (self->config_.allowRawPassthrough &&
      cmd->codecId == RDPGFX_CODECID_UNCOMPRESSED &&
      cmd->format == PIXEL_FORMAT_BGRX32) {
    PendingHintOp op;
    op.kind = PendingHintOp::Kind::RawRect;
    op.rect = r;
    if (self->inGfxFrame_) {
      self->pendingGfxHints_.push_back(std::move(op));
    } else {
      self->pushFrameUpdate(core::Region(r));
      std::vector<PendingHintOp> immediate;
      immediate.push_back(std::move(op));
      self->emitPendingHints(immediate);
    }
  }

  return rc;
}

UINT RdpSession::cbGfxSolidFill(RdpgfxClientContext* gfx,
                                const RDPGFX_SOLID_FILL_PDU* solidFill)
{
  RdpSession* self = fromGfx(gfx);
  if (!self || !self->origGfx_.SolidFill || !solidFill)
    return ERROR_INTERNAL_ERROR;

  UINT rc = self->origGfx_.SolidFill(gfx, solidFill);
  if (rc != CHANNEL_RC_OK)
    return rc;

  int ox = 0;
  int oy = 0;
  if (!self->tryGetMappedSurfaceOffset(gfx, solidFill->surfaceId, ox, oy))
    return rc;

  core::Rect bounds = self->fbBounds();
  for (UINT16 i = 0; i < solidFill->fillRectCount; ++i) {
    const RECTANGLE_16& rc16 = solidFill->fillRects[i];
    core::Rect r(ox + static_cast<int>(rc16.left),
                 oy + static_cast<int>(rc16.top),
                 ox + static_cast<int>(rc16.right),
                 oy + static_cast<int>(rc16.bottom));
    r = r.intersect(bounds);
    if (r.is_empty())
      continue;

    self->invalidatePendingHintsOverlapping(r);

    PendingHintOp op;
    op.kind = PendingHintOp::Kind::SolidFill;
    op.rect = r;
    op.r = solidFill->fillPixel.R;
    op.g = solidFill->fillPixel.G;
    op.b = solidFill->fillPixel.B;
    if (self->inGfxFrame_) {
      self->pendingGfxHints_.push_back(std::move(op));
    } else {
      self->pushFrameUpdate(core::Region(r));
      std::vector<PendingHintOp> immediate;
      immediate.push_back(std::move(op));
      self->emitPendingHints(immediate);
    }
  }
  return rc;
}

UINT RdpSession::cbGfxSurfaceToSurface(
    RdpgfxClientContext* gfx,
    const RDPGFX_SURFACE_TO_SURFACE_PDU* surfaceToSurface)
{
  RdpSession* self = fromGfx(gfx);
  if (!self || !self->origGfx_.SurfaceToSurface || !surfaceToSurface)
    return ERROR_INTERNAL_ERROR;

  UINT rc = self->origGfx_.SurfaceToSurface(gfx, surfaceToSurface);
  if (rc != CHANNEL_RC_OK)
    return rc;

  if (surfaceToSurface->surfaceIdSrc != surfaceToSurface->surfaceIdDest)
    return rc;

  int ox = 0;
  int oy = 0;
  if (!self->tryGetMappedSurfaceOffset(gfx, surfaceToSurface->surfaceIdDest,
                                       ox, oy)) {
    return rc;
  }

  core::Rect bounds = self->fbBounds();
  core::Rect srcRect(ox + static_cast<int>(surfaceToSurface->rectSrc.left),
                     oy + static_cast<int>(surfaceToSurface->rectSrc.top),
                     ox + static_cast<int>(surfaceToSurface->rectSrc.right),
                     oy + static_cast<int>(surfaceToSurface->rectSrc.bottom));
  if (srcRect.is_empty())
    return rc;

  for (UINT16 i = 0; i < surfaceToSurface->destPtsCount; ++i) {
    const RDPGFX_POINT16& pt = surfaceToSurface->destPts[i];
    core::Rect dstRect(ox + static_cast<int>(pt.x),
                       oy + static_cast<int>(pt.y),
                       ox + static_cast<int>(pt.x) + srcRect.width(),
                       oy + static_cast<int>(pt.y) + srcRect.height());

    if (!srcRect.enclosed_by(bounds) || !dstRect.enclosed_by(bounds) ||
        !self->dirtyInGfxFrame_.intersect(srcRect).is_empty()) {
      // Source was modified in this frame or exceeds bounds: fall back to Path A.
      self->invalidatePendingHintsOverlapping(dstRect.intersect(bounds));
      continue;
    }

    if (self->inGfxFrame_) {
      removeRectFromHints(self->pendingGfxHints_, dstRect);
      self->dirtyInGfxFrame_.assign_union(core::Region(dstRect));
      self->pendingGfxCopies_.emplace_back(dstRect, srcRect.tl.x, srcRect.tl.y);
      self->suppressedGfxCopies_.assign_union(core::Region(dstRect));
    } else {
      self->pushCopyRect(PendingCopyOp(dstRect, srcRect.tl.x, srcRect.tl.y));
    }
  }
  return rc;
}

UINT RdpSession::cbDispCaps(DispClientContext* disp, UINT32 maxNumMonitors,
                            UINT32 maxMonitorAreaFactorA,
                            UINT32 maxMonitorAreaFactorB)
{
  if (disp && disp->custom) {
    auto* self = static_cast<RdpSession*>(disp->custom);
    self->dispMaxMonitors_ = maxNumMonitors;
    self->dispMaxArea_ = static_cast<uint64_t>(maxMonitorAreaFactorA) *
                        maxMonitorAreaFactorB;
  }
  return CHANNEL_RC_OK;
}

UINT RdpSession::cbCliprdrServerCaps(CliprdrClientContext* /*cliprdr*/,
                                     const CLIPRDR_CAPABILITIES* /*caps*/)
{
  return CHANNEL_RC_OK;
}

void RdpSession::sendCliprdrFormatList(bool available)
{
  if (!cliprdr_ || !cliprdr_->ClientFormatList)
    return;

  CLIPRDR_FORMAT fmt{};
  fmt.formatId = CF_UNICODETEXT;
  fmt.formatName = nullptr;

  CLIPRDR_FORMAT_LIST list{};
  list.common.msgType = CB_FORMAT_LIST;
  list.common.msgFlags = 0;
  list.numFormats = available ? 1 : 0;
  list.formats = available ? &fmt : nullptr;
  cliprdr_->ClientFormatList(cliprdr_, &list);
}

void RdpSession::sendCliprdrDataResponse(bool ok, const std::string& utf8Text)
{
  if (!cliprdr_ || !cliprdr_->ClientFormatDataResponse)
    return;

  CLIPRDR_FORMAT_DATA_RESPONSE resp{};
  resp.common.msgType = CB_FORMAT_DATA_RESPONSE;

  if (!ok) {
    resp.common.msgFlags = CB_RESPONSE_FAIL;
    resp.common.dataLen = 0;
    resp.requestedFormatData = nullptr;
    cliprdr_->ClientFormatDataResponse(cliprdr_, &resp);
    return;
  }

  std::vector<uint8_t> utf16 = KeyMap::utf8ToUtf16LeCrlf(utf8Text);
  resp.common.msgFlags = CB_RESPONSE_OK;
  resp.common.dataLen = static_cast<UINT32>(utf16.size());
  resp.requestedFormatData = utf16.data();
  cliprdr_->ClientFormatDataResponse(cliprdr_, &resp);
}

UINT RdpSession::cbCliprdrMonitorReady(CliprdrClientContext* cliprdr,
                                       const CLIPRDR_MONITOR_READY* /*ready*/)
{
  RdpSession* self = fromCliprdr(cliprdr);
  if (!self || !cliprdr)
    return CHANNEL_RC_OK;

  CLIPRDR_GENERAL_CAPABILITY_SET genCap{};
  genCap.capabilitySetType = CB_CAPSTYPE_GENERAL;
  genCap.capabilitySetLength = CB_CAPSTYPE_GENERAL_LEN;
  genCap.version = CB_CAPS_VERSION_2;
  genCap.generalFlags = CB_USE_LONG_FORMAT_NAMES;

  CLIPRDR_CAPABILITIES caps{};
  caps.common.msgType = CB_CLIP_CAPS;
  caps.cCapabilitiesSets = 1;
  caps.capabilitySets = reinterpret_cast<CLIPRDR_CAPABILITY_SET*>(&genCap);
  if (cliprdr->ClientCapabilities)
    cliprdr->ClientCapabilities(cliprdr, &caps);

  std::lock_guard<std::mutex> lock(self->clipboardMutex_);
  self->cliprdrReady_ = true;
  if (self->pendingLocalClipboardAnnounce_) {
    self->sendCliprdrFormatList(self->pendingLocalClipboardAvailable_);
  } else {
    self->sendCliprdrFormatList(false);
  }
  return CHANNEL_RC_OK;
}

UINT RdpSession::cbCliprdrServerFormatList(CliprdrClientContext* cliprdr,
                                           const CLIPRDR_FORMAT_LIST* formatList)
{
  RdpSession* self = fromCliprdr(cliprdr);
  if (!self || !cliprdr)
    return CHANNEL_RC_OK;

  bool hasUnicodeText = false;
  if (formatList && formatList->formats) {
    for (UINT32 i = 0; i < formatList->numFormats; ++i) {
      if (formatList->formats[i].formatId == CF_UNICODETEXT) {
        hasUnicodeText = true;
        break;
      }
    }
  }

  if (cliprdr->ClientFormatListResponse) {
    CLIPRDR_FORMAT_LIST_RESPONSE resp{};
    resp.common.msgType = CB_FORMAT_LIST_RESPONSE;
    resp.common.msgFlags = CB_RESPONSE_OK;
    cliprdr->ClientFormatListResponse(cliprdr, &resp);
  }

  RdpEvent ev;
  ev.type = RdpEvent::Type::ClipboardAnnounce;
  ev.clipboardAvailable = hasUnicodeText;
  self->eventQueue_.push(std::move(ev));
  return CHANNEL_RC_OK;
}

UINT RdpSession::cbCliprdrServerFormatListResponse(
    CliprdrClientContext* /*cliprdr*/,
    const CLIPRDR_FORMAT_LIST_RESPONSE* /*response*/)
{
  return CHANNEL_RC_OK;
}

UINT RdpSession::cbCliprdrServerFormatDataRequest(
    CliprdrClientContext* cliprdr, const CLIPRDR_FORMAT_DATA_REQUEST* request)
{
  RdpSession* self = fromCliprdr(cliprdr);
  if (!self || !cliprdr || !request)
    return CHANNEL_RC_OK;

  {
    std::lock_guard<std::mutex> lock(self->clipboardMutex_);
    if (request->requestedFormatId != CF_UNICODETEXT) {
      self->sendCliprdrDataResponse(false, std::string());
      return CHANNEL_RC_OK;
    }

    self->waitingLocalClipboardData_ = true;
    self->localClipboardReqStartMs_ = monotonicNowMs();
  }

  RdpEvent ev;
  ev.type = RdpEvent::Type::ClipboardRequest;
  self->eventQueue_.push(std::move(ev));
  return CHANNEL_RC_OK;
}

UINT RdpSession::cbCliprdrServerFormatDataResponse(
    CliprdrClientContext* cliprdr, const CLIPRDR_FORMAT_DATA_RESPONSE* response)
{
  RdpSession* self = fromCliprdr(cliprdr);
  if (!self || !response)
    return CHANNEL_RC_OK;

  if ((response->common.msgFlags & CB_RESPONSE_OK) == 0 ||
      !response->requestedFormatData || response->common.dataLen == 0) {
    return CHANNEL_RC_OK;
  }

  RdpEvent ev;
  ev.type = RdpEvent::Type::ClipboardData;
  ev.text = KeyMap::utf16LeToUtf8Lf(response->requestedFormatData,
                                    response->common.dataLen);
  self->eventQueue_.push(std::move(ev));
  return CHANNEL_RC_OK;
}

} // namespace rdpvnc
