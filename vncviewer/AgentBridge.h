/* Copyright 2026 TigerVNC team
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

#ifndef __AGENTBRIDGE_H__
#define __AGENTBRIDGE_H__

#ifndef WIN32

#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class CConn;

class AgentBridge {
public:
  explicit AgentBridge(const std::string& socketPath);
  ~AgentBridge();

  void start();
  void stop();

  void setConnection(CConn* conn);
  int getWakeFd() const { return mainWakePipe_[0]; }

  void processPendingRequests();

  static void cleanupOnSignal();

private:
  struct InboundRequest {
    uint64_t clientId;
    std::string headerJson;
    std::vector<uint8_t> payload;
  };

  struct OutboundMessage {
    uint64_t clientId;
    std::string headerJson;
    std::vector<uint8_t> payload;
  };

  struct ClientState {
    uint64_t id;
    int fd;
    std::vector<uint8_t> readBuf;
    std::deque<std::vector<uint8_t>> writeQueue;
    size_t writeOffset;
    size_t queuedBytes;
    size_t pendingRequests;
  };

  void ioLoop();
  void handleRequest(const InboundRequest& req);
  void enqueueResponse(uint64_t clientId,
                       const std::string& headerJson,
                       std::vector<uint8_t>&& payload);
  void wakeIpcThread();
  void wakeMainThread();
  static void drainPipe(int fd);
  static void closePipePair(int fds[2]);

private:
  std::string socketPath_;
  int listenFd_;
  int mainWakePipe_[2];
  int ipcWakePipe_[2];

  CConn* conn_;
  bool started_;
  bool stopRequested_;
  std::thread ioThread_;

  std::mutex queueMutex_;
  std::deque<InboundRequest> reqQueue_;
  std::deque<OutboundMessage> respQueue_;
};

#endif /* !WIN32 */

#endif /* __AGENTBRIDGE_H__ */
