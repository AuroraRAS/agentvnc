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

#include <core/i18n.h>

#include "parameters.h"

core::IntParameter
  rfbport("rfbport",
          _("TCP port to listen for RFB protocol"), 5900, -1, 65535);
core::StringParameter
  rfbunixpath("rfbunixpath",
              _("UNIX socket path to listen for RFB protocol"), "");
core::IntParameter
  rfbunixmode("rfbunixmode",
              _("UNIX socket access mode"), 0600, 0000, 0777);
core::BoolParameter
  localhostOnly("localhost",
                _("Only allow connections from localhost"), false);
core::StringParameter
  desktopName("desktop",
              _("Name of VNC desktop (empty uses Android device name)"), "");
core::StringParameter
  interface("interface",
            _("Listen on the specified network address"), "all");
core::StringParameter
  hostsFile("HostsFile",
            _("File with IP access control rules"), "");

core::StringParameter
  adbHost("AdbHost",
          _("ADB server host"), "127.0.0.1");
core::IntParameter
  adbPort("AdbPort",
          _("ADB server TCP port"), 5037, 1, 65535);
core::StringParameter
  serial("Serial",
         _("Android device serial (empty auto-selects single device)"), "");
core::BoolParameter
  attachMode("Attach",
             _("Attach directly to an already-running scrcpy-server socket"),
             false);
core::StringParameter
  scrcpyServer("ScrcpyServer",
               _("Path to scrcpy-server jar on the host"), "");
core::StringParameter
  scrcpyVersion("ScrcpyVersion",
                _("scrcpy-server version (auto-detected when launching if empty)"), "");
core::StringParameter
  scrcpySocket("ScrcpySocket",
               _("Android abstract socket name in attach mode"), "scrcpy");
core::IntParameter
  scid("Scid",
       _("scrcpy connection ID (-1 uses default socket name)"),
       -1, -1, 0x7fffffff);
core::IntParameter
  bitRate("BitRate",
          _("Target H.264 video bit rate in bits/s"),
          8000000, 100000, 100000000);
core::IntParameter
  maxSize("MaxSize",
          _("Maximum video width or height (0 for native resolution)"),
          0, 0, 16384);
core::IntParameter
  maxFps("MaxFps",
         _("Maximum video capture frame rate (0 for unlimited)"),
         0, 0, 240);
core::IntParameter
  iFrameInterval("IFrameInterval",
                 _("H.264 IDR keyframe interval in seconds (0 for encoder default)"),
                 2, 0, 60);
core::StringParameter
  videoEncoder("VideoEncoder",
               _("Explicit Android MediaCodec H.264 encoder name"), "");
core::BoolParameter
  controlEnabled("Control",
                 _("Enable scrcpy control channel for input and clipboard"),
                 true);
core::BoolParameter
  stayAwake("StayAwake",
            _("Keep the Android device awake while connected"), false);
core::BoolParameter
  sendDummyByte("SendDummyByte",
                _("Expect initial scrcpy dummy byte on video socket"), true);
core::BoolParameter
  sendDeviceMeta("SendDeviceMeta",
                 _("Expect 64-byte device name header on video socket"), true);
core::BoolParameter
  fileTransfer("FileTransfer",
               _("Enable TightVNC file transfer over ADB"), true);
core::StringParameter
  fileTransferRoot("FileTransferRoot",
                   _("Optional Android root directory for TightVNC file transfer"),
                   "");
