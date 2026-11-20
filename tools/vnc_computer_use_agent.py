#!/usr/bin/env python3
"""Gemini Computer Use desktop agent over TigerVNC's -AgentSocket IPC bridge.

Follows the Gemini API Computer Use guidelines:
  https://aistudio.google.com/docs/computer-use
"""

import argparse
import base64
import io
import json
import os
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zlib
from typing import Any, Dict, List, Optional, Tuple

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

from test_agent_bridge import AgentClient  # noqa: E402


INTERACTIONS_URL = "https://generativelanguage.googleapis.com/v1beta/interactions"
DEFAULT_MODEL = "gemini-4-hydrogen"
DEFAULT_VNCVIEWER = os.path.join(REPO_ROOT, "build", "vncviewer", "vncviewer")

SYSTEM_INSTRUCTION = """## RULE 1: Seek User Confirmation (USER_CONFIRMATION)

This is your first and most important check. If the next required action falls
into any of the following categories, you MUST stop immediately, and seek the
user's explicit permission.

**Procedure for Seeking Confirmation:**
* **For Consequential Actions:** Perform all preparatory steps (e.g., navigating,
  filling out forms, typing a message). You will ask for confirmation **AFTER**
  all necessary information is entered on the screen, but **BEFORE** you perform
  the final, irreversible action (e.g., before clicking "Send", "Submit",
  "Confirm Purchase", "Share").
* **For Prohibited Actions:** If the action is strictly forbidden (e.g., accepting
  legal terms, solving a CAPTCHA), you must first inform the user about the
  required action and ask for their confirmation to proceed.

**USER_CONFIRMATION Categories:**
* **Consent and Agreements:** Terms of Service, Privacy Policies, EULAs, or legally binding agreements.
* **Robot Detection:** CAPTCHAs or anti-robot checks.
* **Financial Transactions:** Purchases, money transfers, or payments.
* **Sending Communications:** Sending emails, messages, or social media posts.
* **Accessing or Modifying Sensitive Information:** Health, financial, or government records.
* **Security and Identity:** Logging into user accounts or impersonation.
* **Insurmountable Obstacles:** If you are technically unable to interact with a UI element or stuck in a loop you cannot resolve, ask the user to take over.

---

## RULE 2: Default Behavior (ACTUATE)

If an action does **NOT** fall under the conditions for `USER_CONFIRMATION`,
your default behavior is to **Actuate**.

**Actuation Means:** You MUST proactively perform all necessary steps to move
the user's request forward. Continue to actuate until you either complete the
non-consequential task or encounter a condition defined in Rule 1.

## VNC Desktop Execution Notes:
* The remote desktop is controlled over VNC. When typing text or filenames, use plain ASCII characters only.
* When creating or saving a file on the Desktop, make sure the file is saved and verify on the screenshot that it exists on the Desktop before finishing.

# Final Response Guidelines:
Write final response to the user in the following cases:
- User confirmation
- When the task is complete or you have enough information to respond to the user
"""

KEY_MAP: Dict[str, int] = {
    "backspace": 0xFF08,
    "tab": 0xFF09,
    "return": 0xFF0D,
    "enter": 0xFF0D,
    "pause": 0xFF13,
    "scroll_lock": 0xFF14,
    "scrolllock": 0xFF14,
    "sys_req": 0xFF15,
    "escape": 0xFF1B,
    "esc": 0xFF1B,
    "delete": 0xFFFF,
    "del": 0xFFFF,
    "home": 0xFF50,
    "left": 0xFF51,
    "arrowleft": 0xFF51,
    "up": 0xFF52,
    "arrowup": 0xFF52,
    "right": 0xFF53,
    "arrowright": 0xFF53,
    "down": 0xFF54,
    "arrowdown": 0xFF54,
    "page_up": 0xFF55,
    "pageup": 0xFF55,
    "pgup": 0xFF55,
    "prior": 0xFF55,
    "page_down": 0xFF56,
    "pagedown": 0xFF56,
    "pgdn": 0xFF56,
    "next": 0xFF56,
    "end": 0xFF57,
    "print": 0xFF61,
    "printscreen": 0xFF61,
    "insert": 0xFF63,
    "ins": 0xFF63,
    "menu": 0xFF67,
    "apps": 0xFF67,
    "num_lock": 0xFF7F,
    "numlock": 0xFF7F,
    "f1": 0xFFBE,
    "f2": 0xFFBF,
    "f3": 0xFFC0,
    "f4": 0xFFC1,
    "f5": 0xFFC2,
    "f6": 0xFFC3,
    "f7": 0xFFC4,
    "f8": 0xFFC5,
    "f9": 0xFFC6,
    "f10": 0xFFC7,
    "f11": 0xFFC8,
    "f12": 0xFFC9,
    "shift": 0xFFE1,
    "shift_l": 0xFFE1,
    "lshift": 0xFFE1,
    "shift_r": 0xFFE2,
    "rshift": 0xFFE2,
    "control": 0xFFE3,
    "control_l": 0xFFE3,
    "ctrl": 0xFFE3,
    "lctrl": 0xFFE3,
    "control_r": 0xFFE4,
    "rctrl": 0xFFE4,
    "caps_lock": 0xFFE5,
    "capslock": 0xFFE5,
    "meta": 0xFFE7,
    "meta_l": 0xFFE7,
    "alt": 0xFFE9,
    "alt_l": 0xFFE9,
    "lalt": 0xFFE9,
    "alt_r": 0xFFEA,
    "ralt": 0xFFEA,
    "altgr": 0xFE03,
    "super": 0xFFEB,
    "super_l": 0xFFEB,
    "win": 0xFFEB,
    "windows": 0xFFEB,
    "cmd": 0xFFEB,
    "command": 0xFFEB,
    "super_r": 0xFFEC,
    "space": 0x0020,
    "spacebar": 0x0020,
    "plus": 0x002B,
    "minus": 0x002D,
    "equal": 0x003D,
    "comma": 0x002C,
    "period": 0x002E,
    "slash": 0x002F,
    "backslash": 0x005C,
    "semicolon": 0x003B,
    "apostrophe": 0x0027,
    "grave": 0x0060,
    "bracketleft": 0x005B,
    "bracketright": 0x005D,
}

SHIFT_KEYSYMS = {0xFFE1, 0xFFE2}
MODIFIER_KEYSYMS = {0xFFE1, 0xFFE2, 0xFFE3, 0xFFE4, 0xFFE7, 0xFFE9, 0xFFEA, 0xFFEB, 0xFFEC}
SHIFTED_ASCII = set('ABCDEFGHIJKLMNOPQRSTUVWXYZ~!@#$%^&*()_+{}|:"<>?')


def encode_rgb24_png(width: int, height: int, pixels: bytes) -> bytes:
    """Encode RGB24 raw pixels to PNG bytes in memory."""
    try:
        from PIL import Image  # type: ignore

        buf = io.BytesIO()
        img = Image.frombytes("RGB", (width, height), pixels)
        img.save(buf, format="PNG")
        return buf.getvalue()
    except ImportError:
        pass

    def _chunk(tag: bytes, data: bytes) -> bytes:
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        return struct.pack("!I", len(data)) + tag + data + struct.pack("!I", crc)

    ihdr = struct.pack("!IIBBBBB", width, height, 8, 2, 0, 0, 0)
    stride = width * 3
    raw_rows = bytearray()
    for y in range(height):
        raw_rows.append(0)
        start = y * stride
        raw_rows.extend(pixels[start : start + stride])
    idat = zlib.compress(bytes(raw_rows), level=6)
    return b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr) + _chunk(b"IDAT", idat) + _chunk(b"IEND", b"")


def denormalize_x(x: float, screen_width: int) -> int:
    return max(0, min(screen_width - 1, int(round((float(x) / 1000.0) * screen_width))))


def denormalize_y(y: float, screen_height: int) -> int:
    return max(0, min(screen_height - 1, int(round((float(y) / 1000.0) * screen_height))))


def resolve_keysym(key_name: str) -> int:
    if not isinstance(key_name, str) or not key_name:
        raise ValueError(f"Invalid key name: {key_name!r}")
    norm = key_name.strip()
    lower = norm.lower().replace("-", "_").replace(" ", "_")
    if lower in KEY_MAP:
        return KEY_MAP[lower]
    if len(norm) == 1:
        cp = ord(norm)
        if 0x20 <= cp <= 0x7E:
            return cp
        raise ValueError(f"Non-ASCII key {key_name!r} is not supported over VNC KeyEvent")
    raise ValueError(f"Unsupported key name: {key_name!r}")


def summarize_for_overlay(
    text: str,
    fallback: str = "Awaiting next instruction",
    max_len: int = 72,
) -> str:
    collapsed = " ".join((text or "").split())
    if not collapsed:
        return fallback
    if len(collapsed) <= max_len:
        return collapsed
    return collapsed[: max(0, max_len - 3)] + "..."


class AgentPausedError(RuntimeError):
    """Raised when synthetic input is blocked by operator PAUSE_AGENT."""


class VncDesktopExecutor:
    """Executes Gemini ENVIRONMENT_DESKTOP actions against TigerVNC's AgentSocket."""

    def __init__(self, client: AgentClient) -> None:
        self.client = client
        self.width = 0
        self.height = 0
        self.button_mask = 0

    def wait_ready(self, timeout: float = 15.0) -> Dict[str, Any]:
        deadline = time.monotonic() + timeout
        last_status: Dict[str, Any] = {}
        while time.monotonic() < deadline:
            hdr, _ = self.client.request("GET_STATUS")
            if not hdr.get("ok"):
                raise RuntimeError(f"GET_STATUS failed: {hdr}")
            last_status = hdr
            if hdr.get("ready") and hdr.get("frame_seq", 0) > 0 and hdr.get("width", 0) > 0 and hdr.get("height", 0) > 0:
                self.width = int(hdr["width"])
                self.height = int(hdr["height"])
                return hdr
            time.sleep(0.1)
        raise TimeoutError(f"VNC session not ready within {timeout}s: {last_status}")

    def get_status(self) -> Dict[str, Any]:
        hdr, _ = self.client.request("GET_STATUS")
        if not hdr.get("ok"):
            raise RuntimeError(f"GET_STATUS failed: {hdr}")
        return hdr

    def set_overlay(
        self,
        state: str = "acting",
        action: str = "",
        intent: str = "",
        x: Optional[int] = None,
        y: Optional[int] = None,
        end_x: Optional[int] = None,
        end_y: Optional[int] = None,
        pulse: bool = False,
        button_mask: int = 0,
        clear: bool = False,
    ) -> None:
        fields: Dict[str, Any] = {}
        if clear:
            fields["clear"] = True
        else:
            fields["state"] = state
            if action:
                fields["action"] = action
            if intent:
                fields["intent"] = intent
            if x is not None and y is not None:
                fields["x"] = int(x)
                fields["y"] = int(y)
            if end_x is not None and end_y is not None:
                fields["end_x"] = int(end_x)
                fields["end_y"] = int(end_y)
            fields["pulse"] = bool(pulse)
            if button_mask:
                fields["button_mask"] = int(button_mask)
        resp, _ = self.client.request("AGENT_OVERLAY", **fields)
        if not resp.get("ok") and resp.get("error") != "not_ready":
            print(f"  [WARN] AGENT_OVERLAY failed: {resp}")

    def poll_user_messages(self) -> Tuple[List[str], int, bool]:
        resp, _ = self.client.request("POLL_USER_MESSAGES")
        if not resp.get("ok"):
            return [], 0, False
        return (
            list(resp.get("messages", [])),
            int(resp.get("human_action_count", 0)),
            bool(resp.get("agent_paused", False)),
        )

    def resume_agent(self) -> None:
        resp, _ = self.client.request("RESUME_AGENT")
        if not resp.get("ok"):
            raise RuntimeError(f"RESUME_AGENT failed: {resp}")

    def wait_until_resumed(self) -> Tuple[int, List[str]]:
        st = self.get_status()
        start_human = int(st.get("human_action_count", 0))
        print("  [COLLAB] Agent paused for human takeover; waiting for resume (Ctrl+Alt+P)...")
        self.set_overlay(
            state="waiting_human",
            action="paused",
            intent="Human collaborator in control - resume when ready",
        )
        end_human = start_human
        connected_ok = True
        while True:
            try:
                st = self.get_status()
            except (EOFError, ConnectionError, BrokenPipeError, OSError) as e:
                print(f"  [COLLAB] Viewer connection closed while paused ({e}).")
                connected_ok = False
                break
            end_human = int(st.get("human_action_count", end_human))
            if not st.get("agent_paused") or not st.get("connected"):
                connected_ok = bool(st.get("connected"))
                break
            time.sleep(0.25)
        notes: List[str] = []
        if connected_ok:
            try:
                notes, end_human, _ = self.poll_user_messages()
            except (EOFError, ConnectionError, BrokenPipeError, OSError) as e:
                print(f"  [COLLAB] Viewer connection closed while polling notes ({e}).")
        print(f"  [COLLAB] Resumed (human_actions={max(0, end_human - start_human)}, notes={len(notes)})")
        return max(0, end_human - start_human), notes

    def wait_for_next_instruction(
        self,
        viewer_proc: Optional[subprocess.Popen] = None,
        overlay_intent: str = "Awaiting your next instruction",
        poll_interval: float = 0.25,
        use_stdin: Optional[bool] = None,
    ) -> Optional[Tuple[str, int]]:
        try:
            st = self.get_status()
        except (EOFError, ConnectionError, BrokenPipeError, OSError) as e:
            print(f"\n[COLLAB] Viewer disconnected ({e}); ending session.")
            return None

        if not st.get("connected"):
            print("\n[COLLAB] VNC connection closed; ending session.")
            return None

        start_human = int(st.get("human_action_count", 0))
        self.set_overlay(
            state="waiting_user",
            action="waiting",
            intent=overlay_intent,
        )

        stdin_enabled = (
            bool(sys.stdin is not None and sys.stdin.isatty())
            if use_stdin is None
            else bool(use_stdin)
        )

        print("\n[COLLAB] Waiting for your next instruction:")
        print("  - In vncviewer: press Ctrl+Alt+N (or right-click -> 'Send note to agent...')")
        if stdin_enabled:
            print("  - Or type your next instruction below (type 'exit' or '/exit' to quit).")
            print("collab> ", end="", flush=True)
        else:
            print("  - Send '/exit' or '/quit' as a note, or close vncviewer / press Ctrl+C to end.")

        try:
            while True:
                if viewer_proc is not None and viewer_proc.poll() is not None:
                    print(f"\n[COLLAB] vncviewer exited (code {viewer_proc.returncode}); ending session.")
                    return None

                try:
                    st = self.get_status()
                except (EOFError, ConnectionError, BrokenPipeError, OSError) as e:
                    print(f"\n[COLLAB] Viewer disconnected ({e}); ending session.")
                    return None

                if not st.get("connected"):
                    print("\n[COLLAB] VNC connection closed; ending session.")
                    return None

                if int(st.get("pending_user_messages", 0)) > 0:
                    notes, end_human, paused = self.poll_user_messages()
                    cleaned = [str(n).strip() for n in notes if str(n).strip()]
                    if any(n.lower() in ("/exit", "/quit") for n in cleaned):
                        print("\n[COLLAB] Received exit command; ending session.")
                        return None
                    if cleaned:
                        instruction = "\n".join(cleaned)
                        if paused:
                            self.resume_agent()
                        human_delta = max(0, end_human - start_human)
                        print(f"\n[COLLAB] Received instruction from vncviewer: {instruction}")
                        return instruction, human_delta

                if stdin_enabled:
                    readable, _, _ = select.select([sys.stdin], [], [], poll_interval)
                    if readable:
                        line = sys.stdin.readline()
                        if line == "":
                            print("\n[COLLAB] Stdin EOF received; ending session.")
                            return None
                        instruction = line.strip()
                        if not instruction:
                            print("collab> ", end="", flush=True)
                            continue
                        if instruction.lower() in ("exit", "quit", "/exit", "/quit"):
                            print("\n[COLLAB] Exit requested; ending session.")
                            return None

                        try:
                            st_now = self.get_status()
                        except (EOFError, ConnectionError, BrokenPipeError, OSError) as e:
                            print(f"\n[COLLAB] Viewer disconnected ({e}); ending session.")
                            return None

                        if not st_now.get("connected"):
                            print("\n[COLLAB] VNC connection closed; ending session.")
                            return None

                        paused = bool(st_now.get("agent_paused"))
                        end_human = int(st_now.get("human_action_count", start_human))
                        if int(st_now.get("pending_user_messages", 0)) > 0:
                            notes, end_human, paused = self.poll_user_messages()
                            cleaned = [str(n).strip() for n in notes if str(n).strip()]
                            if any(n.lower() in ("/exit", "/quit") for n in cleaned):
                                print("\n[COLLAB] Received exit command; ending session.")
                                return None
                            if cleaned:
                                instruction = instruction + "\n" + "\n".join(cleaned)

                        if paused:
                            self.resume_agent()
                        human_delta = max(0, end_human - start_human)
                        print(f"\n[COLLAB] Received instruction from terminal: {instruction}")
                        return instruction, human_delta
                else:
                    time.sleep(poll_interval)
        except KeyboardInterrupt:
            print("\n[COLLAB] Interrupted by user; ending collaborative session.")
            return None

    def _pointer(self, x: int, y: int, mask: int) -> None:
        resp, _ = self.client.request("POINTER_EVENT", x=int(x), y=int(y), button_mask=int(mask))
        if not resp.get("ok"):
            if resp.get("error") == "agent_paused":
                raise AgentPausedError(resp.get("message", "Agent input paused by operator"))
            raise RuntimeError(f"POINTER_EVENT failed: {resp}")

    def _key(self, keysym: int, down: bool) -> None:
        resp, _ = self.client.request("KEY_EVENT", keysym=int(keysym), down=bool(down))
        if not resp.get("ok"):
            if resp.get("error") == "agent_paused":
                raise AgentPausedError(resp.get("message", "Agent input paused by operator"))
            raise RuntimeError(f"KEY_EVENT failed: {resp}")

    def _type_char(self, ch: str) -> None:
        resp, _ = self.client.request("TEXT_TYPE", text=ch)
        if not resp.get("ok"):
            if resp.get("error") == "agent_paused":
                raise AgentPausedError(resp.get("message", "Agent input paused by operator"))
            raise RuntimeError(f"TEXT_TYPE failed: {resp}")

    def _click_px(self, x: int, y: int, btn_bit: int = 1, count: int = 1) -> None:
        self._pointer(x, y, 0)
        time.sleep(0.04)
        for idx in range(count):
            self._pointer(x, y, btn_bit)
            time.sleep(0.05)
            self._pointer(x, y, 0)
            if idx + 1 < count:
                time.sleep(0.08)
        self.button_mask = 0

    def capture_png(self, settle_delay: float = 0.45, max_wait: float = 1.2) -> Tuple[bytes, Dict[str, Any]]:
        if settle_delay > 0:
            time.sleep(settle_delay)

        deadline = time.monotonic() + max_wait
        last_seq = None
        stable_since = time.monotonic()
        while time.monotonic() < deadline:
            st, _ = self.client.request("GET_STATUS")
            if st.get("ok") and st.get("ready"):
                seq = st.get("frame_seq")
                if seq != last_seq:
                    last_seq = seq
                    stable_since = time.monotonic()
                elif not st.get("updating") and (time.monotonic() - stable_since) >= 0.25:
                    break
            time.sleep(0.08)

        hdr, pixels = self.client.request("GET_FRAME", format="RGB24")
        if not hdr.get("ok") or not hdr.get("changed"):
            raise RuntimeError(f"GET_FRAME failed: {hdr}")
        self.width = int(hdr["width"])
        self.height = int(hdr["height"])
        png_bytes = encode_rgb24_png(self.width, self.height, pixels)
        return png_bytes, hdr

    def execute(self, fname: str, args: Dict[str, Any]) -> Dict[str, Any]:
        if fname in ("click", "double_click", "triple_click", "middle_click", "right_click", "mouse_down", "mouse_up", "move"):
            px = denormalize_x(args["x"], self.width)
            py = denormalize_y(args["y"], self.height)
            if fname == "click":
                self._click_px(px, py, btn_bit=1, count=1)
            elif fname == "double_click":
                self._click_px(px, py, btn_bit=1, count=2)
            elif fname == "triple_click":
                self._click_px(px, py, btn_bit=1, count=3)
            elif fname == "middle_click":
                self._click_px(px, py, btn_bit=2, count=1)
            elif fname == "right_click":
                self._click_px(px, py, btn_bit=4, count=1)
                time.sleep(0.30)
            elif fname == "mouse_down":
                self.button_mask = 1
                self._pointer(px, py, self.button_mask)
            elif fname == "mouse_up":
                self.button_mask = 0
                self._pointer(px, py, 0)
            elif fname == "move":
                self._pointer(px, py, self.button_mask)
                time.sleep(0.25)
            return {"status": "ok", "pixel_x": px, "pixel_y": py}

        if fname == "drag_and_drop":
            sx = denormalize_x(args["start_x"], self.width)
            sy = denormalize_y(args["start_y"], self.height)
            ex = denormalize_x(args["end_x"], self.width)
            ey = denormalize_y(args["end_y"], self.height)
            self._pointer(sx, sy, 0)
            time.sleep(0.05)
            self._pointer(sx, sy, 1)
            steps = 10
            for i in range(1, steps + 1):
                ix = int(round(sx + (ex - sx) * (i / steps)))
                iy = int(round(sy + (ey - sy) * (i / steps)))
                self._pointer(ix, iy, 1)
                time.sleep(0.025)
            self._pointer(ex, ey, 0)
            self.button_mask = 0
            return {"status": "ok", "start": [sx, sy], "end": [ex, ey]}

        if fname == "scroll":
            px = denormalize_x(args["x"], self.width)
            py = denormalize_y(args["y"], self.height)
            direction = str(args.get("direction", "down")).lower()
            magnitude = int(args.get("magnitude_in_pixels", 300))
            wheel_bits = {"up": 8, "down": 16, "left": 32, "right": 64}
            if direction not in wheel_bits:
                raise ValueError(f"Unsupported scroll direction: {direction!r}")
            bit = wheel_bits[direction]
            pulses = max(1, min(15, int(round(magnitude / 100.0))))
            self._pointer(px, py, 0)
            time.sleep(0.04)
            for _ in range(pulses):
                self._pointer(px, py, bit)
                self._pointer(px, py, 0)
                time.sleep(0.03)
            self.button_mask = 0
            return {"status": "ok", "pulses": pulses}

        if fname == "type":
            if "x" in args and "y" in args and args["x"] is not None and args["y"] is not None:
                px = denormalize_x(args["x"], self.width)
                py = denormalize_y(args["y"], self.height)
                self._click_px(px, py, btn_bit=1, count=1)
                time.sleep(0.15)
            text = str(args.get("text", ""))
            press_enter = bool(args.get("press_enter", False))
            shift_held = False
            try:
                for ch in text:
                    needs_shift = ch in SHIFTED_ASCII
                    if needs_shift and not shift_held:
                        self._key(0xFFE1, True)
                        time.sleep(0.03)
                        shift_held = True
                    elif not needs_shift and shift_held:
                        self._key(0xFFE1, False)
                        time.sleep(0.03)
                        shift_held = False
                    self._type_char(ch)
                    time.sleep(0.035)
            finally:
                if shift_held:
                    self._key(0xFFE1, False)
                    time.sleep(0.03)
            if press_enter:
                self._key(0xFF0D, True)
                time.sleep(0.04)
                self._key(0xFF0D, False)
            return {"status": "ok", "chars_typed": len(text), "press_enter": press_enter}

        if fname == "press_key":
            raw_key = str(args["key"])
            ks = resolve_keysym(raw_key)
            needs_shift = len(raw_key) == 1 and raw_key in SHIFTED_ASCII
            if needs_shift:
                self._key(0xFFE1, True)
                time.sleep(0.03)
            try:
                self._key(ks, True)
                time.sleep(0.04)
                self._key(ks, False)
            finally:
                if needs_shift:
                    time.sleep(0.03)
                    self._key(0xFFE1, False)
            return {"status": "ok", "keysym": ks}

        if fname == "key_down":
            ks = resolve_keysym(args["key"])
            self._key(ks, True)
            return {"status": "ok", "keysym": ks}

        if fname == "key_up":
            ks = resolve_keysym(args["key"])
            self._key(ks, False)
            return {"status": "ok", "keysym": ks}

        if fname == "hotkey":
            raw_keys = args["keys"]
            if isinstance(raw_keys, str):
                key_names = [k.strip() for k in raw_keys.split("+") if k.strip()]
            else:
                key_names = [str(k).strip() for k in raw_keys if str(k).strip()]
            if not key_names:
                raise ValueError("hotkey requires at least one key")

            prelim = [resolve_keysym(k) for k in key_names]
            has_shift = any(ks in SHIFT_KEYSYMS for ks in prelim)
            keysyms: List[int] = []
            for k, ks in zip(key_names, prelim):
                if not has_shift and len(k) == 1 and "A" <= k <= "Z":
                    ks = ord(k.lower())
                keysyms.append(ks)

            for ks in keysyms:
                self._key(ks, True)
                time.sleep(0.04)
            time.sleep(0.06)
            for ks in reversed(keysyms):
                self._key(ks, False)
                time.sleep(0.04)
            return {"status": "ok", "keysyms": keysyms}

        if fname == "wait":
            secs = min(max(float(args.get("seconds", 1.0)), 0.1), 30.0)
            time.sleep(secs)
            return {"status": "ok", "waited_seconds": secs}

        if fname == "take_screenshot":
            return {"status": "ok"}

        return {"error": f"Unsupported desktop action: {fname}"}


class InteractionsClient:
    """Thin REST client for Gemini v1beta Interactions API with computer_use."""

    def __init__(self, api_key: str, model: str, tools: List[Dict[str, Any]], system_instruction: str) -> None:
        self.api_key = api_key
        self.model = model
        self.tools = tools
        self.system_instruction = system_instruction

    def create(self, input_payload: Any, previous_interaction_id: Optional[str] = None) -> Dict[str, Any]:
        body: Dict[str, Any] = {
            "model": self.model,
            "input": input_payload,
            "tools": self.tools,
            "system_instruction": self.system_instruction,
        }
        if previous_interaction_id:
            body["previous_interaction_id"] = previous_interaction_id

        encoded = json.dumps(body).encode("utf-8")
        last_err: Optional[Exception] = None
        for attempt in range(3):
            req = urllib.request.Request(
                INTERACTIONS_URL,
                data=encoded,
                headers={
                    "Content-Type": "application/json",
                    "x-goog-api-key": self.api_key,
                },
                method="POST",
            )
            try:
                with urllib.request.urlopen(req, timeout=60) as resp:
                    return json.loads(resp.read().decode("utf-8"))
            except urllib.error.HTTPError as e:
                err_text = e.read().decode("utf-8", errors="replace")
                if e.code in (429, 500, 502, 503, 504) and attempt < 2:
                    last_err = RuntimeError(f"Interactions API HTTP {e.code}: {err_text}")
                    time.sleep(1.5 * (2**attempt))
                    continue
                raise RuntimeError(f"Interactions API HTTP {e.code}: {err_text}") from None
            except urllib.error.URLError as e:
                if attempt < 2:
                    last_err = e
                    time.sleep(1.5 * (2**attempt))
                    continue
                raise
        raise RuntimeError(f"Interactions API failed after retries: {last_err}")


def handle_safety_decision(
    safety_decision: Dict[str, Any],
    auto_confirm: bool,
) -> Tuple[str, bool]:
    """Process model safety_decision per Gemini Computer Use guidelines.

    Returns (status: 'ALLOW'|'TERMINATE'|'BLOCKED', safety_acknowledgement: bool).
    """
    decision = str(safety_decision.get("decision", "")).lower()
    explanation = str(safety_decision.get("explanation", ""))
    if decision == "blocked":
        print(f"[SAFETY BLOCKED] {explanation}")
        return "BLOCKED", False
    if decision == "require_confirmation":
        print(f"[SAFETY CONFIRMATION REQUIRED] {explanation}")
        if auto_confirm:
            print("  -> Auto-confirmed (--auto-confirm enabled).")
            return "ALLOW", True
        if sys.stdin.isatty():
            ans = input("  Allow this action? [y/N]: ").strip().lower()
            if ans in ("y", "yes"):
                return "ALLOW", True
        print("  -> Denied/non-interactive; halting execution.")
        return "TERMINATE", False
    return "ALLOW", False


def load_api_key(api_key_file: str) -> str:
    env_key = os.environ.get("GEMINI_API_KEY", "").strip()
    if env_key and not api_key_file:
        return env_key
    if api_key_file and os.path.exists(api_key_file):
        with open(api_key_file, "r", encoding="utf-8") as f:
            key = f.read().strip()
        if key:
            return key
    if env_key:
        return env_key
    raise FileNotFoundError(f"Gemini API key not found at {api_key_file!r} or GEMINI_API_KEY")


def extract_model_text(interaction: Dict[str, Any]) -> str:
    texts: List[str] = []
    for step in interaction.get("steps", []):
        if step.get("type") == "model_output":
            for block in step.get("content", []):
                if block.get("type") == "text" and block.get("text"):
                    texts.append(block["text"])
    return "\n".join(texts).strip()


def run_agent(args: argparse.Namespace) -> int:
    api_key = load_api_key(args.api_key_file)
    log_dir = os.path.abspath(args.log_dir) if args.log_dir else None
    if log_dir:
        os.makedirs(log_dir, exist_ok=True)
    trace_file = open(os.path.join(log_dir, "trace.jsonl"), "w", encoding="utf-8") if log_dir else None

    temp_dir: Optional[str] = None
    viewer_proc: Optional[subprocess.Popen] = None
    viewer_log: Optional[Any] = None
    socket_path = args.socket
    collaborate_mode = bool(args.collaborate)

    windowed = bool(args.windowed or args.collaborate)
    action_delay = (
        args.action_delay
        if args.action_delay is not None
        else (0.15 if (windowed or args.collaborate) else 0.0)
    )

    try:
        if not socket_path:
            if not os.path.exists(args.vncviewer_bin):
                raise FileNotFoundError(f"vncviewer binary not found: {args.vncviewer_bin}")
            temp_dir = tempfile.mkdtemp(prefix="tigervnc_agent_")
            socket_path = os.path.join(temp_dir, "agent.sock")
            env = os.environ.copy()
            if not windowed:
                env.pop("DISPLAY", None)
                env.pop("WAYLAND_DISPLAY", None)
            viewer_log_path = os.path.join(log_dir, "vncviewer.log") if log_dir else os.path.join(temp_dir, "vncviewer.log")
            viewer_log = open(viewer_log_path, "w", encoding="utf-8")
            cmd = [
                args.vncviewer_bin,
                "-Shared",
                "-AgentSocket",
                socket_path,
            ]
            if windowed:
                cmd.append("-AgentOverlay")
            else:
                cmd.append("-Headless")
            if args.collaborate:
                cmd.append("-AgentCollaborate")
            cmd.append(args.vnc_host)
            mode_label = "collaborative windowed" if args.collaborate else ("windowed" if windowed else "headless")
            print(f"Starting {mode_label} vncviewer -> {args.vnc_host} (socket={socket_path})")
            viewer_proc = subprocess.Popen(cmd, env=env, stdout=viewer_log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 10.0
            while time.monotonic() < deadline:
                if os.path.exists(socket_path):
                    break
                if viewer_proc.poll() is not None:
                    raise RuntimeError(f"vncviewer exited prematurely with code {viewer_proc.returncode}; see {viewer_log_path}")
                time.sleep(0.1)
            if not os.path.exists(socket_path):
                raise TimeoutError(f"Timed out waiting for AgentSocket at {socket_path}")

        tool_cfg: Dict[str, Any] = {
            "type": "computer_use",
            "environment": "desktop",
            "enable_prompt_injection_detection": True,
        }
        if not args.strict_safety:
            tool_cfg["disabled_safety_policies"] = ["data_modification"]

        llm = InteractionsClient(
            api_key=api_key,
            model=args.model,
            tools=[tool_cfg],
            system_instruction=SYSTEM_INSTRUCTION,
        )

        with AgentClient(socket_path, timeout=15.0) as vnc_client:
            executor = VncDesktopExecutor(vnc_client)
            status = executor.wait_ready(timeout=15.0)
            print(
                f"Connected to VNC desktop: {status['width']}x{status['height']} "
                f"({status.get('desktop_name', '')})"
            )

            collaborate_mode = bool(args.collaborate or status.get("collaborate"))
            stay_interactive = collaborate_mode and not bool(getattr(args, "one_shot", False))

            def start_user_turn(
                prompt_text: str,
                png_bytes: bytes,
                prev_id: Optional[str],
            ) -> Dict[str, Any]:
                payload = [
                    {"type": "text", "text": prompt_text},
                    {
                        "type": "image",
                        "data": base64.b64encode(png_bytes).decode("ascii"),
                        "mime_type": "image/png",
                    },
                ]
                if prev_id:
                    try:
                        return llm.create(input_payload=payload, previous_interaction_id=prev_id)
                    except RuntimeError as e:
                        print(f"  [COLLAB] Starting fresh LLM session (previous interaction unavailable: {e})")
                return llm.create(input_payload=payload, previous_interaction_id=None)

            global_turn = 0
            task_index = 0
            last_completed_interaction_id: Optional[str] = None
            current_prompt: Optional[str] = args.prompt.strip() if args.prompt else ""

            while True:
                if not current_prompt:
                    if stay_interactive:
                        nxt = executor.wait_for_next_instruction(
                            viewer_proc=viewer_proc,
                            overlay_intent="Ready - awaiting your instruction",
                        )
                        if nxt is None:
                            return 0
                        next_text, human_delta = nxt
                        if human_delta > 0:
                            current_prompt = (
                                f"[Human collaborator performed {human_delta} desktop action(s) before this instruction; "
                                f"inspect the updated screenshot.]\n{next_text}"
                            )
                        else:
                            current_prompt = next_text
                    else:
                        raise ValueError("--prompt must not be empty in non-collaborative mode")

                task_index += 1
                init_png, _ = executor.capture_png(settle_delay=0.2, max_wait=1.0)
                if log_dir:
                    init_name = "turn_00_initial.png" if task_index == 1 else f"task_{task_index:02d}_initial.png"
                    with open(os.path.join(log_dir, init_name), "wb") as f:
                        f.write(init_png)

                print(f"Goal: {current_prompt}")
                executor.set_overlay("thinking", "thinking", "Analyzing screen and planning next action...")
                interaction = start_user_turn(current_prompt, init_png, last_completed_interaction_id)
                current_prompt = None

                task_completed = False
                task_halted = False
                wait_intent = "Awaiting next instruction"

                for task_turn in range(1, args.max_turns + 1):
                    global_turn += 1
                    print(
                        f"\n--- Turn {global_turn} "
                        f"(task={task_index}.{task_turn}, interaction={interaction.get('id')}, status={interaction.get('status')}) ---"
                    )
                    steps = interaction.get("steps", [])
                    function_calls = [s for s in steps if s.get("type") == "function_call"]

                    if not function_calls:
                        final_text = extract_model_text(interaction)
                        last_completed_interaction_id = interaction.get("id")
                        print(f"Agent finished:\n{final_text}")
                        final_png, _ = executor.capture_png(settle_delay=0.2, max_wait=0.8)
                        if log_dir:
                            with open(os.path.join(log_dir, "final.png"), "wb") as f:
                                f.write(final_png)
                            if task_index > 1:
                                with open(os.path.join(log_dir, f"task_{task_index:02d}_final.png"), "wb") as f:
                                    f.write(final_png)
                        if trace_file:
                            trace_file.write(
                                json.dumps(
                                    {
                                        "turn": global_turn,
                                        "task": task_index,
                                        "status": interaction.get("status"),
                                        "final_text": final_text,
                                    },
                                    ensure_ascii=False,
                                )
                                + "\n"
                            )
                            trace_file.flush()
                        if not stay_interactive:
                            executor.set_overlay("done", "done", "Task completed")
                            return 0
                        task_completed = True
                        wait_intent = summarize_for_overlay(
                            final_text,
                            fallback="Task completed - awaiting next instruction",
                        )
                        break

                    halted_reason: Optional[str] = None
                    results: List[Tuple[str, str, Dict[str, Any]]] = []
                    turn_log_actions: List[Dict[str, Any]] = []
                    collab_notes: List[str] = []
                    collaborator_intervened = False

                    for idx, fc in enumerate(function_calls):
                        fname = str(fc.get("name", ""))
                        call_id = str(fc.get("id", ""))
                        fc_args = fc.get("arguments") or {}
                        intent = str(fc_args.get("intent", "N/A"))
                        print(f"  -> Action: {fname}({{...}}) | Intent: {intent}")

                        if collaborate_mode:
                            st_now = executor.get_status()
                            if st_now.get("agent_paused"):
                                human_delta, notes = executor.wait_until_resumed()
                                collab_notes.extend(notes)
                                collaborator_intervened = True
                                for rem_fc in function_calls[idx:]:
                                    rem_name = str(rem_fc.get("name", ""))
                                    rem_id = str(rem_fc.get("id", ""))
                                    cancel_res: Dict[str, Any] = {
                                        "status": "paused_for_human_takeover",
                                        "human_actions": human_delta,
                                        "note": "Human collaborator interacted with the desktop and resumed; inspect the updated screenshot before continuing.",
                                    }
                                    results.append((rem_name, rem_id, cancel_res))
                                    turn_log_actions.append({"name": rem_name, "args": rem_fc.get("arguments") or {}, "result": cancel_res})
                                break

                        action_result: Dict[str, Any] = {}
                        safety_decision = fc_args.get("safety_decision")
                        if isinstance(safety_decision, dict):
                            if str(safety_decision.get("decision", "")).lower() == "require_confirmation":
                                executor.set_overlay(
                                    "waiting_confirmation",
                                    fname,
                                    str(safety_decision.get("explanation", "Safety confirmation required")),
                                )
                            s_status, ack = handle_safety_decision(safety_decision, args.auto_confirm)
                            if s_status != "ALLOW":
                                halted_reason = f"Safety decision {s_status}: {safety_decision.get('explanation', '')}"
                                action_result = {"error": halted_reason}
                                results.append((fname, call_id, action_result))
                                turn_log_actions.append({"name": fname, "args": fc_args, "result": action_result})
                                break
                            if ack:
                                action_result["safety_acknowledgement"] = True

                        px: Optional[int] = None
                        py: Optional[int] = None
                        ex: Optional[int] = None
                        ey: Optional[int] = None
                        bmask = 0
                        pulse = False
                        if fname == "drag_and_drop" and all(k in fc_args for k in ("start_x", "start_y", "end_x", "end_y")):
                            px = denormalize_x(fc_args["start_x"], executor.width)
                            py = denormalize_y(fc_args["start_y"], executor.height)
                            ex = denormalize_x(fc_args["end_x"], executor.width)
                            ey = denormalize_y(fc_args["end_y"], executor.height)
                            bmask = 1
                            pulse = True
                        elif "x" in fc_args and "y" in fc_args and fc_args["x"] is not None and fc_args["y"] is not None:
                            px = denormalize_x(fc_args["x"], executor.width)
                            py = denormalize_y(fc_args["y"], executor.height)
                            if fname == "right_click":
                                bmask = 4
                            elif fname == "middle_click":
                                bmask = 2
                            elif fname in ("click", "double_click", "triple_click", "mouse_down", "type"):
                                bmask = 1
                            pulse = (fname != "move")

                        executor.set_overlay(
                            state="acting",
                            action=fname,
                            intent=intent,
                            x=px,
                            y=py,
                            end_x=ex,
                            end_y=ey,
                            pulse=pulse,
                            button_mask=bmask,
                        )
                        if action_delay > 0:
                            time.sleep(action_delay)

                        try:
                            exec_res = executor.execute(fname, fc_args)
                            action_result.update(exec_res)
                        except AgentPausedError as e:
                            if collaborate_mode:
                                human_delta, notes = executor.wait_until_resumed()
                                collab_notes.extend(notes)
                                collaborator_intervened = True
                                action_result = {
                                    "status": "paused_for_human_takeover",
                                    "human_actions": human_delta,
                                    "note": "Human collaborator paused and took control mid-action; inspect the updated screenshot.",
                                }
                                results.append((fname, call_id, action_result))
                                turn_log_actions.append({"name": fname, "args": fc_args, "result": action_result})
                                for rem_fc in function_calls[idx + 1 :]:
                                    rem_name = str(rem_fc.get("name", ""))
                                    rem_id = str(rem_fc.get("id", ""))
                                    cancel_res = {"status": "paused_for_human_takeover"}
                                    results.append((rem_name, rem_id, cancel_res))
                                    turn_log_actions.append({"name": rem_name, "args": rem_fc.get("arguments") or {}, "result": cancel_res})
                                break
                            halted_reason = str(e)
                            action_result = {"error": halted_reason}
                            results.append((fname, call_id, action_result))
                            turn_log_actions.append({"name": fname, "args": fc_args, "result": action_result})
                            break
                        except Exception as e:
                            print(f"  [WARN] Error executing {fname}: {e}")
                            action_result["error"] = str(e)

                        results.append((fname, call_id, action_result))
                        turn_log_actions.append({"name": fname, "args": fc_args, "result": action_result})

                    post_png, post_hdr = executor.capture_png(settle_delay=0.45, max_wait=1.2)
                    drained_notes, _, _ = executor.poll_user_messages()
                    collab_notes.extend(drained_notes)
                    if collab_notes and results:
                        results[0][2]["user_notes"] = collab_notes
                        print(f"  [COLLAB] Delivering user notes to agent: {collab_notes}")
                    if collaborator_intervened:
                        print("  [COLLAB] Re-syncing model with post-takeover screenshot.")

                    if log_dir:
                        with open(os.path.join(log_dir, f"turn_{global_turn:02d}.png"), "wb") as f:
                            f.write(post_png)
                        with open(os.path.join(log_dir, "final.png"), "wb") as f:
                            f.write(post_png)

                    if trace_file:
                        trace_file.write(
                            json.dumps(
                                {
                                    "turn": global_turn,
                                    "task": task_index,
                                    "interaction_id": interaction.get("id"),
                                    "actions": turn_log_actions,
                                    "frame_seq": post_hdr.get("frame_seq"),
                                    "halted_reason": halted_reason,
                                    "user_notes": collab_notes,
                                },
                                ensure_ascii=False,
                            )
                            + "\n"
                        )
                        trace_file.flush()

                    if halted_reason:
                        print(f"Agent halted: {halted_reason}")
                        if not stay_interactive:
                            return 2
                        task_halted = True
                        last_completed_interaction_id = None
                        wait_intent = summarize_for_overlay(
                            f"Halted: {halted_reason}",
                            fallback="Action halted - awaiting next instruction",
                        )
                        break

                    post_b64 = base64.b64encode(post_png).decode("ascii")
                    function_responses = []
                    for fname, call_id, res in results:
                        function_responses.append(
                            {
                                "type": "function_result",
                                "name": fname,
                                "call_id": call_id,
                                "result": [
                                    {"type": "text", "text": json.dumps(res)},
                                    {"type": "image", "data": post_b64, "mime_type": "image/png"},
                                ],
                            }
                        )

                    executor.set_overlay("thinking", "thinking", "Analyzing screen and planning next action...")
                    interaction = llm.create(
                        input_payload=function_responses,
                        previous_interaction_id=interaction["id"],
                    )

                if not task_completed and not task_halted:
                    if not stay_interactive:
                        print(f"Reached turn limit ({args.max_turns}) before task completion.")
                        return 1
                    print(f"Reached per-task turn limit ({args.max_turns}); returning to collaborative wait.")
                    last_completed_interaction_id = None
                    wait_intent = f"Turn limit ({args.max_turns}) reached - awaiting next instruction"

                nxt = executor.wait_for_next_instruction(
                    viewer_proc=viewer_proc,
                    overlay_intent=wait_intent,
                )
                if nxt is None:
                    return 0
                next_text, human_delta = nxt
                if human_delta > 0:
                    current_prompt = (
                        f"[Human collaborator performed {human_delta} desktop action(s) before this instruction; "
                        f"inspect the updated screenshot.]\n{next_text}"
                    )
                else:
                    current_prompt = next_text

    except KeyboardInterrupt:
        print("\n[COLLAB] Interrupted; ending session.")
        return 0
    except (EOFError, ConnectionError, BrokenPipeError) as e:
        if collaborate_mode:
            print(f"\n[COLLAB] Viewer disconnected ({e}); ending session.")
            return 0
        raise

    finally:
        if trace_file:
            trace_file.close()
        if viewer_proc is not None:
            if viewer_proc.poll() is None:
                viewer_proc.send_signal(signal.SIGTERM)
                try:
                    viewer_proc.wait(timeout=5.0)
                except subprocess.TimeoutExpired:
                    viewer_proc.kill()
                    viewer_proc.wait()
        if viewer_log:
            viewer_log.close()
        if temp_dir and os.path.isdir(temp_dir):
            shutil.rmtree(temp_dir, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run a Gemini Computer Use desktop agent over TigerVNC AgentSocket."
    )
    parser.add_argument(
        "--prompt",
        default=(
            "Create a new text document on the desktop named hello.txt containing the text "
            "'Hello from TigerVNC Computer Use Agent!' and make sure it is saved and visible on the desktop."
        ),
        help="Task instruction for the agent (pass '' with --collaborate to start waiting for user input)",
    )
    parser.add_argument(
        "--socket",
        default=None,
        help="Existing vncviewer -AgentSocket Unix domain socket path (if omitted, spawns headless vncviewer)",
    )
    parser.add_argument("--vnc-host", default="localhost:5900", help="Target VNC server host:port")
    parser.add_argument("--vncviewer-bin", default=DEFAULT_VNCVIEWER, help="Path to built vncviewer binary")
    parser.add_argument(
        "--api-key-file",
        default=os.path.join(REPO_ROOT, "gemini.key"),
        help="Path to Gemini API key file",
    )
    parser.add_argument("--model", default=DEFAULT_MODEL, help="Gemini model name supporting computer_use")
    parser.add_argument("--max-turns", type=int, default=20, help="Maximum agent turns per task")
    parser.add_argument("--log-dir", default="/tmp/vnc_agent_run", help="Directory for trace.jsonl and PNG snapshots")
    parser.add_argument(
        "--auto-confirm",
        action="store_true",
        help="Automatically acknowledge require_confirmation safety decisions",
    )
    parser.add_argument(
        "--strict-safety",
        action="store_true",
        help="Do not pass disabled_safety_policies=['data_modification']",
    )
    parser.add_argument(
        "--windowed",
        action="store_true",
        help="Launch vncviewer with a visible GUI window and -AgentOverlay",
    )
    parser.add_argument(
        "--collaborate",
        action="store_true",
        help=(
            "Enable windowed collaborative mode (-AgentOverlay -AgentCollaborate) with automatic human "
            "takeover/resume and interactive waiting for follow-up instructions after completion"
        ),
    )
    parser.add_argument(
        "--one-shot",
        action="store_true",
        help="Exit automatically after the initial task completes even in --collaborate mode",
    )
    parser.add_argument(
        "--action-delay",
        type=float,
        default=None,
        help="Delay in seconds after updating the live overlay before executing each action",
    )
    args = parser.parse_args()
    return run_agent(args)


if __name__ == "__main__":
    sys.exit(main())
