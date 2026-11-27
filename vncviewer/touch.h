/* Copyright 2019-2020 Pierre Ossman <ossman@cendio.se> for Cendio AB
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

#ifndef __TOUCH_H__
#define __TOUCH_H__

class Fl_Window;

void enable_touch();
void disable_touch();

#if !defined(WIN32) && !defined(__APPLE__)
class XInputGIIObserver {
public:
  virtual ~XInputGIIObserver() {}

  virtual bool handleXIDeviceEvent(const void* xiDeviceEvent) = 0;
  virtual void handleXIHierarchyEvent(const void* xiHierarchyEvent) = 0;
};

void x11_register_gii_observer(Fl_Window* win, XInputGIIObserver* observer);
void x11_unregister_gii_observer(Fl_Window* win, XInputGIIObserver* observer);

bool x11_grab_pointer(Fl_Window* win);
void x11_ungrab_pointer(Fl_Window* win);
#endif

#endif
