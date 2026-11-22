/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright 2011-2025 Pierre Ossman <ossman@cendio.se> for Cendio AB
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

#include <algorithm>
#include <cmath>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include <core/Configuration.h>
#include <core/LogWriter.h>
#include <core/i18n.h>
#include <core/string.h>
#include <core/time.h>

#include <rfb/CMsgWriter.h>

#include "DesktopWindow.h"
#include "MonitorIndicesParameter.h"
#include "OptionsDialog.h"
#include "parameters.h"
#include "vncviewer.h"
#include "CConn.h"
#include "Scale.h"
#include "Surface.h"
#include "Viewport.h"
#include "touch.h"
#include "fltk/event_dispatch_handler.h"

#include <FL/Fl.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/fl_draw.H>
#include <FL/platform.H>

#if defined(WIN32)
#include "win32.h"
#elif defined(__APPLE__)
#include "cocoa.h"
#else
#include "x11.h"
#endif

// width of each "edge" region where scrolling happens,
// as a ratio compared to the window size
// default: 1/16th of the window size
#define EDGE_SCROLL_SIZE 16
// edge width is calculated at runtime; these values are just examples
static int edge_scroll_size_x = 128;
static int edge_scroll_size_y = 96;
// maximum pixels to scroll per frame
#define EDGE_SCROLL_SPEED 16
// how long to wait between viewport scroll position changes
// default: roughly 60 fps for smooth motion
#define EDGE_SCROLL_SECONDS_PER_FRAME 0.016666

// Time before we show an overlay tip again
const time_t OVERLAY_REPEAT_TIMEOUT = 600;

static core::LogWriter vlog("DesktopWindow");

// Global due to http://www.fltk.org/str.php?L2177 and the similar
// issue for Fl::event_dispatch.
static std::set<DesktopWindow *> instances;

DesktopWindow::DesktopWindow(int w, int h, CConn* cc_)
  : Fl_Window(100, 100), cc(cc_), offscreen(nullptr),
    firstUpdate(true),
    delayedFullscreen(false), sentDesktopSize(false),
    pendingRemoteResize(false), lastResize({0, 0}),
    lastResizeWidth(0), lastResizeHeight(0),
    keyboardGrabbed(false), mouseGrabbed(false), regrabOnFocus(false),
    lastUIScale(1.0f), lastPhysW(0), lastPhysH(0),
    statsLastUpdates(0), statsLastPixels(0), statsLastPosition(0),
    statsGraph(nullptr)
{
  Fl_Group* group;

  // Support for -geometry option. Note that although we do support
  // negative coordinates, we do not support -XOFF-YOFF (ie
  // coordinates relative to the right edge / bottom edge) at this
  // time.
  int geom_x = 0, geom_y = 0;
  int geom_w = 0, geom_h = 0;
  bool hasGeomSize = false;
  if (strcmp(geometry, "") != 0) {
    int matched;
    matched = sscanf((const char*)geometry, "+%d+%d", &geom_x, &geom_y);
    if (matched == 2) {
      force_position(1);
    } else {
      matched = sscanf((const char*)geometry, "%dx%d+%d+%d", &geom_w, &geom_h, &geom_x, &geom_y);
      switch (matched) {
      case 4:
        force_position(1);
        /* fall through */
      case 2:
        hasGeomSize = true;
        break;
      default:
        geom_x = geom_y = 0;
        vlog.error(_("Invalid geometry specified!"));
      }
    }
  }

  int targetScreen;
  if (force_position()) {
    targetScreen = Fl::screen_num(geom_x, geom_y);
  } else {
    int mx, my;

    // If we don't explicitly request a position then we don't know which
    // monitor the window manager might place us on. Assume the popular
    // behaviour of following the cursor.

    Fl::get_mouse(mx, my);
    targetScreen = Fl::screen_num(mx, my);
  }
  screen_num(targetScreen);

  float initScale = Fl::screen_scale(targetScreen);
#ifdef __APPLE__
  initScale *= cocoa_screen_backing_scale(targetScreen);
#endif
  if (!std::isfinite(initScale) || (initScale <= 0.0f))
    initScale = 1.0f;

  int win_w = hasGeomSize ? geom_w : physicalToLogicalSize(w * (int)scaleFactor, initScale);
  int win_h = hasGeomSize ? geom_h : physicalToLogicalSize(h * (int)scaleFactor, initScale);

  // Dummy group to prevent FLTK from moving our widgets around
  group = new Fl_Group(0, 0, win_w, win_h);
  group->resizable(nullptr);
  resizable(group);

  viewport = new Viewport(w, h, cc);

  // Position will be adjusted later
  hscroll = new Fl_Scrollbar(0, 0, 0, 0);
  vscroll = new Fl_Scrollbar(0, 0, 0, 0);
  hscroll->type(FL_HORIZONTAL);
  hscroll->callback(handleScroll, this);
  vscroll->callback(handleScroll, this);

  group->end();

  callback(handleClose, this);

  updateCaption();

  OptionsDialog::addCallback(handleOptions, this);

  // Some events need to be caught globally
  if (instances.size() == 0)
    Fl::add_handler(fltkHandle);
  instances.insert(this);

  // Hack. See below...
  fl_add_event_dispatch(fltkDispatch, this);

  // Many window managers don't properly resize overly large windows,
  // so we'll have to do some sanity checks ourselves here
  int sx, sy, sw, sh;
  Fl::screen_work_area(sx, sy, sw, sh, targetScreen);
  if ((win_w > sw) || (win_h > sh)) {
    vlog.info(_("Reducing window size to fit on current monitor"));
    if (win_w > sw)
      win_w = sw;
    if (win_h > sh)
      win_h = sh;
  }

#ifdef __APPLE__
  // On OS X we can do the maximize thing properly before the
  // window is showned. Other platforms handled further down...
  if (::maximize) {
    int dummy;
    Fl::screen_work_area(dummy, dummy, win_w, win_h, targetScreen);
  }
#endif

  if (force_position()) {
    resize(geom_x, geom_y, win_w, win_h);
  } else {
    size(win_w, win_h);
  }

  viewport->syncLogicalGeometry(initScale);

  if (fullScreen) {
    // Hack: Window managers seem to be rather crappy at respecting
    // fullscreen hints on initial windows. So on X11 we'll have to
    // wait until after we've been mapped.
#if defined(WIN32) || defined(__APPLE__)
    fullscreen_on();
#else
    delayedFullscreen = true;
#endif
  }

  show();

#ifdef __APPLE__
  // FLTK does its own full screen, so disable the system one
  cocoa_prevent_native_fullscreen(this);
#endif

  // Full screen events are not sent out for a hidden window,
  // so send a fake one here to set up things properly.
  if (fullscreen_active())
    handle(FL_FULLSCREEN);

  // Unfortunately, current FLTK does not allow us to set the
  // maximized property on Windows and X11 before showing the window.
  // See STR #2083 and STR #2178
#ifndef __APPLE__
  if (::maximize) {
    maximizeWindow();
  }
#endif

  lastUIScale = uiScale();
  getPhysicalSize(&lastPhysW, &lastPhysH);

  // Adjust layout now that we're visible and know our final size
  repositionWidgets();

  // Throughput graph for debugging
  if (vlog.getLevel() >= core::LogWriter::LEVEL_DEBUG) {
    memset(&stats, 0, sizeof(stats));
    Fl::add_timeout(0, handleStatsTimeout, this);
  }

  // Show hint about menu shortcut
  unsigned modifierMask;

  modifierMask = 0;
  for (core::EnumListEntry key : shortcutModifiers)
    modifierMask |= ShortcutHandler::parseModifier(key.getValueStr().c_str());

  if (modifierMask)
    addOverlayTip(_("Press %sM to open the context menu"),
                  ShortcutHandler::modifierPrefix(modifierMask));

  // By default we get a slight delay when we warp the pointer, something
  // we don't want or we'll get jerky movement
#ifdef __APPLE__
  cocoa_event_delay(0);
#endif
}


DesktopWindow::~DesktopWindow()
{
  // Don't leave any dangling grabs as they are not automatically
  // cleaned up on all platforms
  ungrabPointer();
  ungrabKeyboard();

  // Unregister all timeouts in case they get a change tro trigger
  // again later when this object is already gone.
  Fl::remove_timeout(handleResizeTimeout, this);
  Fl::remove_timeout(handleFullscreenTimeout, this);
  Fl::remove_timeout(handleEdgeScroll, this);
  Fl::remove_timeout(handleStatsTimeout, this);
  Fl::remove_timeout(updateOverlay, this);
  Fl::remove_idle(checkFocus, this);

  OptionsDialog::removeCallback(handleOptions);

  while (!overlays.empty()) {
    delete overlays.front().surface;
    overlays.pop_front();
  }
  delete offscreen;

  delete statsGraph;

  instances.erase(this);

  if (instances.size() == 0)
    Fl::remove_handler(fltkHandle);

  fl_remove_event_dispatch(fltkDispatch, this);

  // FLTK automatically deletes all child widgets, so we shouldn't touch
  // them ourselves here
}


void DesktopWindow::updateCaption()
{
  const size_t maxLen = 100;
  std::string windowName;
  const char *labelFormat;
  size_t maxNameSize;
  std::string name;

  // FIXME: All of this consideres bytes, not characters

  if (keyboardGrabbed)
    labelFormat = _("%s - TigerVNC (keyboard grabbed)");
  else
    labelFormat = _("%s - TigerVNC");

  // Ignore the length of '%s' since it is
  // a format marker which won't take up space
  maxNameSize = maxLen - strlen(labelFormat) + 2;

  name = cc->server.name();

  if (name.size() > maxNameSize) {
    if (maxNameSize <= strlen("...")) {
      // Even an ellipsis won't fit
      name.clear();
    }
    else {
      int offset;

      // We need to truncate, add an ellipsis
      offset = maxNameSize - strlen("...");
      name.resize(offset);
      name += "...";
    }
  }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"

  windowName = core::format(labelFormat, name.c_str());

#pragma GCC diagnostic pop

  copy_label(windowName.c_str());
}


// Copy the areas of the framebuffer that have been changed (damaged)
// to the displayed window.

void DesktopWindow::updateWindow()
{
  if (firstUpdate) {
    firstUpdate = false;
    remoteResize();
  }

  viewport->updateWindow();
}


int DesktopWindow::currentScreenNum() const
{
  int count = Fl::screen_count();
  DesktopWindow* self = const_cast<DesktopWindow*>(this);
  int n = self->shown() ? self->screen_num()
                        : Fl::screen_num(x(), y(), w(), h());
  if ((count > 0) && ((n < 0) || (n >= count)))
    return 0;
  return n;
}


float DesktopWindow::uiScale() const
{
  int screen = currentScreenNum();
  float s = Fl::screen_scale(screen);
#ifdef __APPLE__
  DesktopWindow* self = const_cast<DesktopWindow*>(this);
  s *= self->shown() ? cocoa_backing_scale(self)
                     : cocoa_screen_backing_scale(screen);
#endif
  return (std::isfinite(s) && (s > 0.0f)) ? s : 1.0f;
}


void DesktopWindow::getPhysicalSize(int* physW, int* physH) const
{
  assert(physW != nullptr);
  assert(physH != nullptr);

  DesktopWindow* self = const_cast<DesktopWindow*>(this);
  if (self->shown()) {
#if defined(WIN32)
    if (win32_win_get_physical_size(self, physW, physH))
      return;
#elif defined(__APPLE__)
    cocoa_win_get_physical_size(self, physW, physH);
    return;
#else
    if (x11_win_get_physical_size(self, physW, physH))
      return;
#endif
  }

  float s = uiScale();
  *physW = logicalToPhysical(w(), s);
  *physH = logicalToPhysical(h(), s);
}


void DesktopWindow::getWindowPhysicalRect(int* wx, int* wy,
                                          int* ww, int* wh) const
{
  assert(wx != nullptr);
  assert(wy != nullptr);
  assert(ww != nullptr);
  assert(wh != nullptr);

  DesktopWindow* self = const_cast<DesktopWindow*>(this);
  if (self->shown()) {
#if defined(WIN32)
    if (win32_win_get_coords(self, wx, wy, ww, wh))
      return;
#elif defined(__APPLE__)
    float s = uiScale();
    *wx = logicalToPhysical(x(), s);
    *wy = logicalToPhysical(y(), s);
    cocoa_win_get_physical_size(self, ww, wh);
    return;
#else
    if (x11_win_get_coords(self, wx, wy, ww, wh))
      return;
#endif
  }

  float s = uiScale();
  *wx = logicalToPhysical(x(), s);
  *wy = logicalToPhysical(y(), s);
  *ww = logicalToPhysical(w(), s);
  *wh = logicalToPhysical(h(), s);
}


core::Point DesktopWindow::getEventPhysicalPos() const
{
  int px = 0, py = 0;

#if defined(WIN32)
  if (win32_event_physical_pos(const_cast<DesktopWindow*>(this), &px, &py))
    return core::Point(px, py);
#elif defined(__APPLE__)
  if (cocoa_event_physical_pos(const_cast<DesktopWindow*>(this), &px, &py))
    return core::Point(px, py);
#else
  if (x11_event_physical_pos(const_cast<DesktopWindow*>(this), &px, &py))
    return core::Point(px, py);
#endif

  float s = uiScale();
  return core::Point(logicalToPhysical(Fl::event_x(), s),
                     logicalToPhysical(Fl::event_y(), s));
}


core::Rect DesktopWindow::getScreenPhysicalRect(int idx) const
{
#ifdef __APPLE__
  // Express all monitor positions in the window's backing coordinates.
  return scaleScreenRectToPhysical(monitorRect(idx),
                                   cocoa_backing_scale(const_cast<DesktopWindow*>(this)));
#else
  return monitorRect(idx);
#endif
}


void DesktopWindow::resizeWindowToSession()
{
  float s = uiScale();
  size(physicalToLogicalSize(viewport->phys_w(), s),
       physicalToLogicalSize(viewport->phys_h(), s));
}


void DesktopWindow::resizeFramebuffer(int new_w, int new_h)
{
  int factor = viewport->getScaleFactor();
  int scaled_w = new_w * factor;
  int scaled_h = new_h * factor;

  if ((new_w == viewport->framebufferWidth()) &&
      (new_h == viewport->framebufferHeight()) &&
      (scaled_w == viewport->phys_w()) &&
      (scaled_h == viewport->phys_h()))
    return;

  float ui = uiScale();
  int physW, physH;
  getPhysicalSize(&physW, &physH);
  int tol = std::max(1, (int)ceilf(ui));

  // If we're letting the viewport match the window perfectly, then
  // keep things that way for the new size, otherwise just keep things
  // like they are.
  if (!fullscreen_active() && !isMaximized() && !pendingRemoteResize &&
      !Fl::has_timeout(handleResizeTimeout, this)) {
    if ((abs(physW - viewport->phys_w()) <= tol) &&
        (abs(physH - viewport->phys_h()) <= tol)) {
      int sx, sy, sw, sh;
      Fl::screen_work_area(sx, sy, sw, sh, currentScreenNum());
      int win_w = std::min(physicalToLogicalSize(scaled_w, ui), sw);
      int win_h = std::min(physicalToLogicalSize(scaled_h, ui), sh);
      size(win_w, win_h);
    }
  }

  viewport->resizeFramebuffer(new_w, new_h);

  repositionWidgets();
}


void DesktopWindow::setDesktopSizeDone(unsigned /*result*/)
{
  pendingRemoteResize = false;

  // We might have resized again whilst waiting for the previous
  // request, so check if we are in sync
  remoteResize();
}


void DesktopWindow::setCursor()
{
  viewport->setCursor();
}


void DesktopWindow::setCursorPos(const core::Point& pos)
{
  if (!mouseGrabbed) {
    // Do nothing if we do not have the mouse captured.
    return;
  }

  core::Point localPos = scaleFromFramebuffer(pos, viewport->getScaleFactor());
  int winX = localPos.x + viewport->phys_x();
  int winY = localPos.y + viewport->phys_y();

#if defined(WIN32)
  win32_win_warp_pointer(this, winX, winY);
#elif defined(__APPLE__)
  cocoa_win_warp_pointer(this, winX, winY);
#else // Assume this is Xlib
  x11_win_warp_pointer(this, winX, winY);
#endif
}


void DesktopWindow::show()
{
  Fl_Window::show();

#if !defined(WIN32) && !defined(__APPLE__)
  // Request ability to grab keyboard under Xwayland
  x11_win_may_grab(this);
#endif
}


void DesktopWindow::draw()
{
  bool fullRedraw;
  int physW, physH;
  float ui;

  physW = lastPhysW;
  physH = lastPhysH;
#ifdef __APPLE__
  // Cocoa can invalidate the backing store without resizing the FLTK window.
  getPhysicalSize(&physW, &physH);
#endif
  ui = uiScale();
  fullRedraw = (damage() & ~FL_DAMAGE_CHILD);
  if ((lastUIScale != ui) || (lastPhysW != physW) || (lastPhysH != physH)) {
    lastUIScale = ui;
    lastPhysW = physW;
    lastPhysH = physH;
    repositionWidgets();
    viewport->setCursor();
    fullRedraw = true;
    Fl::remove_timeout(handleResizeTimeout, this);
    Fl::add_timeout(0, handleResizeTimeout, this);
  }

  // X11 needs an off screen buffer for compositing to avoid flicker,
  // and alpha blending doesn't work for windows on Win32
#if !defined(__APPLE__)

  // Adjust offscreen surface dimensions
  if ((physW > 0) && (physH > 0) &&
      ((offscreen == nullptr) ||
       (offscreen->width() != physW) || (offscreen->height() != physH))) {
    Surface* replacement = new Surface(physW, physH);
    delete offscreen;
    offscreen = replacement;
    fullRedraw = true;
  }

#endif

  // Active area inside scrollbars (logical and physical)
  int activeLogW = w() - (vscroll->visible() ? vscroll->w() : 0);
  int activeLogH = h() - (hscroll->visible() ? hscroll->h() : 0);
  int activePhysW = vscroll->visible()
    ? std::max(0, std::min(physW, (int)floorf((float)vscroll->x() * ui)))
    : physW;
  int activePhysH = hscroll->visible()
    ? std::max(0, std::min(physH, (int)floorf((float)hscroll->y() * ui)))
    : physH;

  // Simplify the clip region to a simple rectangle in order to
  // properly draw all the layers even if they only partially overlap
  core::Rect physClip;
  if (fullRedraw) {
    physClip.setXYWH(0, 0, activePhysW, activePhysH);
  } else {
    int cx, cy, cw, ch;
    fl_clip_box(0, 0, activeLogW, activeLogH, cx, cy, cw, ch);
    physClip = logicalRectToPhysical(core::Rect(cx, cy, cx + cw, cy + ch),
                                     ui, activePhysW, activePhysH);
    if (!physClip.is_empty()) {
      if (cx + cw == activeLogW)
        physClip.br.x = activePhysW;
      if (cy + ch == activeLogH)
        physClip.br.y = activePhysH;
    }
  }

  // Redraw background only on full redraws
  if (fullRedraw) {
    if (offscreen)
      offscreen->clear(40, 40, 40);
    else {
      fl_push_no_clip();
      fl_push_clip(0, 0, activeLogW, activeLogH);
      fl_rectf(0, 0, activeLogW, activeLogH, 40, 40, 40);
      fl_pop_clip();
      fl_pop_clip();
    }
  }

  if (!physClip.is_empty()) {
    if (offscreen) {
      viewport->draw(offscreen, physClip.tl.x, physClip.tl.y,
                     physClip.width(), physClip.height());
      viewport->clear_damage();
    } else {
      fl_push_no_clip();
      viewport->draw(physClip.tl.x, physClip.tl.y,
                     physClip.width(), physClip.height());
      viewport->clear_damage();
      fl_pop_clip();
    }
  }

  // Debug graph (if active)
  if (statsGraph && !physClip.is_empty()) {
    int margin = logicalToPhysical(30, ui);
    int gx = physW - statsGraph->width() - margin;
    int gy = physH - statsGraph->height() - margin;
    core::Rect dstRect(gx, gy,
                       gx + statsGraph->width(), gy + statsGraph->height());
    core::Rect inter = dstRect.intersect(physClip);

    if (!inter.is_empty()) {
      if (offscreen)
        statsGraph->blend(offscreen, inter.tl.x - gx, inter.tl.y - gy,
                          inter.tl.x, inter.tl.y,
                          inter.width(), inter.height(), 204);
      else {
        fl_push_no_clip();
        statsGraph->blend(inter.tl.x - gx, inter.tl.y - gy,
                          inter.tl.x, inter.tl.y,
                          inter.width(), inter.height(), 204);
        fl_pop_clip();
      }
    }
  }

  // Overlay (if active)
  if (!overlays.empty() && !physClip.is_empty()) {
    int sx, sy, sw;
    struct Overlay overlay;

    overlay = overlays.front();

    // Make sure it's properly seen by adjusting it relative to the
    // primary screen rather than the entire window
    if (fullscreen_active()) {
      assert(Fl::screen_count() >= 1);

      int wx, wy, ww, wh;
      getWindowPhysicalRect(&wx, &wy, &ww, &wh);
      int tol = std::max(1, (int)ceilf(ui));
      core::Rect windowRect(wx - tol, wy - tol, wx + ww + tol, wy + wh + tol);

      core::Rect chosen;
      bool foundEnclosedScreen = false;
      for (int idx = 0; idx < Fl::screen_count(); idx++) {
        core::Rect screenRect = getScreenPhysicalRect(idx);

        // The screen with the smallest index that are enclosed by
        // the viewport will be used for showing the overlay.
        if (screenRect.enclosed_by(windowRect)) {
          chosen = screenRect;
          foundEnclosedScreen = true;
          break;
        }
      }

      // If no monitor inside the viewport was found,
      // use the one primary instead.
      if (!foundEnclosedScreen)
        chosen = getScreenPhysicalRect(0);

      // Adjust the coordinates so they are relative to the viewport.
      sx = chosen.tl.x - wx;
      sy = chosen.tl.y - wy;
      sw = chosen.width();

    } else {
      sx = 0;
      sy = 0;
      sw = physW;
    }

    int ox = sx + (sw - overlay.surface->width()) / 2;
    int oy = sy + logicalToPhysical(50, ui);
    core::Rect dstRect(ox, oy,
                       ox + overlay.surface->width(),
                       oy + overlay.surface->height());
    core::Rect inter = dstRect.intersect(physClip);

    if (!inter.is_empty()) {
      if (offscreen)
        overlay.surface->blend(offscreen, inter.tl.x - ox, inter.tl.y - oy,
                               inter.tl.x, inter.tl.y,
                               inter.width(), inter.height(), overlay.alpha);
      else {
        fl_push_no_clip();
        overlay.surface->blend(inter.tl.x - ox, inter.tl.y - oy,
                               inter.tl.x, inter.tl.y,
                               inter.width(), inter.height(), overlay.alpha);
        fl_pop_clip();
      }
    }
  }

  // Flush offscreen surface to screen
  if (offscreen && !physClip.is_empty()) {
    fl_push_no_clip();
    offscreen->draw(physClip.tl.x, physClip.tl.y,
                    physClip.tl.x, physClip.tl.y,
                    physClip.width(), physClip.height());
    fl_pop_clip();
  }

  // Finally the scrollbars

  if (fullRedraw) {
    draw_child(*hscroll);
    draw_child(*vscroll);
  } else {
    update_child(*hscroll);
    update_child(*vscroll);
  }
}


void DesktopWindow::setLEDState(unsigned int state)
{
  viewport->setLEDState(state);
}


void DesktopWindow::handleClipboardRequest()
{
  viewport->handleClipboardRequest();
}

void DesktopWindow::handleClipboardAnnounce(bool available)
{
  viewport->handleClipboardAnnounce(available);
}

void DesktopWindow::handleClipboardData(const char* data)
{
  viewport->handleClipboardData(data);
}


void DesktopWindow::resize(int x, int y, int w, int h)
{
  bool moving, resizing, rescale;

#if ! (defined(WIN32) || defined(__APPLE__))
  // X11 window managers will treat a resize to cover the entire
  // monitor as a request to go full screen. Make sure we avoid this.
  if (!fullscreen_active() && !Fl_Window::is_a_rescale()) {
    bool resize_req;

    // If there is no X11 window, then this must be a resize request,
    // not a notification from the X server.
    if (!shown())
      resize_req = true;
    else {
      // Otherwise we need to get the real window coordinates to tell
      // the difference
      int wx, wy, ww, wh;
      float ui = uiScale();
      int tol = std::max(1, (int)ceilf(ui));

      // Actual resize request?
      if (!x11_win_get_coords(this, &wx, &wy, &ww, &wh) ||
          (abs(wx - logicalToPhysical(x, ui)) > tol) ||
          (abs(wy - logicalToPhysical(y, ui)) > tol) ||
          (abs(ww - logicalToPhysical(w, ui)) > tol) ||
          (abs(wh - logicalToPhysical(h, ui)) > tol))
        resize_req = true;
      else
        resize_req = false;
    }

    if (resize_req) {
      for (int idx = 0;idx < Fl::screen_count();idx++) {
        int sx, sy, sw, sh;

        Fl::screen_xywh(sx, sy, sw, sh, idx);

        // We can't trust x and y if the window isn't mapped as the
        // window manager might adjust those numbers
        if (shown() && ((sx != x) || (sy != y)))
            continue;

        if ((sw != w) || (sh != h))
            continue;

        vlog.info(_("Adjusting window size to avoid accidental full-screen request"));
        // Assume a panel of some form and adjust the height
        h -= 40;
      }
    }
  }
#endif

  moving = resizing = false;
  if ((this->x() != x) || (this->y() != y))
    moving = true;
  if ((this->w() != w) || (this->h() != h))
    resizing = true;
  rescale = Fl_Window::is_a_rescale();

  Fl_Window::resize(x, y, w, h);

  float curScale = uiScale();
  int physW, physH;
  getPhysicalSize(&physW, &physH);

  bool scaleChanged = (curScale != lastUIScale);
  bool physChanged = (physW != lastPhysW) || (physH != lastPhysH);

  lastUIScale = curScale;
  lastPhysW = physW;
  lastPhysH = physH;

  if (moving)
    // We might be overlapping a different set of monitors now, even if
    // our size is the same
    remoteResize();

  if (resizing || rescale || scaleChanged || physChanged) {
    if (!moving)
      remoteResize();

    repositionWidgets();
  }

  if (rescale || scaleChanged)
    viewport->setCursor();
  if (rescale || scaleChanged || physChanged)
    redraw();
}

void DesktopWindow::addOverlayTip(const char* text, ...)
{
  va_list ap;
  char textbuf[1024];

  std::map<std::string, time_t>::iterator iter;

  va_start(ap, text);
  vsnprintf(textbuf, sizeof(textbuf), text, ap);
  textbuf[sizeof(textbuf)-1] = '\0';
  va_end(ap);

  // Purge all old entries
  for (iter = overlayTimes.begin(); iter != overlayTimes.end(); ) {
    if ((time(nullptr) - iter->second) >= OVERLAY_REPEAT_TIMEOUT)
      overlayTimes.erase(iter++);
    else
      iter++;
  }

  // Recently shown?
  if (overlayTimes.count(textbuf) > 0)
    return;

  overlayTimes[textbuf] = time(nullptr);

  addOverlay(textbuf);
}

void DesktopWindow::addOverlayError(const char* text, ...)
{
  va_list ap;
  char textbuf[1024];

  va_start(ap, text);
  vsnprintf(textbuf, sizeof(textbuf), text, ap);
  textbuf[sizeof(textbuf)-1] = '\0';
  va_end(ap);

  addOverlay(textbuf);
}

void DesktopWindow::addOverlay(const char *text)
{
  const Fl_Fontsize fontsize = 16;
  const int margin = 10;

  Fl_Image_Surface *surface;

  Fl_RGB_Image* imageText;
  Fl_RGB_Image* image;

  unsigned char* buffer;

  int x, y;
  int w, h;

  unsigned char* a;
  const unsigned char* b;

  struct Overlay overlay;

  if (shown())
    make_current();

  fl_font(FL_HELVETICA, fontsize);
  w = 0;
  h = 0;
  fl_measure(text, w, h);

  // Margins
  w += margin * 2 * 2;
  h += margin * 2;

  surface = new Fl_Image_Surface(w, h, 1);
  Fl_Surface_Device::push_current(surface);

  fl_rectf(0, 0, w, h, 0, 0, 0);

  fl_font(FL_HELVETICA, fontsize);
  fl_color(FL_WHITE);
  fl_draw(text, 0, 0, w, h, FL_ALIGN_CENTER);

  imageText = surface->image();
  Fl_Surface_Device::pop_current();
  delete surface;

  int iw = imageText->data_w();
  int ih = imageText->data_h();

  buffer = new unsigned char[iw * ih * 4];
  image = new Fl_RGB_Image(buffer, iw, ih, 4);

  a = buffer;
  for (x = 0;x < image->data_w() * image->data_h();x++) {
    a[0] = a[1] = a[2] = 0x40;
    a[3] = 0xcc;
    a += 4;
  }

  a = buffer;
  b = (const unsigned char*)imageText->data()[0];
  for (y = 0;y < ih;y++) {
    for (x = 0;x < iw;x++) {
      unsigned char alpha;
      alpha = *b;
      a[0] = (unsigned)a[0] * (255 - alpha) / 255 + alpha;
      a[1] = (unsigned)a[1] * (255 - alpha) / 255 + alpha;
      a[2] = (unsigned)a[2] * (255 - alpha) / 255 + alpha;
      a[3] = 255 - (255 - a[3]) * (255 - alpha) / 255;
      a += 4;
      b += imageText->d();
    }
    if (imageText->ld() != 0)
      b += imageText->ld() - iw * imageText->d();
  }

  delete imageText;

  overlay.surface = new Surface(image);
  overlay.alpha = 0;
  memset(&overlay.start, 0, sizeof(overlay.start));
  overlays.push_back(overlay);

  delete image;
  delete [] buffer;

  if (overlays.size() == 1)
    Fl::add_timeout(0.5, updateOverlay, this);
}

void DesktopWindow::updateOverlay(void *data)
{
  DesktopWindow *self;
  struct Overlay* overlay;
  unsigned elapsed;

  self = (DesktopWindow*)data;

  if (self->overlays.empty())
    return;

  overlay = &self->overlays.front();

  if (overlay->start.tv_sec == 0)
    gettimeofday(&overlay->start, nullptr);

  elapsed = core::msSince(&overlay->start);

  if (elapsed < 500) {
    overlay->alpha = (unsigned)255 * elapsed / 500;
    Fl::add_timeout(1.0/60, updateOverlay, self);
  } else if (elapsed < 3500) {
    overlay->alpha = 255;
    Fl::add_timeout(3.0, updateOverlay, self);
  } else if (elapsed < 4000) {
    overlay->alpha = (unsigned)255 * (4000 - elapsed) / 500;
    Fl::add_timeout(1.0/60, updateOverlay, self);
  } else {
    delete overlay->surface;
    self->overlays.pop_front();
    if (!self->overlays.empty())
      Fl::add_timeout(0.5, updateOverlay, self);
  }

  // FIXME: Only damage relevant area
  self->damage(FL_DAMAGE_USER1);
}


int DesktopWindow::handle(int event)
{
  switch (event) {
  case FL_FULLSCREEN:
    fullScreen.setParam(fullscreen_active());

    // Update scroll bars
    repositionWidgets();

    // Show how to get out of full screen
    if (fullscreen_active()) {
      unsigned modifierMask;

      modifierMask = 0;
      for (core::EnumListEntry key : shortcutModifiers)
        modifierMask |= ShortcutHandler::parseModifier(key.getValueStr().c_str());

      if (modifierMask)
        addOverlayTip(_("Press %sEnter to leave full-screen mode"),
                      ShortcutHandler::modifierPrefix(modifierMask));
    }

#ifdef __APPLE__
    // Complain to the user if we won't have permission to grab keyboard
    if (fullscreenSystemKeys && fullscreen_active()) {
      // FIXME: There is some race during initial full screen where we
      //        fail to give focus to the popup, but we can work around
      //        it using a timer
      Fl::add_timeout(0, [](void*) { cocoa_is_trusted(true); }, nullptr);
    }
#endif

    // Automatically toggle keyboard grab?
    if (fullscreenSystemKeys) {
      if (fullscreen_active())
        grabKeyboard();
      else
        ungrabKeyboard();
    }

    // The window manager respected our full screen request, but we
    // still need to wait a bit long for it to finish resizing us
    if (delayedFullscreen && fullscreen_active()) {
      Fl::remove_timeout(handleFullscreenTimeout, this);
      Fl::add_timeout(0.1, handleFullscreenTimeout, this);
    }

    break;

  case FL_ENTER:
      if (keyboardGrabbed)
          grabPointer();
      /* fall through */
  case FL_LEAVE:
  case FL_DRAG:
  case FL_MOVE: {
    if (!mouseGrabbed && !fullscreen_active())
      break;

    int physW = lastPhysW;
    int physH = lastPhysH;
    core::Point evPos = getEventPhysicalPos();

    if (mouseGrabbed) {
      // We don't get FL_LEAVE with a grabbed pointer, so check manually
      if ((evPos.x < 0) || (evPos.x >= physW) ||
          (evPos.y < 0) || (evPos.y >= physH)) {
        ungrabPointer();
      }
#if !defined(WIN32) && !defined(__APPLE__)
      // We also don't get sensible coordinates on zaphod setups
      if (!x11_is_pointer_on_same_screen(this))
        ungrabPointer();
#endif
    }
    if (event == FL_LEAVE) {
      Fl::remove_timeout(handleEdgeScroll, this);
    } else if (fullscreen_active()) {
      edgeScrollPos = evPos;
      // calculate width of "edge" regions
      edge_scroll_size_x = physW / EDGE_SCROLL_SIZE;
      edge_scroll_size_y = physH / EDGE_SCROLL_SIZE;
      // if cursor is near the edge of the window, scroll
      if (((viewport->phys_x() < 0) && (evPos.x < edge_scroll_size_x)) ||
          ((viewport->phys_x() + viewport->phys_w() > physW) && (evPos.x >= physW - edge_scroll_size_x)) ||
          ((viewport->phys_y() < 0) && (evPos.y < edge_scroll_size_y)) ||
          ((viewport->phys_y() + viewport->phys_h() > physH) && (evPos.y >= physH - edge_scroll_size_y))) {
        if (!Fl::has_timeout(handleEdgeScroll, this))
          Fl::add_timeout(EDGE_SCROLL_SECONDS_PER_FRAME, handleEdgeScroll, this);
      }
    }
    // Continue processing so that the viewport also gets mouse events
    break;
  }
  }

  return Fl_Window::handle(event);
}


int DesktopWindow::fltkDispatch(int event, Fl_Window *win, void *)
{
#if !defined(WIN32) && !defined(__APPLE__)
  // FLTK passes through the fake grab focus events that can cause us
  // to end up in an infinite loop
  // https://github.com/fltk/fltk/issues/295
  if ((event == FL_FOCUS) || (event == FL_UNFOCUS)) {
    const XFocusChangeEvent* xfocus = &fl_xevent->xfocus;
    if ((xfocus->mode == NotifyGrab) || (xfocus->mode == NotifyUngrab))
      return 1;
  }
#endif

  DesktopWindow *dw = dynamic_cast<DesktopWindow*>(win);

  if (dw) {
    switch (event) {
    // This is hackish and the result of the dodgy focus handling in FLTK.
    // The basic problem is that FLTK's view of focus and the system's tend
    // to differ, and as a result we do not see all the FL_FOCUS events we
    // need. Fortunately we can grab them here...
    case FL_FOCUS:
    case FL_UNFOCUS:
      Fl::add_idle(checkFocus, dw);
      break;

    case FL_SHOW:
      // In this particular place, FL_SHOW means an actual MapNotify,
      // which means we can continue enabling initial fullscreen.
      if (dw->delayedFullscreen && !dw->fullscreen_active()) {
        // Hack: Fullscreen requests may be ignored, so we need a
        // timeout for when we should stop waiting. We also need to wait
        // for the resize, which can come after the fullscreen event.
        Fl::add_timeout(0.5, handleFullscreenTimeout, dw);
        dw->fullscreen_on();
      }
      break;

    case FL_RELEASE:
      // We usually fail to grab the mouse if a mouse button was
      // pressed when we gained focus (e.g. clicking on our window),
      // so we may need to try again when the button is released.
      // (We do it here rather than handle() because a window does not
      // see FL_RELEASE events if a child widget grabs it first)
      if (dw->keyboardGrabbed && !dw->mouseGrabbed)
        dw->grabPointer();
      break;
    }
  }

  return 0;
}

int DesktopWindow::fltkHandle(int event)
{
  switch (event) {
  case FL_SCREEN_CONFIGURATION_CHANGED:
    // Screens removed or added. Recreate fullscreen window if
    // necessary. On Windows, adding a second screen only works
    // reliable if we are using a timer. Otherwise, the window will
    // not be resized to cover the new screen. A timer makes sense
    // also on other systems, to make sure that whatever desktop
    // environment has a chance to deal with things before we do.
    Fl::remove_timeout(reconfigureFullscreen);
    Fl::add_timeout(0.5, reconfigureFullscreen);
  }

  return 0;
}

void DesktopWindow::fullscreen_on()
{
  bool allMonitors = fullScreenMode == "all";
  bool selectedMonitors = fullScreenMode == "selected";
  int top, bottom, left, right;

#ifdef __APPLE__
  // Avoid surprises if we cannot do proper multiheaded full screen
  if (cocoa_screens_have_separate_spaces()) {
    allMonitors = false;
    selectedMonitors = false;
  }
#endif

  if (not selectedMonitors and not allMonitors) {
    top = bottom = left = right = currentScreenNum();
  } else {
    int top_y, bottom_y, left_x, right_x;

    std::set<int> monitors;

    if (selectedMonitors and not allMonitors) {
      std::set<int> selected = fullScreenSelectedMonitors.getMonitors();
      monitors.insert(selected.begin(), selected.end());
    } else {
      for (int idx = 0; idx < Fl::screen_count(); idx++)
        monitors.insert(idx);
    }

    // If no monitors were found in the selected monitors case, we want
    // to explicitly use the window's current monitor.
    if (monitors.size() == 0) {
      monitors.insert(currentScreenNum());
    }

    // If there are monitors selected, calculate the dimensions
    // of the frame buffer, expressed in the monitor indices that
    // limits it.
    std::set<int>::iterator it = monitors.begin();

    // Get first monitor dimensions (in physical pixels for mixed-DPI setups).
    core::Rect firstRect = getScreenPhysicalRect(*it);
    top = bottom = left = right = *it;
    top_y = firstRect.tl.y;
    bottom_y = firstRect.br.y;
    left_x = firstRect.tl.x;
    right_x = firstRect.br.x;

    // Keep going through the rest of the monitors.
    for (; it != monitors.end(); it++) {
      core::Rect r = getScreenPhysicalRect(*it);

      if (r.tl.y < top_y) {
        top = *it;
        top_y = r.tl.y;
      }

      if (r.br.y > bottom_y) {
        bottom = *it;
        bottom_y = r.br.y;
      }

      if (r.tl.x < left_x) {
        left = *it;
        left_x = r.tl.x;
      }

      if (r.br.x > right_x) {
        right = *it;
        right_x = r.br.x;
      }
    }

  }

  fullscreen_screens(top, bottom, left, right);

  if (!fullscreen_active())
    fullscreen();
}

bool DesktopWindow::hasFocus()
{
  Fl_Widget* focus;

  focus = Fl::grab();
  if (!focus)
    focus = Fl::focus();

  if (!focus)
    return false;

  return focus->window() == this;
}

void DesktopWindow::checkFocus(void *data)
{
  Fl::remove_idle(checkFocus, data);

  DesktopWindow *dw = (DesktopWindow*)data;
  // Focus might not stay with us just because we have grabbed the
  // keyboard. E.g. we might have sub windows, or the user clicked on
  // another application. Make sure we update our grabs with the focus
  // changes.
  if (!dw->hasFocus()) {
    // If the grab is active when we lose focus, the user likely wants
    // the grab to remain once we regain focus
    if (dw->keyboardGrabbed)
      dw->regrabOnFocus = true;
    dw->ungrabKeyboard();
  } else {
    if (dw->regrabOnFocus ||
        (fullscreenSystemKeys && dw->fullscreen_active()))
      dw->grabKeyboard();
    dw->regrabOnFocus = false;
  }
}

void DesktopWindow::grabKeyboard()
{
  unsigned modifierMask;

  // Grabbing the keyboard is fairly safe as FLTK reroutes events to the
  // correct widget regardless of which low level window got the system
  // event.

  // FIXME: Push this stuff into FLTK.

  if (keyboardGrabbed)
    return;

  if (!hasFocus())
    return;

#if defined(WIN32)
  int ret;
  
  ret = win32_enable_lowlevel_keyboard(this);
  if (ret != 0) {
    vlog.error(_("Failure grabbing control of the keyboard"));
    addOverlayError(_("Failure grabbing control of the keyboard"));
    return;
  }
#elif defined(__APPLE__)
  bool ret;

  ret = cocoa_tap_keyboard();
  if (!ret) {
    vlog.error(_("Failure grabbing control of the keyboard"));
    addOverlayError(_("Failure grabbing control of the keyboard"));
    return;
  }
#else
  bool ret;

  ret = x11_grab_keyboard(this);
  if (!ret) {
    vlog.error(_("Failure grabbing control of the keyboard"));
    addOverlayError(_("Failure grabbing control of the keyboard"));
    return;
  }
#endif

  keyboardGrabbed = true;

  if (contains(Fl::belowmouse()))
    grabPointer();

  updateCaption();

  modifierMask = 0;
  for (core::EnumListEntry key : shortcutModifiers)
    modifierMask |= ShortcutHandler::parseModifier(key.getValueStr().c_str());

  if (modifierMask)
    addOverlayTip(_("Press %s to release keyboard control from the session"),
                  ShortcutHandler::modifierPrefix(modifierMask, true));
}


void DesktopWindow::ungrabKeyboard()
{
  keyboardGrabbed = false;

  ungrabPointer();

  updateCaption();

#if defined(WIN32)
  win32_disable_lowlevel_keyboard(this);
#elif defined(__APPLE__)
  cocoa_untap_keyboard();
#else
  // FLTK has a grab so lets not mess with it
  if (Fl::grab())
    return;

  x11_ungrab_keyboard();
#endif
}


void DesktopWindow::grabPointer()
{
#if !defined(WIN32) && !defined(__APPLE__)
  // We also need to grab the pointer as some WMs like to grab buttons
  // combined with modifies (e.g. Alt+Button0 in metacity).

  // Having a button pressed prevents us from grabbing, we make
  // a new attempt in fltkHandle()
  if (!x11_grab_pointer(this))
    return;
#endif

  mouseGrabbed = true;
}


void DesktopWindow::ungrabPointer()
{
  mouseGrabbed = false;

#if !defined(WIN32) && !defined(__APPLE__)
  x11_ungrab_pointer(this);
#endif
}


void DesktopWindow::maximizeWindow()
{
#if defined(WIN32)
  win32_win_maximize(this);
#elif defined(__APPLE__)
  if (fullscreen_active())
    return;
  cocoa_win_zoom(this);
#else
  x11_win_maximize(this);
#endif
}


bool DesktopWindow::isMaximized()
{
#ifdef WIN32
  return win32_win_is_maximized(this);
#elif defined(__APPLE__)
  return cocoa_win_is_zoomed(this);
#else
  return x11_win_is_maximized(this);
#endif
}


void DesktopWindow::handleResizeTimeout(void *data)
{
  DesktopWindow *self = (DesktopWindow *)data;

  assert(self);

  self->remoteResize();
}


void DesktopWindow::reconfigureFullscreen(void* /*data*/)
{
  std::set<DesktopWindow *>::iterator iter;

  for (iter = instances.begin(); iter != instances.end(); ++iter) {
    if ((*iter)->fullscreen_active())
      (*iter)->fullscreen_on();
  }
}


void DesktopWindow::remoteResize()
{
  int width, height;
  rfb::ScreenSet layout;
  rfb::ScreenSet::const_iterator iter;

  if (viewOnly)
    return;

  if (!::remoteResize)
    return;
  if (!cc->server.supportsSetDesktopSize)
    return;

  // Don't pester the server with a resize until we have our final size
  // FIXME: Some window managers (e.g. mutter) will do multiple resizes
  //        every time we enter or leave full screen, which we'd also
  //        like to avoid
  if (delayedFullscreen)
    return;

  // Rate limit to one pending resize at a time
  if (pendingRemoteResize)
    return;

  // And no more than once every 100ms
  if (core::msSince(&lastResize) < 100) {
    Fl::remove_timeout(handleResizeTimeout, this);
    Fl::add_timeout((100.0 - core::msSince(&lastResize)) / 1000.0,
                    handleResizeTimeout, this);
    return;
  }

  int physW, physH;
  getPhysicalSize(&physW, &physH);

  int factor = viewport->getScaleFactor();
  core::Point remoteSize = computeRemoteDesktopSize(physW, physH, factor);
  width = remoteSize.x;
  height = remoteSize.y;

  bool explicitDesktopSize = false;
  if (!sentDesktopSize && (strcmp(desktopSize, "") != 0)) {
    // An explicit size has been requested

    sentDesktopSize = true;
    explicitDesktopSize = parseRemoteDesktopSize(desktopSize, &width, &height);
    if (!explicitDesktopSize)
      vlog.error(_("Invalid desktop size: %s"), (const char*)desktopSize);
  }

  if (!fullscreen_active() || (width * factor > physW) || (height * factor > physH)) {
    // In windowed mode (or the framebuffer is so large that we need
    // to scroll) we just report a single virtual screen that covers
    // the entire framebuffer.

    layout = cc->server.screenLayout();

    // Not sure why we have no screens, but adding a new one should be
    // safe as there is nothing to conflict with...
    if (layout.num_screens() == 0)
      layout.add_screen(rfb::Screen());
    else if (layout.num_screens() != 1) {
      // More than one screen. Remove all but the first (which we
      // assume is the "primary").

      while (true) {
        iter = layout.begin();
        ++iter;

        if (iter == layout.end())
          break;

        layout.remove_screen(iter->id);
      }
    }

    // Resize the remaining single screen to the complete framebuffer
    layout.begin()->dimensions.tl.x = 0;
    layout.begin()->dimensions.tl.y = 0;
    layout.begin()->dimensions.br.x = width;
    layout.begin()->dimensions.br.y = height;
  } else {
    uint32_t id;
    int wx, wy, ww, wh;
    core::Rect ref_rect, screen_rect;

    // In full screen we report all screens that are fully covered.

    getWindowPhysicalRect(&wx, &wy, &ww, &wh);

    if (!explicitDesktopSize) {
      ref_rect.setXYWH(wx, wy, ww, wh);
    } else {
      ref_rect.setXYWH(wx + (ww - width * factor) / 2,
                       wy + (wh - height * factor) / 2,
                       width * factor, height * factor);
    }

    // If we can find a matching screen in the existing set, we use
    // that, otherwise we create a brand new screen.
    //
    // FIXME: We should really track screens better so we can handle
    //        a resized one.
    //
    for (int idx = 0;idx < Fl::screen_count();idx++) {
      screen_rect = getScreenPhysicalRect(idx);
#ifdef __APPLE__
      float scrScale = uiScale();
#else
      float scrScale = Fl::screen_scale(idx);
      if (scrScale <= 0.0f)
        scrScale = 1.0f;
#endif
      int tol = std::max(1, (int)ceilf(scrScale));
      core::Rect expandedRef(ref_rect.tl.x - tol, ref_rect.tl.y - tol,
                             ref_rect.br.x + tol, ref_rect.br.y + tol);

      // Check that the screen is fully inside the framebuffer
      if (!screen_rect.enclosed_by(expandedRef))
        continue;

      screen_rect = screen_rect.intersect(ref_rect);
      core::Rect r = scaleScreenRectToRemote(screen_rect, ref_rect,
                                             width, height, factor);
      if (r.is_empty())
        continue;

      // Look for perfectly matching existing screen that is not yet present in
      // in the screen layout...
      for (iter = cc->server.screenLayout().begin();
           iter != cc->server.screenLayout().end(); ++iter) {
        if ((iter->dimensions.tl.x == r.tl.x) &&
            (iter->dimensions.tl.y == r.tl.y) &&
            (iter->dimensions.width() == r.width()) &&
            (iter->dimensions.height() == r.height()) &&
            (std::find(layout.begin(), layout.end(), *iter) == layout.end()))
          break;
      }

      // Found it?
      if (iter != cc->server.screenLayout().end()) {
        layout.add_screen(*iter);
        continue;
      }

      // Need to add a new one, which means we need to find an unused id
      while (true) {
        id = rand();
        for (iter = cc->server.screenLayout().begin();
             iter != cc->server.screenLayout().end(); ++iter) {
          if (iter->id == id)
            break;
        }

        if (iter == cc->server.screenLayout().end())
          break;
      }

      layout.add_screen(rfb::Screen(id, r.tl.x, r.tl.y,
                                    r.width(), r.height(), 0));
    }

    // If the viewport doesn't match a physical screen, then we might
    // end up with no screens in the layout. Add a fake one...
    if (layout.num_screens() == 0)
      layout.add_screen(rfb::Screen(0, 0, 0, width, height, 0));

    // Try to be kind to servers in the simple case of toggling between
    // windowed and full screen by retaining the same screen
    if ((layout.num_screens() == 1) &&
        (cc->server.screenLayout().num_screens() == 1)) {
      layout.begin()->id = cc->server.screenLayout().begin()->id;
      layout.begin()->flags = cc->server.screenLayout().begin()->flags;
    }
  }

  // Do we actually change anything?
  if ((width == cc->server.width()) &&
      (height == cc->server.height()) &&
      (layout == cc->server.screenLayout()))
    return;

  // Have we already requested this size?
  if ((width == lastResizeWidth) &&
      (height == lastResizeHeight) &&
      (layout.num_screens() == lastResizeLayout.num_screens())) {
    // We can't compare the layouts directly, as we allocate random ids
    // on each attempt
    bool same = true;
    for (const rfb::Screen& s1 : layout) {
      bool found = false;
      for (const rfb::Screen& s2 : lastResizeLayout) {
        if ((s1.dimensions == s2.dimensions) &&
            (s1.flags == s2.flags)) {
          found = true;
          break;
        }
      }
      if (!found) {
        same = false;
        break;
      }
    }
    if (same)
      return;
  }

  if (!layout.validate(width, height)) {
    vlog.error(_("Invalid screen layout computed for resize request!"));
    vlog.error("%s", layout.print().c_str());
    return;
  }

  pendingRemoteResize = true;
  gettimeofday(&lastResize, nullptr);
  lastResizeWidth = width;
  lastResizeHeight = height;
  lastResizeLayout = layout;
  try {
    cc->requestScreenLayout(width, height, layout);
  } catch (std::exception& e) {
    vlog.error("%s", e.what());
    abort_connection_unexpected(e);
  }
}


void DesktopWindow::repositionWidgets()
{
  int physW, physH;
  float ui;
  int new_x, new_y;

  getPhysicalSize(&physW, &physH);
  ui = uiScale();

  // Viewport position

  new_x = viewport->phys_x();
  new_y = viewport->phys_y();

  if (physW > viewport->phys_w())
    new_x = (physW - viewport->phys_w()) / 2;
  else {
    if (viewport->phys_x() > 0)
      new_x = 0;
    else if (physW > (viewport->phys_x() + viewport->phys_w()))
      new_x = physW - viewport->phys_w();
  }

  // Same thing for y axis
  if (physH > viewport->phys_h())
    new_y = (physH - viewport->phys_h()) / 2;
  else {
    if (viewport->phys_y() > 0)
      new_y = 0;
    else if (physH > (viewport->phys_y() + viewport->phys_h()))
      new_y = physH - viewport->phys_h();
  }

  if ((new_x != viewport->phys_x()) || (new_y != viewport->phys_y())) {
    viewport->setPhysicalPosition(new_x, new_y, ui);
    damage(FL_DAMAGE_SCROLL);
  } else {
    viewport->syncLogicalGeometry(ui);
  }

  // Scrollbars visibility

  if (fullscreen_active()) {
    hscroll->hide();
    vscroll->hide();
  } else {
    // Decide whether to show a scrollbar by checking if the window
    // size (possibly minus scrollbar_size) is less than the viewport
    // (remote framebuffer) size.
    //
    // We decide whether to subtract scrollbar_size on an axis by
    // checking if the other axis *definitely* needs a scrollbar.  You
    // might be tempted to think that this becomes a weird recursive
    // problem, but it isn't: If the window size is less than the
    // viewport size (without subtracting the scrollbar_size), then
    // that axis *definitely* needs a scrollbar; if the check changes
    // when we subtract scrollbar_size, then that axis only *maybe*
    // needs a scrollbar.  If both axes only "maybe" need a scrollbar,
    // then neither does; so we don't need to recurse on the "maybe"
    // cases.

    int sbPhys = logicalToPhysical(Fl::scrollbar_size(), ui);

    if (physW - (physH < viewport->phys_h() ? sbPhys : 0) < viewport->phys_w())
      hscroll->show();
    else
      hscroll->hide();

    if (physH - (physW < viewport->phys_w() ? sbPhys : 0) < viewport->phys_h())
      vscroll->show();
    else
      vscroll->hide();
  }

  // Scrollbars positions (logical FLTK units)

  hscroll->resize(0, h() - Fl::scrollbar_size(),
                  w() - (vscroll->visible() ? Fl::scrollbar_size() : 0),
                  Fl::scrollbar_size());
  vscroll->resize(w() - Fl::scrollbar_size(), 0,
                  Fl::scrollbar_size(),
                  h() - (hscroll->visible() ? Fl::scrollbar_size() : 0));

  // Scrollbars range (physical pixels)

  int activePhysW = vscroll->visible()
    ? std::max(0, std::min(physW, (int)floorf((float)vscroll->x() * ui)))
    : physW;
  int activePhysH = hscroll->visible()
    ? std::max(0, std::min(physH, (int)floorf((float)hscroll->y() * ui)))
    : physH;

  hscroll->value(-viewport->phys_x(), activePhysW, 0, viewport->phys_w());
  vscroll->value(-viewport->phys_y(), activePhysH, 0, viewport->phys_h());
  hscroll->value(hscroll->clamp(hscroll->value()));
  vscroll->value(vscroll->clamp(vscroll->value()));
}

void DesktopWindow::handleClose(Fl_Widget* /*wnd*/, void* /*data*/)
{
  disconnect();
}


void DesktopWindow::handleOptions(void *data)
{
  DesktopWindow *self = (DesktopWindow*)data;
  int newScale = scaleFactor;
  bool scaleChanged = (newScale != self->viewport->getScaleFactor());
  bool wasFullscreen = self->fullscreen_active();
  float ui = self->uiScale();
  int physW, physH;
  self->getPhysicalSize(&physW, &physH);
  int tol = std::max(1, (int)ceilf(ui));
  bool matchedViewport = (abs(physW - self->viewport->phys_w()) <= tol) &&
                         (abs(physH - self->viewport->phys_h()) <= tol);

  if (scaleChanged)
    self->viewport->setScaleFactor(newScale);

  // Call fullscreen_on even if active since it handles
  // fullScreenMode
  if (fullScreen)
    self->fullscreen_on();
  else if (!fullScreen && self->fullscreen_active())
    self->fullscreen_off();

  if (scaleChanged) {
    if (!fullScreen && !wasFullscreen && !self->isMaximized() &&
        matchedViewport) {
      int sx, sy, sw, sh;
      Fl::screen_work_area(sx, sy, sw, sh, self->currentScreenNum());
      float curUi = self->uiScale();
      int win_w = std::min(physicalToLogicalSize(self->viewport->phys_w(), curUi), sw);
      int win_h = std::min(physicalToLogicalSize(self->viewport->phys_h(), curUi), sh);
      self->size(win_w, win_h);
    }

    self->repositionWidgets();
    self->remoteResize();
    self->redraw();
  }
}

void DesktopWindow::handleFullscreenTimeout(void *data)
{
  DesktopWindow *self = (DesktopWindow *)data;

  assert(self);

  // We are here because we got tired of waiting for the window manager
  // to finish switching to fullscreen mode, or because we are waiting
  // for all resize events so we get our final position

  self->delayedFullscreen = false;
  self->remoteResize();
}

void DesktopWindow::scrollTo(int sx, int sy)
{
  sx = hscroll->clamp(sx);
  sy = vscroll->clamp(sy);

  hscroll->value(sx);
  vscroll->value(sy);

  // Scrollbar position results in inverse movement of
  // the viewport widget
  sx = -sx;
  sy = -sy;

  if ((viewport->phys_x() == sx) && (viewport->phys_y() == sy))
    return;

  viewport->setPhysicalPosition(sx, sy, uiScale());
  damage(FL_DAMAGE_SCROLL);
}

void DesktopWindow::handleScroll(Fl_Widget* /*widget*/, void *data)
{
  DesktopWindow *self = (DesktopWindow *)data;

  self->scrollTo(self->hscroll->value(), self->vscroll->value());
}

void DesktopWindow::handleEdgeScroll(void *data)
{
  DesktopWindow *self = (DesktopWindow *)data;

  int physW, physH;
  int mx, my;
  int dx, dy;
  int speed;

  assert(self);

  if (!self->fullscreen_active())
    return;

  physW = self->lastPhysW;
  physH = self->lastPhysH;
  mx = self->edgeScrollPos.x;
  my = self->edgeScrollPos.y;
  speed = std::max(1, logicalToPhysical(EDGE_SCROLL_SPEED, self->uiScale()));

  dx = dy = 0;

  // Clamp mouse position in case it is outside the window
  if (mx < 0)
    mx = 0;
  if (mx > physW)
    mx = physW;
  if (my < 0)
    my = 0;
  if (my > physH)
    my = physH;

  if ((edge_scroll_size_x > 0) &&
      (self->viewport->phys_x() < 0) && (mx < edge_scroll_size_x))
    dx = speed - speed * mx / edge_scroll_size_x;
  if ((edge_scroll_size_x > 0) &&
      (self->viewport->phys_x() + self->viewport->phys_w() > physW) &&
      (mx >= physW - edge_scroll_size_x))
    dx = speed * (physW - mx) / edge_scroll_size_x - speed - 1;
  if ((edge_scroll_size_y > 0) &&
      (self->viewport->phys_y() < 0) && (my < edge_scroll_size_y))
    dy = speed - speed * my / edge_scroll_size_y;
  if ((edge_scroll_size_y > 0) &&
      (self->viewport->phys_y() + self->viewport->phys_h() > physH) &&
      (my >= physH - edge_scroll_size_y))
    dy = speed * (physH - my) / edge_scroll_size_y - speed - 1;

  if ((dx == 0) && (dy == 0))
    return;

  self->scrollTo(self->hscroll->value() - dx, self->vscroll->value() - dy);

  Fl::repeat_timeout(EDGE_SCROLL_SECONDS_PER_FRAME, handleEdgeScroll, data);
}

void DesktopWindow::handleStatsTimeout(void *data)
{
  DesktopWindow *self = (DesktopWindow*)data;

  const size_t statsCount = sizeof(self->stats)/sizeof(self->stats[0]);

  unsigned updates, pixels, pos;
  unsigned elapsed;

  const unsigned statsWidth = 200;
  const unsigned statsHeight = 100;
  const unsigned graphWidth = statsWidth - 10;
  const unsigned graphHeight = statsHeight - 25;

  Fl_Image_Surface *surface;
  Fl_RGB_Image *image;

  unsigned maxUPS, maxPPS, maxBPS;
  size_t i;

  char buffer[256];

  updates = self->cc->getUpdateCount();
  pixels = self->cc->getPixelCount();
  pos = self->cc->getPosition();
  elapsed = core::msSince(&self->statsLastTime);
  if (elapsed < 1)
    elapsed = 1;

  memmove(&self->stats[0], &self->stats[1], sizeof(self->stats[0])*(statsCount-1));

  self->stats[statsCount-1].ups = (updates - self->statsLastUpdates) * 1000 / elapsed;
  self->stats[statsCount-1].pps = (pixels - self->statsLastPixels) * 1000 / elapsed;
  self->stats[statsCount-1].bps = (pos - self->statsLastPosition) * 1000 / elapsed;

  gettimeofday(&self->statsLastTime, nullptr);
  self->statsLastUpdates = updates;
  self->statsLastPixels = pixels;
  self->statsLastPosition = pos;

  if (self->shown())
    self->make_current();

  surface = new Fl_Image_Surface(statsWidth, statsHeight, 1);
  Fl_Surface_Device::push_current(surface);

  fl_rectf(0, 0, statsWidth, statsHeight, FL_BLACK);

  fl_rect(5, 5, graphWidth, graphHeight, FL_WHITE);

  maxUPS = maxPPS = maxBPS = 0;
  for (i = 0;i < statsCount;i++) {
    if (self->stats[i].ups > maxUPS)
      maxUPS = self->stats[i].ups;
    if (self->stats[i].pps > maxPPS)
      maxPPS = self->stats[i].pps;
    if (self->stats[i].bps > maxBPS)
      maxBPS = self->stats[i].bps;
  }

  if (maxUPS != 0) {
    fl_color(FL_GREEN);
    for (i = 0;i < statsCount-1;i++) {
      fl_line(5 + i * graphWidth / statsCount,
              5 + graphHeight - graphHeight * self->stats[i].ups / maxUPS,
              5 + (i+1) * graphWidth / statsCount,
              5 + graphHeight - graphHeight * self->stats[i+1].ups / maxUPS);
    }
  }

  if (maxPPS != 0) {
    fl_color(FL_YELLOW);
    for (i = 0;i < statsCount-1;i++) {
      fl_line(5 + i * graphWidth / statsCount,
              5 + graphHeight - graphHeight * self->stats[i].pps / maxPPS,
              5 + (i+1) * graphWidth / statsCount,
              5 + graphHeight - graphHeight * self->stats[i+1].pps / maxPPS);
    }
  }

  if (maxBPS != 0) {
    fl_color(FL_RED);
    for (i = 0;i < statsCount-1;i++) {
      fl_line(5 + i * graphWidth / statsCount,
              5 + graphHeight - graphHeight * self->stats[i].bps / maxBPS,
              5 + (i+1) * graphWidth / statsCount,
              5 + graphHeight - graphHeight * self->stats[i+1].bps / maxBPS);
    }
  }

  fl_font(FL_HELVETICA, 10);

  fl_color(FL_GREEN);
  snprintf(buffer, sizeof(buffer), "%u upd/s", self->stats[statsCount-1].ups);
  fl_draw(buffer, 5, statsHeight - 5);

  fl_color(FL_YELLOW);
  fl_draw(core::siPrefix(self->stats[statsCount-1].pps, "pix/s").c_str(),
          5 + (statsWidth-10)/3, statsHeight - 5);

  fl_color(FL_RED);
  fl_draw(core::siPrefix(self->stats[statsCount-1].bps * 8, "bps").c_str(),
          5 + (statsWidth-10)*2/3, statsHeight - 5);

  image = surface->image();
  Fl_Surface_Device::pop_current();
  delete surface;

  delete self->statsGraph;
  self->statsGraph = new Surface(image);
  delete image;

  self->damage(FL_DAMAGE_CHILD, self->w() - statsWidth - 32,
               self->h() - statsHeight - 32,
               statsWidth + 4, statsHeight + 4);

  Fl::repeat_timeout(0.5, handleStatsTimeout, data);
}
