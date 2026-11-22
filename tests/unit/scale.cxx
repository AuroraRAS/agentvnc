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
