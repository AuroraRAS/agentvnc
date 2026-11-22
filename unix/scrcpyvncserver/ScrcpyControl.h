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

#include <string>
#include <vector>

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

private:
  void enqueue(const uint8_t* data, size_t len);
  uint32_t vncButtonsToAndroid(uint16_t mask) const;

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
};

#endif // __SCRCPYVNCSERVER_SCRCPYCONTROL_H__
