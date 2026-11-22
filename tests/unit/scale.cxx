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

#include <math.h>

#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "Scale.h"

TEST(Scale, floorDiv)
{
  EXPECT_EQ(floorDiv(0, 1), 0);
  EXPECT_EQ(floorDiv(0, 3), 0);
  EXPECT_EQ(floorDiv(5, 2), 2);
  EXPECT_EQ(floorDiv(6, 2), 3);
  EXPECT_EQ(floorDiv(7, 3), 2);

  EXPECT_EQ(floorDiv(-1, 1), -1);
  EXPECT_EQ(floorDiv(-1, 2), -1);
  EXPECT_EQ(floorDiv(-2, 2), -1);
  EXPECT_EQ(floorDiv(-3, 2), -2);
  EXPECT_EQ(floorDiv(-1, 3), -1);
  EXPECT_EQ(floorDiv(-3, 3), -1);
  EXPECT_EQ(floorDiv(-4, 3), -2);
}

TEST(Scale, scaleToFramebuffer)
{
  for (int s = MIN_SCALE_FACTOR; s <= MAX_SCALE_FACTOR; s++) {
    EXPECT_EQ(scaleToFramebuffer(core::Point(0, 0), s), core::Point(0, 0));
    EXPECT_EQ(scaleToFramebuffer(core::Point(s - 1, s - 1), s),
              core::Point(0, 0));
    EXPECT_EQ(scaleToFramebuffer(core::Point(s, s), s), core::Point(1, 1));
    EXPECT_EQ(scaleToFramebuffer(core::Point(-1, -1), s),
              core::Point(-1, -1));
    EXPECT_EQ(scaleToFramebuffer(core::Point(-s, -s), s),
              core::Point(-1, -1));
    EXPECT_EQ(scaleToFramebuffer(core::Point(-s - 1, -s - 1), s),
              core::Point(-2, -2));
  }
}

TEST(Scale, scaleFromFramebufferAndRoundTrip)
{
  EXPECT_EQ(scaleFromFramebuffer(core::Point(10, 20), 1),
            core::Point(10, 20));
  EXPECT_EQ(scaleFromFramebuffer(core::Point(10, 20), 2),
            core::Point(21, 41));
  EXPECT_EQ(scaleFromFramebuffer(core::Point(10, 20), 3),
            core::Point(31, 61));

  for (int s = MIN_SCALE_FACTOR; s <= MAX_SCALE_FACTOR; s++) {
    for (int y = -3; y <= 5; y++) {
      for (int x = -3; x <= 5; x++) {
        core::Point fb(x, y);
        core::Point local = scaleFromFramebuffer(fb, s);
        EXPECT_EQ(scaleToFramebuffer(local, s), fb);
      }
    }
  }
}

TEST(Scale, scaleRectFromFramebuffer)
{
  EXPECT_TRUE(scaleRectFromFramebuffer(core::Rect(), 2).is_empty());
  EXPECT_TRUE(scaleRectFromFramebuffer(core::Rect(5, 5, 5, 5), 3).is_empty());
  EXPECT_TRUE(scaleRectFromFramebuffer(core::Rect(5, 5, 3, 5), 3).is_empty());

  core::Rect r(10, 20, 30, 50);
  EXPECT_EQ(scaleRectFromFramebuffer(r, 1), core::Rect(10, 20, 30, 50));
  EXPECT_EQ(scaleRectFromFramebuffer(r, 2), core::Rect(20, 40, 60, 100));
  EXPECT_EQ(scaleRectFromFramebuffer(r, 3), core::Rect(30, 60, 90, 150));
}

TEST(Scale, parseRemoteDesktopSize)
{
  int width = 0, height = 0;
  ASSERT_TRUE(parseRemoteDesktopSize("65535x1", &width, &height));
  EXPECT_EQ(width, 65535);
  EXPECT_EQ(height, 1);

  for (const char* invalid : {"", "640", "0x480", "640x-1", "65536x1",
                              "1x65536", "999999999999999999999x1",
                              "640x480junk"}) {
    EXPECT_FALSE(parseRemoteDesktopSize(invalid, &width, &height)) << invalid;
    EXPECT_EQ(width, 65535);
    EXPECT_EQ(height, 1);
  }
}

TEST(Scale, computeRemoteDesktopSize)
{
  EXPECT_EQ(computeRemoteDesktopSize(1920, 1080, 2), core::Point(960, 540));
  EXPECT_EQ(computeRemoteDesktopSize(3840, 2160, 3), core::Point(1280, 720));

  core::Point nonDiv = computeRemoteDesktopSize(1366, 768, 3);
  EXPECT_EQ(nonDiv, core::Point(455, 256));
  EXPECT_LE(nonDiv.x * 3, 1366);
  EXPECT_LE(nonDiv.y * 3, 768);

  EXPECT_EQ(computeRemoteDesktopSize(1, 1, 4), core::Point(1, 1));
  EXPECT_EQ(computeRemoteDesktopSize(0, 0, 2), core::Point(1, 1));
}

TEST(Scale, scaleScreenRectToRemote)
{
  core::Rect ref1(0, 0, 1920, 1080);
  EXPECT_EQ(scaleScreenRectToRemote(ref1, ref1, 960, 540, 2),
            core::Rect(0, 0, 960, 540));

  // Dual monitor with non-divisible width at 3x: no gap and no overlap
  core::Rect refDual(0, 0, 3286, 1080);
  core::Rect scr1(0, 0, 1366, 768);
  core::Rect scr2(1366, 0, 3286, 1080);
  core::Rect r1 = scaleScreenRectToRemote(scr1, refDual, 1095, 360, 3);
  core::Rect r2 = scaleScreenRectToRemote(scr2, refDual, 1095, 360, 3);
  EXPECT_EQ(r1, core::Rect(0, 0, 455, 256));
  EXPECT_EQ(r2, core::Rect(455, 0, 1095, 360));
  EXPECT_FALSE(r1.overlaps(r2));
  EXPECT_EQ(r1.br.x, r2.tl.x);
  EXPECT_TRUE(r1.enclosed_by(core::Rect(0, 0, 1095, 360)));
  EXPECT_TRUE(r2.enclosed_by(core::Rect(0, 0, 1095, 360)));

  // Centered explicit viewport
  core::Rect centeredRef(160, 120, 800, 600);
  EXPECT_EQ(scaleScreenRectToRemote(centeredRef, centeredRef, 320, 240, 2),
            core::Rect(0, 0, 320, 240));

  // Out-of-bounds screen returns empty
  core::Rect outside(2000, 0, 3000, 1000);
  EXPECT_TRUE(scaleScreenRectToRemote(outside, ref1, 960, 540, 2).is_empty());
}

TEST(Scale, computeCursorScale)
{
  EXPECT_EQ(computeCursorScale(16, 16, 4), 4);
  EXPECT_EQ(computeCursorScale(64, 64, 4), 4);
  EXPECT_EQ(computeCursorScale(64, 64, 5), 4);
  EXPECT_EQ(computeCursorScale(200, 100, 3), 1);
  EXPECT_EQ(computeCursorScale(128, 32, 4), 2);
  EXPECT_EQ(computeCursorScale(0, 0, 3), 1);
}

TEST(Scale, scaleCursorHotspot)
{
  EXPECT_EQ(scaleCursorHotspot(core::Point(0, 0), 16, 16, 3),
            core::Point(1, 1));
  EXPECT_EQ(scaleCursorHotspot(core::Point(5, 7), 16, 16, 2),
            core::Point(11, 15));
  EXPECT_EQ(scaleCursorHotspot(core::Point(15, 15), 16, 16, 2),
            core::Point(31, 31));
  EXPECT_EQ(scaleCursorHotspot(core::Point(20, 20), 16, 16, 2),
            core::Point(31, 31));
  EXPECT_EQ(scaleCursorHotspot(core::Point(-1, -1), 16, 16, 2),
            core::Point(0, 0));
}

TEST(Scale, scalePixelBuffer)
{
  const int w = 3;
  const int h = 2;

  // Test depth=3 (RGB) and depth=4 (RGBA), with zero and non-zero lineStride
  for (int depth : {3, 4}) {
    for (int extraStride : {0, 5}) {
      int stride = (extraStride == 0) ? 0 : (w * depth + extraStride);
      int actualStride = (stride != 0) ? stride : (w * depth);
      std::vector<uint8_t> src(h * actualStride, 0xaa);

      for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
          for (int c = 0; c < depth; c++) {
            src[y * actualStride + x * depth + c] =
              static_cast<uint8_t>(y * 40 + x * 10 + c + 1);
          }
        }
      }

      for (int s : {1, 2, 3}) {
        int dw = w * s;
        int dh = h * s;
        std::vector<uint8_t> dst(dw * dh * depth, 0);
        scalePixelBuffer(src.data(), w, h, depth, stride, s, dst.data());

        for (int dy = 0; dy < dh; dy++) {
          for (int dx = 0; dx < dw; dx++) {
            int sx = dx / s;
            int sy = dy / s;
            for (int c = 0; c < depth; c++) {
              EXPECT_EQ(dst[(dy * dw + dx) * depth + c],
                        src[sy * actualStride + sx * depth + c]);
            }
          }
        }
      }
    }
  }
}

TEST(Scale, computeScaledBlitParams)
{
  struct Case {
    int srcX, srcY, dstX, dstY, dstW, dstH;
  };
  Case cases[] = {
    {0, 0, 0, 0, 20, 16},
    {1, 2, 3, 4, 17, 13},
    {7, 11, 5, 9, 25, 19},
    {12, 15, 0, 0, 1, 1}
  };

  for (int s = 1; s <= 4; s++) {
    for (const Case& tc : cases) {
      ScaledBlitParams p = computeScaledBlitParams(tc.srcX, tc.srcY,
                                                   tc.dstX, tc.dstY,
                                                   tc.dstW, tc.dstH, s);

      EXPECT_LE(p.srcX * s, tc.srcX);
      EXPECT_GE((p.srcX + p.srcW) * s, tc.srcX + tc.dstW);
      EXPECT_LE(p.srcY * s, tc.srcY);
      EXPECT_GE((p.srcY + p.srcH) * s, tc.srcY + tc.dstH);

      EXPECT_EQ(p.dstW, p.srcW * s);
      EXPECT_EQ(p.dstH, p.srcH * s);
      EXPECT_EQ(p.bottomOverhang, (p.srcY + p.srcH) * s - (tc.srcY + tc.dstH));
      EXPECT_GE(p.bottomOverhang, 0);
      EXPECT_LT(p.bottomOverhang, s);

      for (int dy = 0; dy < tc.dstH; dy++) {
        for (int dx = 0; dx < tc.dstW; dx++) {
          int localDx = (tc.dstX + dx) - p.dstX;
          int localDy = (tc.dstY + dy) - p.dstY;
          EXPECT_EQ(p.srcX + localDx / s, (tc.srcX + dx) / s);
          EXPECT_EQ(p.srcY + localDy / s, (tc.srcY + dy) / s);
        }
      }
    }
  }
}

TEST(Scale, logicalToPhysicalAndBack)
{
  EXPECT_EQ(logicalToPhysical(0, 1.0f), 0);
  EXPECT_EQ(logicalToPhysical(100, 1.0f), 100);
  EXPECT_EQ(logicalToPhysical(100, 1.25f), 125);
  EXPECT_EQ(logicalToPhysical(100, 1.5f), 150);
  EXPECT_EQ(logicalToPhysical(101, 1.5f), 152);
  EXPECT_EQ(logicalToPhysical(100, 2.0f), 200);
  EXPECT_EQ(logicalToPhysical(-10, 1.5f), -15);
  EXPECT_EQ(logicalToPhysical(-3, 1.5f), -5);

  EXPECT_EQ(physicalToLogicalSize(0, 2.0f), 0);
  EXPECT_EQ(physicalToLogicalSize(-5, 2.0f), 0);
  EXPECT_EQ(physicalToLogicalSize(1, 2.0f), 1);
  EXPECT_EQ(physicalToLogicalSize(200, 2.0f), 100);
  EXPECT_EQ(physicalToLogicalSize(150, 1.5f), 100);
  EXPECT_EQ(physicalToLogicalSize(125, 1.25f), 100);
  EXPECT_EQ(physicalToLogicalSize(1920, 1.75f), 1098);
  EXPECT_EQ(physicalToLogicalSize(641, 1.5f), 428);
}

TEST(Scale, logicalAndPhysicalRectRoundTrip)
{
  EXPECT_TRUE(logicalRectToPhysical(core::Rect(), 2.0f, 100, 100).is_empty());
  EXPECT_TRUE(logicalRectToPhysical(core::Rect(0, 0, 10, 10), 2.0f, 0, 100).is_empty());
  EXPECT_TRUE(logicalRectToPhysical(core::Rect(60, 0, 70, 10), 2.0f, 100, 100).is_empty());
  EXPECT_TRUE(physicalRectToLogical(core::Rect(), 2.0f).is_empty());

  EXPECT_EQ(logicalRectToPhysical(core::Rect(-5, -5, 60, 60), 2.0f, 100, 100),
            core::Rect(0, 0, 100, 100));

  for (float s : {1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f}) {
    const int maxW = 500;
    const int maxH = 400;
    core::Rect phys(7, 13, 83, 97);
    core::Rect log = physicalRectToLogical(phys, s);
    core::Rect back = logicalRectToPhysical(log, s, maxW, maxH);
    EXPECT_TRUE(phys.enclosed_by(back)) << "scale=" << s;

    core::Rect farRight(maxW + (int)ceilf(s) + 2, 10, maxW + 30, 40);
    EXPECT_TRUE(logicalRectToPhysical(physicalRectToLogical(farRight, s),
                                      s, maxW, maxH).is_empty()) << "scale=" << s;
  }
}

TEST(Scale, screenRectToPhysicalAndMatch)
{
  EXPECT_TRUE(scaleScreenRectToPhysical(core::Rect(), 1.5f).is_empty());
  EXPECT_EQ(scaleScreenRectToPhysical(core::Rect(0, 0, 2560, 1440), 2.0f),
            core::Rect(0, 0, 5120, 2880));

  // FLTK integer truncation: 1366 / 1.5 = 910, 910 * 1.5 = 1365 (1px diff)
  core::Rect fltkTruncated(0, 0, 910, 512);
  core::Rect xrandrExact(0, 0, 1366, 768);
  EXPECT_TRUE(screenRectMatchesPhysical(fltkTruncated, 1.5f, xrandrExact));

  // Fractional 1.25x with offset monitor
  EXPECT_TRUE(screenRectMatchesPhysical(core::Rect(1536, 0, 3072, 864),
                                        1.25f,
                                        core::Rect(1920, 0, 3840, 1080)));

  EXPECT_FALSE(screenRectMatchesPhysical(fltkTruncated, 1.5f,
                                         core::Rect(0, 0, 1920, 1080)));
  EXPECT_FALSE(screenRectMatchesPhysical(fltkTruncated, 1.5f,
                                         core::Rect(1366, 0, 2732, 768)));
  EXPECT_FALSE(screenRectMatchesPhysical(core::Rect(), 1.5f, xrandrExact));
}

TEST(Scale, parseXftDpiFromResources)
{
  float dpi = -1.0f;

  EXPECT_FALSE(parseXftDpiFromResources(nullptr, &dpi));
  EXPECT_FALSE(parseXftDpiFromResources("", &dpi));
  EXPECT_FALSE(parseXftDpiFromResources("! Xft.dpi: 192\nXft.hinting: 1\n", &dpi));
  EXPECT_FLOAT_EQ(dpi, -1.0f);

  EXPECT_TRUE(parseXftDpiFromResources("Xft.dpi:\t192\n", &dpi));
  EXPECT_FLOAT_EQ(dpi, 192.0f);

  EXPECT_TRUE(parseXftDpiFromResources("*.Xft.dpi :  144 \r\n", &dpi));
  EXPECT_FLOAT_EQ(dpi, 144.0f);

  EXPECT_TRUE(parseXftDpiFromResources("*Xft.dpi: 120.5\n", &dpi));
  EXPECT_FLOAT_EQ(dpi, 120.5f);

  // Last entry wins; invalid entries do not clobber earlier valid ones
  const char* multi =
    "Xft.dpi: 96\n"
    "!Xft.dpi: 240\n"
    "Xft.antialias: 1\n"
    "Xft.dpi: 168\n"
    "Xft.dpi: 0\n"
    "Xft.dpi: -96\n"
    "Xft.dpi: nan\n"
    "Xft.dpi: 120,5\n"
    "Xft.dpi: 120px\n"
    "Xft.dpi:\n";
  EXPECT_TRUE(parseXftDpiFromResources(multi, &dpi));
  EXPECT_FLOAT_EQ(dpi, 168.0f);

  dpi = 77.0f;
  EXPECT_FALSE(parseXftDpiFromResources("Xft.dpi: 0\nXft.dpi: inf\nXft.dpi: abc\n", &dpi));
  EXPECT_FLOAT_EQ(dpi, 77.0f);
}

TEST(Scale, computeUIScaleFromDpi)
{
  EXPECT_FLOAT_EQ(computeUIScaleFromDpi(96.0f), 1.0f);
  EXPECT_FLOAT_EQ(computeUIScaleFromDpi(120.0f), 1.25f);
  EXPECT_FLOAT_EQ(computeUIScaleFromDpi(144.0f), 1.5f);
  EXPECT_FLOAT_EQ(computeUIScaleFromDpi(168.0f), 1.75f);
  EXPECT_FLOAT_EQ(computeUIScaleFromDpi(192.0f), 2.0f);

  EXPECT_FLOAT_EQ(computeUIScaleFromDpi(192.0f, 1.25f), 2.5f);
  EXPECT_FLOAT_EQ(computeUIScaleFromDpi(0.0f), 1.0f);
  EXPECT_FLOAT_EQ(computeUIScaleFromDpi(-96.0f), 1.0f);
  EXPECT_FLOAT_EQ(computeUIScaleFromDpi(std::numeric_limits<float>::quiet_NaN()), 1.0f);
  EXPECT_FLOAT_EQ(computeUIScaleFromDpi(192.0f, -2.0f), 2.0f);

  EXPECT_FLOAT_EQ(computeUIScaleFromDpi(12.0f), 0.5f);
  EXPECT_FLOAT_EQ(computeUIScaleFromDpi(9600.0f), 10.0f);
}
