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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <limits.h>
#include <math.h>
#include <stdio.h>

#include <algorithm>
#include <cctype>

#include "GIIDeviceManager.h"

static std::string toLowerAscii(const std::string& s)
{
  std::string out(s);
  for (size_t i = 0; i < out.size(); i++)
    out[i] = (char)std::tolower((unsigned char)out[i]);
  return out;
}

static bool containsSubstr(const std::string& haystack, const char* needle)
{
  return haystack.find(needle) != std::string::npos;
}

static bool containsWord(const std::string& name, const std::string& word)
{
  for (size_t pos = name.find(word); pos != std::string::npos;
       pos = name.find(word, pos + word.size())) {
    size_t end = pos + word.size();
    if ((pos == 0 || !std::isalnum((unsigned char)name[pos - 1])) &&
        (end == name.size() || !std::isalnum((unsigned char)name[end])))
      return true;
  }
  return false;
}

static int32_t clampToInt32(double v)
{
  if (v <= (double)INT32_MIN)
    return INT32_MIN;
  if (v >= (double)INT32_MAX)
    return INT32_MAX;
  return (int32_t)llround(v);
}

static bool isXLabel(const std::string& lowerLabel)
{
  return (lowerLabel == "x") || (lowerLabel == "abs x") ||
         (lowerLabel == "rel x") || (lowerLabel == "abs_x") ||
         (lowerLabel == "rel_x");
}

static bool isYLabel(const std::string& lowerLabel)
{
  return (lowerLabel == "y") || (lowerLabel == "abs y") ||
         (lowerLabel == "rel y") || (lowerLabel == "abs_y") ||
         (lowerLabel == "rel_y");
}

static bool isZLabel(const std::string& lowerLabel)
{
  return (lowerLabel == "z") || (lowerLabel == "abs z") ||
         (lowerLabel == "rel z") || (lowerLabel == "abs_z") ||
         (lowerLabel == "rel_z");
}

static bool isExtendedAxisLabel(const std::string& lowerLabel)
{
  return containsSubstr(lowerLabel, "pressure") ||
         containsSubstr(lowerLabel, "tilt") ||
         containsSubstr(lowerLabel, "distance") ||
         containsSubstr(lowerLabel, "rotation") ||
         containsSubstr(lowerLabel, "wheel");
}

static std::string deriveShortName(const std::string& lowerLabel, size_t idx)
{
  if (isXLabel(lowerLabel))
    return "X";
  if (isYLabel(lowerLabel))
    return "Y";
  if (isZLabel(lowerLabel))
    return "Z";
  if (containsSubstr(lowerLabel, "pressure"))
    return "Prs";
  if (containsSubstr(lowerLabel, "tilt x") ||
      containsSubstr(lowerLabel, "tilt_x"))
    return "TltX";
  if (containsSubstr(lowerLabel, "tilt y") ||
      containsSubstr(lowerLabel, "tilt_y"))
    return "TltY";
  if (containsSubstr(lowerLabel, "distance"))
    return "Dst";
  if (containsSubstr(lowerLabel, "wheel"))
    return "Whl";
  if (containsSubstr(lowerLabel, "rot x") ||
      containsSubstr(lowerLabel, "rot_x"))
    return "RX";
  if (containsSubstr(lowerLabel, "rot y") ||
      containsSubstr(lowerLabel, "rot_y"))
    return "RY";
  if (containsSubstr(lowerLabel, "rot z") ||
      containsSubstr(lowerLabel, "rot_z"))
    return "RZ";
  if (containsSubstr(lowerLabel, "rotation"))
    return "Rot";

  char buf[16];
  snprintf(buf, sizeof(buf), "V%u", (unsigned)idx);
  std::string out(buf);
  if (out.size() >= rfb::giiValuatorShortNameLen)
    out.resize(rfb::giiValuatorShortNameLen - 1);
  return out;
}

bool classifyAndBuildGIIDevice(const RawInputDeviceInfo& raw,
                               rfb::GIIDevice* outDesc,
                               std::vector<int>* outAxisNumbers,
                               std::vector<bool>* outAxisAbsolute,
                               int* outXValIdx,
                               int* outYValIdx)
{
  if (!raw.enabled)
    return false;

  if (!raw.isSlavePointer && !raw.isFloatingSlave)
    return false;

  std::string lowerName = toLowerAscii(raw.name);

  if (containsSubstr(lowerName, "xtest"))
    return false;
  if (lowerName.compare(0, 16, "xwayland-pointer") == 0)
    return false;

  std::vector<RawInputAxisInfo> nonScrollAxes;
  bool hasExtendedAxis = false;

  for (const RawInputAxisInfo& ax : raw.axes) {
    if (ax.isScroll)
      continue;
    nonScrollAxes.push_back(ax);
    if (isExtendedAxisLabel(toLowerAscii(ax.label)))
      hasExtendedAxis = true;
  }

  // Reject touchpads (indirect touch, or named touchpad/trackpad with no
  // extended tablet/3D axes).
  if (raw.hasTouch && !raw.isDirectTouch)
    return false;

  bool isTouchpadName = containsSubstr(lowerName, "touchpad") ||
                        containsSubstr(lowerName, "trackpad");
  if (isTouchpadName && (nonScrollAxes.size() <= 2) && !hasExtendedAxis)
    return false;

  // Reject plain direct touchscreens (already handled by touch gesture code).
  if (raw.hasTouch && raw.isDirectTouch &&
      (nonScrollAxes.size() <= 2) && !hasExtendedAxis)
    return false;

  // Reject 2-axis virtual VM pointers (e.g. QEMU USB Tablet, VMware pointer).
  bool isVmPointer = containsSubstr(lowerName, "qemu") ||
                     containsSubstr(lowerName, "vmware") ||
                     containsSubstr(lowerName, "vbox") ||
                     containsSubstr(lowerName, "virtualbox") ||
                     containsSubstr(lowerName, "virtual");
  if (isVmPointer && (nonScrollAxes.size() <= 2) && !hasExtendedAxis)
    return false;

  bool hasNamedExtended =
    !isTouchpadName &&
    (containsSubstr(lowerName, "tablet") ||
     containsWord(lowerName, "pen") ||
     containsSubstr(lowerName, "stylus") ||
     containsSubstr(lowerName, "eraser") ||
     containsWord(lowerName, "pad") ||
     containsSubstr(lowerName, "wacom") ||
     containsSubstr(lowerName, "huion") ||
     containsSubstr(lowerName, "xp-pen") ||
     containsSubstr(lowerName, "digitizer") ||
     containsSubstr(lowerName, "spacemouse") ||
     containsSubstr(lowerName, "joystick") ||
     containsSubstr(lowerName, "gamepad"));

  bool accepted =
    (nonScrollAxes.size() > 2) ||
    hasExtendedAxis ||
    (raw.isFloatingSlave && (!nonScrollAxes.empty() || raw.numButtons > 0)) ||
    (hasNamedExtended && (!nonScrollAxes.empty() || raw.numButtons > 0));

  if (!accepted)
    return false;

  std::sort(nonScrollAxes.begin(), nonScrollAxes.end(),
            [](const RawInputAxisInfo& a, const RawInputAxisInfo& b) {
              return a.number < b.number;
            });

  if (nonScrollAxes.size() > rfb::giiMaxValuatorsPerDevice)
    nonScrollAxes.resize(rfb::giiMaxValuatorsPerDevice);

  outDesc->name = raw.name;
  outDesc->vendorID = raw.vendorID;
  outDesc->productID = raw.productID;
  outDesc->numRegisters = 0;
  outDesc->numButtons = raw.numButtons;
  outDesc->valuators.clear();

  outAxisNumbers->clear();
  outAxisAbsolute->clear();
  *outXValIdx = -1;
  *outYValIdx = -1;

  bool hasAbsVal = false;
  bool hasRelVal = false;

  for (size_t i = 0; i < nonScrollAxes.size(); i++) {
    const RawInputAxisInfo& ax = nonScrollAxes[i];
    std::string lowerLabel = toLowerAscii(ax.label);

    rfb::GIIValuator val;
    val.index = (uint32_t)i;
    val.longName = ax.label;
    val.shortName = deriveShortName(lowerLabel, i);

    bool isX = isXLabel(lowerLabel);
    bool isY = isYLabel(lowerLabel);
    if (ax.isAbsolute) {
      hasAbsVal = true;
      if (isX && (*outXValIdx == -1))
        *outXValIdx = (int)i;
      if (isY && (*outYValIdx == -1))
        *outYValIdx = (int)i;
    } else {
      hasRelVal = true;
    }

    double minD = ax.minVal;
    double maxD = ax.maxVal;
    if (maxD <= minD) {
      if (isX || isY) {
        minD = 0.0;
        maxD = 65535.0;
      } else {
        minD = 0.0;
        maxD = 0.0;
      }
    }

    val.rangeMin = clampToInt32(minD);
    val.rangeMax = clampToInt32(maxD);
    if (val.rangeMax < val.rangeMin)
      val.rangeMax = val.rangeMin;
    val.rangeCenter =
      (int32_t)(((int64_t)val.rangeMin + (int64_t)val.rangeMax) / 2);

    val.siUnit = rfb::giiUnitUnknown;
    val.siAdd = 0;
    val.siMul = 0;
    val.siDiv = 1;
    val.siShift = 0;

    outDesc->valuators.push_back(val);
    outAxisNumbers->push_back(ax.number);
    outAxisAbsolute->push_back(ax.isAbsolute);
  }

  uint32_t canGen = 0;
  if (outDesc->numButtons > 0)
    canGen |= rfb::giiEventMaskPtrButtonPress |
              rfb::giiEventMaskPtrButtonRelease;
  if (hasAbsVal)
    canGen |= rfb::giiEventMaskValuatorAbsolute;
  if (hasRelVal)
    canGen |= rfb::giiEventMaskValuatorRelative;
  outDesc->canGenerate = canGen;

  return true;
}

int32_t scaleCoordinate(double coord, int fbExtent,
                        int32_t rangeMin, int32_t rangeMax)
{
  if (rangeMax <= rangeMin)
    return rangeMin;

  if (fbExtent <= 1)
    return rangeMin;

  double maxCoord = (double)(fbExtent - 1);
  if (coord < 0.0)
    coord = 0.0;
  else if (coord > maxCoord)
    coord = maxCoord;

  double frac = coord / maxCoord;
  double scaled = (double)rangeMin +
                  frac * ((double)rangeMax - (double)rangeMin);
  long long rounded = llround(scaled);
  if (rounded < (long long)rangeMin)
    return rangeMin;
  if (rounded > (long long)rangeMax)
    return rangeMax;
  return (int32_t)rounded;
}

GIIDeviceManager::GIIDeviceManager(GIIEventSender* sender_)
  : sender(sender_), nextGeneration(1)
{
}

GIIDeviceManager::~GIIDeviceManager()
{
}

void GIIDeviceManager::addDevice(int localId, bool isFloatingSlave,
                                 const rfb::GIIDevice& desc,
                                 const std::vector<int>& axisNumbers,
                                 const std::vector<bool>& axisAbsolute,
                                 int xValIdx, int yValIdx)
{
  if (devices.count(localId) != 0)
    removeDevice(localId);

  uint64_t gen = nextGeneration++;

  DeviceEntry entry;
  entry.desc = desc;

  // Some servers reuse an existing origin when wire names coincide.
  std::set<std::string> names;
  for (const auto& device : devices) {
    if (device.second.active)
      names.insert(device.second.desc.name);
  }
  for (const PendingCreation& pending : pendingCreations)
    names.insert(pending.name);

  const size_t maxNameLength = rfb::giiDeviceNameLen - 1;
  entry.desc.name.resize(std::min(desc.name.size(), maxNameLength));
  for (unsigned id = 2; names.count(entry.desc.name); id++) {
    std::string suffix = " " + std::to_string(id);
    entry.desc.name = desc.name.substr(0, maxNameLength - suffix.size()) + suffix;
  }

  entry.axisNumbers = axisNumbers;
  entry.axisAbsolute = axisAbsolute;
  entry.xValIdx = xValIdx;
  entry.yValIdx = yValIdx;
  entry.isFloating = isFloatingSlave;
  entry.remoteOrigin = 0;
  entry.active = false;
  entry.generation = gen;

  devices[localId] = entry;

  PendingCreation pending;
  pending.localId = localId;
  pending.generation = gen;
  pending.name = entry.desc.name;
  pendingCreations.push_back(pending);

  sender->createGIIDevice(entry.desc);
}

void GIIDeviceManager::removeDevice(int localId)
{
  std::map<int, DeviceEntry>::iterator it = devices.find(localId);
  if (it == devices.end())
    return;

  if (it->second.active) {
    releaseDeviceButtons(it->second);
    sender->destroyGIIDevice(it->second.remoteOrigin);
  }

  devices.erase(it);
}

void GIIDeviceManager::handleDeviceCreated(uint32_t remoteOrigin)
{
  if (pendingCreations.empty())
    return;

  PendingCreation pending = pendingCreations.front();
  pendingCreations.pop_front();

  if (remoteOrigin == 0)
    return;

  for (const auto& device : devices) {
    if (device.second.active && (device.second.remoteOrigin == remoteOrigin))
      return;
  }

  std::map<int, DeviceEntry>::iterator it = devices.find(pending.localId);
  if ((it == devices.end()) || (it->second.generation != pending.generation)) {
    sender->destroyGIIDevice(remoteOrigin);
    return;
  }

  it->second.remoteOrigin = remoteOrigin;
  it->second.active = true;
}

bool GIIDeviceManager::hasDevice(int localId) const
{
  return devices.count(localId) != 0;
}

bool GIIDeviceManager::isActiveDevice(int localId) const
{
  std::map<int, DeviceEntry>::const_iterator it = devices.find(localId);
  if (it == devices.end())
    return false;
  return it->second.active;
}

bool GIIDeviceManager::isFloatingDevice(int localId) const
{
  std::map<int, DeviceEntry>::const_iterator it = devices.find(localId);
  if (it == devices.end())
    return false;
  return it->second.isFloating;
}

bool GIIDeviceManager::hasPressedButtons(int localId) const
{
  std::map<int, DeviceEntry>::const_iterator it = devices.find(localId);
  if (it == devices.end())
    return false;
  return !it->second.pressedButtons.empty();
}

std::vector<int> GIIDeviceManager::knownLocalIds() const
{
  std::vector<int> ids;
  ids.reserve(devices.size());
  for (std::map<int, DeviceEntry>::const_iterator it = devices.begin();
       it != devices.end(); ++it)
    ids.push_back(it->first);
  return ids;
}

bool GIIDeviceManager::handleInputEvent(
  int localId, EventKind kind, uint32_t button,
  double fbX, double fbY, int fbWidth, int fbHeight,
  const std::vector<std::pair<int, double>>& axisValues)
{
  std::map<int, DeviceEntry>::iterator it = devices.find(localId);
  if ((it == devices.end()) || !it->second.active)
    return false;

  DeviceEntry& entry = it->second;
  if (kind != EventMotion) {
    if ((button == 0) || (button > entry.desc.numButtons))
      return false;
    if ((kind == EventButtonRelease) &&
        (entry.pressedButtons.count(button) == 0))
      return false;
  }

  size_t nVals = entry.desc.valuators.size();

  std::vector<bool> hasVal(nVals, false);
  std::vector<int32_t> valData(nVals, 0);

  for (const std::pair<int, double>& av : axisValues) {
    for (size_t i = 0; i < nVals; i++) {
      if (entry.axisNumbers[i] != av.first)
        continue;

      int32_t v = clampToInt32(av.second);
      if (entry.axisAbsolute[i]) {
        const rfb::GIIValuator& valDesc = entry.desc.valuators[i];
        if (valDesc.rangeMax > valDesc.rangeMin) {
          if (v < valDesc.rangeMin)
            v = valDesc.rangeMin;
          else if (v > valDesc.rangeMax)
            v = valDesc.rangeMax;
        }
      }

      hasVal[i] = true;
      valData[i] = v;
      break;
    }
  }

  if (entry.xValIdx >= 0 && (size_t)entry.xValIdx < nVals) {
    size_t xi = (size_t)entry.xValIdx;
    if (hasVal[xi] || (kind != EventMotion)) {
      hasVal[xi] = true;
      valData[xi] = scaleCoordinate(fbX, fbWidth,
                                    entry.desc.valuators[xi].rangeMin,
                                    entry.desc.valuators[xi].rangeMax);
    }
  }

  if (entry.yValIdx >= 0 && (size_t)entry.yValIdx < nVals) {
    size_t yi = (size_t)entry.yValIdx;
    if (hasVal[yi] || (kind != EventMotion)) {
      hasVal[yi] = true;
      valData[yi] = scaleCoordinate(fbY, fbHeight,
                                    entry.desc.valuators[yi].rangeMin,
                                    entry.desc.valuators[yi].rangeMax);
    }
  }

  std::vector<rfb::GIIEvent> events;

  size_t i = 0;
  while (i < nVals) {
    if (!hasVal[i]) {
      i++;
      continue;
    }

    bool absMode = entry.axisAbsolute[i];
    size_t runStart = i;
    std::vector<int32_t> runValues;

    while ((i < nVals) && hasVal[i] && (entry.axisAbsolute[i] == absMode) &&
           (runValues.size() < rfb::giiMaxValuatorsPerEvent)) {
      runValues.push_back(valData[i]);
      i++;
    }

    rfb::GIIEvent ev;
    ev.type = absMode ? rfb::giiEventValuatorAbsolute
                      : rfb::giiEventValuatorRelative;
    ev.origin = entry.remoteOrigin;
    ev.first = (uint32_t)runStart;
    ev.values = runValues;
    events.push_back(ev);
  }

  if ((kind == EventButtonPress) || (kind == EventButtonRelease)) {
    if (kind == EventButtonPress)
      entry.pressedButtons.insert(button);
    else
      entry.pressedButtons.erase(button);

    rfb::GIIEvent ev;
    ev.type = (kind == EventButtonPress) ? rfb::giiEventPtrButtonPress
                                         : rfb::giiEventPtrButtonRelease;
    ev.origin = entry.remoteOrigin;
    ev.button = button;
    events.push_back(ev);
  }

  if (events.empty())
    return false;

  sender->sendGIIEvents(events);
  return true;
}

void GIIDeviceManager::releaseDeviceButtons(DeviceEntry& entry)
{
  if (!entry.active || entry.pressedButtons.empty())
    return;

  std::vector<rfb::GIIEvent> events;
  events.reserve(entry.pressedButtons.size());
  for (uint32_t btn : entry.pressedButtons) {
    rfb::GIIEvent ev;
    ev.type = rfb::giiEventPtrButtonRelease;
    ev.origin = entry.remoteOrigin;
    ev.button = btn;
    events.push_back(ev);
  }
  entry.pressedButtons.clear();
  sender->sendGIIEvents(events);
}

void GIIDeviceManager::releaseAllButtons()
{
  for (std::map<int, DeviceEntry>::iterator it = devices.begin();
       it != devices.end(); ++it)
    releaseDeviceButtons(it->second);
}

void GIIDeviceManager::destroyAllDevices()
{
  releaseAllButtons();

  for (std::map<int, DeviceEntry>::iterator it = devices.begin();
       it != devices.end(); ++it) {
    if (it->second.active)
      sender->destroyGIIDevice(it->second.remoteOrigin);
  }

  devices.clear();
  // Replies for outstanding requests must still be drained in order.
}
