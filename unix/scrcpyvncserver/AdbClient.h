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

#ifndef __SCRCPYVNCSERVER_ADBCLIENT_H__
#define __SCRCPYVNCSERVER_ADBCLIENT_H__

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

struct AdbDevice {
  std::string serial;
  std::string state;
};

class AdbClient {
public:
  AdbClient(const std::string& host = "127.0.0.1", int port = 5037);

  int connectDaemon() const;

  std::vector<AdbDevice> listDevices() const;
  std::string resolveSerial(const std::string& requestedSerial) const;

  int connectLocalAbstract(const std::string& serial,
                           const std::string& socketName) const;

  void pushFile(const std::string& serial,
                const std::string& localPath,
                const std::string& remotePath,
                unsigned int mode = 0100644) const;

  int startShell(const std::string& serial,
                 const std::string& command) const;

  static void selectTransport(int fd, const std::string& serial);
  static void sendCommand(int fd, const std::string& cmd);
  static bool readStatus(int fd, std::string* errorMsg);
  static std::string readLengthPrefixed(int fd);

  static std::string detectVersionFromPath(const std::string& path);
  static std::string extractExpectedVersionFromError(const std::string& text);

private:
  std::string host_;
  int port_;
};

#endif // __SCRCPYVNCSERVER_ADBCLIENT_H__
