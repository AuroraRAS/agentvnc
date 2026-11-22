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

#ifndef __SCRCPYVNCSERVER_SCRCPYCLIENT_H__
#define __SCRCPYVNCSERVER_SCRCPYCLIENT_H__

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include <rfb/SConnection.h>

#include "ZeroCopyOutStream.h"

namespace network { class Socket; }

class ScrcpyServer;

class ScrcpyClient : public rfb::SConnection {
public:
  ScrcpyClient(ScrcpyServer* server, network::Socket* sock,
               rfb::AccessRights accessRights);
  ~ScrcpyClient() override;

  bool init();
  void processSocketReadEvent();
  void processSocketWriteEvent();

  bool hasBufferedWriteData();

  void close(const char* reason) override;
  bool accessCheck(rfb::AccessRights ar) const override;

  const std::string& closeReason() const { return closeReason_; }

  bool readyForVideo();
  bool needsKeyFrame() const { return needsKeyFrame_; }

  void onDesktopResize(int width, int height);
  void onConfigUpdated();
  void setDesktopNameOrClose(const char* name);
  void announceClipboardOrClose(bool available);
  void sendClipboardDataOrClose(const char* data);

  // Zero-copy RFB Open H.264 frame delivery. Returns true if a frame was
  // written (or already satisfied), false if skipped/dropped.
  bool sendH264Frame(int width, int height,
                     const std::vector<uint8_t>& spsPps,
                     const uint8_t* nalData, size_t nalLen,
                     bool isKeyFrame, bool hasInlineSpsPps);

protected:
  void authSuccess() override;
  void queryConnection() override;
  void clientReady(bool shared) override;
  void setEncodings(int nEncodings, const int32_t* encodings) override;
  void supportsFence() override;
  void supportsContinuousUpdates() override;
  void setPixelFormat(const rfb::PixelFormat& pf) override;
  void framebufferUpdateRequest(const core::Rect& r,
                                bool incremental) override;
  void setDesktopSize(int fb_width, int fb_height,
                      const rfb::ScreenSet& layout) override;
  void fence(uint32_t flags, unsigned len, const uint8_t data[]) override;
  void enableContinuousUpdates(bool enable, int x, int y,
                               int w, int h) override;
  void pointerEvent(const core::Point& pos, uint16_t buttonMask) override;
  void keyEvent(uint32_t keysym, uint32_t keycode, bool down) override;
  void handleClipboardRequest() override;
  void handleClipboardAnnounce(bool available) override;
  void handleClipboardData(const char* data) override;

private:
  void flushNoDataUpdateIfReady();

  ScrcpyServer* server_;
  ZeroCopyOutStream zeroCopyOs_;
  bool inProcessMessages_;
  bool receivedEncodings_;
  bool continuousUpdates_;
  bool updateRequested_;
  bool pendingKeyFrameRequest_;
  bool needsKeyFrame_;
  bool needsReset_;
  bool needsResetAll_;
  bool needsSpsPps_;

  core::Timer idleTimer_;
  std::string closeReason_;

  class IdleTimerCallback : public core::Timer::Callback {
  public:
    explicit IdleTimerCallback(ScrcpyClient* c) : client_(c) {}
    void handleTimeout(core::Timer* t) override { client_->onIdleTimeout(t); }
  private:
    ScrcpyClient* client_;
  } idleCb_;

  void onIdleTimeout(core::Timer* t);

  ScrcpyClient(const ScrcpyClient&) = delete;
  ScrcpyClient& operator=(const ScrcpyClient&) = delete;
};

#endif // __SCRCPYVNCSERVER_SCRCPYCLIENT_H__
