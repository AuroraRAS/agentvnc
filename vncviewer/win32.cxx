/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright 2011 Pierre Ossman <ossman@cendio.se> for Cendio AB
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

#include <assert.h>
#include <windows.h>

#include <FL/Fl_Window.H>
#include <FL/platform.H>

#include "Scale.h"
#include "win32.h"

static HANDLE thread;
static DWORD thread_id;

static HHOOK kbd_hook = nullptr;
static HHOOK msg_hook = nullptr;
static BYTE kbd_state[256];
static HWND target_wnd = nullptr;

static LRESULT CALLBACK keyboard_hook(int nCode, WPARAM wParam, LPARAM lParam)
{
  if (nCode >= 0) {
    KBDLLHOOKSTRUCT* msgInfo = (KBDLLHOOKSTRUCT*)lParam;

    BYTE vkey;
    BYTE scanCode;
    BYTE flags;

    int intercept;

    vkey = msgInfo->vkCode;
    scanCode = msgInfo->scanCode;
    flags = msgInfo->flags;

    // Windows stops updating the global keyboard state if we intercept
    // the key events. So we need to let some of them through to not
    // break things too badly.

    intercept = 1;

    // Keys that were pressed when we started intercepting things must
    // be allowed to return to their neutral state
    if (((wParam == WM_KEYUP) || (wParam == WM_SYSKEYUP)) &&
        (kbd_state[msgInfo->vkCode] & 0x80)) {
      intercept = 0;
      // This key has been handled, so intercept further events
      kbd_state[msgInfo->vkCode] &= ~0x80;
    }

    // We can't modify the global lock key state, so we have no choice
    // but to let these through
    if ((msgInfo->vkCode == VK_CAPITAL) ||
        (msgInfo->vkCode == VK_NUMLOCK) ||
        (msgInfo->vkCode == VK_SCROLL)) {
      intercept = 0;
    }

    if (intercept) {
      PostMessage(target_wnd, wParam, vkey,
                  scanCode << 16 | flags << 24);
      return 1;
    }
  }

  return CallNextHookEx(kbd_hook, nCode, wParam, lParam);
}

static LRESULT CALLBACK message_hook(int nCode, WPARAM wParam, LPARAM lParam)
{
  if (nCode >= 0) {
    MSG* msg = (MSG*)lParam;

    if ((msg->message == WM_KEYDOWN) ||
        (msg->message == WM_SYSKEYDOWN) ||
        (msg->message == WM_KEYUP) ||
        (msg->message == WM_SYSKEYUP)) {
      int down;
      BYTE state[256];

      // Windows stops updating our local keyboard state when we do a
      // low-level intercept of events, so we need to do that manually

      GetKeyboardState(state);

      // First the key itself
      down = !(msg->lParam & (1<<31));
      if (down)
        state[msg->wParam] |= 0x80;
      else
        state[msg->wParam] &= ~0x80;

      // Then the combined fake vkeys for the modifiers

      if ((state[VK_LSHIFT] & 0x80) || (state[VK_RSHIFT] & 0x80))
        state[VK_SHIFT] |= 0x80;
      else
        state[VK_SHIFT] &= ~0x80;

      if ((state[VK_LCONTROL] & 0x80) || (state[VK_RCONTROL] & 0x80))
        state[VK_CONTROL] |= 0x80;
      else
        state[VK_CONTROL] &= ~0x80;

      if ((state[VK_LMENU] & 0x80) || (state[VK_RMENU] & 0x80))
        state[VK_MENU] |= 0x80;
      else
        state[VK_MENU] &= ~0x80;

      SetKeyboardState(state);

      // We get the low level vkeys here, but the application code
      // expects this to have been translated to the generic ones
      switch (msg->wParam) {
      case VK_LSHIFT:
      case VK_RSHIFT:
        msg->wParam = VK_SHIFT;
        // The extended bit is also always missing for right shift
        msg->lParam &= ~(1 << 24);
        break;
      case VK_LCONTROL:
      case VK_RCONTROL:
        msg->wParam = VK_CONTROL;
        break;
      case VK_LMENU:
      case VK_RMENU:
        msg->wParam = VK_MENU;
        break;
      }
    }
  }

  return CallNextHookEx(msg_hook, nCode, wParam, lParam);
}

static DWORD WINAPI keyboard_thread(LPVOID data)
{
  MSG msg;

  target_wnd = (HWND)data;

  // Make sure a message queue is created
  PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE | PM_NOYIELD);

  // We need to know which keys are currently pressed
  GetKeyboardState(kbd_state);

  kbd_hook = SetWindowsHookEx(WH_KEYBOARD_LL, keyboard_hook, GetModuleHandle(nullptr), 0);

  // If something goes wrong then there is not much we can do.
  // Just sit around and wait for WM_QUIT...

  while (GetMessage(&msg, NULL, 0, 0));

  if (kbd_hook)
    UnhookWindowsHookEx(kbd_hook);

  target_wnd = nullptr;

  return 0;
}

int win32_enable_lowlevel_keyboard(Fl_Window* win)
{
  HWND hwnd;

  hwnd = fl_xid(win);

  // Only one target at a time for now
  if (thread != NULL) {
    if (hwnd == target_wnd)
      return 0;

    return 1;
  }

  // We create a separate thread as it is crucial that hooks are processed
  // in a timely manner.
  thread = CreateThread(NULL, 0, keyboard_thread, hwnd, 0, &thread_id);
  if (thread == NULL)
    return 1;

  msg_hook = SetWindowsHookEx(WH_GETMESSAGE, message_hook, nullptr, GetCurrentThreadId());
  if (msg_hook == NULL) {
    win32_disable_lowlevel_keyboard(win);
    return 1;
  }

  return 0;
}

void win32_disable_lowlevel_keyboard(Fl_Window* win)
{
  if (fl_xid(win) != target_wnd)
    return;

  PostThreadMessage(thread_id, WM_QUIT, 0, 0);

  CloseHandle(thread);
  thread = NULL;

  if (msg_hook)
    UnhookWindowsHookEx(msg_hook);
  msg_hook = NULL;
}

void win32_win_maximize(Fl_Window* win)
{
  // We cannot use ShowWindow() in full screen mode as it will
  // resize things implicitly. Fortunately modifying the style
  // directly results in a maximized state once we leave full screen.
  if (win->fullscreen_active()) {
    WINDOWINFO wi;
    wi.cbSize = sizeof(WINDOWINFO);
    GetWindowInfo(fl_xid(win), &wi);
    SetWindowLongPtr(fl_xid(win), GWL_STYLE, wi.dwStyle | WS_MAXIMIZE);
  } else
    ShowWindow(fl_xid(win), SW_MAXIMIZE);
}

bool win32_win_is_maximized(Fl_Window* win)
{
  WINDOWPLACEMENT wndpl;
  memset(&wndpl, 0, sizeof(WINDOWPLACEMENT));
  wndpl.length = sizeof(WINDOWPLACEMENT);
  GetWindowPlacement(fl_xid(win), &wndpl);
  if (wndpl.showCmd == SW_SHOWMAXIMIZED)
    return true;
  return false;
}

bool win32_win_get_physical_size(Fl_Window* win, int* w, int* h)
{
  RECT r;

  assert(win && win->shown());
  assert(w);
  assert(h);

  if (!GetClientRect(fl_xid(win), &r))
    return false;
  *w = r.right - r.left;
  *h = r.bottom - r.top;
  return true;
}

bool win32_win_get_coords(Fl_Window* win, int* x, int* y, int* w, int* h)
{
  POINT pt = {0, 0};

  assert(win && win->shown());
  assert(x);
  assert(y);
  assert(w);
  assert(h);

  if (!win32_win_get_physical_size(win, w, h) ||
      !ClientToScreen(fl_xid(win), &pt))
    return false;

  *x = pt.x;
  *y = pt.y;
  return true;
}

struct MonitorMatch {
  core::Rect logicalRect;
  float scale;
  core::Rect physicalRect;
};

static BOOL CALLBACK matchMonitorRect(HMONITOR, HDC, LPRECT bounds, LPARAM data)
{
  MonitorMatch* match = reinterpret_cast<MonitorMatch*>(data);
  core::Rect rect(bounds->left, bounds->top, bounds->right, bounds->bottom);
  if (!screenRectMatchesPhysical(match->logicalRect, match->scale, rect))
    return TRUE;

  match->physicalRect = rect;
  return FALSE;
}

core::Rect win32_screen_rect(const core::Rect& logicalRect, float scale)
{
  MonitorMatch match = {logicalRect, scale, scaleScreenRectToPhysical(logicalRect, scale)};
  EnumDisplayMonitors(nullptr, nullptr, matchMonitorRect,
                      reinterpret_cast<LPARAM>(&match));
  return match.physicalRect;
}

void win32_win_warp_pointer(Fl_Window* win, int x, int y)
{
  POINT pt = {x, y};

  if (win && win->shown() && ClientToScreen(fl_xid(win), &pt))
    SetCursorPos(pt.x, pt.y);
}

bool win32_event_physical_pos(Fl_Window* win, int* x, int* y)
{
  assert(x);
  assert(y);

  if (!win || !win->shown())
    return false;

  if (fl_msg.hwnd != fl_xid(win))
    return false;

  switch (fl_msg.message) {
  case WM_MOUSEMOVE:
  case WM_LBUTTONDOWN:
  case WM_LBUTTONUP:
  case WM_RBUTTONDOWN:
  case WM_RBUTTONUP:
  case WM_MBUTTONDOWN:
  case WM_MBUTTONUP:
  case WM_XBUTTONDOWN:
  case WM_XBUTTONUP:
    *x = (int)(short)LOWORD(fl_msg.lParam);
    *y = (int)(short)HIWORD(fl_msg.lParam);
    return true;
  case WM_MOUSEWHEEL:
#ifdef WM_MOUSEHWHEEL
  case WM_MOUSEHWHEEL:
#endif
  {
    POINT pt;
    pt.x = (int)(short)LOWORD(fl_msg.lParam);
    pt.y = (int)(short)HIWORD(fl_msg.lParam);
    if (!ScreenToClient(fl_xid(win), &pt))
      return false;
    *x = pt.x;
    *y = pt.y;
    return true;
  }
  }

  return false;
}
