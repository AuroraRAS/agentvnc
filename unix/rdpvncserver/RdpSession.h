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

#ifndef __RDPVNCSERVER_RDPSESSION_H__
#define __RDPVNCSERVER_RDPSESSION_H__

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <core/Rect.h>
#include <core/Region.h>
#include <rfb/ScreenSet.h>

#include <freerdp/freerdp.h>
#include <freerdp/client.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/client/disp.h>
#include <freerdp/client/rdpgfx.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/graphics.h>
#include <freerdp/primary.h>
#include <freerdp/update.h>
#include <winpr/synch.h>

#include "KeyMap.h"
#include "RdpH264Passthrough.h"
#include "RdpPixelBuffer.h"

namespace rdpvnc {

/**
 * Upstream RDP connection configuration passed to RdpSession.
 */
struct RdpConfig {
  std::string host = "127.0.0.1";
  int port = 3389;
  std::string username;
  std::string password;
  std::string domain;

  bool nla = true;
  bool tls = true;
  bool rdpSecurity = true;
  bool ignoreCert = false;

  bool enableGfx = true;
  bool enableH264Passthrough = true;
  bool allowRawPassthrough = false;
  bool enableJpegPassthrough = true;
  int jpegQuality = -1;

  bool enableClipboard = true;
  bool enableDynamicResize = true;

  int initialWidth = 1280;
  int initialHeight = 720;
  int connectTimeoutMs = 15000;
};

/**
 * Credential provider abstraction designed so future extensions can pass VNC
 * authentication credentials directly into upstream RDP NLA without blocking.
 */
class RdpCredentialProvider {
public:
  virtual ~RdpCredentialProvider() = default;

  virtual bool getCredentials(const std::string& host, int port,
                              std::string& username, std::string& password,
                              std::string& domain, bool isRetry) = 0;
};

/**
 * Default non-blocking credential provider seeded from CLI/config parameters.
 */
class StaticCredentialProvider : public RdpCredentialProvider {
public:
  StaticCredentialProvider(std::string username, std::string password,
                           std::string domain);

  bool getCredentials(const std::string& host, int port,
                      std::string& username, std::string& password,
                      std::string& domain, bool isRetry) override;

private:
  std::string username_;
  std::string password_;
  std::string domain_;
};

/**
 * FreeRDP 3 worker-thread owner. Runs the entire FreeRDP network/TLS/GDI/GFX
 * event loop off the TigerVNC poll() thread and pushes thread-safe graphics,
 * cursor, clipboard, resize, and disconnect events via RdpEventQueue.
 */
class RdpSession {
public:
  RdpSession(const RdpConfig& config, RdpEventQueue& eventQueue,
             std::shared_ptr<RdpCredentialProvider> credProvider = nullptr);
  ~RdpSession();

  RdpSession(const RdpSession&) = delete;
  RdpSession& operator=(const RdpSession&) = delete;

  struct PendingHintOp {
    enum class Kind {
      SolidFill,
      RawRect,
      JpegRect,
    };
    Kind kind = Kind::SolidFill;
    core::Rect rect;
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    int qualityLevel = -1;
    std::vector<uint8_t> payload;
  };

  struct PendingCopyOp {
    PendingCopyOp() = default;
    PendingCopyOp(const core::Rect& dst, int sx, int sy)
      : dstRect(dst), srcX(sx), srcY(sy)
    {
    }
    core::Rect dstRect;
    int srcX = 0;
    int srcY = 0;
  };

  struct PendingH264Op {
    core::Rect rect;
    std::vector<uint8_t> annexB;
    uint32_t flags = 0;
    bool isIdr = false;
    core::Region fallbackDirty;
  };

  /**
   * Spawns the FreeRDP worker thread and connects to the configured upstream
   * RDP server. Idempotent if already running.
   */
  bool start();

  /**
   * Signals the worker thread to disconnect and joins it cleanly.
   */
  void stop();

  bool isRunning() const { return running_.load(); }
  bool isConnected() const { return connected_.load(); }

  // Thread-safe outbound commands enqueued from the TigerVNC main thread
  void sendKeyEvent(uint32_t keysym, uint32_t keycode, bool down);
  void sendPointerEvent(int x, int y, uint16_t buttonMask,
                        uint16_t prevButtonMask, int fbWidth, int fbHeight);
  void sendMonitorLayout(const std::vector<DISPLAY_CONTROL_MONITOR_LAYOUT>& monitors);
  void sendClipboardAnnounce(bool available);
  void sendClipboardRequest();
  void sendClipboardData(const std::string& utf8Text);
  void requestRefresh();

  /**
   * Updates downstream VNC client H.264 capability summary so AVC420 bypass
   * vs parallel Path A GDI decoding can adapt immediately.
   */
  void setClientH264Caps(bool anyH264Client, bool allH264Synced);

private:
  struct OutboundCommand {
    enum class Type {
      Key,
      Pointer,
      MonitorLayout,
      ClipboardAnnounce,
      ClipboardRequest,
      ClipboardData,
      RefreshScreen,
      Stop,
    };

    Type type = Type::Stop;
    KeyAction key;
    std::vector<PointerAction> pointerActions;
    std::vector<DISPLAY_CONTROL_MONITOR_LAYOUT> monitors;
    bool clipboardAvailable = false;
    std::string text;
  };

  void workerMain();
  void enqueueOutbound(OutboundCommand&& cmd);
  void drainOutboundCommands(rdpContext* ctx);
  void executeOutboundCommand(rdpContext* ctx, const OutboundCommand& cmd);
  void pushDisconnect(uint32_t code, const std::string& reason);

  core::Rect fbBounds() const;
  uint64_t nextGeneration();
  bool reallocateFramebuffer(rdpContext* ctx, int width, int height,
                             const rfb::ScreenSet* explicitLayout = nullptr);

  // FreeRDP instance / lifecycle callbacks
  static void noopGdiFree(void* ptr);
  static BOOL cbPreConnect(freerdp* instance);
  static BOOL cbPostConnect(freerdp* instance);
  static void cbPostDisconnect(freerdp* instance);
  static BOOL cbAuthenticateEx(freerdp* instance, char** username,
                               char** password, char** domain,
                               rdp_auth_reason reason);
  static DWORD cbVerifyCertificateEx(freerdp* instance, const char* host,
                                     UINT16 port, const char* commonName,
                                     const char* subject, const char* issuer,
                                     const char* fingerprint, DWORD flags);
  static DWORD cbVerifyChangedCertificateEx(
      freerdp* instance, const char* host, UINT16 port,
      const char* commonName, const char* subject, const char* issuer,
      const char* newFingerprint, const char* oldSubject,
      const char* oldIssuer, const char* oldFingerprint, DWORD flags);
  static void cbOnChannelConnected(void* context,
                                   const ChannelConnectedEventArgs* e);
  static void cbOnChannelDisconnected(void* context,
                                      const ChannelDisconnectedEventArgs* e);

  // Update / GDI / Primary / Pointer callbacks
  static BOOL cbBeginPaint(rdpContext* ctx);
  static BOOL cbEndPaint(rdpContext* ctx);
  static BOOL cbDesktopResize(rdpContext* ctx);
  static BOOL cbSurfaceBits(rdpContext* ctx, const SURFACE_BITS_COMMAND* cmd);
  static BOOL cbScrBlt(rdpContext* ctx, const SCRBLT_ORDER* scrBlt);
  static BOOL cbPatBlt(rdpContext* ctx, PATBLT_ORDER* patBlt);
  static BOOL cbOpaqueRect(rdpContext* ctx,
                           const OPAQUE_RECT_ORDER* opaqueRect);

  static BOOL cbPointerNew(rdpContext* ctx, rdpPointer* pointer);
  static void cbPointerFree(rdpContext* ctx, rdpPointer* pointer);
  static BOOL cbPointerSet(rdpContext* ctx, rdpPointer* pointer);
  static BOOL cbPointerSetNull(rdpContext* ctx);
  static BOOL cbPointerSetDefault(rdpContext* ctx);
  static BOOL cbPointerSetPosition(rdpContext* ctx, UINT32 x, UINT32 y);

  // MS-RDPEGFX callbacks
  static UINT cbGfxResetGraphics(RdpgfxClientContext* gfx,
                                 const RDPGFX_RESET_GRAPHICS_PDU* resetGraphics);
  static UINT cbGfxStartFrame(RdpgfxClientContext* gfx,
                              const RDPGFX_START_FRAME_PDU* startFrame);
  static UINT cbGfxEndFrame(RdpgfxClientContext* gfx,
                            const RDPGFX_END_FRAME_PDU* endFrame);
  static UINT cbGfxSurfaceCommand(RdpgfxClientContext* gfx,
                                  const RDPGFX_SURFACE_COMMAND* cmd);
  static UINT cbGfxSolidFill(RdpgfxClientContext* gfx,
                             const RDPGFX_SOLID_FILL_PDU* solidFill);
  static UINT cbGfxSurfaceToSurface(
      RdpgfxClientContext* gfx,
      const RDPGFX_SURFACE_TO_SURFACE_PDU* surfaceToSurface);
  static UINT cbGfxDeleteSurface(
      RdpgfxClientContext* gfx,
      const RDPGFX_DELETE_SURFACE_PDU* deleteSurface);

  // MS-RDPEDISP callbacks
  static UINT cbDispCaps(DispClientContext* disp, UINT32 maxNumMonitors,
                         UINT32 maxMonitorAreaFactorA,
                         UINT32 maxMonitorAreaFactorB);

  // CLIPRDR callbacks
  static UINT cbCliprdrServerCaps(CliprdrClientContext* cliprdr,
                                  const CLIPRDR_CAPABILITIES* caps);
  static UINT cbCliprdrMonitorReady(CliprdrClientContext* cliprdr,
                                    const CLIPRDR_MONITOR_READY* monitorReady);
  static UINT cbCliprdrServerFormatList(
      CliprdrClientContext* cliprdr, const CLIPRDR_FORMAT_LIST* formatList);
  static UINT cbCliprdrServerFormatListResponse(
      CliprdrClientContext* cliprdr,
      const CLIPRDR_FORMAT_LIST_RESPONSE* response);
  static UINT cbCliprdrServerFormatDataRequest(
      CliprdrClientContext* cliprdr,
      const CLIPRDR_FORMAT_DATA_REQUEST* request);
  static UINT cbCliprdrServerFormatDataResponse(
      CliprdrClientContext* cliprdr,
      const CLIPRDR_FORMAT_DATA_RESPONSE* response);

  // Graphics coherence helpers (worker thread only)
  bool tryGetMappedSurfaceOffset(RdpgfxClientContext* gfx, UINT16 surfaceId,
                                 int& originX, int& originY) const;
  void invalidatePendingHintsOverlapping(const core::Rect& rect);
  void emitPendingHints(std::vector<PendingHintOp>& hints);
  void emitPendingCopiesAndDirty(const core::Region& hwndDirty,
                                 std::vector<PendingCopyOp>& copies,
                                 core::Region& suppressedCopies);
  void flushPostGfxHints();
  void sendCliprdrFormatList(bool available);
  void sendCliprdrDataResponse(bool ok, const std::string& utf8Text);

  static RdpSession* fromContext(rdpContext* ctx);
  static RdpSession* fromGfx(RdpgfxClientContext* gfx);
  static RdpSession* fromCliprdr(CliprdrClientContext* cliprdr);

  RdpConfig config_;
  RdpEventQueue& eventQueue_;
  std::shared_ptr<RdpCredentialProvider> credProvider_;

  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};
  std::atomic<bool> stopRequested_{false};
  std::atomic<bool> anyH264Client_{false};
  std::atomic<bool> allH264Synced_{false};

  std::thread workerThread_;
  HANDLE wakeEvent_ = nullptr;

  std::mutex outboundMutex_;
  std::deque<OutboundCommand> outboundQueue_;

  // Worker-thread-only FreeRDP runtime state
  std::shared_ptr<AlignedBuffer> currentFb_;
  uint64_t fbGeneration_ = 0;
  bool gdiInitialized_ = false;
  bool authAttempted_ = false;

  rdpUpdate origUpdate_{};
  rdpPrimaryUpdate origPrimary_{};
  RdpgfxClientContext origGfx_{};
  bool gfxHooked_ = false;
  RdpgfxClientContext* gfx_ = nullptr;
  DispClientContext* disp_ = nullptr;
  CliprdrClientContext* cliprdr_ = nullptr;

  bool cliprdrReady_ = false;
  bool pendingLocalClipboardAnnounce_ = false;
  bool pendingLocalClipboardAvailable_ = false;
  bool waitingLocalClipboardData_ = false;
  uint64_t localClipboardReqStartMs_ = 0;

  UINT32 dispMaxMonitors_ = 16;

  // Frame / paint tracking (worker thread only)
  bool inGfxFrame_ = false;
  bool inGfxEndFrame_ = false;
  bool inPaint_ = false;
  core::Region dirtyInGfxFrame_;
  core::Region suppressedGfxCopies_;
  core::Region suppressedLegacyCopies_;
  std::vector<PendingCopyOp> pendingGfxCopies_;
  std::vector<PendingHintOp> pendingGfxHints_;
  std::vector<PendingH264Op> pendingGfxH264_;
  std::vector<PendingCopyOp> pendingLegacyCopies_;
  std::vector<PendingHintOp> pendingLegacyHints_;

  RdpH264StreamState h264State_;
};

} // namespace rdpvnc

#endif // __RDPVNCSERVER_RDPSESSION_H__
