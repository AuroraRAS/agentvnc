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

struct AdbFeatures {
  bool statV2 = false;
  bool lsV2 = false;
  bool shellV2 = false;
};

struct AdbStat {
  uint32_t error = 0;
  uint32_t mode = 0;
  uint64_t size = 0;
  int64_t mtime = 0;
};

struct AdbDirEntry {
  std::string name;
  uint32_t mode = 0;
  uint64_t size = 0;
  int64_t mtime = 0;
};

struct AdbShellResult {
  int exitCode = -1;
  std::string stdoutData;
  std::string stderrData;
};

class AdbClient {
public:
  AdbClient(const std::string& host = "127.0.0.1", int port = 5037);

  int connectDaemon(int timeoutMs = 5000) const;

  std::vector<AdbDevice> listDevices() const;
  std::string resolveSerial(const std::string& requestedSerial) const;
  AdbFeatures queryFeatures(const std::string& serial) const;

  int connectLocalAbstract(const std::string& serial,
                           const std::string& socketName) const;

  void pushFile(const std::string& serial,
                const std::string& localPath,
                const std::string& remotePath,
                unsigned int mode = 0100644) const;

  int openSync(const std::string& serial, int timeoutMs = 30000) const;

  static bool syncStat(int syncFd, const std::string& remotePath,
                       bool useV2, bool followSymlinks, AdbStat* out);
  static std::vector<AdbDirEntry> syncList(int syncFd,
                                           const std::string& dirPath,
                                           bool useLsV2,
                                           bool useStatV2);

  static void syncStartRecv(int syncFd, const std::string& remotePath);
  // Returns 1 on DATA (appended to chunkOut), 0 on DONE, -1 on FAIL (errOut).
  static int syncReadRecvChunk(int syncFd,
                               std::vector<uint8_t>* chunkOut,
                               std::string* errOut);

  static void syncStartSend(int syncFd, const std::string& remotePath,
                            unsigned int mode = 0100644);
  static bool syncCheckImmediateError(int syncFd, int timeoutMs,
                                      std::string* errOut);
  static void syncWriteSendData(int syncFd, const uint8_t* data, size_t len);
  static bool syncFinishSend(int syncFd, uint32_t mtimeSec,
                             std::string* errOut);
  static void syncQuit(int syncFd);

  int startShell(const std::string& serial,
                 const std::string& command) const;
  int openShellV2(const std::string& serial,
                  const std::string& command,
                  int timeoutMs = 30000) const;
  static void shellV2WriteStdin(int fd, const uint8_t* data, size_t len);
  static void shellV2CloseStdin(int fd);
  static bool shellV2ReadPacket(int fd, uint8_t* idOut,
                                std::vector<uint8_t>* payloadOut);
  AdbShellResult runShell(const std::string& serial,
                          const std::string& command,
                          bool useShellV2,
                          int timeoutMs = 30000) const;

  static std::string quoteShellArg(const std::string& value);

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
