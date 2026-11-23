/* Copyright (C) 2026 TigerVNC Team
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

#include "RdpPixelBuffer.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <unistd.h>
#include <sys/eventfd.h>

namespace rdpvnc {

namespace {

int roundUpStride(int width, int minStridePixels)
{
  int target = (width > minStridePixels) ? width : minStridePixels;
  int align = AlignedBuffer::kPixelAlignment;
  return ((target + align - 1) / align) * align;
}

} // namespace

AlignedBuffer::AlignedBuffer(uint8_t* rawPtr, uint8_t* alignedPtr,
                             int width, int height, int stridePixels,
                             size_t byteSize, uint64_t generation)
  : raw_(rawPtr), data_(alignedPtr), width_(width), height_(height),
    stridePixels_(stridePixels), strideBytes_(stridePixels * 4),
    byteSize_(byteSize), generation_(generation)
{
}

AlignedBuffer::~AlignedBuffer()
{
  std::free(raw_);
}

std::shared_ptr<AlignedBuffer> AlignedBuffer::create(int width, int height,
                                                     uint64_t generation,
                                                     int minStridePixels)
{
  if (width <= 0 || height <= 0 ||
      width > kMaxDimension || height > kMaxDimension) {
    throw std::invalid_argument("Invalid framebuffer dimensions");
  }

  int stridePixels = roundUpStride(width, minStridePixels);
  if (stridePixels <= 0 || stridePixels > kMaxDimension) {
    throw std::invalid_argument("Invalid framebuffer stride");
  }

  size_t strideBytes = static_cast<size_t>(stridePixels) * 4U;
  size_t byteSize = strideBytes * static_cast<size_t>(height);

  // Over-allocate by 63 bytes to guarantee 64-byte cache-line alignment.
  void* raw = std::calloc(1, byteSize + 63);
  if (!raw) {
    throw std::bad_alloc();
  }

  uintptr_t addr = reinterpret_cast<uintptr_t>(raw);
  uintptr_t alignedAddr = (addr + 63U) & ~static_cast<uintptr_t>(63U);

  return std::shared_ptr<AlignedBuffer>(new AlignedBuffer(
      static_cast<uint8_t*>(raw), reinterpret_cast<uint8_t*>(alignedAddr),
      width, height, stridePixels, byteSize, generation));
}

const rfb::PixelFormat& RdpPixelBuffer::bgrx32PixelFormat()
{
  // FreeRDP PIXEL_FORMAT_BGRX32 in memory is [B, G, R, X] -> LE 0x00RRGGBB.
  static const rfb::PixelFormat pf(32, 24, false, true,
                                   255, 255, 255, 16, 8, 0);
  return pf;
}

RdpPixelBuffer::RdpPixelBuffer()
  : rfb::FullFramePixelBuffer()
{
  format = bgrx32PixelFormat();
}

RdpPixelBuffer::RdpPixelBuffer(int initialWidth, int initialHeight)
  : rfb::FullFramePixelBuffer()
{
  format = bgrx32PixelFormat();
  rebind(AlignedBuffer::create(initialWidth, initialHeight, 0));
}

RdpPixelBuffer::~RdpPixelBuffer() = default;

void RdpPixelBuffer::rebind(std::shared_ptr<AlignedBuffer> buf)
{
  if (!buf) {
    detach();
    return;
  }
  backing_ = std::move(buf);
  setBuffer(backing_->width(), backing_->height(),
            backing_->data(), backing_->stridePixels());
}

void RdpPixelBuffer::detach()
{
  setBuffer(0, 0, nullptr, 0);
  backing_.reset();
}

RdpEventQueue::RdpEventQueue()
  : eventfd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
    signaled_(false)
{
  if (eventfd_ < 0) {
    throw std::runtime_error(std::string("eventfd() failed: ") +
                             std::strerror(errno));
  }
}

RdpEventQueue::~RdpEventQueue()
{
  if (eventfd_ >= 0) {
    ::close(eventfd_);
    eventfd_ = -1;
  }
}

void RdpEventQueue::push(RdpEvent&& ev)
{
  bool needSignal = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ev.type == RdpEvent::Type::FrameUpdate &&
        !queue_.empty() &&
        queue_.back().type == RdpEvent::Type::FrameUpdate &&
        queue_.back().fbGeneration == ev.fbGeneration) {
      queue_.back().dirty.assign_union(ev.dirty);
    } else {
      queue_.push_back(std::move(ev));
    }
    if (!signaled_) {
      signaled_ = true;
      needSignal = true;
    }
  }

  if (needSignal && eventfd_ >= 0) {
    uint64_t one = 1;
    ssize_t n;
    do {
      n = ::write(eventfd_, &one, sizeof(one));
    } while (n < 0 && errno == EINTR);
  }
}

void RdpEventQueue::drain(std::vector<RdpEvent>& out)
{
  out.clear();
  if (eventfd_ >= 0) {
    uint64_t val = 0;
    ssize_t n;
    do {
      n = ::read(eventfd_, &val, sizeof(val));
    } while (n < 0 && errno == EINTR);
  }

  std::lock_guard<std::mutex> lock(mutex_);
  signaled_ = false;
  out.reserve(queue_.size());
  while (!queue_.empty()) {
    out.push_back(std::move(queue_.front()));
    queue_.pop_front();
  }
}

void RdpEventQueue::clear()
{
  if (eventfd_ >= 0) {
    uint64_t val = 0;
    ssize_t n;
    do {
      n = ::read(eventfd_, &val, sizeof(val));
    } while (n < 0 && errno == EINTR);
  }

  std::lock_guard<std::mutex> lock(mutex_);
  signaled_ = false;
  queue_.clear();
}

size_t RdpEventQueue::approximateSize() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

} // namespace rdpvnc
