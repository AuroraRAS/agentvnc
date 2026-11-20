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

#ifndef __SCALE_H__
#define __SCALE_H__

#include <stdint.h>

#include <core/Rect.h>

static const int MIN_SCALE_FACTOR = 1;
static const int MAX_SCALE_FACTOR = 8;
static const int MAX_SCALED_CURSOR_SIZE = 256;

struct ScaledBlitParams {
  int srcX;
  int srcY;
  int srcW;
  int srcH;
  int dstX;
  int dstY;
  int dstW;
  int dstH;
  int bottomOverhang;
};

int floorDiv(int a, int b);

core::Point scaleToFramebuffer(const core::Point& localPos, int scale);
core::Point scaleFromFramebuffer(const core::Point& fbPos, int scale);
core::Rect scaleRectFromFramebuffer(const core::Rect& fbRect, int scale);

core::Point computeRemoteDesktopSize(int winWidth, int winHeight, int scale);
core::Rect scaleScreenRectToRemote(const core::Rect& screenRect,
                                   const core::Rect& refRect,
                                   int remoteWidth, int remoteHeight,
                                   int scale);

int computeCursorScale(int width, int height, int scale,
                       int maxSize = MAX_SCALED_CURSOR_SIZE);
core::Point scaleCursorHotspot(const core::Point& hotspot,
                               int width, int height, int cursorScale);
void scalePixelBuffer(const uint8_t* src, int width, int height,
                      int depth, int lineStride, int scale,
                      uint8_t* dst);

ScaledBlitParams computeScaledBlitParams(int srcX, int srcY,
                                         int dstX, int dstY,
                                         int dstW, int dstH,
                                         int scale);

#endif
