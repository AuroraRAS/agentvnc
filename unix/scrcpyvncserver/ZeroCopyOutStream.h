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

#ifndef __SCRCPYVNCSERVER_ZEROCOPYOUTSTREAM_H__
#define __SCRCPYVNCSERVER_ZEROCOPYOUTSTREAM_H__

#include <stddef.h>
#include <stdint.h>
#include <sys/uio.h>

#include <vector>

#include <rdr/OutStream.h>

class ZeroCopyOutStream : public rdr::OutStream {
public:
  explicit ZeroCopyOutStream(int fd);
  ~ZeroCopyOutStream() override;

  int getFd() const { return fd_; }

  size_t length() override;
  void flush() override;
  void cork(bool enable) override;

  bool hasBufferedData() const;

  // Sends a frame with scatter-gather I/O after any queued bytes. A partial
  // write queues the unsent tail. Returns false if no frame bytes were sent
  // due to backpressure, so the caller can drop it and resync at an IDR.
  bool writevFrame(const struct iovec* iov, int iovcnt);

private:
  void overrun(size_t needed) override;
  void moveStagedToPending();
  void drainPending();

  ZeroCopyOutStream(const ZeroCopyOutStream&) = delete;
  ZeroCopyOutStream& operator=(const ZeroCopyOutStream&) = delete;

  int fd_;
  std::vector<uint8_t> stage_;
  std::vector<uint8_t> pending_;
  size_t pendingOffset_;
  size_t bytesSent_;
};

#endif // __SCRCPYVNCSERVER_ZEROCOPYOUTSTREAM_H__
