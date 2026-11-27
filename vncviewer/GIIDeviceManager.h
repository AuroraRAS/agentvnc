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

#ifndef __GIIDEVICEMANAGER_H__
#define __GIIDEVICEMANAGER_H__

#include <stdint.h>

#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <rfb/giiTypes.h>

struct RawInputAxisInfo {
  RawInputAxisInfo()
    : number(0), minVal(0.0), maxVal(0.0), resolution(0),
      isAbsolute(false), isScroll(false)
  {}

  int number;
  std::string label;
  double minVal;
  double maxVal;
  int resolution;
  bool isAbsolute;
  bool isScroll;
};

struct RawInputDeviceInfo {
  RawInputDeviceInfo()
    : localId(0), enabled(false), isSlavePointer(false),
      isFloatingSlave(false), hasTouch(false), isDirectTouch(false),
      vendorID(0), productID(0), numButtons(0)
  {}

  int localId;
  std::string name;
  bool enabled;
  bool isSlavePointer;
  bool isFloatingSlave;
  bool hasTouch;
  bool isDirectTouch;
  uint32_t vendorID;
  uint32_t productID;
  uint32_t numButtons;
  std::vector<RawInputAxisInfo> axes;
};

bool classifyAndBuildGIIDevice(const RawInputDeviceInfo& raw,
                               rfb::GIIDevice* outDesc,
                               std::vector<int>* outAxisNumbers,
                               std::vector<bool>* outAxisAbsolute,
                               int* outXValIdx,
                               int* outYValIdx);

int32_t scaleCoordinate(double coord, int fbExtent,
                        int32_t rangeMin, int32_t rangeMax);

class GIIEventSender {
public:
  virtual ~GIIEventSender() {}

  virtual void createGIIDevice(const rfb::GIIDevice& device) = 0;
  virtual void destroyGIIDevice(uint32_t deviceOrigin) = 0;
  virtual void sendGIIEvents(const std::vector<rfb::GIIEvent>& events) = 0;
};

class GIIDeviceManager {
public:
  enum EventKind {
    EventMotion,
    EventButtonPress,
    EventButtonRelease,
  };

  GIIDeviceManager(GIIEventSender* sender_);
  ~GIIDeviceManager();

  void addDevice(int localId, bool isFloatingSlave,
                 const rfb::GIIDevice& desc,
                 const std::vector<int>& axisNumbers,
                 const std::vector<bool>& axisAbsolute,
                 int xValIdx, int yValIdx);
  void removeDevice(int localId);
  void handleDeviceCreated(uint32_t remoteOrigin);

  bool hasDevice(int localId) const;
  bool isActiveDevice(int localId) const;
  bool isFloatingDevice(int localId) const;
  bool hasPressedButtons(int localId) const;
  std::vector<int> knownLocalIds() const;

  bool handleInputEvent(int localId, EventKind kind, uint32_t button,
                        double fbX, double fbY, int fbWidth, int fbHeight,
                        const std::vector<std::pair<int, double>>& axisValues);

  void releaseAllButtons();
  void destroyAllDevices();

private:
  struct DeviceEntry {
    DeviceEntry()
      : xValIdx(-1), yValIdx(-1), isFloating(false),
        remoteOrigin(0), active(false), generation(0)
    {}

    rfb::GIIDevice desc;
    std::vector<int> axisNumbers;
    std::vector<bool> axisAbsolute;
    int xValIdx;
    int yValIdx;
    bool isFloating;
    uint32_t remoteOrigin;
    bool active;
    uint64_t generation;
    std::set<uint32_t> pressedButtons;
  };

  struct PendingCreation {
    int localId;
    uint64_t generation;
  };

  void releaseDeviceButtons(DeviceEntry& entry);

  GIIEventSender* sender;
  std::map<int, DeviceEntry> devices;
  std::deque<PendingCreation> pendingCreations;
  uint64_t nextGeneration;
};

#endif
