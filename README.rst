About AgentVNC
==============

AgentVNC is a derivative of `TigerVNC`_ and is developed as a feature-enhanced
superset of TigerVNC that tracks upstream updates over the long term.

Virtual Network Computing (VNC) is a remote display system which allows you to
view and interact with a virtual desktop environment that is running on another
computer on the network.  Using VNC, you can run graphical applications on a
remote machine and send only the display from these applications to your local
machine.  VNC is platform-independent and supports a wide variety of operating
systems and architectures as both servers and clients.

AgentVNC builds on TigerVNC, a high-speed version of VNC based on the RealVNC 4
and X.org code bases.  TigerVNC started as a next-generation development effort
for TightVNC on Unix and Linux platforms, splitting from its parent project in
early 2009 so that TightVNC could focus on Windows platforms.  Like upstream
TigerVNC, AgentVNC supports a variant of Tight encoding that is greatly
accelerated by the use of the libjpeg-turbo JPEG codec.

.. _TigerVNC: https://github.com/TigerVNC/tigervnc


Development Model
=================

AgentVNC tracks the `official TigerVNC repository`_ over the long term so that
upstream improvements and fixes are continuously incorporated.

To keep enhancements cleanly decoupled from both upstream code and one another,
development is organized into multiple modular "draft" branches.  During
upstream updates, these draft branches are rebased against the upstream
TigerVNC ``master`` branch, ensuring that no fixes from upstream are missed.

.. _official TigerVNC repository: https://github.com/TigerVNC/tigervnc


AgentVNC Features and Usage
===========================

Compared to upstream TigerVNC, AgentVNC adds three main feature sets to
``vncviewer`` (plus supporting Python tools in ``tools/``):

1. An external AI agent IPC bridge with headless mode, a live action overlay,
   and human-agent collaborative takeover.
2. Integer-factor HiDPI screen scaling (``1x`` to ``8x``) with crisp
   nearest-neighbor rendering.
3. An embedded FTP server bridge that exposes RFB file transfer extensions
   (TightVNC 2.x, TightVNC 1.3.x, and UltraVNC) over standard FTP.


Agent IPC Bridge, Headless Mode, and Collaborative Control
----------------------------------------------------------

On Unix platforms (Linux and macOS), ``vncviewer`` can expose a local Unix
domain socket IPC bridge for external automation and AI desktop agents, run
entirely without a display server in headless mode, render a real-time action
overlay, and let a human operator take over or send notes at any time.

Agent Viewer Options
~~~~~~~~~~~~~~~~~~~~

* ``-AgentSocket <path>`` - Start the local Unix domain socket IPC bridge at
  ``<path>`` (created with mode ``0600``).  Unix platforms only.
* ``-Headless`` - Run ``vncviewer`` without opening a GUI window or requiring
  X11/Wayland, driving RFB I/O, ``-AgentSocket``, and ``-FtpServer`` from a
  single ``poll()`` event loop.  Unix platforms only.
* ``-AgentOverlay`` - Draw a live HUD on the viewer showing the agent's status
  and intent, click reticles with pulse feedback, and drag vectors.  Overlay
  coordinates automatically scale with ``-ScaleFactor``.
* ``-AgentCollaborate`` - Enable collaborative mode (implies ``-AgentOverlay``)
  where local mouse or keyboard input automatically pauses agent input for
  immediate human takeover.

Interactive Controls (Windowed Mode)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

When ``vncviewer`` is running with ``-AgentSocket`` in a GUI window, the
operator can use the viewer shortcut modifiers (configured via
``-ShortcutModifiers``) or the right-click context menu:

* ``<Shortcut>+P`` or **Pause agent input** - Pause or resume agent input.
* ``<Shortcut>+N`` or **Send note to agent...** - Queue a text note that the
  agent retrieves via ``POLL_USER_MESSAGES``.
* **Show agent overlay** / **Collaborative takeover** - Toggle the live overlay
  and automatic human takeover from the context menu.

Example viewer invocations::

  # Headless viewer with IPC bridge and embedded FTP bridge (auto port)
  vncviewer -Headless -AgentSocket /tmp/agentvnc.sock -FtpServer -FtpPort 0 \
      localhost:5900

  # Windowed collaborative session with live overlay and human takeover
  vncviewer -AgentSocket /tmp/agentvnc.sock -AgentCollaborate localhost:5900

IPC Protocol and Test Client
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Messages on ``-AgentSocket`` use length-prefixed binary frames::

  [4-byte big-endian header_len][4-byte big-endian payload_len]
  [UTF-8 JSON header][optional binary payload]

Supported JSON ``cmd`` values are: ``PING``, ``GET_STATUS``, ``GET_FRAME``
(returns raw ``RGB24`` or ``RGBA32`` pixel buffers in the binary payload),
``REQUEST_REFRESH``, ``POINTER_EVENT``, ``KEY_EVENT``, ``TEXT_TYPE``,
``CLIPBOARD_GET``, ``CLIPBOARD_SET``, ``PAUSE_AGENT``, ``RESUME_AGENT``,
``AGENT_OVERLAY``, and ``POLL_USER_MESSAGES``.  When the embedded FTP bridge is
enabled, ``GET_STATUS`` also reports ``ftp_enabled``, ``ftp_running``,
``ftp_url``, ``ftp_supported``, ``ftp_ready``, and ``ftp_protocol``.

To verify a running ``-AgentSocket`` session, run the bundled Python test
client (uses only the Python standard library)::

  python3 tools/test_agent_bridge.py --socket /tmp/agentvnc.sock

Running the Gemini Computer Use Agent
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``tools/vnc_computer_use_agent.py`` implements a Gemini Computer Use desktop
agent over ``-AgentSocket`` using only the Python standard library.  Provide an
API key via the ``GEMINI_API_KEY`` environment variable or ``--api-key-file``
(default: ``./gemini.key``)::

  # Spawn a headless viewer automatically and run a task
  GEMINI_API_KEY=... python3 tools/vnc_computer_use_agent.py \
      --vnc-host localhost:5900 \
      --prompt "Create hello.txt on the desktop"

  # Launch a visible window with live overlay and interactive collaboration
  GEMINI_API_KEY=... python3 tools/vnc_computer_use_agent.py \
      --vnc-host localhost:5900 \
      --collaborate \
      --prompt "Inspect the desktop and wait for follow-up instructions"

  # Attach to an already running vncviewer -AgentSocket instance
  GEMINI_API_KEY=... python3 tools/vnc_computer_use_agent.py \
      --socket /tmp/agentvnc.sock \
      --prompt "Summarize the files in the home directory"

Key agent flags:

* ``--windowed`` - Launch ``vncviewer`` with a visible window and
  ``-AgentOverlay`` instead of ``-Headless``.
* ``--collaborate`` - Launch with ``-AgentOverlay -AgentCollaborate`` and wait
  for follow-up user instructions after task completion (pass ``--one-shot`` to
  exit after the initial task).
* ``--socket <path>`` - Connect to an existing ``-AgentSocket`` instead of
  spawning ``vncviewer`` (FTP bridge URL is auto-discovered from
  ``GET_STATUS``).
* ``--model <name>``, ``--max-turns <n>``, ``--thinking-level <level>``,
  ``--recovery-thinking-level <level>`` - Configure model behavior and adaptive
  error recovery.
* ``--ftp`` / ``--no-ftp`` / ``--ftp-url <url>`` - Control direct remote file
  I/O tools (``file_list``, ``file_stat``, ``file_read``, ``file_write``,
  ``file_mkdir``, ``file_delete``, ``file_rename``) backed by ``-FtpServer``.
  Pass ``--ftp`` to require FTP file tools to be available, or ``--no-ftp`` to
  disable them.
* ``--no-custom-tools`` - Disable deterministic helper tools
  (``clipboard_get``, ``clipboard_set``, ``paste_text_at``, ``click_and_type``,
  ``get_desktop_status``, ``yield_to_user``).
* ``--vncviewer-bin <path>`` - Path to the built ``vncviewer`` binary
  (default: ``build/vncviewer/vncviewer``).

Run ``python3 tools/test_agent_file_io.py`` for offline unit tests of the agent
file I/O tools (or pass ``--live --vnc-host localhost:5901`` for a live VNC
integration test).


Integer-Factor HiDPI Scaling
----------------------------

``vncviewer`` supports integer display scaling from ``1x`` to ``8x`` across all
platforms using crisp nearest-neighbor pixel duplication, avoiding blurry
fractional scaling on HiDPI displays.

* ``-ScaleFactor <1..8>`` - Set the integer scaling factor (default: ``1``).
  Pointer coordinates are mapped back to unscaled remote framebuffer
  coordinates, remote cursors are scaled automatically, and when
  ``-RemoteResize`` is active the requested remote desktop dimensions are
  computed by dividing the viewer window size by the scaling factor.  An
  explicit ``-DesktopSize`` continues to specify unscaled remote framebuffer
  dimensions.
* **GUI configuration**: Open **Options -> Display -> Scaling** and choose a
  **Scale factor** from ``1x`` to ``8x``.  The selection is saved in the viewer
  configuration and updates the active window immediately.

Example::

  vncviewer -ScaleFactor 2 localhost:5900


Embedded FTP Server Bridge (RFB File Transfer)
----------------------------------------------

On Unix platforms (Linux and macOS), ``vncviewer`` includes an embedded local
FTP server that bridges standard FTP commands (including ``LIST``/``MLSD``,
``RETR``, ``STOR``/``APPE``, ``REST``, ``SIZE``/``MDTM``, ``MKD``, ``RMD``,
``DELE``, ``RNFR``/``RNTO``, and ``PASV``/``EPSV``/``PORT``/``EPRT``) onto the
remote VNC server's RFB file transfer extension.  It supports TightVNC 2.x
(``Tight2``), TightVNC 1.3.x (``Tight13``), and UltraVNC (``Ultra``) file
transfer protocols and works in both GUI and ``-Headless`` modes.

FTP Bridge Viewer Options
~~~~~~~~~~~~~~~~~~~~~~~~~

* ``-FtpServer`` - Enable the embedded FTP bridge (default: off).  Enable this
  before connecting when the VNC server requires ``Tight`` capability
  negotiation.
* ``-FtpBindAddress <address>`` - Local address to bind to (default:
  ``127.0.0.1``).
* ``-FtpPort <port>`` - Local TCP listen port; ``0`` selects an ephemeral port
  automatically (default: ``2121``).
* ``-FtpUsername <username>`` / ``-FtpPassword <password>`` - Optional login
  credentials required by the local FTP bridge (by default any login is
  accepted).
* ``-FtpProtocol <Auto|Tight2|Tight13|Ultra>`` - Preferred RFB file transfer
  protocol (default: ``Auto``).

GUI and CLI Usage
~~~~~~~~~~~~~~~~~

In windowed mode, you can toggle the bridge from the context menu (**FTP
server**), configure persistent defaults under **Options -> Misc -> Enable FTP
server bridge**, and check the active listening URL in **Connection info...**.

Example::

  # Start vncviewer with the embedded FTP bridge on localhost:2121
  vncviewer -FtpServer -FtpPort 2121 localhost:5900

  # Transfer files using any standard FTP client
  curl ftp://127.0.0.1:2121/
  curl -T local.txt ftp://127.0.0.1:2121/remote.txt


Legal
=====

Incomplete and generally out of date copyright list::

        Copyright (C) 1999 AT&T Laboratories Cambridge
        Copyright (C) 2002-2005 RealVNC Ltd.
        Copyright (C) 2000-2006 TightVNC Group
        Copyright (C) 2005-2006 Martin Koegler
        Copyright (C) 2005-2006 Sun Microsystems, Inc.
        Copyright (C) 2006 OCCAM Financial Technology
        Copyright (C) 2000-2008 Constantin Kaplinsky
        Copyright (C) 2004-2017 Peter Astrand for Cendio AB
        Copyright (C) 2010 Antoine Martin
        Copyright (C) 2010 m-privacy GmbH
        Copyright (C) 2009-2011 D. R. Commander
        Copyright (C) 2009-2011 Pierre Ossman for Cendio AB
        Copyright (C) 2004, 2009-2011 Red Hat, Inc.
        Copyright (C) 2009-2026 TigerVNC team
        All Rights Reserved.

This software is distributed under the GNU General Public Licence as published
by the Free Software Foundation.  See the file LICENCE.TXT for the conditions
under which this software is made available.  AgentVNC is derived from TigerVNC
and also contains code from other sources.  See the Acknowledgements section
below, and the individual source files, for details of the conditions under
which they are made available.


All Platforms
=============

All versions of AgentVNC contain the following programs:

* vncviewer - the cross-platform VNC viewer (derived from TigerVNC Viewer),
              written using FLTK.  vncviewer connects to a VNC server and
              allows you to interact with the remote desktop being displayed by
              the VNC server.  The VNC server can be running on a Windows or a
              Unix/Linux machine.


Windows-specific
================

The Windows version of AgentVNC contains the following programs:

* winvnc - the VNC server for Windows.  winvnc allows a Windows desktop to be
           accessed remotely using a VNC viewer.

WARNING: winvnc is currently unmaintained and may not function correctly.

winvnc may not work if the Fast user switching or Remote desktop features are
in use.


Unix/Linux-specific (not Mac)
=============================

The Unix/Linux version of AgentVNC contains the following programs:

* Xvnc - the VNC server for Unix.  Xvnc is both a VNC server and an X server
         with a "virtual" framebuffer.  You should normally use the vncserver
         service to start Xvnc.

* vncpasswd - a program which allows you to change the VNC password used to
              access your VNC server sessions (assuming that VNC authentication
              is being used.) This command must be run to set a password before
              using VNC authentication with any of the servers or services.

* vncconfig - a program which is used to configure and control a running
              instance of Xvnc.

* x0vncserver - an inefficient VNC server which continuously polls any X
                display, allowing it to be controlled via VNC.  It is intended
                mainly as a demonstration of a simple VNC server.

* w0vncserver - a VNC server for Wayland compositors which gives control
                over your local Wayland session.

It also contains the following systemd service:

* vncserver@.service - a service to start a user session with Xvnc and one of
                       the desktop environments available on the system.

ACKNOWLEDGEMENTS
================

This distribution contains public domain DES software by Richard Outerbridge.
This is:

    Copyright (c) 1988,1989,1990,1991,1992 by Richard Outerbridge.
    (GEnie : OUTER; CIS : [71755,204]) Graven Imagery, 1992.


This distribution contains software from the X Window System.  This is:

 Copyright 1987, 1988, 1998  The Open Group
 
 Permission to use, copy, modify, distribute, and sell this software and its
 documentation for any purpose is hereby granted without fee, provided that
 the above copyright notice appear in all copies and that both that
 copyright notice and this permission notice appear in supporting
 documentation.
 
 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.
 
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 OPEN GROUP BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
 AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 
 Except as contained in this notice, the name of The Open Group shall not be
 used in advertising or otherwise to promote the sale, use or other dealings
 in this Software without prior written authorization from The Open Group.
 
 
 Copyright 1987, 1988 by Digital Equipment Corporation, Maynard, Massachusetts.
 
                         All Rights Reserved
 
 Permission to use, copy, modify, and distribute this software and its 
 documentation for any purpose and without fee is hereby granted, 
 provided that the above copyright notice appear in all copies and that
 both that copyright notice and this permission notice appear in 
 supporting documentation, and that the name of Digital not be
 used in advertising or publicity pertaining to distribution of the
 software without specific, written prior permission.  
 
 DIGITAL DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING
 ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL
 DIGITAL BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR
 ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
 ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 SOFTWARE.
