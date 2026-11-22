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

#include "ZeroCopyOutStream.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>

#include <core/Exception.h>

namespace {

static const size_t kInitialStageSize = 16384;
static const size_t kMaxPendingSize = 32u * 1024u * 1024u;

} // namespace

ZeroCopyOutStream::ZeroCopyOutStream(int fd)
  : fd_(fd),
    stage_(kInitialStageSize),
    pendingOffset_(0),
    bytesSent_(0)
{
  ptr = stage_.data();
  end = stage_.data() + stage_.size();
}

ZeroCopyOutStream::~ZeroCopyOutStream()
{
}

size_t ZeroCopyOutStream::length()
{
  return bytesSent_ +
         static_cast<size_t>(ptr - stage_.data()) +
         (pending_.size() - pendingOffset_);
}

bool ZeroCopyOutStream::hasBufferedData() const
{
  return (ptr != stage_.data()) || (pendingOffset_ < pending_.size());
}

void ZeroCopyOutStream::moveStagedToPending()
{
  size_t staged = static_cast<size_t>(ptr - stage_.data());
  if (staged == 0)
    return;

  if ((pending_.size() - pendingOffset_) + staged > kMaxPendingSize)
    throw std::runtime_error("Output queue exceeds maximum buffer size");

  pending_.insert(pending_.end(), stage_.data(), stage_.data() + staged);
  ptr = stage_.data();
}

void ZeroCopyOutStream::drainPending()
{
  while (pendingOffset_ < pending_.size()) {
    ssize_t n = ::send(fd_,
                       pending_.data() + pendingOffset_,
                       pending_.size() - pendingOffset_,
                       MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n > 0) {
      pendingOffset_ += static_cast<size_t>(n);
      bytesSent_ += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      break;
    throw core::socket_error("send", errno);
  }

  if (pendingOffset_ == pending_.size()) {
    pending_.clear();
    pendingOffset_ = 0;
  } else if (pendingOffset_ > 65536) {
    pending_.erase(pending_.begin(),
                   pending_.begin() + static_cast<ptrdiff_t>(pendingOffset_));
    pendingOffset_ = 0;
  }
}

void ZeroCopyOutStream::flush()
{
  size_t staged = static_cast<size_t>(ptr - stage_.data());
  if (corked && pendingOffset_ == pending_.size() && staged < 1024)
    return;

  if (staged > 0 && pendingOffset_ == pending_.size()) {
    // Fast path for small control messages when no backlog exists
    size_t off = 0;
    while (off < staged) {
      ssize_t n = ::send(fd_, stage_.data() + off, staged - off,
                         MSG_DONTWAIT | MSG_NOSIGNAL);
      if (n > 0) {
        off += static_cast<size_t>(n);
        bytesSent_ += static_cast<size_t>(n);
        continue;
      }
      if (n < 0 && errno == EINTR)
        continue;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        break;
      throw core::socket_error("send", errno);
    }
    if (off == staged) {
      ptr = stage_.data();
      return;
    }
    pending_.insert(pending_.end(),
                    stage_.data() + off,
                    stage_.data() + staged);
    ptr = stage_.data();
    return;
  }

  moveStagedToPending();
  drainPending();
}

void ZeroCopyOutStream::cork(bool enable)
{
  corked = enable;
  if (!enable)
    flush();
}

void ZeroCopyOutStream::overrun(size_t needed)
{
  bool wasCorked = corked;
  corked = false;
  moveStagedToPending();
  drainPending();
  corked = wasCorked;

  if (needed > stage_.size()) {
    if (needed > kMaxPendingSize)
      throw std::runtime_error("Requested write exceeds maximum buffer size");
    size_t newSize = stage_.size();
    while (newSize < needed)
      newSize *= 2;
    stage_.resize(newSize);
  }

  ptr = stage_.data();
  end = stage_.data() + stage_.size();
}

bool ZeroCopyOutStream::writevFrame(const struct iovec* iov, int iovcnt)
{
  if (iovcnt <= 0)
    return true;

  // Preserve strict byte-stream ordering: drain any pending control/tail bytes
  // before touching the socket with a new frame.
  moveStagedToPending();
  drainPending();

  if (pendingOffset_ < pending_.size())
    return false;

  size_t totalBytes = 0;
  for (int i = 0; i < iovcnt; ++i) {
    if (iov[i].iov_len > kMaxPendingSize - totalBytes)
      throw std::runtime_error("Frame exceeds maximum output buffer size");
    totalBytes += iov[i].iov_len;
  }

  if (totalBytes == 0)
    return true;

  struct msghdr msg = {};
  msg.msg_iov = const_cast<struct iovec*>(iov);
  msg.msg_iovlen = iovcnt;
  ssize_t written;
  do {
    written = ::sendmsg(fd_, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
  } while (written < 0 && errno == EINTR);

  if (written < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return false;
    throw core::socket_error("sendmsg", errno);
  }
  if (written == 0)
    return false;

  bytesSent_ += static_cast<size_t>(written);

  if (static_cast<size_t>(written) == totalBytes)
    return true;

  // Socket accepted a prefix of the frame; buffer only the unsent tail so
  // the RFB framing remains intact and will drain on POLLOUT.
  size_t remaining = totalBytes - static_cast<size_t>(written);
  pending_.reserve(remaining);
  size_t skip = static_cast<size_t>(written);
  for (int i = 0; i < iovcnt; ++i) {
    const uint8_t* base = static_cast<const uint8_t*>(iov[i].iov_base);
    size_t len = iov[i].iov_len;
    if (skip >= len) {
      skip -= len;
      continue;
    }
    pending_.insert(pending_.end(), base + skip, base + len);
    skip = 0;
  }

  return true;
}
