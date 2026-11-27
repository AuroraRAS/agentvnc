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

#include <X11/Xatom.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XI2.h>

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/x.H>

#include <core/LogWriter.h>

#include "GIIInputX11.h"
#include "Viewport.h"
#include "parameters.h"

static core::LogWriter vlog("GIIInputX11");

static void queryDeviceProductID(int deviceid,
                                 uint32_t* vendorID, uint32_t* productID)
{
  *vendorID = 0;
  *productID = 0;

  Atom prop = XInternAtom(fl_display, "Device Product ID", True);
  if (prop == None)
    return;

  Atom typeReturn = None;
  int formatReturn = 0;
  unsigned long numItems = 0;
  unsigned long bytesAfter = 0;
  unsigned char* data = nullptr;

  if (XIGetProperty(fl_display, deviceid, prop, 0, 2, False, XA_INTEGER,
                    &typeReturn, &formatReturn, &numItems, &bytesAfter,
                    &data) == Success) {
    if ((typeReturn == XA_INTEGER) && (formatReturn == 32) &&
        (numItems >= 2) && (data != nullptr)) {
      // libXi returns packed property data, including on 64-bit systems.
      const uint32_t* vals = reinterpret_cast<const uint32_t*>(data);
      *vendorID = vals[0];
      *productID = vals[1];
    }
    if (data != nullptr)
      XFree(data);
  }
}

GIIInputX11::GIIInputX11(Fl_Window* win_, Viewport* viewport_,
                         GIIEventSender* sender_)
  : win(win_), viewport(viewport_), manager(sender_), enabled(false)
{
}

GIIInputX11::~GIIInputX11()
{
  setEnabled(false);
}

void GIIInputX11::setEnabled(bool newEnabled)
{
  if (enabled == newEnabled)
    return;

  enabled = newEnabled;
  if (enabled) {
    x11_register_gii_observer(win, this);
    syncDevices();
  } else {
    x11_unregister_gii_observer(win, this);
    manager.destroyAllDevices();
  }
}

void GIIInputX11::handleDeviceCreated(uint32_t deviceOrigin)
{
  manager.handleDeviceCreated(deviceOrigin);
}

void GIIInputX11::releaseAllButtons()
{
  manager.releaseAllButtons();
}

void GIIInputX11::syncDevices()
{
  int ndevices = 0;
  XIDeviceInfo* devices = XIQueryDevice(fl_display, XIAllDevices, &ndevices);
  if (devices == nullptr)
    return;

  std::set<int> currentAccepted;

  for (int i = 0; i < ndevices; i++) {
    const XIDeviceInfo& dev = devices[i];
    if (!dev.enabled)
      continue;
    if ((dev.use != XISlavePointer) && (dev.use != XIFloatingSlave))
      continue;

    RawInputDeviceInfo raw;
    raw.localId = dev.deviceid;
    raw.name = dev.name ? dev.name : "";
    raw.enabled = dev.enabled != 0;
    raw.isSlavePointer = (dev.use == XISlavePointer);
    raw.isFloatingSlave = (dev.use == XIFloatingSlave);

    std::set<int> scrollAxes;
    for (int c = 0; c < dev.num_classes; c++) {
      const XIAnyClassInfo* cls = dev.classes[c];
      if (cls->type == XIScrollClass) {
        const XIScrollClassInfo* sc = (const XIScrollClassInfo*)cls;
        scrollAxes.insert(sc->number);
      }
    }

    for (int c = 0; c < dev.num_classes; c++) {
      const XIAnyClassInfo* cls = dev.classes[c];
      if (cls->type == XIButtonClass) {
        const XIButtonClassInfo* bc = (const XIButtonClassInfo*)cls;
        if (bc->num_buttons > 0)
          raw.numButtons = (uint32_t)bc->num_buttons;
        // X11 button 2 (middle) is GII button 3.
        if (raw.numButtons == 2)
          raw.numButtons = 3;
      } else if (cls->type == XITouchClass) {
        const XITouchClassInfo* tc = (const XITouchClassInfo*)cls;
        raw.hasTouch = true;
        raw.isDirectTouch = (tc->mode == XIDirectTouch);
      } else if (cls->type == XIValuatorClass) {
        const XIValuatorClassInfo* vc = (const XIValuatorClassInfo*)cls;
        RawInputAxisInfo ax;
        ax.number = vc->number;
        ax.minVal = vc->min;
        ax.maxVal = vc->max;
        ax.resolution = vc->resolution;
        ax.isAbsolute = (vc->mode == XIModeAbsolute);
        ax.isScroll = (scrollAxes.count(vc->number) != 0);
        if (vc->label != None) {
          char* labelName = XGetAtomName(fl_display, vc->label);
          if (labelName != nullptr) {
            ax.label = labelName;
            XFree(labelName);
          }
        }
        raw.axes.push_back(ax);
      }
    }

    rfb::GIIDevice giiDesc;
    std::vector<int> axisNumbers;
    std::vector<bool> axisAbsolute;
    int xValIdx = -1;
    int yValIdx = -1;

    if (!classifyAndBuildGIIDevice(raw, &giiDesc, &axisNumbers,
                                   &axisAbsolute, &xValIdx, &yValIdx))
      continue;

    currentAccepted.insert(dev.deviceid);

    if (!manager.hasDevice(dev.deviceid) ||
        (manager.isFloatingDevice(dev.deviceid) != raw.isFloatingSlave)) {
      queryDeviceProductID(dev.deviceid, &giiDesc.vendorID, &giiDesc.productID);
      vlog.debug("Registering GII device %d: %s (%u valuators, %u buttons)",
                 dev.deviceid, giiDesc.name.c_str(),
                 (unsigned)giiDesc.valuators.size(),
                 (unsigned)giiDesc.numButtons);
      manager.addDevice(dev.deviceid, raw.isFloatingSlave, giiDesc,
                        axisNumbers, axisAbsolute, xValIdx, yValIdx);
    }
  }

  XIFreeDeviceInfo(devices);

  std::vector<int> known = manager.knownLocalIds();
  for (int id : known) {
    if (currentAccepted.count(id) == 0) {
      vlog.debug("Removing GII device %d", id);
      manager.removeDevice(id);
    }
  }
}

void GIIInputX11::handleXIHierarchyEvent(const void* xiHierarchyEvent)
{
  const XIHierarchyEvent* event = (const XIHierarchyEvent*)xiHierarchyEvent;
  for (int i = 0; i < event->num_info; i++) {
    const XIHierarchyInfo& device = event->info[i];
    if (device.flags & (XISlaveRemoved | XIDeviceDisabled))
      manager.removeDevice(device.deviceid);
  }

  syncDevices();
}

bool GIIInputX11::handleXIDeviceEvent(const void* xiDeviceEvent)
{
  const XIDeviceEvent* devev = (const XIDeviceEvent*)xiDeviceEvent;

  GIIDeviceManager::EventKind kind;
  uint32_t button = 0;

  switch (devev->evtype) {
  case XI_Motion:
    kind = GIIDeviceManager::EventMotion;
    break;
  case XI_ButtonPress:
    kind = GIIDeviceManager::EventButtonPress;
    button = (uint32_t)devev->detail;
    break;
  case XI_ButtonRelease:
    kind = GIIDeviceManager::EventButtonRelease;
    button = (uint32_t)devev->detail;
    break;
  default:
    return false;
  }

  if (devev->flags & XIPointerEmulated)
    return false;

  // GII orders the secondary and tertiary buttons opposite to X11.
  if (button == 2)
    button = 3;
  else if (button == 3)
    button = 2;

  if (!manager.isActiveDevice(devev->sourceid))
    return false;

  bool isMaster = (devev->deviceid != devev->sourceid);
  bool isFloating = manager.isFloatingDevice(devev->sourceid);

  if (!isFloating && !isMaster)
    return true;
  if (isFloating && isMaster)
    return false;

  if (viewOnly || (Fl::grab() != nullptr))
    return false;

  double scale = 1.0;
#if FL_API_VERSION >= 10400
  scale = Fl::screen_scale(win->screen_num());
#endif
  double fbX = devev->event_x / scale - viewport->x();
  double fbY = devev->event_y / scale - viewport->y();

  bool inViewport = (fbX >= 0) && (fbX < viewport->w()) &&
                    (fbY >= 0) && (fbY < viewport->h());

  if (!inViewport && !manager.hasPressedButtons(devev->sourceid) && !isFloating)
    return false;

  std::vector<std::pair<int, double>> axisValues;
  int valIdx = 0;
  int maxAxes = devev->valuators.mask_len * 8;
  for (int axis = 0; axis < maxAxes; axis++) {
    if (XIMaskIsSet(devev->valuators.mask, axis)) {
      axisValues.push_back(std::make_pair(axis, devev->valuators.values[valIdx]));
      valIdx++;
    }
  }

  return manager.handleInputEvent(devev->sourceid, kind, button,
                                  fbX, fbY, viewport->w(), viewport->h(),
                                  axisValues);
}
