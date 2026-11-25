"""TypeSafe System One REST client, frame metrics (pixel diff & SSIM), and image/BBox helpers."""

from dataclasses import dataclass
import base64
import io
import json
import math
import os
import struct
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import zlib
from typing import Any, Dict, List, Optional, Sequence, Tuple

from .constants import REPO_ROOT

try:
    import numpy as np  # type: ignore
except ImportError:  # pragma: no cover
    np = None  # type: ignore

try:
    from PIL import Image, ImageDraw  # type: ignore
except ImportError:  # pragma: no cover
    Image = None  # type: ignore
    ImageDraw = None  # type: ignore

try:
    from scipy.ndimage import uniform_filter  # type: ignore
except ImportError:  # pragma: no cover
    uniform_filter = None  # type: ignore


TYPESAFE_API_BASE = "https://api.typesafe.ai"
TYPESAFE_SYSTEM_ONE_URL = "https://api.typesafe.ai/v1/systemone"
DEFAULT_SYSTEM_ONE_MODEL = "jev-latest"
TYPESAFE_IMAGES_HEADER = "TypeSafe-Danger-Zone-Images"

TYPESAFE_MIN_IMAGE_DIM = 280
TYPESAFE_MAX_IMAGE_DIM = 1568
TYPESAFE_MIN_IMAGE_PIXELS = 78400
TYPESAFE_MAX_IMAGE_PIXELS = 2458624
TYPESAFE_PNG_JPEG_FALLBACK_BYTES = 8 * 1024 * 1024


def _encode_rgb_png(width: int, height: int, rgb_bytes: bytes) -> bytes:
    """Encode raw RGB24 bytes into a PNG byte buffer."""
    if Image is not None:
        buf = io.BytesIO()
        img = Image.frombytes("RGB", (width, height), rgb_bytes)
        img.save(buf, format="PNG")
        return buf.getvalue()

    def _chunk(tag: bytes, data: bytes) -> bytes:
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        return struct.pack("!I", len(data)) + tag + data + struct.pack("!I", crc)

    ihdr = struct.pack("!IIBBBBB", width, height, 8, 2, 0, 0, 0)
    stride = width * 3
    raw_rows = bytearray()
    for y in range(height):
        raw_rows.append(0)
        start = y * stride
        raw_rows.extend(rgb_bytes[start : start + stride])
    idat = zlib.compress(bytes(raw_rows), level=6)
    return b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr) + _chunk(b"IDAT", idat) + _chunk(b"IEND", b"")


def load_typesafe_api_key(key_file: Optional[str] = None) -> str:
    """Load TypeSafe API key from ``key_file`` (default ``typesafe.key``) or ``TYPESAFE_API_KEY``."""
    resolved = key_file if key_file is not None else os.path.join(REPO_ROOT, "typesafe.key")
    env_key = os.environ.get("TYPESAFE_API_KEY", "").strip()
    if env_key and not key_file:
        return env_key
    if resolved and os.path.exists(resolved):
        with open(resolved, "r", encoding="utf-8") as f:
            key = f.read().strip()
        if key:
            return key
    if env_key:
        return env_key
    raise FileNotFoundError(f"TypeSafe API key not found at {resolved!r} or TYPESAFE_API_KEY")


def try_load_typesafe_api_key(key_file: Optional[str] = None) -> Optional[str]:
    """Return TypeSafe API key if available, otherwise ``None``."""
    try:
        return load_typesafe_api_key(key_file)
    except (FileNotFoundError, OSError):
        return None


class SystemOneApiError(RuntimeError):
    """Structured error from the TypeSafe System One REST API."""

    def __init__(self, status_code: Optional[int], raw_body: str) -> None:
        self.status_code = status_code
        self.raw_body = raw_body or ""
        self.error_type = ""
        self.error_message = self.raw_body.strip()

        try:
            parsed = json.loads(self.raw_body)
        except (json.JSONDecodeError, TypeError):
            parsed = None

        if isinstance(parsed, dict):
            detail = parsed.get("detail")
            if isinstance(detail, dict):
                msg = detail.get("message")
                etype = detail.get("error_type") or detail.get("code")
                if isinstance(msg, str) and msg.strip():
                    self.error_message = msg.strip()
                if isinstance(etype, str) and etype.strip():
                    self.error_type = etype.strip()
            elif isinstance(detail, list) and detail:
                parts: List[str] = []
                for item in detail:
                    if isinstance(item, dict):
                        loc = ".".join(str(x) for x in item.get("loc", []) if x is not None)
                        msg = str(item.get("msg", "")).strip()
                        parts.append(f"{loc}: {msg}" if loc and msg else (msg or json.dumps(item)))
                    else:
                        parts.append(str(item))
                if parts:
                    self.error_message = "; ".join(parts)
                    self.error_type = "validation_error"
            elif isinstance(detail, str) and detail.strip():
                self.error_message = detail.strip()
            elif isinstance(parsed.get("message"), str) and parsed["message"].strip():
                self.error_message = parsed["message"].strip()

        if status_code is not None:
            summary = f"TypeSafe System One HTTP {status_code}: {self.error_message or self.error_type or 'Unknown error'}"
        else:
            summary = f"TypeSafe System One error: {self.error_message or 'Unknown error'}"
        super().__init__(summary)

    @property
    def is_auth_error(self) -> bool:
        return self.status_code in (401, 403)

    @property
    def is_retryable(self) -> bool:
        return self.status_code in (429, 500, 502, 503, 504) or self.status_code is None


@dataclass
class FrameSnapshot:
    """In-memory RGB24 desktop frame snapshot."""

    width: int
    height: int
    rgb_bytes: bytes
    frame_seq: int = 0
    timestamp: float = 0.0
    png_bytes: Optional[bytes] = None

    def to_png_bytes(self) -> bytes:
        if self.png_bytes is None:
            self.png_bytes = _encode_rgb_png(self.width, self.height, self.rgb_bytes)
        return self.png_bytes


def validate_bbox(bbox: Any, name: str = "roi") -> List[int]:
    """Validate and normalize ``BBox`` ``[ymin, xmin, ymax, xmax]`` in ``0..1000``."""
    if not isinstance(bbox, (list, tuple)) or len(bbox) != 4:
        raise ValueError(f"{name} must be a 4-element [ymin, xmin, ymax, xmax] list, got {bbox!r}")
    coords: List[int] = []
    for idx, raw in enumerate(bbox):
        if isinstance(raw, bool) or not isinstance(raw, (int, float)) or not math.isfinite(float(raw)):
            raise ValueError(f"{name}[{idx}] must be a finite number in 0..1000, got {raw!r}")
        coords.append(int(round(float(raw))))
    ymin, xmin, ymax, xmax = coords
    if ymin < 0 or xmin < 0 or ymax > 1000 or xmax > 1000:
        raise ValueError(f"{name} coordinates must lie within 0..1000, got {coords!r}")
    if ymin >= ymax or xmin >= xmax:
        raise ValueError(f"{name} must satisfy ymin < ymax and xmin < xmax, got {coords!r}")
    return [ymin, xmin, ymax, xmax]


def bbox_to_pixels(bbox: Sequence[int], width: int, height: int) -> Tuple[int, int, int, int]:
    """Convert normalized ``[ymin, xmin, ymax, xmax]`` (0..1000) to ``(x0, y0, x1, y1)`` pixels."""
    if width <= 0 or height <= 0:
        raise ValueError(f"Invalid frame dimensions: {width}x{height}")
    ymin, xmin, ymax, xmax = [int(v) for v in bbox]
    x0 = max(0, min(width - 1, int(math.floor((xmin / 1000.0) * width))))
    y0 = max(0, min(height - 1, int(math.floor((ymin / 1000.0) * height))))
    x1 = max(x0 + 1, min(width, int(math.ceil((xmax / 1000.0) * width))))
    y1 = max(y0 + 1, min(height, int(math.ceil((ymax / 1000.0) * height))))
    return x0, y0, x1, y1


def pad_bbox_norm(bbox: Sequence[int], amount: float, mode: str = "pad") -> List[int]:
    """Expand a normalized ``BBox`` around its center, clamped to ``0..1000``.

    - ``mode='scale'``: multiply width and height by ``amount`` (e.g. ``1.5`` for 1.5x ROI).
    - ``mode='pad'``: add ``amount * span`` padding on each side (e.g. ``0.20`` for 20% pad).
    """
    ymin, xmin, ymax, xmax = validate_bbox(list(bbox), name="bbox")
    h = ymax - ymin
    w = xmax - xmin
    cy = (ymin + ymax) / 2.0
    cx = (xmin + xmax) / 2.0
    if mode == "scale":
        new_h = max(1.0, h * float(amount))
        new_w = max(1.0, w * float(amount))
    elif mode == "pad":
        new_h = max(1.0, h * (1.0 + 2.0 * float(amount)))
        new_w = max(1.0, w * (1.0 + 2.0 * float(amount)))
    else:
        raise ValueError(f"Unsupported pad mode: {mode!r}")

    ny0 = max(0, int(round(cy - new_h / 2.0)))
    nx0 = max(0, int(round(cx - new_w / 2.0)))
    ny1 = min(1000, int(round(cy + new_h / 2.0)))
    nx1 = min(1000, int(round(cx + new_w / 2.0)))
    if ny1 <= ny0:
        ny1 = min(1000, ny0 + 1)
        ny0 = max(0, ny1 - 1)
    if nx1 <= nx0:
        nx1 = min(1000, nx0 + 1)
        nx0 = max(0, nx1 - 1)
    return [ny0, nx0, ny1, nx1]


@dataclass
class PreparedImage:
    """Image prepared within TypeSafe's 280..1568px constraints with inverse coordinate mapping."""

    image_bytes: bytes
    image_format: str
    orig_width: int
    orig_height: int
    out_width: int
    out_height: int
    pad_x: int
    pad_y: int
    content_width: int
    content_height: int
    crop_bbox_norm: List[int]

    def map_box_to_screen_bbox(self, box_edges: Dict[str, Any]) -> List[int]:
        """Map TypeSafe ``BoxAnswer.box`` (0..1 edges) back to full-screen ``[ymin, xmin, ymax, xmax]`` (0..1000)."""
        top = float(box_edges["top"])
        bottom = float(box_edges["bottom"])
        left = float(box_edges["left"])
        right = float(box_edges["right"])
        if not (0 <= top < bottom <= 1 and 0 <= left < right <= 1):
            raise ValueError("System One box edges must be ordered within 0..1")

        px_left = left * self.out_width
        px_right = right * self.out_width
        px_top = top * self.out_height
        px_bottom = bottom * self.out_height

        rel_left = max(0.0, min(1.0, (px_left - self.pad_x) / max(1.0, float(self.content_width))))
        rel_right = max(0.0, min(1.0, (px_right - self.pad_x) / max(1.0, float(self.content_width))))
        rel_top = max(0.0, min(1.0, (px_top - self.pad_y) / max(1.0, float(self.content_height))))
        rel_bottom = max(0.0, min(1.0, (px_bottom - self.pad_y) / max(1.0, float(self.content_height))))

        if rel_right <= rel_left or rel_bottom <= rel_top:
            raise ValueError("System One box is outside the image content")

        cy0, cx0, cy1, cx1 = self.crop_bbox_norm
        span_y = cy1 - cy0
        span_x = cx1 - cx0

        sy0 = max(0, min(999, int(round(cy0 + rel_top * span_y))))
        sx0 = max(0, min(999, int(round(cx0 + rel_left * span_x))))
        sy1 = max(sy0 + 1, min(1000, int(round(cy0 + rel_bottom * span_y))))
        sx1 = max(sx0 + 1, min(1000, int(round(cx0 + rel_right * span_x))))
        return [sy0, sx0, sy1, sx1]


def _crop_rgb_fallback(
    frame: FrameSnapshot, x0: int, y0: int, x1: int, y1: int
) -> Tuple[int, int, bytes]:
    cw = x1 - x0
    ch = y1 - y0
    stride = frame.width * 3
    row_start = x0 * 3
    row_end = x1 * 3
    out = bytearray()
    for y in range(y0, y1):
        base = y * stride
        out.extend(frame.rgb_bytes[base + row_start : base + row_end])
    return cw, ch, bytes(out)


def prepare_image_for_system_one(
    frame: FrameSnapshot,
    crop_bbox_norm: Optional[Sequence[int]] = None,
) -> PreparedImage:
    """Crop ``frame`` to ``crop_bbox_norm`` and resize/pad into TypeSafe's 280..1568 px bounds."""
    crop_norm = (
        validate_bbox(list(crop_bbox_norm), name="crop_bbox")
        if crop_bbox_norm is not None
        else [0, 0, 1000, 1000]
    )
    x0, y0, x1, y1 = bbox_to_pixels(crop_norm, frame.width, frame.height)
    orig_w = x1 - x0
    orig_h = y1 - y0

    max_side = max(orig_w, orig_h)
    min_side = min(orig_w, orig_h)
    scale = 1.0
    if max_side > TYPESAFE_MAX_IMAGE_DIM:
        scale = TYPESAFE_MAX_IMAGE_DIM / float(max_side)
    elif min_side < TYPESAFE_MIN_IMAGE_DIM:
        desired_up = max(2.0, min(3.0, 140.0 / float(min_side)))
        scale = min(
            desired_up,
            TYPESAFE_MIN_IMAGE_DIM / float(min_side),
            TYPESAFE_MAX_IMAGE_DIM / float(max_side),
        )

    content_w = max(1, min(TYPESAFE_MAX_IMAGE_DIM, int(round(orig_w * scale))))
    content_h = max(1, min(TYPESAFE_MAX_IMAGE_DIM, int(round(orig_h * scale))))

    out_w = max(TYPESAFE_MIN_IMAGE_DIM, content_w)
    out_h = max(TYPESAFE_MIN_IMAGE_DIM, content_h)
    pad_x = (out_w - content_w) // 2
    pad_y = (out_h - content_h) // 2

    if Image is not None:
        full_img = Image.frombytes("RGB", (frame.width, frame.height), frame.rgb_bytes)
        cropped = full_img.crop((x0, y0, x1, y1))
        if (content_w, content_h) != (orig_w, orig_h):
            resample = Image.Resampling.BILINEAR if hasattr(Image, "Resampling") else Image.BILINEAR
            cropped = cropped.resize((content_w, content_h), resample=resample)
        if (out_w, out_h) != (content_w, content_h):
            canvas = Image.new("RGB", (out_w, out_h), (128, 128, 128))
            canvas.paste(cropped, (pad_x, pad_y))
            final_img = canvas
        else:
            final_img = cropped

        buf = io.BytesIO()
        final_img.save(buf, format="PNG")
        img_bytes = buf.getvalue()
        img_fmt = "png"
        if len(img_bytes) > TYPESAFE_PNG_JPEG_FALLBACK_BYTES:
            jbuf = io.BytesIO()
            final_img.convert("RGB").save(jbuf, format="JPEG", quality=90)
            img_bytes = jbuf.getvalue()
            img_fmt = "jpeg"
    else:  # pragma: no cover
        cw, ch, cropped_rgb = _crop_rgb_fallback(frame, x0, y0, x1, y1)
        if np is not None:
            arr = np.frombuffer(cropped_rgb, dtype=np.uint8).reshape((ch, cw, 3))
            if (content_w, content_h) != (cw, ch):
                ys = np.clip((np.arange(content_h) * ch / content_h).astype(np.int64), 0, ch - 1)
                xs = np.clip((np.arange(content_w) * cw / content_w).astype(np.int64), 0, cw - 1)
                arr = arr[np.ix_(ys, xs)]
            if (out_w, out_h) != (content_w, content_h):
                canvas = np.full((out_h, out_w, 3), 128, dtype=np.uint8)
                canvas[pad_y : pad_y + content_h, pad_x : pad_x + content_w] = arr
                arr = canvas
            img_bytes = _encode_rgb_png(out_w, out_h, arr.tobytes())
        else:
            pad_l = b"\x80\x80\x80" * pad_x
            pad_r = b"\x80\x80\x80" * (out_w - content_w - pad_x)
            blank_row = b"\x80\x80\x80" * out_w
            rows = [blank_row] * pad_y
            columns = [3 * (x * cw // content_w) for x in range(content_w)]
            stride = cw * 3
            for y in range(content_h):
                offset = (y * ch // content_h) * stride
                source = cropped_rgb[offset : offset + stride]
                row = source if cw == content_w else b"".join(source[x : x + 3] for x in columns)
                rows.append(pad_l + row + pad_r)
            rows.extend([blank_row] * (out_h - content_h - pad_y))
            img_bytes = _encode_rgb_png(out_w, out_h, b"".join(rows))
        img_fmt = "png"

    return PreparedImage(
        image_bytes=img_bytes,
        image_format=img_fmt,
        orig_width=orig_w,
        orig_height=orig_h,
        out_width=out_w,
        out_height=out_h,
        pad_x=pad_x,
        pad_y=pad_y,
        content_width=content_w,
        content_height=content_h,
        crop_bbox_norm=crop_norm,
    )


def render_annotated_crop_png(
    frame: FrameSnapshot,
    crop_bbox_norm: Sequence[int],
    highlight_box_norm: Optional[Sequence[int]] = None,
    label: str = "",
) -> bytes:
    """Render a PNG crop of ``crop_bbox_norm`` with an optional highlighted candidate box."""
    crop_norm = validate_bbox(list(crop_bbox_norm), name="crop_bbox")
    x0, y0, x1, y1 = bbox_to_pixels(crop_norm, frame.width, frame.height)
    cw, ch = x1 - x0, y1 - y0

    if Image is not None:
        img = Image.frombytes("RGB", (frame.width, frame.height), frame.rgb_bytes).crop((x0, y0, x1, y1))
        if highlight_box_norm is not None:
            hx0, hy0, hx1, hy1 = bbox_to_pixels(highlight_box_norm, frame.width, frame.height)
            rx0 = max(0, min(cw - 1, hx0 - x0))
            ry0 = max(0, min(ch - 1, hy0 - y0))
            rx1 = max(rx0 + 1, min(cw - 1, hx1 - x0))
            ry1 = max(ry0 + 1, min(ch - 1, hy1 - y0))
            draw = ImageDraw.Draw(img)
            draw.rectangle([rx0, ry0, rx1, ry1], outline=(255, 64, 64), width=3)
            if label:
                tx = max(0, rx0)
                ty = max(0, ry0 - 14)
                draw.text((tx, ty), label[:64], fill=(255, 64, 64))
        buf = io.BytesIO()
        img.save(buf, format="PNG")
        return buf.getvalue()

    _, _, cropped_rgb = _crop_rgb_fallback(frame, x0, y0, x1, y1)
    return _encode_rgb_png(cw, ch, cropped_rgb)


def _extract_roi_rgb_array(frame: FrameSnapshot, roi: Optional[Sequence[int]] = None):
    if np is None:
        return None
    arr = np.frombuffer(frame.rgb_bytes, dtype=np.uint8).reshape((frame.height, frame.width, 3))
    if roi is not None:
        norm = validate_bbox(list(roi), name="roi")
        x0, y0, x1, y1 = bbox_to_pixels(norm, frame.width, frame.height)
        return arr[y0:y1, x0:x1]
    return arr


def compute_pixel_diff_ratio(
    frame_a: FrameSnapshot,
    frame_b: FrameSnapshot,
    roi: Optional[Sequence[int]] = None,
    channel_threshold: int = 8,
) -> float:
    """Fraction of pixels in ``roi`` whose RGB channels changed by more than ``channel_threshold``."""
    if frame_a.width != frame_b.width or frame_a.height != frame_b.height:
        return 1.0
    if frame_a.rgb_bytes == frame_b.rgb_bytes:
        return 0.0

    if np is not None:
        a = _extract_roi_rgb_array(frame_a, roi).astype(np.int16)
        b = _extract_roi_rgb_array(frame_b, roi).astype(np.int16)
        diff = np.abs(a - b).max(axis=2) > int(channel_threshold)
        return float(np.mean(diff))

    norm = validate_bbox(list(roi), name="roi") if roi is not None else [0, 0, 1000, 1000]
    x0, y0, x1, y1 = bbox_to_pixels(norm, frame_a.width, frame_a.height)
    stride = frame_a.width * 3
    changed = 0
    total = (x1 - x0) * (y1 - y0)
    thr = int(channel_threshold)
    for y in range(y0, y1):
        row_off = y * stride
        for x in range(x0, x1):
            p = row_off + x * 3
            if (
                abs(frame_a.rgb_bytes[p] - frame_b.rgb_bytes[p]) > thr
                or abs(frame_a.rgb_bytes[p + 1] - frame_b.rgb_bytes[p + 1]) > thr
                or abs(frame_a.rgb_bytes[p + 2] - frame_b.rgb_bytes[p + 2]) > thr
            ):
                changed += 1
    return changed / max(1, total)


def compute_ssim(
    frame_a: FrameSnapshot,
    frame_b: FrameSnapshot,
    roi: Optional[Sequence[int]] = None,
) -> float:
    """Compute structural similarity (SSIM in ``[0.0, 1.0]``) over ``roi`` (or full screen)."""
    if frame_a.width != frame_b.width or frame_a.height != frame_b.height:
        return 0.0
    if frame_a.rgb_bytes == frame_b.rgb_bytes:
        return 1.0

    c1 = (0.01 * 255.0) ** 2
    c2 = (0.03 * 255.0) ** 2

    if np is not None:
        a_rgb = _extract_roi_rgb_array(frame_a, roi)
        b_rgb = _extract_roi_rgb_array(frame_b, roi)
        if a_rgb.shape == b_rgb.shape and np.array_equal(a_rgb, b_rgb):
            return 1.0

        # Downsample large ROIs to <= 320px max side for ~1-2ms latency while keeping structure.
        h, w = a_rgb.shape[:2]
        stride = max(1, int(math.ceil(max(h, w) / 320.0)))
        if stride > 1:
            a_rgb = a_rgb[::stride, ::stride]
            b_rgb = b_rgb[::stride, ::stride]

        weights = np.array([0.299, 0.587, 0.114], dtype=np.float64)
        x = np.tensordot(a_rgb.astype(np.float64), weights, axes=([2], [0]))
        y = np.tensordot(b_rgb.astype(np.float64), weights, axes=([2], [0]))

        win = min(7, x.shape[0], x.shape[1])
        if uniform_filter is not None and win >= 3:
            mu_x = uniform_filter(x, size=win, mode="reflect")
            mu_y = uniform_filter(y, size=win, mode="reflect")
            sigma_x2 = np.maximum(0.0, uniform_filter(x * x, size=win, mode="reflect") - mu_x * mu_x)
            sigma_y2 = np.maximum(0.0, uniform_filter(y * y, size=win, mode="reflect") - mu_y * mu_y)
            sigma_xy = uniform_filter(x * y, size=win, mode="reflect") - mu_x * mu_y
            num = (2.0 * mu_x * mu_y + c1) * (2.0 * sigma_xy + c2)
            den = (mu_x * mu_x + mu_y * mu_y + c1) * (sigma_x2 + sigma_y2 + c2)
            ssim_val = float(np.mean(num / np.maximum(den, 1e-12)))
        else:
            mu_x = float(np.mean(x))
            mu_y = float(np.mean(y))
            sigma_x2 = float(np.var(x))
            sigma_y2 = float(np.var(y))
            sigma_xy = float(np.mean((x - mu_x) * (y - mu_y)))
            num = (2.0 * mu_x * mu_y + c1) * (2.0 * sigma_xy + c2)
            den = (mu_x * mu_x + mu_y * mu_y + c1) * (sigma_x2 + sigma_y2 + c2)
            ssim_val = num / max(den, 1e-12)
        return max(0.0, min(1.0, float(ssim_val)))

    # Pure-Python fallback
    norm = validate_bbox(list(roi), name="roi") if roi is not None else [0, 0, 1000, 1000]
    x0, y0, x1, y1 = bbox_to_pixels(norm, frame_a.width, frame_a.height)
    lum_a: List[float] = []
    lum_b: List[float] = []
    stride_bytes = frame_a.width * 3
    for yy in range(y0, y1):
        row_off = yy * stride_bytes
        for xx in range(x0, x1):
            p = row_off + xx * 3
            la = 0.299 * frame_a.rgb_bytes[p] + 0.587 * frame_a.rgb_bytes[p + 1] + 0.114 * frame_a.rgb_bytes[p + 2]
            lb = 0.299 * frame_b.rgb_bytes[p] + 0.587 * frame_b.rgb_bytes[p + 1] + 0.114 * frame_b.rgb_bytes[p + 2]
            lum_a.append(la)
            lum_b.append(lb)
    n = max(1, len(lum_a))
    mu_a = sum(lum_a) / n
    mu_b = sum(lum_b) / n
    var_a = sum((v - mu_a) ** 2 for v in lum_a) / n
    var_b = sum((v - mu_b) ** 2 for v in lum_b) / n
    cov = sum((va - mu_a) * (vb - mu_b) for va, vb in zip(lum_a, lum_b)) / n
    num = (2.0 * mu_a * mu_b + c1) * (2.0 * cov + c2)
    den = (mu_a * mu_a + mu_b * mu_b + c1) * (var_a + var_b + c2)
    return max(0.0, min(1.0, num / max(den, 1e-12)))


class SystemOneClient:
    """Thin stdlib REST client for TypeSafe's ``/v1/systemone`` endpoint."""

    def __init__(
        self,
        api_key: str,
        model: str = DEFAULT_SYSTEM_ONE_MODEL,
        base_url: str = TYPESAFE_SYSTEM_ONE_URL,
        timeout: float = 30.0,
        use_multipart: bool = True,
    ) -> None:
        if not api_key or not str(api_key).strip():
            raise ValueError("TypeSafe System One API key must not be empty")
        self.api_key = str(api_key).strip()
        self.model = str(model or DEFAULT_SYSTEM_ONE_MODEL).strip()
        self.base_url = str(base_url or TYPESAFE_SYSTEM_ONE_URL).strip()
        self.timeout = float(timeout)
        self.use_multipart = bool(use_multipart)

    def _models_url(self) -> str:
        parsed = urllib.parse.urlsplit(self.base_url)
        return urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, "/v1/models", "", ""))

    def list_models(self) -> Dict[str, Any]:
        req = urllib.request.Request(
            self._models_url(),
            headers={"Authorization": f"Bearer {self.api_key}"},
            method="GET",
        )
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            try:
                body = e.read().decode("utf-8", errors="replace")
                code = e.code
            finally:
                e.close()
            raise SystemOneApiError(code, body) from None
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            raise SystemOneApiError(None, f"Network error: {e}") from None

    @staticmethod
    def _needs_images_header(
        questions: Dict[str, Dict[str, Any]],
        image_assets: Optional[Dict[str, Tuple[bytes, str]]],
    ) -> bool:
        if image_assets:
            return True
        return any(str(q.get("type", "")).lower() == "box" for q in questions.values() if isinstance(q, dict))

    def _encode_multipart(
        self,
        req_payload: Dict[str, Any],
        image_assets: Dict[str, Tuple[bytes, str]],
    ) -> Tuple[bytes, str]:
        boundary = f"----typesafe-{uuid.uuid4().hex}"
        assets_map: Dict[str, Dict[str, Any]] = {}
        file_parts: List[Tuple[str, bytes, str, str]] = []

        for idx, (ref, (raw_bytes, fmt)) in enumerate(image_assets.items()):
            norm_fmt = str(fmt or "png").lower()
            if norm_fmt == "jpg":
                norm_fmt = "jpeg"
            part_name = f"img_{idx}"
            assets_map[ref] = {"type": "image", "part": part_name}
            mime = "image/jpeg" if norm_fmt == "jpeg" else "image/png"
            ext = "jpg" if norm_fmt == "jpeg" else "png"
            file_parts.append((part_name, raw_bytes, mime, f"{part_name}.{ext}"))

        payload = dict(req_payload)
        payload["assets"] = assets_map

        chunks: List[bytes] = [
            (
                f"--{boundary}\r\n"
                f'Content-Disposition: form-data; name="request"\r\n'
                f"Content-Type: application/json\r\n\r\n"
            ).encode("utf-8")
            + json.dumps(payload, separators=(",", ":")).encode("utf-8")
            + b"\r\n"
        ]
        for part_name, raw_bytes, mime, filename in file_parts:
            chunks.append(
                (
                    f"--{boundary}\r\n"
                    f'Content-Disposition: form-data; name="{part_name}"; filename="{filename}"\r\n'
                    f"Content-Type: {mime}\r\n\r\n"
                ).encode("utf-8")
                + raw_bytes
                + b"\r\n"
            )
        chunks.append(f"--{boundary}--\r\n".encode("utf-8"))
        return b"".join(chunks), f"multipart/form-data; boundary={boundary}"

    def _encode_json(
        self,
        req_payload: Dict[str, Any],
        image_assets: Optional[Dict[str, Tuple[bytes, str]]],
    ) -> Tuple[bytes, str]:
        payload = dict(req_payload)
        if image_assets:
            assets_map: Dict[str, Dict[str, Any]] = {}
            for ref, (raw_bytes, fmt) in image_assets.items():
                norm_fmt = str(fmt or "png").lower()
                if norm_fmt == "jpeg":
                    norm_fmt = "jpeg"
                elif norm_fmt not in ("png", "jpg"):
                    norm_fmt = "png"
                assets_map[ref] = {
                    "type": "image",
                    "format": norm_fmt,
                    "data_base64": base64.b64encode(raw_bytes).decode("ascii"),
                }
            payload["assets"] = assets_map
        return json.dumps(payload, separators=(",", ":")).encode("utf-8"), "application/json"

    def query(
        self,
        state: Any,
        questions: Dict[str, Dict[str, Any]],
        image_assets: Optional[Dict[str, Tuple[bytes, str]]] = None,
        model: Optional[str] = None,
        timeout: Optional[float] = None,
    ) -> Dict[str, Any]:
        """Evaluate ``questions`` in parallel against ``state`` via ``POST /v1/systemone``."""
        if not questions:
            raise ValueError("System One query requires at least one question")
        deadline = time.monotonic() + timeout if timeout is not None else None

        req_payload: Dict[str, Any] = {
            "model": str(model or self.model),
            "state": state,
            "questions": questions,
        }
        use_images_hdr = self._needs_images_header(questions, image_assets)
        if image_assets and self.use_multipart:
            body, content_type = self._encode_multipart(req_payload, image_assets)
        else:
            body, content_type = self._encode_json(req_payload, image_assets)

        headers: Dict[str, str] = {
            "Authorization": f"Bearer {self.api_key}",
            "Content-Type": content_type,
        }
        if use_images_hdr:
            headers[TYPESAFE_IMAGES_HEADER] = "true"

        for attempt in range(3):
            req = urllib.request.Request(
                self.base_url,
                data=body,
                headers=headers,
                method="POST",
            )
            remaining = deadline - time.monotonic() if deadline is not None else self.timeout
            if remaining <= 0:
                raise TimeoutError("System One query exceeded the caller's deadline")
            try:
                with urllib.request.urlopen(req, timeout=min(self.timeout, remaining)) as resp:
                    raw_resp = resp.read().decode("utf-8")
                try:
                    parsed = json.loads(raw_resp)
                except json.JSONDecodeError as e:
                    raise SystemOneApiError(None, f"Invalid JSON response from System One API: {e}") from None
                if not isinstance(parsed, dict) or not isinstance(parsed.get("answers"), dict):
                    raise SystemOneApiError(None, f"Unexpected System One response structure: {raw_resp[:300]}")
                return parsed
            except urllib.error.HTTPError as e:
                try:
                    err_text = e.read().decode("utf-8", errors="replace")
                    code = e.code
                finally:
                    e.close()
                api_err = SystemOneApiError(code, err_text)
                if not api_err.is_retryable or attempt == 2:
                    raise api_err from None
            except OSError as e:
                if deadline is not None and time.monotonic() >= deadline:
                    raise TimeoutError("System One query exceeded the caller's deadline") from None
                if attempt == 2:
                    raise SystemOneApiError(None, f"Network error: {e}") from None
            backoff = 0.5 * (2**attempt)
            if deadline is not None and deadline - time.monotonic() <= backoff:
                raise TimeoutError("System One retry would exceed the caller's deadline")
            time.sleep(backoff)

        raise SystemOneApiError(None, "System One API failed after retries")
