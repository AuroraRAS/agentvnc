"""VNC desktop, remote file, local workspace, memory, and peer coordination executor."""

import codecs
import io
import json
import os
import select
import struct
import subprocess
import sys
import time
import uuid
import zlib
from typing import Any, Dict, List, Optional, Tuple

from test_agent_bridge import AgentClient

from .constants import (
    AGENT_COMPUTER_USE,
    COORDINATION_FUNCTION_NAMES,
    DEFAULT_FILE_LIST_MAX_ENTRIES,
    DEFAULT_FILE_READ_MAX_BYTES,
    FILE_FUNCTION_NAMES,
    FILE_MUTATING_FUNCTIONS,
    KEY_MAP,
    REMOTE_TRANSFER_FUNCTION_NAMES,
    REMOTE_TRANSFER_MUTATING_FUNCTIONS,
    SHIFTED_ASCII,
    SHIFT_KEYSYMS,
    SYSTEM_ONE_FUNCTION_NAMES,
    WORKSPACE_FUNCTION_NAMES,
)
from .coordination import WorkspaceCoordinator
from .ftp_client import VncFtpClient
from .system_one_client import FrameSnapshot
from .workspace import LocalWorkspace


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


def parse_legacy_clipboard_encoding(value: str) -> str:
    """Accept text codecs that preserve the legacy clipboard's ASCII bytes."""
    ascii_text = "".join(chr(code) for code in range(128))
    try:
        name = codecs.lookup(value).name
        if ascii_text.encode(name) == ascii_text.encode("ascii"):
            return name
    except (LookupError, UnicodeError):
        pass
    raise ValueError(f"Expected an ASCII-compatible text encoding, got {value!r}")


class AgentPausedError(RuntimeError):
    """Raised when synthetic input is blocked by operator PAUSE_AGENT."""


class VncDesktopExecutor:
    """Executes Gemini ENVIRONMENT_DESKTOP, FTP, workspace, memory, and peer tools."""

    def __init__(
        self,
        client: AgentClient,
        ftp_client: Optional[VncFtpClient] = None,
        workspace: Optional[LocalWorkspace] = None,
        coordinator: Optional[WorkspaceCoordinator] = None,
        legacy_clipboard_encoding: Optional[str] = None,
        system_one_engine: Optional[Any] = None,
    ) -> None:
        self.client = client
        self.ftp_client = ftp_client
        self.workspace = workspace
        self.coordinator = coordinator
        self.system_one_engine = system_one_engine
        self.legacy_clipboard_encoding = (
            parse_legacy_clipboard_encoding(legacy_clipboard_encoding) if legacy_clipboard_encoding else None
        )
        self.width = 0
        self.height = 0
        self.button_mask = 0
        self._last_button_mask = 0
        self._last_pointer_xy: Tuple[int, int] = (0, 0)
        self._held_keysyms: set = set()

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

    def pause_agent(self) -> None:
        resp, _ = self.client.request("PAUSE_AGENT")
        if not resp.get("ok"):
            raise RuntimeError(f"PAUSE_AGENT failed: {resp}")

    def resume_agent(self) -> None:
        resp, _ = self.client.request("RESUME_AGENT")
        if not resp.get("ok"):
            raise RuntimeError(f"RESUME_AGENT failed: {resp}")

    def _get_clipboard_reply(self) -> Dict[str, Any]:
        resp, _ = self.client.request("CLIPBOARD_GET")
        if not resp.get("ok"):
            raise RuntimeError(f"CLIPBOARD_GET failed: {resp}")
        return resp

    def _decode_clipboard_text(self, resp: Dict[str, Any]) -> str:
        text = str(resp.get("text", ""))
        if resp.get("legacy") and self.legacy_clipboard_encoding:
            # The viewer maps the incoming legacy bytes to Latin-1 code points.
            return text.encode("latin-1").decode(self.legacy_clipboard_encoding)
        return text

    def clipboard_get(self) -> Dict[str, Any]:
        resp = self._get_clipboard_reply()
        return {
            "status": "ok",
            "available": bool(resp.get("available")),
            "text": self._decode_clipboard_text(resp),
        }

    def clipboard_set(self, text: str) -> Dict[str, Any]:
        wire_text = text
        if self.legacy_clipboard_encoding and not text.isascii():
            clipboard_utf8 = self.get_status().get("clipboard_utf8")
            if clipboard_utf8 is None:
                raise RuntimeError("The viewer must report clipboard_utf8 to use --legacy-clipboard-encoding")
            if not clipboard_utf8:
                try:
                    wire_text = text.encode(self.legacy_clipboard_encoding).decode("latin-1")
                except UnicodeEncodeError as exc:
                    raise ValueError(f"The legacy clipboard cannot represent the text in {self.legacy_clipboard_encoding}") from exc
        resp, _ = self.client.request("CLIPBOARD_SET", text=wire_text)
        if not resp.get("ok"):
            if resp.get("error") == "agent_paused":
                raise AgentPausedError(resp.get("message", "Agent input paused by operator"))
            raise RuntimeError(f"CLIPBOARD_SET failed: {resp}")
        return {
            "status": "ok",
            "bytes": len(text.encode("utf-8")),
        }

    def clipboard_verify_selection(self, expected_text: str) -> Dict[str, Any]:
        sequence = self._get_clipboard_reply().get("sequence")
        if not isinstance(sequence, int):
            return {"error": "The viewer does not report clipboard updates. Rebuild vncviewer to verify the selection."}

        # Some servers suppress copies identical to the previous clipboard contents.
        marker = uuid.uuid4().hex
        self.clipboard_set(marker)
        time.sleep(0.05)
        self._hotkey_syms([0xFFE3, ord("c")])
        expected = expected_text.replace("\r\n", "\n").replace("\r", "\n")
        deadline = time.monotonic() + 1.5
        while time.monotonic() < deadline:
            resp = self._get_clipboard_reply()
            if resp.get("available") and resp.get("sequence", 0) > sequence:
                actual = self._decode_clipboard_text(resp).replace("\r\n", "\n").replace("\r", "\n")
                if actual == expected:
                    return {"status": "ok", "matches": True}
                if actual != marker:
                    return {"error": "The copied selection differs from the expected text.", "matches": False}
            time.sleep(0.05)
        return {"error": "The server sent no fresh clipboard update after Ctrl+C. The selection was not verified."}

    def wait_until_resumed(self, intent: Optional[str] = None) -> Tuple[int, List[str]]:
        st = self.get_status()
        start_human = int(st.get("human_action_count", 0))
        overlay_intent = summarize_for_overlay(
            intent or "",
            fallback="Human collaborator in control - resume when ready",
        )
        print("  [COLLAB] Agent paused for human takeover; waiting for resume (Ctrl+Alt+P)...")
        self.set_overlay(
            state="waiting_human",
            action="paused",
            intent=overlay_intent,
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
            self.release_all_inputs()
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

                if (
                    not st.get("agent_paused")
                    and self.coordinator is not None
                    and self.coordinator.has_wake(AGENT_COMPUTER_USE, include_inbox=False)
                ):
                    inbox = self.coordinator.read_messages(AGENT_COMPUTER_USE, mark_read=True)
                    wake_doc = inbox["wake_consumed"]
                    msgs = inbox["messages"]
                    if wake_doc or msgs:
                        parts = ["[Async wakeup from Research Assistant via `.coord/` marker]"]
                        if wake_doc:
                            parts.append(f"Wake reason: {wake_doc.get('reason', '')}")
                        parts.extend(json.dumps(msg, ensure_ascii=False) for msg in msgs)
                        end_human = int(st.get("human_action_count", start_human))
                        print(f"\n[COLLAB/PEER] Woken by Research Assistant (messages={len(msgs)}).")
                        return "\n".join(parts), max(0, end_human - start_human)

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
        self._last_pointer_xy = (int(x), int(y))
        if mask:
            self._last_button_mask = int(mask)
        resp, _ = self.client.request("POINTER_EVENT", x=int(x), y=int(y), button_mask=int(mask))
        if not resp.get("ok"):
            if resp.get("error") == "agent_paused":
                raise AgentPausedError(resp.get("message", "Agent input paused by operator"))
            raise RuntimeError(f"POINTER_EVENT failed: {resp}")
        self._last_button_mask = int(mask)

    def _key(self, keysym: int, down: bool) -> None:
        ks = int(keysym)
        if down:
            self._held_keysyms.add(ks)
        resp, _ = self.client.request("KEY_EVENT", keysym=ks, down=bool(down))
        if not resp.get("ok"):
            if resp.get("error") == "agent_paused":
                raise AgentPausedError(resp.get("message", "Agent input paused by operator"))
            raise RuntimeError(f"KEY_EVENT failed: {resp}")
        if not down:
            self._held_keysyms.discard(ks)

    def release_all_inputs(self) -> None:
        """Release any held mouse buttons and keyboard keys (used during S5_HANDOFF_TO_S2)."""
        if self._last_button_mask or self.button_mask:
            px, py = self._last_pointer_xy
            try:
                self._pointer(px, py, 0)
            except (EOFError, OSError, RuntimeError):
                pass
            else:
                self.button_mask = 0
        for ks in list(self._held_keysyms):
            try:
                self._key(ks, False)
            except (EOFError, OSError, RuntimeError):
                pass

    def capture_rgb_frame(self) -> FrameSnapshot:
        """Capture a raw RGB24 ``FrameSnapshot`` via ``GET_FRAME`` without PNG encoding overhead."""
        hdr, pixels = self.client.request("GET_FRAME", format="RGB24")
        if not hdr.get("ok") or not pixels:
            raise RuntimeError(f"GET_FRAME failed: {hdr}")
        w = int(hdr.get("width", self.width) or self.width)
        h = int(hdr.get("height", self.height) or self.height)
        if w <= 0 or h <= 0:
            raise RuntimeError(f"Invalid GET_FRAME dimensions: {w}x{h}")
        if len(pixels) != w * h * 3:
            raise RuntimeError(f"Invalid RGB24 payload size for {w}x{h} frame: {len(pixels)} bytes")
        self.width = w
        self.height = h
        return FrameSnapshot(
            width=w,
            height=h,
            rgb_bytes=pixels,
            frame_seq=int(hdr.get("frame_seq", 0) or 0),
            timestamp=time.monotonic(),
        )

    def execute_host_action(self, action: Dict[str, Any]) -> Dict[str, Any]:
        """Execute a validated closed-set ``HostAction`` primitive."""
        atype = str(action.get("type", "")).strip().lower()
        if atype == "hover":
            px = denormalize_x(action["x"], self.width)
            py = denormalize_y(action["y"], self.height)
            self._pointer(px, py, 0)
            self.button_mask = 0
            return {"status": "ok", "type": "hover", "pixel_x": px, "pixel_y": py}
        if atype == "click":
            px = denormalize_x(action["x"], self.width)
            py = denormalize_y(action["y"], self.height)
            btn = str(action.get("button", "left")).strip().lower()
            btn_bit = {"left": 1, "middle": 2, "right": 4}[btn]
            count = int(action.get("count", 1))
            self._click_px(px, py, btn_bit=btn_bit, count=count)
            return {"status": "ok", "type": "click", "pixel_x": px, "pixel_y": py, "button": btn, "count": count}
        if atype == "wheel_scroll":
            px = denormalize_x(action["x"], self.width)
            py = denormalize_y(action["y"], self.height)
            direction = str(action.get("direction", "down")).strip().lower()
            magnitude = int(action.get("magnitude", 300))
            wheel_bits = {"up": 8, "down": 16, "left": 32, "right": 64}
            bit = wheel_bits[direction]
            pulses = max(1, min(15, int(round(magnitude / 100.0))))
            self._pointer(px, py, 0)
            time.sleep(0.03)
            for _ in range(pulses):
                self._pointer(px, py, bit)
                self._pointer(px, py, 0)
                time.sleep(0.02)
            self.button_mask = 0
            return {"status": "ok", "type": "wheel_scroll", "pulses": pulses, "direction": direction}
        if atype == "key_press":
            raw_key = str(action["key"])
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
            return {"status": "ok", "type": "key_press", "keysym": ks}
        if atype == "drag_step":
            sx = denormalize_x(action["start_x"], self.width)
            sy = denormalize_y(action["start_y"], self.height)
            ex = denormalize_x(action["end_x"], self.width)
            ey = denormalize_y(action["end_y"], self.height)
            self._pointer(sx, sy, 0)
            time.sleep(0.04)
            self._pointer(sx, sy, 1)
            steps = 8
            for i in range(1, steps + 1):
                ix = int(round(sx + (ex - sx) * (i / steps)))
                iy = int(round(sy + (ey - sy) * (i / steps)))
                self._pointer(ix, iy, 1)
                time.sleep(0.02)
            self._pointer(ex, ey, 0)
            self.button_mask = 0
            return {"status": "ok", "type": "drag_step", "start": [sx, sy], "end": [ex, ey]}
        raise ValueError(f"Unsupported HostAction type: {atype!r}")

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

    def _hotkey_syms(self, keysyms: List[int]) -> None:
        for ks in keysyms:
            self._key(ks, True)
            time.sleep(0.04)
        time.sleep(0.06)
        for ks in reversed(keysyms):
            self._key(ks, False)
            time.sleep(0.04)

    def _clear_text_field(self) -> None:
        # Legacy Windows edit controls, including the Save As filename box,
        # do not always implement Ctrl+A.
        self._hotkey_syms([KEY_MAP["ctrl"], KEY_MAP["home"]])
        self._hotkey_syms([KEY_MAP["ctrl"], KEY_MAP["shift"], KEY_MAP["end"]])
        self._hotkey_syms([KEY_MAP["backspace"]])

    def _type_text_impl(self, text: str, press_enter: bool) -> None:
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

    def verify_action_effect(self, pre_status: Dict[str, Any], settle_wait: float = 0.05) -> Dict[str, Any]:
        if settle_wait > 0:
            time.sleep(settle_wait)
        post = self.get_status()
        new_w = int(post.get("width", self.width) or self.width)
        new_h = int(post.get("height", self.height) or self.height)
        prev_w = int(pre_status.get("width", self.width) or self.width)
        prev_h = int(pre_status.get("height", self.height) or self.height)
        prev_dmg = int(pre_status.get("damage_count", 0))
        post_dmg = int(post.get("damage_count", 0))
        prev_seq = int(pre_status.get("frame_seq", 0))
        post_seq = int(post.get("frame_seq", 0))
        prev_resize = int(pre_status.get("resize_count", 0))
        post_resize = int(post.get("resize_count", 0))
        resized = (post_resize > prev_resize) or (new_w != prev_w) or (new_h != prev_h)
        if new_w > 0 and new_h > 0:
            self.width = new_w
            self.height = new_h
        damage_delta = max(0, post_dmg - prev_dmg)
        seq_delta = max(0, post_seq - prev_seq)
        return {
            "frame_changed": (damage_delta > 0) or (seq_delta > 0) or resized,
            "damage_delta": damage_delta,
            "frame_seq_delta": seq_delta,
            "frame_seq": post_seq,
            "resized": resized,
            "width": new_w,
            "height": new_h,
        }

    def _execute_workspace_tool(self, fname: str, args: Dict[str, Any]) -> Dict[str, Any]:
        if self.workspace is None:
            return {"error": f"Local workspace tool {fname!r} is unavailable (workspace not initialized)."}
        if fname == "workspace_list":
            path_arg = "." if args.get("path") is None else str(args["path"])
            max_entries = (
                DEFAULT_FILE_LIST_MAX_ENTRIES if args.get("max_entries") is None else int(args["max_entries"])
            )
            return self.workspace.list_dir(
                path=path_arg,
                recursive=bool(args.get("recursive", False)),
                max_entries=max_entries,
            )
        if fname == "workspace_stat":
            if args.get("path") is None:
                raise ValueError("workspace_stat requires 'path'")
            return self.workspace.stat(str(args["path"]))
        if fname == "workspace_read":
            if args.get("path") is None:
                raise ValueError("workspace_read requires 'path'")
            return self.workspace.read_file(
                path=str(args["path"]),
                encoding=str(args.get("encoding") or "utf-8"),
                as_base64=bool(args.get("as_base64", False)),
                offset=0 if args.get("offset") is None else int(args["offset"]),
                max_bytes=(
                    DEFAULT_FILE_READ_MAX_BYTES if args.get("max_bytes") is None else int(args["max_bytes"])
                ),
            )
        if fname == "workspace_write":
            if args.get("path") is None:
                raise ValueError("workspace_write requires 'path'")
            return self.workspace.write_file(
                path=str(args["path"]),
                content=args.get("content"),
                content_base64=args.get("content_base64"),
                encoding=str(args.get("encoding") or "utf-8"),
                append=bool(args.get("append", False)),
                create_parents=(True if args.get("create_parents") is None else bool(args["create_parents"])),
            )
        if fname == "workspace_mkdir":
            if args.get("path") is None:
                raise ValueError("workspace_mkdir requires 'path'")
            return self.workspace.mkdir(
                path=str(args["path"]),
                parents=(True if args.get("parents") is None else bool(args["parents"])),
            )
        if fname == "workspace_delete":
            if args.get("path") is None:
                raise ValueError("workspace_delete requires 'path'")
            return self.workspace.delete(
                path=str(args["path"]),
                recursive=bool(args.get("recursive", False)),
            )
        if fname == "workspace_rename":
            if args.get("from_path") is None or args.get("to_path") is None:
                raise ValueError("workspace_rename requires 'from_path' and 'to_path'")
            return self.workspace.rename(from_path=str(args["from_path"]), to_path=str(args["to_path"]))
        if fname == "workspace_search":
            if args.get("query") is None:
                raise ValueError("workspace_search requires 'query'")
            return self.workspace.search_files(
                query=str(args["query"]),
                path="." if args.get("path") is None else str(args["path"]),
                glob_pattern=args.get("glob_pattern"),
                max_matches=50 if args.get("max_matches") is None else int(args["max_matches"]),
            )
        if fname == "memory_list":
            return self.workspace.memory_list(
                tag=args.get("tag"),
                limit=100 if args.get("limit") is None else int(args["limit"]),
            )
        if fname == "memory_read":
            return self.workspace.memory_read(key=args.get("key"))
        if fname == "memory_write":
            if args.get("key") is None or args.get("value") is None:
                raise ValueError("memory_write requires 'key' and 'value'")
            return self.workspace.memory_write(
                key=str(args["key"]),
                value=str(args["value"]),
                tags=args.get("tags"),
                author=AGENT_COMPUTER_USE,
                append=bool(args.get("append", False)),
            )
        if fname == "memory_delete":
            if args.get("key") is None:
                raise ValueError("memory_delete requires 'key'")
            return self.workspace.memory_delete(key=str(args["key"]))
        if fname == "memory_search":
            if args.get("query") is None:
                raise ValueError("memory_search requires 'query'")
            return self.workspace.memory_search(
                query=str(args["query"]),
                tag=args.get("tag"),
                limit=20 if args.get("limit") is None else int(args["limit"]),
            )
        return {"error": f"Unsupported workspace tool: {fname}"}

    def _execute_coordination_tool(self, fname: str, args: Dict[str, Any]) -> Dict[str, Any]:
        if self.coordinator is None:
            return {"error": f"Peer coordination tool {fname!r} is unavailable (assistant disabled)."}
        peer = self.coordinator.peer_of(AGENT_COMPUTER_USE)

        if fname == "peer_get_status":
            return self.coordinator.get_status()
        if fname == "peer_set_status":
            if args.get("state") is None:
                raise ValueError("peer_set_status requires 'state'")
            return {
                "status": "ok",
                "marker": self.coordinator.set_status(
                    agent=AGENT_COMPUTER_USE,
                    state=str(args["state"]),
                    task=str(args.get("task") or ""),
                    detail=str(args.get("detail") or ""),
                ),
            }
        if fname == "peer_send_message":
            if args.get("subject") is None or args.get("body") is None:
                raise ValueError("peer_send_message requires 'subject' and 'body'")
            return self.coordinator.send_message(
                sender=AGENT_COMPUTER_USE,
                recipient=peer,
                subject=str(args["subject"]),
                body=str(args["body"]),
                kind=str(args.get("kind") or "request"),
                attachments=args.get("attachments"),
                in_reply_to=args.get("in_reply_to"),
                wake=(True if args.get("wake") is None else bool(args["wake"])),
                priority=str(args.get("priority") or "normal"),
            )
        if fname == "peer_read_messages":
            return self.coordinator.read_messages(
                agent=AGENT_COMPUTER_USE,
                include_read=bool(args.get("include_read", False)),
                mark_read=(True if args.get("mark_read") is None else bool(args["mark_read"])),
                limit=20 if args.get("limit") is None else int(args["limit"]),
            )
        if fname == "peer_wake":
            if args.get("reason") is None:
                raise ValueError("peer_wake requires 'reason'")
            return self.coordinator.wake_peer(
                sender=AGENT_COMPUTER_USE,
                recipient=peer,
                reason=str(args["reason"]),
                priority=str(args.get("priority") or "normal"),
            )
        if fname == "peer_wait_for_message":
            raw_to = 30 if args.get("timeout_seconds") is None else int(args["timeout_seconds"])
            secs = float(max(1, min(raw_to, 120)))
            intent_str = summarize_for_overlay(
                str(args.get("intent") or ""),
                fallback="Waiting for research assistant...",
            )
            self.coordinator.set_status(AGENT_COMPUTER_USE, "waiting_peer", task=intent_str)
            self.set_overlay(state="thinking", action="waiting_peer", intent=intent_str)

            def _check_interrupt() -> Optional[Dict[str, Any]]:
                try:
                    st_now = self.get_status()
                except (EOFError, ConnectionError, BrokenPipeError, OSError) as exc:
                    return {"reason": "viewer_disconnected", "detail": str(exc)}
                if not st_now.get("connected"):
                    return {"reason": "vnc_disconnected"}
                if st_now.get("agent_paused"):
                    return {"reason": "agent_paused"}
                if int(st_now.get("pending_user_messages", 0)) > 0:
                    return {"reason": "pending_user_messages"}
                return None

            wait_res = self.coordinator.wait_for_wake(
                AGENT_COMPUTER_USE,
                timeout=secs,
                poll_interval=0.2,
                check_interrupt=_check_interrupt,
            )
            self.coordinator.set_status(AGENT_COMPUTER_USE, "working", task="Resumed after peer wait")
            if wait_res.get("woken"):
                inbox = self.coordinator.read_messages(AGENT_COMPUTER_USE, mark_read=True)
                return {
                    "status": "woken",
                    "wake_marker": inbox["wake_consumed"],
                    "messages": inbox.get("messages", []),
                    "peer_status": self.coordinator.get_status(peer)["agents"][peer],
                }
            if wait_res.get("interrupted"):
                return {
                    "status": "interrupted",
                    "interrupt": wait_res["interrupted"],
                    "peer_status": self.coordinator.get_status(peer)["agents"][peer],
                }
            return {
                "status": "stopped" if wait_res.get("stopped") else "timeout",
                "waited_seconds": secs,
                "peer_status": self.coordinator.get_status(peer)["agents"][peer],
            }
        return {"error": f"Unsupported coordination tool: {fname}"}

    def execute(
        self,
        fname: str,
        args: Dict[str, Any],
        collaborate_mode: bool = False,
    ) -> Dict[str, Any]:
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
            self._type_text_impl(text, press_enter)
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

            self._hotkey_syms(keysyms)
            return {"status": "ok", "keysyms": keysyms}

        if fname == "wait":
            secs = min(max(float(args.get("seconds", 1.0)), 0.1), 30.0)
            time.sleep(secs)
            return {"status": "ok", "waited_seconds": secs}

        if fname == "take_screenshot":
            return {"status": "ok"}

        if fname == "clipboard_get":
            return self.clipboard_get()

        if fname == "clipboard_verify_selection":
            return self.clipboard_verify_selection(str(args["expected_text"]))

        if fname == "clipboard_set":
            text = str(args.get("text", ""))
            paste = bool(args.get("paste", False))
            clip_res = self.clipboard_set(text)
            if paste:
                time.sleep(0.05)
                self._hotkey_syms([0xFFE3, ord("v")])
                if not text.isascii():
                    clip_res["_attach_screenshot"] = True
                    clip_res["text_verified"] = False
            clip_res["pasted"] = paste
            return clip_res

        if fname == "paste_text_at":
            px = denormalize_x(args["x"], self.width)
            py = denormalize_y(args["y"], self.height)
            text = str(args.get("text", ""))
            clear_existing = bool(args.get("clear_existing", False))
            needs_visual_check = not text.isascii()
            enter_requested = bool(args.get("press_enter", False))
            press_enter = enter_requested and not needs_visual_check
            clip_res = self.clipboard_set(text)
            self._click_px(px, py, btn_bit=1, count=1)
            time.sleep(0.10)
            if clear_existing:
                self._clear_text_field()
            time.sleep(0.05)
            self._hotkey_syms([0xFFE3, ord("v")])
            if press_enter:
                time.sleep(0.05)
                self._hotkey_syms([0xFF0D])
            res_paste: Dict[str, Any] = {
                "status": "ok",
                "pixel_x": px,
                "pixel_y": py,
                "chars_pasted": len(text),
                "bytes": clip_res["bytes"],
                "clear_existing": clear_existing,
                "press_enter": press_enter,
            }
            if needs_visual_check:
                res_paste["_attach_screenshot"] = True
                res_paste["text_verified"] = False
                if enter_requested:
                    res_paste["note"] = "Enter was deferred. Verify the pasted text in the screenshot before pressing Enter."
            return res_paste

        if fname == "click_and_type":
            px = denormalize_x(args["x"], self.width)
            py = denormalize_y(args["y"], self.height)
            text = str(args.get("text", ""))
            clear_existing = bool(args.get("clear_existing", False))
            press_enter = bool(args.get("press_enter", False))
            self._click_px(px, py, btn_bit=1, count=1)
            time.sleep(0.10)
            if clear_existing:
                self._clear_text_field()
            self._type_text_impl(text, press_enter)
            return {
                "status": "ok",
                "pixel_x": px,
                "pixel_y": py,
                "chars_typed": len(text),
                "clear_existing": clear_existing,
                "press_enter": press_enter,
            }

        if fname == "get_desktop_status":
            st = self.get_status()
            w = int(st.get("width", self.width) or self.width)
            h = int(st.get("height", self.height) or self.height)
            if w > 0 and h > 0:
                self.width = w
                self.height = h
            return {
                "status": "ok",
                "width": w,
                "height": h,
                "desktop_name": str(st.get("desktop_name", "")),
                "frame_seq": int(st.get("frame_seq", 0)),
                "damage_count": int(st.get("damage_count", 0)),
                "resize_count": int(st.get("resize_count", 0)),
                "updating": bool(st.get("updating", False)),
                "has_clipboard": bool(st.get("has_clipboard", False)),
            }

        if fname in FILE_FUNCTION_NAMES:
            if self.ftp_client is None:
                return {"error": f"Direct file tool {fname!r} is unavailable (FTP bridge not active)."}
            if fname in FILE_MUTATING_FUNCTIONS:
                st = self.get_status()
                if st.get("view_only"):
                    return {"error": "Viewer is running in ViewOnly mode; file modifications are disabled."}
                if st.get("agent_paused"):
                    raise AgentPausedError("Agent input is paused by operator")

            if fname == "file_list":
                path_arg = "/" if args.get("path") is None else str(args["path"])
                max_entries = (
                    DEFAULT_FILE_LIST_MAX_ENTRIES
                    if args.get("max_entries") is None
                    else int(args["max_entries"])
                )
                return self.ftp_client.list_dir(path=path_arg, max_entries=max_entries)
            if fname == "file_stat":
                if args.get("path") is None:
                    raise ValueError("file_stat requires 'path'")
                return self.ftp_client.stat(str(args["path"]))
            if fname == "file_read":
                if args.get("path") is None:
                    raise ValueError("file_read requires 'path'")
                return self.ftp_client.read_file(
                    path=str(args["path"]),
                    encoding=str(args.get("encoding") or "utf-8"),
                    as_base64=bool(args.get("as_base64", False)),
                    offset=0 if args.get("offset") is None else int(args["offset"]),
                    max_bytes=(
                        DEFAULT_FILE_READ_MAX_BYTES
                        if args.get("max_bytes") is None
                        else int(args["max_bytes"])
                    ),
                )
            if fname == "file_write":
                if args.get("path") is None:
                    raise ValueError("file_write requires 'path'")
                return self.ftp_client.write_file(
                    path=str(args["path"]),
                    content=args.get("content"),
                    content_base64=args.get("content_base64"),
                    encoding=str(args.get("encoding") or "utf-8"),
                    append=bool(args.get("append", False)),
                    create_parents=bool(args.get("create_parents", False)),
                )
            if fname == "file_mkdir":
                if args.get("path") is None:
                    raise ValueError("file_mkdir requires 'path'")
                return self.ftp_client.mkdir(
                    path=str(args["path"]),
                    parents=(True if args.get("parents") is None else bool(args["parents"])),
                )
            if fname == "file_delete":
                if args.get("path") is None:
                    raise ValueError("file_delete requires 'path'")
                return self.ftp_client.delete(
                    path=str(args["path"]),
                    is_dir=bool(args.get("is_dir", False)),
                )
            if fname == "file_rename":
                if args.get("from_path") is None or args.get("to_path") is None:
                    raise ValueError("file_rename requires 'from_path' and 'to_path'")
                return self.ftp_client.rename(
                    from_path=str(args["from_path"]),
                    to_path=str(args["to_path"]),
                )

        if fname in WORKSPACE_FUNCTION_NAMES:
            return self._execute_workspace_tool(fname, args)

        if fname in REMOTE_TRANSFER_FUNCTION_NAMES:
            if self.workspace is None:
                return {"error": f"Remote transfer tool {fname!r} is unavailable (workspace not initialized)."}
            if self.ftp_client is None:
                return {"error": f"Remote transfer tool {fname!r} is unavailable (FTP bridge not active)."}
            if fname in REMOTE_TRANSFER_MUTATING_FUNCTIONS:
                st = self.get_status()
                if st.get("view_only"):
                    return {"error": "Viewer is running in ViewOnly mode; remote uploads are disabled."}
                if st.get("agent_paused"):
                    raise AgentPausedError("Agent input is paused by operator")
            if fname == "workspace_download_from_remote":
                if args.get("remote_path") is None or args.get("local_path") is None:
                    raise ValueError("workspace_download_from_remote requires 'remote_path' and 'local_path'")
                return self.workspace.download_from_remote(
                    ftp_client=self.ftp_client,
                    remote_path=str(args["remote_path"]),
                    local_path=str(args["local_path"]),
                    overwrite=(True if args.get("overwrite") is None else bool(args["overwrite"])),
                )
            if fname == "workspace_upload_to_remote":
                if args.get("local_path") is None or args.get("remote_path") is None:
                    raise ValueError("workspace_upload_to_remote requires 'local_path' and 'remote_path'")
                return self.workspace.upload_to_remote(
                    ftp_client=self.ftp_client,
                    local_path=str(args["local_path"]),
                    remote_path=str(args["remote_path"]),
                    create_parents=(True if args.get("create_parents") is None else bool(args["create_parents"])),
                )

        if fname in COORDINATION_FUNCTION_NAMES:
            return self._execute_coordination_tool(fname, args)

        if fname in SYSTEM_ONE_FUNCTION_NAMES:
            if self.system_one_engine is None:
                return {
                    "error": (
                        f"System One tool {fname!r} is unavailable "
                        "(TypeSafe API key not configured or --no-system-one set)."
                    )
                }
            return self.system_one_engine.execute_tool(fname, args, collaborate_mode=collaborate_mode)

        if fname == "yield_to_user":
            reason = str(args.get("reason", "Agent requested human assistance")).strip()
            if not reason:
                reason = "Agent requested human assistance"
            if collaborate_mode:
                self.pause_agent()
                human_delta, notes = self.wait_until_resumed(intent=reason)
                return {
                    "status": "resumed_by_user",
                    "reason": reason,
                    "human_actions": human_delta,
                    "user_notes": notes,
                    "_attach_screenshot": True,
                }
            return {
                "status": "yield_requested",
                "reason": reason,
                "note": "Run with --collaborate for interactive human takeover.",
            }

        return {"error": f"Unsupported desktop action: {fname}"}
