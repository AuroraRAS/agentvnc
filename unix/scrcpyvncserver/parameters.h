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

#ifndef __SCRCPYVNCSERVER_PARAMETERS_H__
#define __SCRCPYVNCSERVER_PARAMETERS_H__

#include <core/Configuration.h>

extern core::IntParameter rfbport;
extern core::StringParameter rfbunixpath;
extern core::IntParameter rfbunixmode;
extern core::BoolParameter localhostOnly;
extern core::StringParameter desktopName;
extern core::StringParameter interface;
extern core::StringParameter hostsFile;

extern core::StringParameter adbHost;
extern core::IntParameter adbPort;
extern core::StringParameter serial;
extern core::BoolParameter attachMode;
extern core::StringParameter scrcpyServer;
extern core::StringParameter scrcpyVersion;
extern core::StringParameter scrcpySocket;
extern core::IntParameter scid;
extern core::IntParameter bitRate;
extern core::IntParameter maxSize;
extern core::IntParameter maxFps;
extern core::IntParameter iFrameInterval;
extern core::StringParameter videoEncoder;
extern core::BoolParameter controlEnabled;
extern core::BoolParameter stayAwake;
extern core::BoolParameter sendDummyByte;
extern core::BoolParameter sendDeviceMeta;

#endif // __SCRCPYVNCSERVER_PARAMETERS_H__
