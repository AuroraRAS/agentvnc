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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <locale>
#include <sstream>
#include <string>

#include "Scale.h"

int floorDiv(int a, int b)
{
  assert(b >= 1);

  if (a >= 0)
    return a / b;

  int q = a / b;
  int r = a % b;
  return (r != 0) ? (q - 1) : q;
}

int logicalToPhysical(int v, float uiScale)
{
  assert(uiScale > 0.0f);

  return (int)lroundf((float)v * uiScale);
}

int physicalToLogicalSize(int phys, float uiScale)
{
  assert(uiScale > 0.0f);

  if (phys <= 0)
    return 0;

  // FLTK window sizes are integral. Round up so the entire framebuffer fits.
  return (int)ceil(phys / (double)uiScale);
}

core::Rect logicalRectToPhysical(const core::Rect& logicalRect,
                                 float uiScale, int maxW, int maxH)
{
  assert(uiScale > 0.0f);

  if (logicalRect.is_empty() || (maxW <= 0) || (maxH <= 0))
    return core::Rect();

  int x0 = std::max(0, std::min(maxW, (int)floorf((float)logicalRect.tl.x * uiScale)));
  int y0 = std::max(0, std::min(maxH, (int)floorf((float)logicalRect.tl.y * uiScale)));
  int x1 = std::max(0, std::min(maxW, (int)ceilf((float)logicalRect.br.x * uiScale)));
  int y1 = std::max(0, std::min(maxH, (int)ceilf((float)logicalRect.br.y * uiScale)));

  if ((x1 <= x0) || (y1 <= y0))
    return core::Rect();

  return core::Rect(x0, y0, x1, y1);
}

core::Rect physicalRectToLogical(const core::Rect& physRect, float uiScale)
{
  assert(uiScale > 0.0f);

  if (physRect.is_empty())
    return core::Rect();

  int x0 = (int)floorf((float)physRect.tl.x / uiScale);
  int y0 = (int)floorf((float)physRect.tl.y / uiScale);
  int x1 = (int)ceilf((float)physRect.br.x / uiScale);
  int y1 = (int)ceilf((float)physRect.br.y / uiScale);

  return core::Rect(x0, y0, x1, y1);
}

core::Rect scaleScreenRectToPhysical(const core::Rect& logicalRect,
                                     float uiScale)
{
  assert(uiScale > 0.0f);

  if (logicalRect.is_empty())
    return core::Rect();

  int px = logicalToPhysical(logicalRect.tl.x, uiScale);
  int py = logicalToPhysical(logicalRect.tl.y, uiScale);
  int pw = logicalToPhysical(logicalRect.width(), uiScale);
  int ph = logicalToPhysical(logicalRect.height(), uiScale);

  return core::Rect(px, py, px + pw, py + ph);
}

bool screenRectMatchesPhysical(const core::Rect& logicalRect,
                               float uiScale,
                               const core::Rect& osPhysRect)
{
  core::Rect p = scaleScreenRectToPhysical(logicalRect, uiScale);
  if (p.is_empty() || osPhysRect.is_empty())
    return false;

  int tol = std::max(1, (int)ceilf(uiScale));
  return (abs(p.tl.x - osPhysRect.tl.x) <= tol) &&
         (abs(p.tl.y - osPhysRect.tl.y) <= tol) &&
         (abs(p.width() - osPhysRect.width()) <= tol) &&
         (abs(p.height() - osPhysRect.height()) <= tol);
}

bool parsePositiveScale(const std::string& value, float* result)
{
  std::istringstream input(value);
  float parsed;

  input.imbue(std::locale::classic());
  if (!(input >> parsed) || !std::isfinite(parsed) || (parsed <= 0.0f))
    return false;

  input >> std::ws;
  if (!input.eof())
    return false;

  *result = parsed;
  return true;
}

bool parseXftDpiFromResources(const char* resources, float* dpi)
{
  assert(dpi != nullptr);

  if (resources == nullptr)
    return false;

  bool found = false;
  float lastDpi = 0.0f;
  const char* p = resources;

  while (*p != '\0') {
    const char* lineEnd = strchr(p, '\n');
    size_t len = lineEnd ? (size_t)(lineEnd - p) : strlen(p);
    const char* next = lineEnd ? (lineEnd + 1) : (p + len);

    if ((len > 0) && (p[len - 1] == '\r'))
      len--;

    const char* cur = p;
    const char* end = p + len;
    while ((cur < end) && isspace((unsigned char)*cur))
      cur++;

    if ((cur < end) && (*cur != '!')) {
      const char* colon = (const char*)memchr(cur, ':', (size_t)(end - cur));
      if (colon != nullptr) {
        const char* keyEnd = colon;
        while ((keyEnd > cur) && isspace((unsigned char)keyEnd[-1]))
          keyEnd--;

        size_t keyLen = (size_t)(keyEnd - cur);
        bool keyMatches =
          ((keyLen == 7) && (strncmp(cur, "Xft.dpi", 7) == 0)) ||
          ((keyLen == 8) && (strncmp(cur, "*Xft.dpi", 8) == 0)) ||
          ((keyLen == 9) && (strncmp(cur, "*.Xft.dpi", 9) == 0));

        if (keyMatches && parsePositiveScale(std::string(colon + 1, end), &lastDpi))
          found = true;
      }
    }

    p = next;
  }

  if (found)
    *dpi = lastDpi;

  return found;
}

float computeUIScaleFromDpi(float dpi, float envMultiplier)
{
  if (!std::isfinite(dpi) || (dpi <= 0.0f))
    return 1.0f;

  if (!std::isfinite(envMultiplier) || (envMultiplier <= 0.0f))
    envMultiplier = 1.0f;

  float s = (dpi / 96.0f) * envMultiplier;
  return std::max(0.5f, std::min(10.0f, s));
}

core::Point scaleToFramebuffer(const core::Point& localPos, int scale)
{
  return core::Point(floorDiv(localPos.x, scale),
                     floorDiv(localPos.y, scale));
}

core::Point scaleFromFramebuffer(const core::Point& fbPos, int scale)
{
  assert(scale >= 1);

  return core::Point(fbPos.x * scale + scale / 2,
                     fbPos.y * scale + scale / 2);
}

core::Rect scaleRectFromFramebuffer(const core::Rect& fbRect, int scale)
{
  assert(scale >= 1);

  if (fbRect.is_empty())
    return core::Rect();

  return core::Rect(fbRect.tl.x * scale, fbRect.tl.y * scale,
                    fbRect.br.x * scale, fbRect.br.y * scale);
}

bool parseRemoteDesktopSize(const char* spec, int* width, int* height)
{
  char* end;
  long w = strtol(spec, &end, 10);
  if (end == spec || *end != 'x' || w < 1 || w > UINT16_MAX)
    return false;

  const char* heightStart = end + 1;
  long h = strtol(heightStart, &end, 10);
  if (end == heightStart || *end != '\0' || h < 1 || h > UINT16_MAX)
    return false;

  *width = (int)w;
  *height = (int)h;
  return true;
}

core::Point computeRemoteDesktopSize(int winWidth, int winHeight, int scale)
{
  assert(scale >= 1);

  return core::Point(std::max(1, winWidth / scale),
                     std::max(1, winHeight / scale));
}

core::Rect scaleScreenRectToRemote(const core::Rect& screenRect,
                                   const core::Rect& refRect,
                                   int remoteWidth, int remoteHeight,
                                   int scale)
{
  assert(scale >= 1);

  int sx = screenRect.tl.x - refRect.tl.x;
  int sy = screenRect.tl.y - refRect.tl.y;
  int sw = screenRect.width();
  int sh = screenRect.height();

  int rx0 = std::max(0, sx / scale);
  int ry0 = std::max(0, sy / scale);
  int rx1 = std::min((sx + sw) / scale, remoteWidth);
  int ry1 = std::min((sy + sh) / scale, remoteHeight);

  if ((rx1 <= rx0) || (ry1 <= ry0))
    return core::Rect();

  return core::Rect(rx0, ry0, rx1, ry1);
}

int computeCursorScale(int width, int height, int scale, int maxSize)
{
  assert(scale >= 1);

  if ((width <= 0) || (height <= 0))
    return 1;

  int cursorScale = scale;
  while ((cursorScale > 1) &&
         ((width * cursorScale > maxSize) ||
          (height * cursorScale > maxSize))) {
    cursorScale--;
  }

  return cursorScale;
}

core::Point scaleCursorHotspot(const core::Point& hotspot,
                               int width, int height, int cursorScale)
{
  assert(cursorScale >= 1);
  assert(width > 0);
  assert(height > 0);

  int hx = hotspot.x * cursorScale + cursorScale / 2;
  int hy = hotspot.y * cursorScale + cursorScale / 2;

  hx = std::max(0, std::min(hx, width * cursorScale - 1));
  hy = std::max(0, std::min(hy, height * cursorScale - 1));

  return core::Point(hx, hy);
}

void scalePixelBuffer(const uint8_t* src, int width, int height,
                      int depth, int lineStride, int scale,
                      uint8_t* dst)
{
  assert(src != nullptr);
  assert(dst != nullptr);
  assert(width > 0);
  assert(height > 0);
  assert(depth > 0);
  assert(scale >= 1);

  int srcStride = (lineStride != 0) ? lineStride : width * depth;
  int dstRowBytes = width * scale * depth;

  for (int y = 0; y < height; y++) {
    const uint8_t* srcRow = src + y * srcStride;
    uint8_t* firstDstRow = dst + y * scale * dstRowBytes;
    uint8_t* out = firstDstRow;

    for (int x = 0; x < width; x++) {
      const uint8_t* px = srcRow + x * depth;
      for (int s = 0; s < scale; s++) {
        memcpy(out, px, depth);
        out += depth;
      }
    }

    for (int s = 1; s < scale; s++) {
      memcpy(firstDstRow + s * dstRowBytes, firstDstRow, dstRowBytes);
    }
  }
}

ScaledBlitParams computeScaledBlitParams(int srcX, int srcY,
                                         int dstX, int dstY,
                                         int dstW, int dstH,
                                         int scale)
{
  assert(scale >= 1);
  assert(srcX >= 0);
  assert(srcY >= 0);
  assert(dstW > 0);
  assert(dstH > 0);

  ScaledBlitParams p;
  p.srcX = srcX / scale;
  p.srcY = srcY / scale;

  int srcX1 = (srcX + dstW + scale - 1) / scale;
  int srcY1 = (srcY + dstH + scale - 1) / scale;

  p.srcW = srcX1 - p.srcX;
  p.srcH = srcY1 - p.srcY;

  p.dstX = dstX - (srcX - p.srcX * scale);
  p.dstY = dstY - (srcY - p.srcY * scale);
  p.dstW = p.srcW * scale;
  p.dstH = p.srcH * scale;
  p.bottomOverhang = srcY1 * scale - (srcY + dstH);

  return p;
}
