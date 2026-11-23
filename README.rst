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

Compared to upstream TigerVNC, AgentVNC adds five main feature sets across
``vncviewer``, its supporting Python tools in ``tools/``, and the
``scrcpyvncserver`` and ``rdpvncserver`` servers.

1. An external AI agent IPC bridge with headless mode, a live action overlay,
   and human-agent collaborative takeover.
2. Automatic HiDPI scaling for the viewer interface and independent integer
   scaling (``1x`` to ``8x``) for the remote desktop.
3. An embedded FTP server bridge that exposes RFB file transfer extensions
   (TightVNC 2.x, TightVNC 1.3.x, and UltraVNC) over standard FTP.
4. ``scrcpyvncserver`` - a zero-transcoding VNC server bridging Android
   ``scrcpy-server`` H.264 streams over ADB to RFB Open H.264 viewers.
5. ``rdpvncserver`` - an RDP-to-VNC bridge that exposes an RDP desktop to
   VNC viewers.


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
  points use remote framebuffer coordinates and stay aligned with the desktop
  as the local display scale or ``-ScaleFactor`` changes.
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


HiDPI and Remote Desktop Scaling
--------------------------------

The viewer interface follows the local display scale automatically, including
fractional scaling and macOS Retina displays.  On Linux, the graphical viewer
uses X11 and runs through XWayland in a Wayland session.  It updates the
interface scale when the X11 ``Xft.dpi`` resource changes.

``-ScaleFactor <1..8>`` controls how the remote desktop is drawn.  At the
default of ``1``, each remote framebuffer pixel uses one physical pixel in the
viewer, even when the viewer interface is scaled.  Higher factors enlarge
the desktop using nearest-neighbor pixel duplication.  Pointer input maps
back to remote framebuffer coordinates.

To change the factor in the viewer, open **Options -> Display -> Scaling** and
choose a **Scale factor**.  The setting updates the active session and is
saved in the viewer configuration.

When ``-RemoteResize`` is enabled and supported by the server, the viewer
requests a remote desktop size based on the window's physical client area
divided by ``-ScaleFactor``.  Use ``-DesktopSize WIDTHxHEIGHT`` to request an
explicit initial size in remote framebuffer pixels.

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


Android scrcpy VNC Server (scrcpyvncserver)
-------------------------------------------

On Unix/Linux platforms (excluding macOS), ``scrcpyvncserver`` exposes an
Android device running ``scrcpy-server`` as a VNC server using zero-transcoding
passthrough over RFB Open H.264 encoding (``50``).  Eliminating host-side
decoding, color-space conversion, and re-encoding keeps bridge-induced latency
minimal.

It connects directly over TCP to the local ADB daemon (``127.0.0.1:5037`` by
default) using ``host:transport:<serial>`` and ``localabstract:<socket>`` with
``TCP_NODELAY`` enabled, strips scrcpy's packet header, and uses
scatter-gather ``writev()`` I/O to prepend RFB ``FramebufferUpdate`` and Open
H.264 headers directly onto raw H.264 Annex-B NAL payloads.  The server caches
SPS/PPS parameters, manages decoder reset and recovery flags across new
clients, non-incremental refreshes, encoder reconfigurations, and congestion
or skipped frames, pushes updates with ``ContinuousUpdates`` (``-313``),
forwards rotation or resolution changes via ``ExtendedDesktopSize`` (``-308``)
or ``DesktopSize`` (``-223``), and relays pointer, keyboard, and clipboard
input over the scrcpy control channel.

scrcpyvncserver Options
~~~~~~~~~~~~~~~~~~~~~~~

* ``-Serial <serial>`` - Select the Android device serial (empty auto-selects
  when a single device is connected).
* ``-AdbHost <host>`` / ``-AdbPort <port>`` - ADB daemon address and TCP port
  (default: ``127.0.0.1:5037``).
* ``-ScrcpyServer <path>`` - Path to the ``scrcpy-server`` JAR on the host;
  when omitted, checks ``SCRCPY_SERVER_PATH`` and standard installation paths.
* ``-ScrcpyVersion <version>`` - Override the ``scrcpy-server`` version string
  (auto-detected when launching; in ``-Attach`` mode, specify this for
  versions older than ``3.3.1`` to select the matching control protocol).
* ``-Attach`` - Attach directly to an already-running forward-tunneled H.264
  ``scrcpy-server`` configured with audio disabled and codec/frame metadata
  enabled.  Match ``-Control``, ``-SendDummyByte``, and ``-SendDeviceMeta``,
  and select the socket with ``-ScrcpySocket`` or ``-Scid``.
* ``-MaxSize <px>``, ``-MaxFps <fps>``, ``-BitRate <bps>`` (default:
  ``8000000``), ``-IFrameInterval <sec>`` (default: ``2``), and
  ``-VideoEncoder <name>`` - Configure Android H.264 capture and encoding.
* ``-Control`` / ``-StayAwake`` - Enable the scrcpy input/clipboard control
  channel (default: on) and keep the device awake while connected
  (default: off).
* ``-rfbport <port>`` (default: ``5900``), ``-rfbunixpath <path>``,
  ``-localhost``, ``-SecurityTypes <list>`` (default: ``TLSVnc,VncAuth``),
  ``-PasswordFile <path>``, and ``-HostsFile <path>`` - Listener, security, and
  IP access filtering (``-HostsFile`` applies to TCP listeners including
  systemd-activated sockets; connections requiring interactive approval are
  rejected).

Run ``scrcpyvncserver -h`` or see ``scrcpyvncserver(1)`` for the complete
parameter list.

Viewer Requirements and Usage
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Viewers must support RFB Open H.264 encoding (``50``), plus
``ExtendedDesktopSize`` (``-308``) or ``DesktopSize`` (``-223``) for Android
display resizes and ``ContinuousUpdates`` (``-313``) for immediate frame
push delivery.  With an H.264-enabled ``vncviewer``, explicitly set
``-AutoSelect=0 -PreferredEncoding=H.264`` so it stays on Open H.264.

Example::

  # Ensure the local ADB daemon is connected to the Android target
  adb connect 192.168.240.112:5555

  # Launch scrcpyvncserver on localhost port 5900 (or use -PasswordFile)
  scrcpyvncserver -Serial 192.168.240.112:5555 \
      -ScrcpyServer /path/to/scrcpy-server -MaxSize 1920 \
      -rfbport 5900 -localhost -SecurityTypes None

  # Or attach to an already-running forward-tunneled scrcpy-server socket
  scrcpyvncserver -Attach=1 -Serial 192.168.240.112:5555 \
      -rfbport 5900 -localhost -SecurityTypes None

  # Connect with TigerVNC/AgentVNC viewer (supports -Headless / -AgentSocket)
  vncviewer -AutoSelect=0 -PreferredEncoding=H.264 -SecurityTypes=None \
      127.0.0.1::5900


RDP-to-VNC Bridge (rdpvncserver)
--------------------------------

On Linux, ``rdpvncserver`` uses FreeRDP to expose a Windows or other RDP
desktop to VNC viewers.  The first authenticated VNC client starts the RDP
session, which viewers can share.  The bridge relays keyboard and pointer
input and cursor updates.  It synchronizes clipboard text in both directions
when the RDP server provides the clipboard channel.

Standard VNC viewers receive decoded framebuffer updates.  The bridge can
also forward compatible full-frame AVC420 streams directly to viewers that
negotiate RFB Open H.264.  Dynamic resizing requires
``ExtendedDesktopSize`` on the VNC side and the MS-RDPEDISP display-control
channel on the RDP side.

Build and Connect
~~~~~~~~~~~~~~~~~

The bridge is optional and requires FreeRDP 3.2 or later in the 3.x series
and the ``freerdp3``, ``freerdp-client3``, and ``winpr3`` pkg-config modules.
Build it from the repository root.

.. code-block:: sh

  cmake -S . -B build -DBUILD_RDPVNCSERVER=ON
  cmake --build build --target rdpvncserver vncpasswd

For a local VNC listener, create a VNC password file with ``vncpasswd``.
Store the upstream RDP password on the first line of a separate file readable
only by your account.  Replace the RDP hostname and username in the example.

.. code-block:: sh

  mkdir -p "$HOME/.config/tigervnc"
  build/unix/vncpasswd/vncpasswd "$HOME/.config/tigervnc/rdp-bridge.passwd"

  build/unix/rdpvncserver/rdpvncserver \
      -RdpHost windows.example.com -RdpUser your-user \
      -RdpPasswordFile "$HOME/.config/tigervnc/rdp-password" \
      -localhost -rfbport 5901 -SecurityTypes TLSVnc \
      -PasswordFile "$HOME/.config/tigervnc/rdp-bridge.passwd"

In another terminal, connect with ``vncviewer 127.0.0.1::5901`` and enter the
VNC password.

Passwordless VNC on Loopback
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

For local testing, ``-SecurityTypes None`` disables VNC authentication and
encryption, and ``-localhost`` limits the VNC listener to loopback.  The bridge
still uses the upstream RDP credentials.  This example assumes a test RDP
service on ``localhost:3390`` and uses ``-RdpIgnoreCert`` to accept its test
certificate.

.. code-block:: sh

  build/unix/rdpvncserver/rdpvncserver \
      -RdpHost localhost -RdpPort 3390 -RdpUser your-user \
      -RdpPasswordFile "$HOME/.config/tigervnc/rdp-password" \
      -RdpIgnoreCert -localhost -rfbport 5901 -SecurityTypes None

In another terminal, open the windowed viewer.

.. code-block:: sh

  vncviewer -SecurityTypes=None localhost:1

The viewer address ``localhost:1`` selects VNC port ``5901``.  The bridge
selects the upstream port separately with ``-RdpPort``, which defaults to
``3389``.  If the viewer reports ``End of stream`` after choosing security
type ``None``, check the bridge's ``RdpSession`` log for an upstream
connection, certificate, or authentication error.

rdpvncserver Options
~~~~~~~~~~~~~~~~~~~~

* ``-RdpHost <host>`` and ``-RdpPort <port>`` select the upstream RDP server
  (defaults to ``127.0.0.1:3389``).  Use ``-RdpDomain <domain>`` when the
  RDP account requires a Windows domain.
* Supply the upstream password with ``-RdpPasswordFile <path>`` or the
  ``RDPVNCSERVER_PASSWORD`` environment variable.  ``-PasswordFile`` controls
  VNC authentication.
* ``-RdpIgnoreCert`` accepts untrusted or changed upstream TLS certificates.
  Certificate verification is enabled by default.
* ``-RdpClipboard=0``, ``-RdpDynamicResize=0``, and
  ``-RdpH264Passthrough=0`` disable their respective features.
* ``-RdpIdleDisconnectSec <sec>`` keeps the upstream session alive for a
  grace period after the last viewer leaves (default ``0``).
  ``-RdpReconnectAttempts <count>`` sets the number of retries after an
  unexpected RDP disconnect (default ``0``).

Run ``rdpvncserver --help`` or see ``rdpvncserver(1)`` for the complete
parameter list.


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

Building ``vncviewer`` requires FLTK 1.4 or a newer 1.x release.  On Unix
platforms other than macOS, FLTK must include X11 support.  See
`BUILDING.txt <BUILDING.txt>`_ for build instructions.


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

* scrcpyvncserver - a zero-transcoding VNC server that bridges Android
                    ``scrcpy-server`` H.264 streams over ADB to RFB Open H.264
                    viewers.

* rdpvncserver - an optional RDP-to-VNC bridge server for Linux that connects
                 to an upstream RDP desktop using FreeRDP.

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
