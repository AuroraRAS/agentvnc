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

#ifndef __SCRCPYVNCSERVER_SCRCPYSERVER_H__
#define __SCRCPYVNCSERVER_SCRCPYSERVER_H__

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>

#include <list>
#include <string>
#include <vector>

#include <core/Timer.h>
#include <rfb/AccessRights.h>
#include <rfb/Blacklist.h>

#include "ScrcpyControl.h"
#include "ScrcpyStream.h"

namespace network {
  class Socket;
  class SocketListener;
}

namespace rfb {
  struct GIIDevice;
  struct GIIEvent;
}

class ScrcpyClient;

class ScrcpyServer : public ScrcpyStream::Callbacks,
                     public ScrcpyControl::Callbacks,
                     public core::Timer::Callback {
public:
  ScrcpyServer();
  ~ScrcpyServer() override;

  // Starts/connects scrcpy-server over direct ADB TCP (127.0.0.1:5037 by
  // default) and performs the initial video stream handshake.
  void startScrcpy();

  // Attaches already-connected fds (useful for unit tests).
  void attachStreamsForTest(int videoFd, int controlFd,
                            ScrcpyProtocol protocol,
                            int width, int height,
                            const std::string& deviceName);
  void setAdbTargetForTest(const std::string& host, int port,
                           const std::string& serial);

  // Main poll() event loop. Runs until caughtSignal becomes true or
  // terminate() is called.
  void run(const std::list<network::SocketListener*>& listeners,
           const volatile sig_atomic_t* caughtSignal);

  // Processes one poll() iteration; exposed for deterministic unit tests.
  bool step(const std::list<network::SocketListener*>& listeners,
            int timeoutMs);

  void terminate();

  bool addSocket(network::Socket* sock,
                 rfb::AccessRights ar = rfb::AccessDefault);
  void removeSocket(network::Socket* sock);
  void closeClients(const char* reason, ScrcpyClient* except = nullptr);

  int width() const { return stream_.width(); }
  int height() const { return stream_.height(); }
  const std::string& desktopName() const { return desktopName_; }
  const std::string& adbHost() const { return adbHost_; }
  int adbPort() const { return adbPort_; }
  const std::string& deviceSerial() const { return deviceSerial_; }

  // Called by ScrcpyClient
  void clientReady(ScrcpyClient* client, bool shared);
  void clearBlacklist(const char* address);
  void noteClientActivity();
  void clientRequestsKeyFrame(ScrcpyClient* client);
  void clientPointerEvent(ScrcpyClient* client,
                          int x, int y, uint16_t buttonMask);
  void clientKeyEvent(uint32_t keysym, uint32_t keycode, bool down);
  void clientClipboardRequest(ScrcpyClient* client);
  void clientClipboardAnnounce(ScrcpyClient* client, bool available);
  void clientClipboardData(ScrcpyClient* client, const char* data);
  bool supportsGII() const;
  uint32_t clientGIIDeviceCreate(ScrcpyClient* client,
                                 const rfb::GIIDevice& device);
  void clientGIIDeviceDestroy(ScrcpyClient* client, uint32_t deviceOrigin);
  void clientGIIEvent(ScrcpyClient* client, const rfb::GIIEvent& event);
  size_t activeGIIDeviceCount() const { return control_.activeGIIDeviceCount(); }

  // ScrcpyStream::Callbacks
  void onResize(int width, int height) override;
  void onConfig(const std::vector<uint8_t>& spsPps) override;
  void onFrame(const uint8_t* data, size_t len,
               bool isKeyFrame, bool hasInlineSpsPps) override;

  // ScrcpyControl::Callbacks
  void onDeviceClipboard(const std::string& text) override;

  // core::Timer::Callback
  void handleTimeout(core::Timer* t) override;

private:
  int authClientCount() const;
  std::string resolveServerPath() const;
  std::string buildServerCommand(const std::string& version) const;
  void requestVideoReset();
  bool drainShellOutput();
  void shutdownScrcpy();

  rfb::Blacklist blacklist_;
  std::list<ScrcpyClient*> clients_;
  ScrcpyClient* pointerClient_;
  time_t pointerClientTime_;
  ScrcpyClient* clipboardClient_;

  ScrcpyStream stream_;
  ScrcpyControl control_;
  int shellFd_;
  std::string shellOutput_;
  std::string detectedShellVersion_;

  std::string desktopName_;
  std::string adbHost_;
  int adbPort_;
  std::string deviceSerial_;
  std::string deviceClipboard_;
  uint64_t clipboardSeq_;

  std::vector<uint8_t> lastIdrFrame_;
  bool lastIdrHasInlineSpsPps_;
  bool lastIdrValid_;

  struct timeval lastResetReq_;
  bool hasResetReq_;
  bool terminateRequested_;

  core::Timer idleTimer_;
  core::Timer disconnectTimer_;
  core::Timer connectTimer_;

  ScrcpyServer(const ScrcpyServer&) = delete;
  ScrcpyServer& operator=(const ScrcpyServer&) = delete;
};

#endif // __SCRCPYVNCSERVER_SCRCPYSERVER_H__
