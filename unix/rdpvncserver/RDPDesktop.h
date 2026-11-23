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

#ifndef __RDPVNCSERVER_RDPDESKTOP_H__
#define __RDPVNCSERVER_RDPDESKTOP_H__

#include <cstdint>
#include <memory>
#include <string>

#include <core/Rect.h>
#include <core/Timer.h>
#include <rfb/SDesktop.h>
#include <rfb/ScreenSet.h>
#include <rfb/VNCServer.h>

#include "RdpPixelBuffer.h"
#include "RdpSession.h"

namespace rdpvnc {

/**
 * Runtime policy parameters for RDPDesktop (lifecycle, resize debounce,
 * bounded reconnect, and clipboard anti-loop guards).
 */
struct DesktopPolicyConfig {
  int resizeDebounceMs = 400; // Clamped to mandatory [300, 500] ms
  int idleDisconnectSec = 0;  // 0 = disconnect upstream immediately in stop()
  int maxReconnectAttempts = 0; // 0 = close VNC clients immediately on drop
};

/**
 * TigerVNC SDesktop implementation running on the main poll() thread.
 * Consumes events from RdpSession via an eventfd-backed FIFO queue, updates
 * VNCServerST without blocking on FreeRDP I/O, debounces dynamic resize events,
 * and enforces the Zero Silent-Hang invariant on upstream disconnects.
 */
class RDPDesktop : public rfb::SDesktop, public core::Timer::Callback {
public:
  enum class State {
    Idle,
    Connecting,
    Connected,
    IdleGrace,
    Reconnecting,
  };

  RDPDesktop(const RdpConfig& rdpConfig,
             const DesktopPolicyConfig& policy = DesktopPolicyConfig(),
             std::shared_ptr<RdpCredentialProvider> credProvider = nullptr);
  ~RDPDesktop() override;

  RDPDesktop(const RDPDesktop&) = delete;
  RDPDesktop& operator=(const RDPDesktop&) = delete;

  int eventFd() const { return eventQueue_.fd(); }
  State state() const { return state_; }

  /**
   * Drains and dispatches all queued events from the FreeRDP worker thread on
   * the TigerVNC main poll() thread.
   */
  void processEvents();

  // rfb::SDesktop interface
  void init(rfb::VNCServer* vs) override;
  void start() override;
  void stop() override;
  void queryConnection(rfb::SConnection* conn) override;
  void terminate() override;

  void pointerEvent(const core::Point& pos, uint16_t buttonMask) override;
  void keyEvent(uint32_t keysym, uint32_t keycode, bool down) override;

  void handleClipboardRequest() override;
  void handleClipboardAnnounce(bool available) override;
  void handleClipboardData(const char* data) override;

  unsigned int setScreenLayout(int fb_width, int fb_height,
                               const rfb::ScreenSet& layout) override;

  void clientEncodingsChanged() override;

  // core::Timer::Callback interface
  void handleTimeout(core::Timer* t) override;

private:
  enum class ClipboardOrigin {
    None,
    LocalVnc,
    RemoteRdp,
  };

  void startUpstreamSession();
  void teardownUpstreamSession();
  void handleSingleEvent(RdpEvent& ev);
  void applyPixels(const core::Region& region, const uint8_t* pixels);
  void flushDebouncedResize();
  void finishResize(unsigned int result);
  void finishReconnectFailure(const std::string& reason);

  RdpConfig rdpConfig_;
  DesktopPolicyConfig policy_;
  std::shared_ptr<RdpCredentialProvider> credProvider_;

  rfb::VNCServer* server_ = nullptr;
  RdpPixelBuffer pixelBuffer_;
  RdpEventQueue eventQueue_;
  std::unique_ptr<RdpSession> session_;

  State state_ = State::Idle;
  bool started_ = false;
  uint16_t prevButtonMask_ = 0;

  // Debounced VNC -> RDP MS-RDPEDISP resize state
  core::Timer resizeTimer_;
  bool hasPendingResize_ = false;
  bool resizeSent_ = false;
  uint64_t resizeId_ = 0;
  int pendingResizeWidth_ = 0;
  int pendingResizeHeight_ = 0;
  rfb::ScreenSet pendingResizeLayout_;
  rfb::ScreenSet currentLayout_;

  // Idle disconnect & bounded reconnect state
  core::Timer idleTimer_;
  core::Timer reconnectTimer_;
  int reconnectAttempts_ = 0;
  std::string lastDisconnectReason_;

  // Bidirectional clipboard echo-suppression state
  ClipboardOrigin lastClipboardOrigin_ = ClipboardOrigin::None;
  uint64_t lastRemoteAnnounceMs_ = 0;
  uint64_t lastRemoteDataMs_ = 0;
  uint64_t lastLocalDataSentMs_ = 0;
  std::string lastRemoteClipboardText_;
  std::string lastLocalClipboardText_;
  bool awaitingVncClipboardForRdp_ = false;
};

} // namespace rdpvnc

#endif // __RDPVNCSERVER_RDPDESKTOP_H__
