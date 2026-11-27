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

#ifndef __GIIINPUTX11_H__
#define __GIIINPUTX11_H__

#include <stdint.h>

#include "GIIDeviceManager.h"
#include "GIIInput.h"
#include "touch.h"

class Fl_Window;
class Viewport;

class GIIInputX11 : public GIIInput, public XInputGIIObserver {
public:
  GIIInputX11(Fl_Window* win_, Viewport* viewport_, GIIEventSender* sender_);
  ~GIIInputX11();

  void handleDeviceCreated(uint32_t deviceOrigin) override;
  void releaseAllButtons() override;
  void shutdown() override;

  bool handleXIDeviceEvent(const void* xiDeviceEvent) override;
  void handleXIHierarchyEvent(const void* xiHierarchyEvent) override;

private:
  void syncDevices();

  Fl_Window* win;
  Viewport* viewport;
  GIIDeviceManager manager;
  bool shutDown;
};

#endif
