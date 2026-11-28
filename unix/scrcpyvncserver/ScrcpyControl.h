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

#ifndef __SCRCPYVNCSERVER_SCRCPYCONTROL_H__
#define __SCRCPYVNCSERVER_SCRCPYCONTROL_H__

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace rfb {
  struct GIIDevice;
  struct GIIEvent;
}

class ScrcpyControl {
public:
  class Callbacks {
  public:
    virtual ~Callbacks() {}
    virtual void onDeviceClipboard(const std::string& text) = 0;
  };

  ScrcpyControl();
  ~ScrcpyControl();

  void attach(int fd);
  void setServerVersion(const std::string& version);
  void close();
  int fd() const { return fd_; }

  bool hasPendingWrites() const { return sendOffset_ < sendBuf_.size(); }
  void processWritable();
  bool processReadable(Callbacks& cb); // returns false on EOF

  // Scrcpy control_msg serializers
  void sendTouchEvent(uint8_t action, uint64_t pointerId,
                      int32_t x, int32_t y, uint16_t screenW, uint16_t screenH,
                      float pressure, uint32_t actionButton, uint32_t buttons);
  void sendScrollEvent(int32_t x, int32_t y, uint16_t screenW, uint16_t screenH,
                       float hscroll, float vscroll, uint32_t buttons);
  void sendKeycode(uint8_t action, uint32_t androidKeycode,
                   uint32_t repeat, uint32_t metastate);
  void sendText(const std::string& utf8);
  void sendSetClipboard(uint64_t sequence, const std::string& text, bool paste);
  void sendResetVideo();

  // VNC input translation helpers
  void handleVncPointer(int x, int y, uint16_t buttonMask,
                        int screenW, int screenH);
  void releasePointer(int screenW, int screenH);
  void handleVncKey(uint32_t keysym, uint32_t keycode, bool down);

  // RFB GII (General Input Interface) device lifecycle & event translation
  uint32_t createGIIDevice(const rfb::GIIDevice& device);
  void destroyGIIDevice(uint32_t origin, int screenW, int screenH);
  void handleGIIEvent(const rfb::GIIEvent& event, int screenW, int screenH);
  size_t activeGIIDeviceCount() const { return giiDevices_.size(); }

private:
  enum class GIIValuatorRole {
    Unknown,
    X,
    Y,
    Pressure,
    WheelV,
    WheelH,
    TiltX,
    TiltY,
    Z,
  };

  struct GIIValuatorState {
    GIIValuatorState()
      : role(GIIValuatorRole::Unknown), rangeMin(0), rangeMax(0),
        hasPrev(false), prevValue(0)
    {}

    GIIValuatorRole role;
    int32_t rangeMin;
    int32_t rangeMax;
    bool hasPrev;
    int32_t prevValue;
  };

  struct GIIDeviceState {
    GIIDeviceState()
      : pointerId(0), canGenerate(0), numButtons(0),
        xValIdx(-1), yValIdx(-1), pressureValIdx(-1),
        posX(0.0), posY(0.0), hasPosition(false),
        pressure(0.0f), hasPressureSample(false),
        touchDown(false), mouseButtons(0),
        lastSentX(0), lastSentY(0), lastSentPressure(0.0f),
        hasHoverSent(false),
        hasPtrWheel(false), lastPtrWheel(0)
    {}

    uint64_t pointerId;
    uint32_t canGenerate;
    uint32_t numButtons;
    std::vector<GIIValuatorState> valuators;
    int xValIdx;
    int yValIdx;
    int pressureValIdx;
    double posX;
    double posY;
    bool hasPosition;
    float pressure;
    bool hasPressureSample;
    bool touchDown;
    uint32_t mouseButtons;
    int lastSentX;
    int lastSentY;
    float lastSentPressure;
    bool hasHoverSent;
    bool hasPtrWheel;
    int32_t lastPtrWheel;
    std::map<std::pair<bool, uint32_t>, uint32_t> pressedKeys;
  };

  void enqueue(const uint8_t* data, size_t len);
  bool processDeviceMessages(Callbacks& cb);
  uint32_t vncButtonsToAndroid(uint16_t mask) const;
  uint32_t giiMouseButtons() const;
  uint32_t combinedMouseButtons() const;
  bool anyGiiTouchDown() const;
  void injectKey(uint32_t keysym, bool down, uint32_t repeat,
                 uint32_t extraMeta);
  void emitGIIMotion(GIIDeviceState& dev, int x, int y,
                     uint16_t w, uint16_t h,
                     bool posChanged, bool pressureChanged);
  void handleGIIKeyEvent(GIIDeviceState& dev, const rfb::GIIEvent& event);
  void handleGIIButtonEvent(GIIDeviceState& dev, const rfb::GIIEvent& event,
                            int screenW, int screenH);
  void handleGIIValuatorEvent(GIIDeviceState& dev, const rfb::GIIEvent& event,
                              int screenW, int screenH);
  void handleGIIPtrMoveEvent(GIIDeviceState& dev, const rfb::GIIEvent& event,
                             int screenW, int screenH);

  ScrcpyControl(const ScrcpyControl&) = delete;
  ScrcpyControl& operator=(const ScrcpyControl&) = delete;

  int fd_;
  std::vector<uint8_t> sendBuf_;
  size_t sendOffset_;
  std::vector<uint8_t> recvBuf_;
  size_t recvOffset_;

  uint32_t androidButtons_;
  uint16_t lastVncMask_;
  int lastX_;
  int lastY_;
  uint32_t metaState_;
  bool supportsVideoReset_;
  float scrollRange_;

  std::map<uint32_t, GIIDeviceState> giiDevices_;
  uint32_t nextGiiOrigin_;
};

#endif // __SCRCPYVNCSERVER_SCRCPYCONTROL_H__
