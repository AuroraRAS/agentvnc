/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright 2009-2014 Pierre Ossman for Cendio AB
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

#ifndef __CCONN_H__
#define __CCONN_H__

#include <stdint.h>

#include <deque>
#include <map>
#include <string>
#include <vector>

#include <FL/Fl.H>

#include <core/Timer.h>

#include <rfb/CConnection.h>

namespace network { class Socket; }
#ifndef WIN32
struct pollfd;
#endif

class AgentBridge;
class AudioOutput;
class DesktopWindow;
#ifndef WIN32
class FtpEventLoop;
class FtpBackend;
class FtpServer;
#endif

class CConn : public rfb::CConnection
{
public:
  CConn();
  ~CConn();

  void connect(const char* vncServerName, network::Socket* sock=nullptr);

  std::string connectionInfo();

#ifndef WIN32
  bool startFtpServer(std::string* errorMsg=nullptr);
  void stopFtpServer();
  void updateFtpServer();
  bool isFtpServerRunning() const;
  std::string ftpServerUrl() const;
  void flushOutput();
  bool isOutputCongested() const;
  void collectFtpPollFds(std::vector<struct pollfd>* outFds,
                         int* nextTimeoutMs) const;
  void handleFtpPollEvents(const std::vector<struct pollfd>& pfds,
                           size_t startIdx);
#endif

  unsigned getUpdateCount();
  unsigned getPixelCount();
  unsigned getPosition();

  void setAgentBridge(AgentBridge* bridge);
  bool hasAgentBridge() const { return agentBridge != nullptr; }

  bool isSessionReady();
  bool isConnected() {
    return sock != nullptr &&
           state() != RFBSTATE_CLOSING &&
           state() != RFBSTATE_INVALID;
  }

  int getSocketFd() const;
  bool isSocketWatched() const { return socketWatched; }
  bool wantsSocketWrite() const { return socketWatchWrite; }
  void onSocketReady();
  void ensureSocketWriteWatch();

  bool isAgentPaused() const { return agentPaused; }
  void setAgentPaused(bool paused, const char* reason = nullptr);
  const std::string& getAgentPauseReason() const { return agentPauseReason; }

  struct AgentOverlayState {
    std::string state;
    std::string action;
    std::string intent;
    bool hasPoint;
    int x;
    int y;
    bool hasEndPoint;
    int endX;
    int endY;
    uint64_t seq;
    bool triggerPulse;
    uint16_t buttonMask;

    AgentOverlayState()
      : state("idle"), hasPoint(false), x(0), y(0),
        hasEndPoint(false), endX(0), endY(0),
        seq(0), triggerPulse(false), buttonMask(0) {}
  };

  bool isAgentOverlayEnabled() const;
  void setAgentOverlayState(const std::string& st,
                            const std::string& action,
                            const std::string& intent,
                            bool hasPoint, int x, int y,
                            bool hasEndPoint, int endX, int endY,
                            bool triggerPulse,
                            uint16_t buttonMask = 0);
  void clearAgentOverlayState();
  const AgentOverlayState& getAgentOverlayState() const { return overlayState; }

  void notifyHumanInput();
  uint64_t getHumanActionCount() const { return humanActionCount; }
  int64_t getLastHumanInputMs() const { return lastHumanInputMs; }

  void pushUserMessage(const std::string& msg);
  std::vector<std::string> drainUserMessages();
  size_t getPendingUserMessageCount() const { return userMessages.size(); }

  void refreshAgentOverlayDisplay();

  uint64_t getFrameSeq() const { return frameSeq; }
  uint64_t getDamageCount() const { return damageCount; }
  uint64_t getResizeCount() const { return resizeCount; }
  int64_t getLastUpdateMs() const { return lastUpdateMs; }
  bool isUpdating() const { return inFramebufferUpdate; }

  bool captureFramebuffer(bool rgba32,
                          int* outWidth, int* outHeight,
                          int* outStrideBytes,
                          int* outFbBpp, int* outFbDepth,
                          std::vector<uint8_t>* outPixels);

  void injectPointerEvent(int x, int y, uint16_t buttonMask);
  void injectKeyEvent(uint32_t keysym, uint32_t keycode, bool down);
  size_t injectText(const std::string& utf8Text);
  void injectClipboard(const std::string& utf8Text);
  bool getRemoteClipboard(std::string* outText) const;
  void releaseAgentKeys();
  void clearAgentClipboardOwnership();

protected:

  // Callback when socket is ready (or broken)
  static void socketEvent(FL_SOCKET fd, void *data);

  void processNextMsg(core::Timer*);

  // CConnection callback methods

  void getUserPasswd(std::string *user, std::string *password) override;
  bool verifyCertificate(unsigned int status,
                         const uint8_t* certificate,
                         size_t length) override;
  bool verifyHostKey(const uint8_t* key, size_t length,
                     const char* fingerprint) override;

  void initDone() override;

  void setExtendedDesktopSize(unsigned reason, unsigned result,
                              int w, int h,
                              const rfb::ScreenSet& layout) override;

  void setName(const char* name) override;

  void bell() override;

  void framebufferUpdateStart() override;
  void framebufferUpdateEnd() override;
  bool dataRect(const core::Rect& r, int encoding) override;

  void setCursor(int width, int height, const core::Point& hotspot,
                 const uint8_t* data) override;
  void setCursorPos(const core::Point& pos) override;

  void setLEDState(unsigned int state) override;

  void handleClipboardRequest() override;
  void handleClipboardAnnounce(bool available) override;
  void handleClipboardData(const char* data) override;

  void handleAudioBegin() override;
  void handleAudioEnd() override;
  void handleAudioData(const uint8_t* data, size_t length) override;

private:

  void resizeFramebuffer() override;

  void updateEncoding();
  void updateCompressLevel();
  void updateQualityLevel();
  void updatePixelFormat();

  static void handleOptions(void *data);

  static void handleUpdateTimeout(void *data);

private:
  std::string serverHost;
  int serverPort;
  network::Socket* sock;
  core::MethodTimer<CConn> msgTimer;

  DesktopWindow *desktop;

  AudioOutput *audioOutput;

#ifndef WIN32
  FtpEventLoop *ftpLoop;
  FtpBackend *ftpBackend;
  FtpServer *ftpServer;
#endif

  unsigned updateCount;
  unsigned pixelCount;

  int lastServerEncoding;

  struct timeval updateStartTime;
  size_t updateStartPos;
  unsigned long long bpsEstimate;

  AgentBridge* agentBridge;
  bool sessionInitialized;
  bool socketWatched;
  bool socketWatchWrite;
  bool agentPaused;
  bool inFramebufferUpdate;
  uint64_t frameSeq;
  uint64_t damageCount;
  uint64_t resizeCount;
  int64_t lastUpdateMs;

  std::map<uint32_t, int> agentDownKeys;
  int nextAgentKeyCode;

  std::string agentClipboardText;
  bool hasAgentClipboard;
  std::string remoteClipboardText;
  bool hasRemoteClipboardData;

  std::string agentPauseReason;
  AgentOverlayState overlayState;
  uint64_t humanActionCount;
  int64_t lastHumanInputMs;
  uint16_t lastAgentButtonMask;
  std::deque<std::string> userMessages;

  static std::string savedUsername;
  static std::string savedPassword;
};

#endif
