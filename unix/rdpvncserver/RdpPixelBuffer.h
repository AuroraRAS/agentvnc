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

#ifndef __RDPVNCSERVER_RDPPIXELBUFFER_H__
#define __RDPVNCSERVER_RDPPIXELBUFFER_H__

#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <core/Rect.h>
#include <core/Region.h>
#include <rfb/PixelBuffer.h>
#include <rfb/PixelFormat.h>
#include <rfb/ScreenSet.h>

namespace rdpvnc {

/**
 * Cache-line (64-byte) aligned framebuffer storage. FreeRDP and TigerVNC
 * use separate buffers. A resize event transfers the newly allocated
 * TigerVNC buffer to the main thread before the first pixel update.
 */
class AlignedBuffer {
public:
  static constexpr int kMaxDimension = 16384;
  static constexpr int kPixelAlignment = 16; // 16 pixels * 4 bytes = 64 bytes

  ~AlignedBuffer();

  AlignedBuffer(const AlignedBuffer&) = delete;
  AlignedBuffer& operator=(const AlignedBuffer&) = delete;

  static std::shared_ptr<AlignedBuffer> create(int width, int height,
                                               uint64_t generation = 0,
                                               int minStridePixels = 0);

  uint8_t* data() { return data_; }
  const uint8_t* data() const { return data_; }
  int width() const { return width_; }
  int height() const { return height_; }
  int stridePixels() const { return stridePixels_; }
  int strideBytes() const { return strideBytes_; }
  size_t byteSize() const { return byteSize_; }
  uint64_t generation() const { return generation_; }

private:
  AlignedBuffer(uint8_t* rawPtr, uint8_t* alignedPtr,
                int width, int height, int stridePixels,
                size_t byteSize, uint64_t generation);

  uint8_t* raw_;
  uint8_t* data_;
  int width_;
  int height_;
  int stridePixels_;
  int strideBytes_;
  size_t byteSize_;
  uint64_t generation_;
};

/**
 * TigerVNC framebuffer with main-thread-owned BGRX32 pixels.
 */
class RdpPixelBuffer : public rfb::FullFramePixelBuffer {
public:
  RdpPixelBuffer();
  RdpPixelBuffer(int initialWidth, int initialHeight);
  ~RdpPixelBuffer() override;

  /**
   * Returns the TigerVNC PixelFormat matching FreeRDP PIXEL_FORMAT_BGRX32
   * ([B, G, R, X] in little-endian memory).
   */
  static const rfb::PixelFormat& bgrx32PixelFormat();

  /**
   * Rebinds this pixel buffer to a new backing buffer.
   */
  void rebind(std::shared_ptr<AlignedBuffer> buf);

  /**
   * Releases the active backing buffer when no desktop session is running.
   */
  void detach();

  std::shared_ptr<AlignedBuffer> backing() const { return backing_; }
  uint64_t generation() const {
    return backing_ ? backing_->generation() : 0;
  }
  bool hasValidBuffer() const {
    return backing_ != nullptr && width() > 0 && height() > 0;
  }

private:
  std::shared_ptr<AlignedBuffer> backing_;
};

/**
 * Single unified event queued from the FreeRDP worker thread to the TigerVNC
 * main poll() thread. Preserving strict FIFO order across graphics primitives
 * ensures CopyRect, SolidFill, Raw, JPEG, H.264, and dirty-rect updates never
 * reorder against each other.
 */
struct RdpEvent {
  enum class Type {
    FrameUpdate,
    CopyRect,
    SolidFill,
    RawRect,
    JpegRect,
    H264Frame,
    CursorShape,
    CursorNull,
    CursorPosition,
    Resize,
    ResizeFailed,
    Connected,
    ClipboardAnnounce,
    ClipboardData,
    ClipboardRequest,
    Disconnect,
  };

  Type type = Type::FrameUpdate;
  uint64_t fbGeneration = 0;

  // FrameUpdate
  core::Region dirty;
  // Tightly packed BGRX32 pixels for dirty.get_rects(), or for rect if
  // this event is a CopyRect or H264Frame.
  std::vector<uint8_t> pixels;

  // CopyRect, SolidFill, RawRect, JpegRect, H264Frame
  core::Rect rect;

  // CopyRect
  int srcX = 0;
  int srcY = 0;

  // SolidFill (BGRX32 decomposed R, G, B)
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;

  // JpegRect
  int qualityLevel = -1;

  // H264Frame & JpegRect payload bytes; CursorShape RGBA bytes
  std::vector<uint8_t> payload;
  uint32_t h264Flags = 0;
  bool h264IsIdr = false;
  core::Region fallbackDirty;

  // CursorShape / CursorPosition
  int width = 0;
  int height = 0;
  int hotX = 0;
  int hotY = 0;
  int x = 0;
  int y = 0;

  // Resize
  std::shared_ptr<AlignedBuffer> framebuffer;
  rfb::ScreenSet layout;
  uint64_t resizeId = 0;

  // Clipboard
  bool clipboardAvailable = false;
  std::string text;

  // Disconnect
  uint32_t reasonCode = 0;
};

/**
 * Thread-safe multi-producer / single-consumer FIFO event queue backed by a
 * non-blocking Linux eventfd for integration into the main poll() loop.
 */
class RdpEventQueue {
public:
  RdpEventQueue();
  ~RdpEventQueue();

  RdpEventQueue(const RdpEventQueue&) = delete;
  RdpEventQueue& operator=(const RdpEventQueue&) = delete;

  int fd() const { return eventfd_; }

  /**
   * Pushes an event and signals eventfd. Waits for the consumer if queued
   * pixel data would exceed the queue budget.
   */
  void push(RdpEvent&& ev);

  /**
   * Drains all currently queued events in exact FIFO order and clears eventfd.
   */
  void drain(std::vector<RdpEvent>& out);

  // Wake producers waiting for space during session shutdown.
  void cancel();

  /**
   * Removes all pending events (e.g. during teardown).
   */
  void clear();

  size_t approximateSize() const;

private:
  void drainSignal();

  int eventfd_;
  mutable std::mutex mutex_;
  std::condition_variable spaceAvailable_;
  std::deque<RdpEvent> queue_;
  size_t queuedBytes_ = 0;
  bool cancelled_ = false;
};

} // namespace rdpvnc

#endif // __RDPVNCSERVER_RDPPIXELBUFFER_H__
