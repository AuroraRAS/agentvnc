#!/usr/bin/env python3
"""Standalone test client for TigerVNC's external agent Unix Domain Socket bridge."""

import argparse
import base64
import json
import os
import socket
import struct
import sys
import time
import urllib.error
import urllib.request
import zlib


MAX_HEADER_BYTES = 1024 * 1024
MAX_PAYLOAD_BYTES = 256 * 1024 * 1024


def _recv_exact(sock: socket.socket, n: int) -> bytes:
    chunks = []
    remaining = n
    while remaining > 0:
        chunk = sock.recv(remaining)
        if not chunk:
            raise EOFError(f"Socket closed while reading {n} bytes ({remaining} remaining)")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


class AgentClient:
    """Framed JSON + binary payload client over TigerVNC AgentSocket."""

    def __init__(self, socket_path: str, timeout: float = 10.0) -> None:
        self.socket_path = socket_path
        self.timeout = timeout
        self.sock = None
        self._next_id = 1

    def __enter__(self) -> "AgentClient":
        self.connect()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()

    def connect(self) -> None:
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.socket_path)

    def close(self) -> None:
        if self.sock is not None:
            try:
                self.sock.close()
            finally:
                self.sock = None

    def request(self, cmd: str, payload: bytes = b"", **fields):
        if self.sock is None:
            raise RuntimeError("Client socket is not connected")
        req_id = self._next_id
        self._next_id += 1

        header = {"cmd": cmd, "id": req_id}
        header.update(fields)
        header_bytes = json.dumps(header, separators=(",", ":")).encode("utf-8")

        frame = struct.pack("!II", len(header_bytes), len(payload)) + header_bytes + payload
        self.sock.sendall(frame)

        len_buf = _recv_exact(self.sock, 8)
        resp_hdr_len, resp_pay_len = struct.unpack("!II", len_buf)
        if resp_hdr_len == 0 or resp_hdr_len > MAX_HEADER_BYTES:
            raise ValueError(f"Invalid response header length: {resp_hdr_len}")
        if resp_pay_len > MAX_PAYLOAD_BYTES:
            raise ValueError(f"Invalid response payload length: {resp_pay_len}")

        resp_hdr_raw = _recv_exact(self.sock, resp_hdr_len)
        resp_payload = _recv_exact(self.sock, resp_pay_len) if resp_pay_len > 0 else b""
        resp_hdr = json.loads(resp_hdr_raw.decode("utf-8"))
        if resp_hdr.get("id") != req_id:
            raise AssertionError(f"Mismatched response id: expected {req_id}, got {resp_hdr.get('id')}")
        return resp_hdr, resp_payload


def write_png(path: str, width: int, height: int, channels: int, pixels: bytes) -> None:
    try:
        from PIL import Image  # type: ignore

        mode = "RGBA" if channels == 4 else "RGB"
        img = Image.frombytes(mode, (width, height), pixels)
        img.save(path, format="PNG")
        return
    except ImportError:
        pass

    # Pure-stdlib PNG encoder fallback
    def _chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack("!I", len(data)) + tag + data + struct.pack("!I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    color_type = 6 if channels == 4 else 2
    ihdr = struct.pack("!IIBBBBB", width, height, 8, color_type, 0, 0, 0)
    stride = width * channels
    raw_rows = bytearray()
    for y in range(height):
        raw_rows.append(0)
        start = y * stride
        raw_rows.extend(pixels[start : start + stride])
    idat = zlib.compress(bytes(raw_rows), level=6)
    png_bytes = b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr) + _chunk(b"IDAT", idat) + _chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png_bytes)


def verify_gemini_vision(key_file: str, png_path: str) -> str:
    with open(key_file, "r", encoding="utf-8") as f:
        api_key = f.read().strip()
    if not api_key:
        raise ValueError("Gemini API key file is empty")

    with open(png_path, "rb") as f:
        b64_png = base64.b64encode(f.read()).decode("ascii")

    body = {
        "contents": [
            {
                "parts": [
                    {
                        "text": "Briefly describe in 1-2 sentences what is visible in this captured VNC framebuffer screenshot to confirm the image is legible and colors look normal."
                    },
                    {
                        "inline_data": {
                            "mime_type": "image/png",
                            "data": b64_png,
                        }
                    },
                ]
            }
        ]
    }
    url = (
        "https://generativelanguage.googleapis.com/v1beta/models/"
        f"gemini-2.5-flash:generateContent?key={api_key}"
    )
    req = urllib.request.Request(
        url,
        data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        err_body = e.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"Gemini API HTTP {e.code}: {err_body}") from None

    candidates = data.get("candidates") or []
    if not candidates:
        raise RuntimeError(f"Unexpected Gemini API response: {data}")
    parts = candidates[0].get("content", {}).get("parts", [])
    return "".join(p.get("text", "") for p in parts).strip()


def run_tests(args: argparse.Namespace) -> int:
    with AgentClient(args.socket) as client:
        # 1. PING
        hdr, pay = client.request("PING")
        assert hdr["ok"] is True and hdr["pong"] is True and pay == b"", f"PING failed: {hdr}"
        print("[PASS] PING")

        # 2. Wait for RFB session readiness and initial frame
        deadline = time.monotonic() + args.timeout
        status = None
        while time.monotonic() < deadline:
            hdr, _ = client.request("GET_STATUS")
            assert hdr["ok"] is True, f"GET_STATUS failed: {hdr}"
            status = hdr
            if hdr["ready"] and hdr["frame_seq"] > 0 and hdr["width"] > 0 and hdr["height"] > 0:
                break
            time.sleep(0.1)
        assert status and status["ready"] and status["frame_seq"] > 0, (
            f"Session did not become ready within {args.timeout}s: {status}"
        )
        for key in ("collaborate", "overlay", "human_action_count", "pause_reason", "pending_user_messages"):
            assert key in status, f"Missing GET_STATUS field {key!r}: {status}"
        print(
            f"[PASS] GET_STATUS ready={status['ready']} {status['width']}x{status['height']} "
            f"frame_seq={status['frame_seq']} damage={status['damage_count']} "
            f"overlay={status['overlay']} collab={status['collaborate']} "
            f"name={status['desktop_name']!r}"
        )

        # 3. GET_FRAME RGB24
        hdr_rgb, pay_rgb = client.request("GET_FRAME", format="RGB24")
        assert hdr_rgb["ok"] is True and hdr_rgb["changed"] is True, f"GET_FRAME RGB24 failed: {hdr_rgb}"
        w, h = hdr_rgb["width"], hdr_rgb["height"]
        assert hdr_rgb["bytes_per_pixel"] == 3
        assert hdr_rgb["stride"] == w * 3
        assert hdr_rgb["byte_length"] == len(pay_rgb) == w * h * 3
        assert hdr_rgb["pixel_format"]["channels"] == 3
        out_dir = os.path.dirname(os.path.abspath(args.out))
        if out_dir:
            os.makedirs(out_dir, exist_ok=True)
        write_png(args.out, w, h, 3, pay_rgb)
        nonzero_bytes = sum(1 for b in pay_rgb[::97] if b != 0)
        print(
            f"[PASS] GET_FRAME RGB24 {w}x{h} stride={hdr_rgb['stride']} "
            f"seq={hdr_rgb['frame_seq']} sample_nonzero={nonzero_bytes} -> {args.out}"
        )

        # 4. GET_FRAME if_changed_since
        hdr_same, pay_same = client.request("GET_FRAME", format="RGB24", if_changed_since=hdr_rgb["frame_seq"])
        assert hdr_same["ok"] is True, f"if_changed_since failed: {hdr_same}"
        if not hdr_same["changed"]:
            assert pay_same == b""
            print(f"[PASS] GET_FRAME if_changed_since={hdr_rgb['frame_seq']} -> changed=False (0 bytes)")
        else:
            print(f"[PASS] GET_FRAME if_changed_since -> live frame advanced to {hdr_same['frame_seq']}")

        # 5. GET_FRAME RGBA32
        hdr_rgba, pay_rgba = client.request("GET_FRAME", format="RGBA32")
        assert hdr_rgba["ok"] is True and hdr_rgba["changed"] is True, f"GET_FRAME RGBA32 failed: {hdr_rgba}"
        assert hdr_rgba["bytes_per_pixel"] == 4
        assert hdr_rgba["stride"] == w * 4
        assert hdr_rgba["byte_length"] == len(pay_rgba) == w * h * 4
        # Verify alpha channel is 0xFF on sampled pixels
        assert all(a == 255 for a in pay_rgba[3 : min(len(pay_rgba), 4000) : 4]), "RGBA32 alpha != 255"
        if hdr_rgba["frame_seq"] == hdr_rgb["frame_seq"]:
            for idx in range(0, min(w * h, 1000)):
                assert pay_rgba[idx * 4 : idx * 4 + 3] == pay_rgb[idx * 3 : idx * 3 + 3], (
                    f"Channel mismatch between RGB24 and RGBA32 at pixel {idx}"
                )
        print(f"[PASS] GET_FRAME RGBA32 {w}x{h} stride={hdr_rgba['stride']} alpha=255")

        # 6. Input injection tests
        if not args.skip_input:
            cx, cy = max(1, w // 2), max(1, h // 2)
            # Pointer move, click, drag, scroll wheel bitmask
            for x, y, mask in [
                (cx, cy, 0),
                (cx, cy, 1),
                (cx + 20, cy + 10, 1),
                (cx + 20, cy + 10, 0),
                (cx + 20, cy + 10, 8),   # wheel up
                (cx + 20, cy + 10, 0),
                (cx + 20, cy + 10, 16),  # wheel down
                (cx + 20, cy + 10, 0),
            ]:
                r, _ = client.request("POINTER_EVENT", x=x, y=y, button_mask=mask)
                assert r["ok"] is True, f"POINTER_EVENT ({x},{y},{mask}) failed: {r}"
            r_bad, _ = client.request("POINTER_EVENT", x=0, y=0, button_mask=0x8000)
            assert r_bad["ok"] is False and r_bad["error"] == "invalid_args"
            print("[PASS] POINTER_EVENT (move, click, drag, scroll, invalid mask rejected)")

            # Key event (Shift_L press & release)
            for down in (True, False):
                r, _ = client.request("KEY_EVENT", keysym=0xFFE1, down=down)
                assert r["ok"] is True, f"KEY_EVENT failed: {r}"
            if args.type_text is not None:
                r, _ = client.request("TEXT_TYPE", text=args.type_text)
                assert r["ok"] is True, f"TEXT_TYPE failed: {r}"
                print(f"[PASS] KEY_EVENT & TEXT_TYPE ({r['chars_sent']} chars)")
            else:
                r, _ = client.request("TEXT_TYPE", text="")
                assert r["ok"] is True and r["chars_sent"] == 0, f"TEXT_TYPE empty failed: {r}"
                print("[PASS] KEY_EVENT & TEXT_TYPE")

            # Clipboard set / get
            test_clip = "TigerVNC AgentBridge UTF-8 Test \u2713"
            r, _ = client.request("CLIPBOARD_SET", text=test_clip)
            assert r["ok"] is True and r["bytes"] == len(test_clip.encode("utf-8")), f"CLIPBOARD_SET failed: {r}"
            r_get, _ = client.request("CLIPBOARD_GET")
            assert r_get["ok"] is True, f"CLIPBOARD_GET failed: {r_get}"
            print(f"[PASS] CLIPBOARD_SET / CLIPBOARD_GET (remote available={r_get['available']})")

            # Pause / Resume override
            r_pause, _ = client.request("PAUSE_AGENT")
            assert r_pause["ok"] is True and r_pause["agent_paused"] is True
            r_blocked, _ = client.request("POINTER_EVENT", x=cx, y=cy, button_mask=0)
            assert r_blocked["ok"] is False and r_blocked["error"] == "agent_paused", (
                f"Expected agent_paused, got: {r_blocked}"
            )
            r_frame_paused, _ = client.request("GET_FRAME", format="RGB24")
            assert r_frame_paused["ok"] is True and r_frame_paused["agent_paused"] is True
            r_resume, _ = client.request("RESUME_AGENT")
            assert r_resume["ok"] is True and r_resume["agent_paused"] is False
            r_unblocked, _ = client.request("POINTER_EVENT", x=cx, y=cy, button_mask=0)
            assert r_unblocked["ok"] is True
            print("[PASS] PAUSE_AGENT / RESUME_AGENT preemption")

        # 7. AGENT_OVERLAY & POLL_USER_MESSAGES
        r_ov1, _ = client.request(
            "AGENT_OVERLAY",
            state="acting",
            action="click",
            intent="Test overlay click reticle",
            x=w // 2,
            y=h // 2,
            pulse=True,
            button_mask=1,
        )
        assert r_ov1["ok"] is True, f"AGENT_OVERLAY click failed: {r_ov1}"
        r_ov2, _ = client.request(
            "AGENT_OVERLAY",
            state="acting",
            action="drag_and_drop",
            intent="Test drag vector",
            x=100,
            y=100,
            end_x=250,
            end_y=200,
            pulse=True,
            button_mask=1,
        )
        assert r_ov2["ok"] is True, f"AGENT_OVERLAY drag failed: {r_ov2}"
        r_ov_clr, _ = client.request("AGENT_OVERLAY", clear=True)
        assert r_ov_clr["ok"] is True, f"AGENT_OVERLAY clear failed: {r_ov_clr}"
        r_msgs, _ = client.request("POLL_USER_MESSAGES")
        assert r_msgs["ok"] is True and isinstance(r_msgs.get("messages"), list), f"POLL_USER_MESSAGES failed: {r_msgs}"
        print("[PASS] AGENT_OVERLAY & POLL_USER_MESSAGES")

        # 8. REQUEST_REFRESH
        r_ref, _ = client.request("REQUEST_REFRESH")
        assert r_ref["ok"] is True, f"REQUEST_REFRESH failed: {r_ref}"
        deadline = time.monotonic() + 5.0
        new_seq = hdr_rgba["frame_seq"]
        while time.monotonic() < deadline:
            st, _ = client.request("GET_STATUS")
            if st["frame_seq"] > new_seq:
                new_seq = st["frame_seq"]
                break
            time.sleep(0.05)
        print(f"[PASS] REQUEST_REFRESH (frame_seq={new_seq})")

        # 8. Optional Gemini API vision test
        if args.gemini_key:
            summary = verify_gemini_vision(args.gemini_key, args.out)
            print(f"[PASS] Gemini Vision verified frame: {summary}")

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Test TigerVNC AgentSocket IPC bridge")
    parser.add_argument("--socket", required=True, help="Path to vncviewer -AgentSocket Unix socket")
    parser.add_argument("--out", default="/tmp/tigervnc_test_frame.png", help="Output PNG path")
    parser.add_argument("--timeout", type=float, default=10.0, help="Session readiness timeout (seconds)")
    parser.add_argument("--skip-input", action="store_true", help="Skip pointer/key/clipboard injection tests")
    parser.add_argument("--type-text", default=None, help="Optional non-empty string to inject via TEXT_TYPE")
    parser.add_argument("--gemini-key", default=None, help="Optional path to Gemini API key file for vision check")
    args = parser.parse_args()
    return run_tests(args)


if __name__ == "__main__":
    sys.exit(main())
