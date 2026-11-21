#!/usr/bin/env python3
"""Gemini Computer Use desktop agent over TigerVNC's -AgentSocket IPC bridge.

Implements official Gemini Computer Use & Interactions API best practices:
  https://aistudio.google.com/docs/computer-use
  https://aistudio.google.com/docs/interactions-overview
  https://aistudio.google.com/docs/tools
"""

import argparse
import base64
import codecs
import copy
import ftplib
import io
import json
import os
import re
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
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

THINKING_LEVELS: Tuple[str, ...] = ("low", "medium", "high", "xhigh", "default")
RECOVERY_THINKING_LEVELS: Tuple[str, ...] = ("low", "medium", "high", "xhigh", "off")
FTP_PROTOCOLS: Tuple[str, ...] = ("Auto", "Tight2", "Tight13", "Ultra")

DEFAULT_FILE_READ_MAX_BYTES = 131072
MAX_FILE_READ_BYTES = 1048576
MAX_FILE_WRITE_BYTES = 16 * 1024 * 1024
DEFAULT_FILE_LIST_MAX_ENTRIES = 200
MAX_FILE_LIST_ENTRIES = 1000

PREDEFINED_DESKTOP_FUNCTIONS: Tuple[str, ...] = (
    "click",
    "double_click",
    "triple_click",
    "middle_click",
    "right_click",
    "mouse_down",
    "mouse_up",
    "move",
    "drag_and_drop",
    "scroll",
    "type",
    "press_key",
    "key_down",
    "key_up",
    "hotkey",
    "wait",
    "take_screenshot",
)

CUSTOM_FUNCTION_DECLARATIONS: List[Dict[str, Any]] = [
    {
        "type": "function",
        "name": "clipboard_get",
        "description": (
            "Read the current remote VNC clipboard text via ClientCutText without clicking or taking a screenshot. "
            "Returns text-only JSON."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "intent": {"type": "string", "description": "Short reason for reading the clipboard."},
            },
        },
    },
    {
        "type": "function",
        "name": "clipboard_set",
        "description": (
            "Write UTF-8 text to the remote VNC clipboard via ClientCutText without typing character by character. "
            "Optionally sends Ctrl+V afterward if paste=true. Returns text-only JSON."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "text": {"type": "string", "description": "UTF-8 text to copy into the remote clipboard."},
                "paste": {"type": "boolean", "description": "If true, also press Ctrl+V after setting clipboard."},
                "intent": {"type": "string", "description": "Short reason for setting clipboard contents."},
            },
            "required": ["text"],
        },
    },
    {
        "type": "function",
        "name": "paste_text_at",
        "description": (
            "Deterministic multi-step text macro: click at normalized (x, y), optionally clear existing field (Ctrl+A + BackSpace), "
            "copy UTF-8 text (including non-ASCII/multiline) into VNC clipboard, paste with Ctrl+V, and optionally press Enter. "
            "Preferred over click+type for reliable text entry. Returns text-only JSON on success."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "x": {"type": "integer", "description": "Normalized X coordinate (0-1000)."},
                "y": {"type": "integer", "description": "Normalized Y coordinate (0-1000)."},
                "text": {"type": "string", "description": "UTF-8 text to paste into the target input."},
                "clear_existing": {"type": "boolean", "description": "If true, select all (Ctrl+A) and delete before pasting."},
                "press_enter": {"type": "boolean", "description": "If true, press Enter after pasting."},
                "intent": {"type": "string", "description": "Short description of why you are pasting here."},
            },
            "required": ["x", "y", "text"],
        },
    },
    {
        "type": "function",
        "name": "click_and_type",
        "description": (
            "Deterministic macro: click at normalized (x, y), optionally clear existing text (Ctrl+A + BackSpace), "
            "type plain ASCII text directly via VNC KeyEvents, and optionally press Enter. Returns text-only JSON on success."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "x": {"type": "integer", "description": "Normalized X coordinate (0-1000)."},
                "y": {"type": "integer", "description": "Normalized Y coordinate (0-1000)."},
                "text": {"type": "string", "description": "ASCII text to type into the focused widget."},
                "clear_existing": {"type": "boolean", "description": "If true, select all (Ctrl+A) and delete before typing."},
                "press_enter": {"type": "boolean", "description": "If true, press Enter after typing."},
                "intent": {"type": "string", "description": "Short description of why you are typing here."},
            },
            "required": ["x", "y", "text"],
        },
    },
    {
        "type": "function",
        "name": "get_desktop_status",
        "description": (
            "Check lightweight VNC desktop metadata (resolution, desktop name, frame_seq, damage_count, resize_count, "
            "updating, has_clipboard) without capturing or transferring a screenshot image. Returns text-only JSON."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "intent": {"type": "string", "description": "Short reason for checking desktop status."},
            },
        },
    },
    {
        "type": "function",
        "name": "yield_to_user",
        "description": (
            "Pause automation and hand control over to the human collaborator in vncviewer for login, CAPTCHA, "
            "MFA, or manual intervention, resuming with an updated screenshot when the operator resumes."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "reason": {"type": "string", "description": "Clear explanation of what the human operator needs to do."},
                "intent": {"type": "string", "description": "Short summary for the collaborative overlay banner."},
            },
            "required": ["reason"],
        },
    },
]

CUSTOM_FUNCTION_NAMES = frozenset(d["name"] for d in CUSTOM_FUNCTION_DECLARATIONS)

FILE_FUNCTION_DECLARATIONS: List[Dict[str, Any]] = [
    {
        "type": "function",
        "name": "file_list",
        "description": (
            "List files and subdirectories in a remote directory over TigerVNC's RFB file transfer FTP bridge "
            "without opening a GUI file manager. Returns JSON entries with name, path, type ('file' or 'dir'), "
            "and optional size/modified timestamp."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "Remote directory path (default '/'). Forward slashes or Windows paths like 'C:\\Users' are accepted.",
                },
                "max_entries": {
                    "type": "integer",
                    "description": "Maximum number of entries to return (1-1000, default 200).",
                },
                "intent": {"type": "string", "description": "Short reason for listing this directory."},
            },
        },
    },
    {
        "type": "function",
        "name": "file_stat",
        "description": (
            "Check whether a remote file or directory exists and retrieve its type ('file' or 'dir'), size in bytes, "
            "and modified timestamp via the FTP bridge."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Remote file or directory path."},
                "intent": {"type": "string", "description": "Short reason for checking file metadata."},
            },
            "required": ["path"],
        },
    },
    {
        "type": "function",
        "name": "file_read",
        "description": (
            "Read bytes or decoded text directly from a remote file over the FTP bridge without taking a screenshot. "
            "Supports byte offset and max_bytes chunking for large files, or as_base64=true for binary files."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Remote file path to read."},
                "encoding": {"type": "string", "description": "Text encoding when as_base64 is false (default 'utf-8')."},
                "as_base64": {"type": "boolean", "description": "If true, return base64-encoded bytes in content_base64."},
                "offset": {"type": "integer", "description": "Byte offset to start reading from (default 0)."},
                "max_bytes": {"type": "integer", "description": "Maximum bytes to read (default 131072, max 1048576)."},
                "intent": {"type": "string", "description": "Short reason for reading this file."},
            },
            "required": ["path"],
        },
    },
    {
        "type": "function",
        "name": "file_write",
        "description": (
            "Write or append text or binary data directly to a remote file over the FTP bridge without GUI editors. "
            "Provide either 'content' (text) or 'content_base64' (binary)."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Remote destination file path."},
                "content": {"type": "string", "description": "Text content to write (mutually exclusive with content_base64)."},
                "content_base64": {
                    "type": "string",
                    "description": "Base64-encoded binary content to write (mutually exclusive with content).",
                },
                "encoding": {"type": "string", "description": "Text encoding when using content (default 'utf-8')."},
                "append": {"type": "boolean", "description": "If true, append to existing file instead of overwriting."},
                "create_parents": {"type": "boolean", "description": "If true, create missing parent directories first."},
                "intent": {"type": "string", "description": "Short reason for writing this file."},
            },
            "required": ["path"],
        },
    },
    {
        "type": "function",
        "name": "file_mkdir",
        "description": "Create a remote directory (and optionally missing parent directories) via the FTP bridge.",
        "parameters": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Remote directory path to create."},
                "parents": {"type": "boolean", "description": "If true (default), create missing parent directories."},
                "intent": {"type": "string", "description": "Short reason for creating this directory."},
            },
            "required": ["path"],
        },
    },
    {
        "type": "function",
        "name": "file_delete",
        "description": "Delete a remote file or empty directory via the FTP bridge.",
        "parameters": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Remote file or empty directory path to delete."},
                "is_dir": {"type": "boolean", "description": "Set true to delete an empty directory (default false for files)."},
                "intent": {"type": "string", "description": "Short reason for deleting this path."},
            },
            "required": ["path"],
        },
    },
    {
        "type": "function",
        "name": "file_rename",
        "description": "Rename or move a remote file or directory via the FTP bridge.",
        "parameters": {
            "type": "object",
            "properties": {
                "from_path": {"type": "string", "description": "Existing remote source path."},
                "to_path": {"type": "string", "description": "Target remote destination path."},
                "intent": {"type": "string", "description": "Short reason for renaming/moving."},
            },
            "required": ["from_path", "to_path"],
        },
    },
]

FILE_READ_ONLY_FUNCTIONS = frozenset({"file_list", "file_stat", "file_read"})
FILE_MUTATING_FUNCTIONS = frozenset({"file_write", "file_mkdir", "file_delete", "file_rename"})
FILE_FUNCTION_NAMES = FILE_READ_ONLY_FUNCTIONS | FILE_MUTATING_FUNCTIONS
ALL_CUSTOM_FUNCTION_NAMES = CUSTOM_FUNCTION_NAMES | FILE_FUNCTION_NAMES
READ_ONLY_FUNCTIONS = (
    frozenset({"take_screenshot", "clipboard_get", "get_desktop_status", "wait", "yield_to_user"})
    | FILE_READ_ONLY_FUNCTIONS
)

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
* **Insurmountable Obstacles:** If you are technically unable to interact with a UI element or stuck in a loop you cannot resolve, use `yield_to_user` (in collaborative mode) or ask the user to take over.

---

## RULE 2: Default Behavior (ACTUATE)

If an action does **NOT** fall under the conditions for `USER_CONFIRMATION`,
your default behavior is to **Actuate**.

**Actuation Means:** You MUST proactively perform all necessary steps to move
the user's request forward. Continue to actuate until you either complete the
non-consequential task or encounter a condition defined in Rule 1.

## VNC Desktop Execution & Optimization Guidelines:
* **Prefer Custom Deterministic Macros:** When available, prefer `paste_text_at`, `click_and_type`, `clipboard_get`, `clipboard_set`, and `get_desktop_status` over multi-step click+type sequences; they execute deterministically and return compact text-only status results without extra round-trips.
* **Parallel Batching & Intent:** Always provide a clear `intent` argument on actions. Only batch independent actions in a single turn when their order is deterministic and does not depend on intermediate visual layout changes.
* **ASCII vs UTF-8:** Direct key-typing (`type`, `click_and_type`) supports plain ASCII characters only; use `paste_text_at` or `clipboard_set` for arbitrary UTF-8 or multi-line text.
* **Verification:** When creating or saving a file on the Desktop, make sure the file is saved and verify on the screenshot that it exists on the Desktop before finishing.

# Final Response Guidelines:
Write final response to the user in the following cases:
- User confirmation
- When the task is complete or you have enough information to respond to the user
"""

FTP_SYSTEM_INSTRUCTION = """## Direct Remote File I/O Tools (RFB-to-FTP Bridge):
* **Prefer Direct File Tools:** When reading, writing, creating, inspecting, renaming, or deleting remote files/directories, prefer `file_list`, `file_stat`, `file_read`, `file_write`, `file_mkdir`, `file_delete`, and `file_rename` directly instead of opening GUI editors, file managers, or terminals.
* **Path Format:** Use `/` separators (e.g. `/tmp/notes.txt` or `/C:/Users/Alice/Desktop/hello.txt`). Windows paths such as `C:\\Users\\Alice\\Desktop\\hello.txt` are also normalized automatically.
* **Chunked & Binary I/O:** For large files, pass `offset` and `max_bytes` to `file_read`; for binary files, use `as_base64=true` on `file_read` and `content_base64` on `file_write`. Pass `append=true` or `create_parents=true` to `file_write` when appropriate.
* **Visual Verification When Requested:** Direct file tools return compact JSON without capturing a screenshot; if the task requires confirming a file is visually displayed on the desktop/UI, call `take_screenshot` afterward.
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


def normalize_remote_path(path: str) -> str:
    """Normalize POSIX or Windows remote file paths into canonical FTP paths."""
    if not isinstance(path, str):
        raise ValueError("Remote path must be a string")
    if "\x00" in path or "\r" in path or "\n" in path:
        raise ValueError("Remote path contains invalid control characters")
    replaced = path.strip().replace("\\", "/")
    parts: List[str] = []
    for seg in replaced.split("/"):
        if not seg or seg == ".":
            continue
        if seg == "..":
            if parts:
                parts.pop()
            continue
        parts.append(seg)
    if parts and len(parts[0]) == 2 and parts[0][0].isascii() and parts[0][0].isalpha() and parts[0][1] == ":":
        parts[0] = parts[0][0].upper() + ":"
    return "/" if not parts else "/" + "/".join(parts)


def is_windows_drive_root(norm_path: str) -> bool:
    """Return True if norm_path is a top-level Windows drive root like '/C:'."""
    return (
        len(norm_path) == 3
        and norm_path[0] == "/"
        and norm_path[1].isascii()
        and norm_path[1].isalpha()
        and norm_path[2] == ":"
    )


def split_remote_path(norm_path: str) -> Tuple[str, str]:
    """Split a normalized remote path into (parent_dir, base_name)."""
    norm = normalize_remote_path(norm_path)
    if norm == "/":
        return "/", ""
    parent, _, name = norm.rpartition("/")
    return (parent or "/"), name


def parse_ftp_url(url: str) -> Tuple[str, int, Optional[str], Optional[str]]:
    """Parse an ftp:// URL into (host, port, username, password)."""
    raw = (url or "").strip()
    if not raw:
        raise ValueError("FTP URL must not be empty")
    parsed = urllib.parse.urlsplit(raw)
    if parsed.scheme.lower() != "ftp" or not parsed.hostname:
        raise ValueError(f"Invalid FTP URL: {url!r}")
    port = parsed.port if parsed.port is not None else 2121
    user = urllib.parse.unquote(parsed.username) if parsed.username is not None else None
    passwd = urllib.parse.unquote(parsed.password) if parsed.password is not None else None
    return parsed.hostname, int(port), user, passwd


def redact_ftp_url(url: str) -> str:
    """Redact any embedded password in an FTP URL for logging."""
    try:
        parsed = urllib.parse.urlsplit(url)
        if parsed.password is None:
            return url
        host = parsed.hostname or ""
        if ":" in host:
            host = f"[{host}]"
        netloc = f"{host}:{parsed.port}" if parsed.port is not None else host
        if parsed.username is not None:
            netloc = f"{parsed.username}:***@{netloc}"
        else:
            netloc = f"***@{netloc}"
        return urllib.parse.urlunsplit((parsed.scheme, netloc, parsed.path, parsed.query, parsed.fragment))
    except Exception:
        return url


def vncviewer_supports_ftp(vncviewer_bin: str) -> bool:
    """Return True if the vncviewer binary advertises -FtpServer in --help."""
    if not vncviewer_bin or not os.path.exists(vncviewer_bin):
        return False
    try:
        proc = subprocess.run(
            [vncviewer_bin, "--help"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=5.0,
            check=False,
        )
        return "FtpServer" in (proc.stdout or "")
    except (OSError, subprocess.SubprocessError):
        return False


def discover_ftp_url_from_log(log_path: Optional[str]) -> Optional[str]:
    """Extract the most recent FTP bridge URL from a vncviewer log file."""
    if not log_path or not os.path.exists(log_path):
        return None
    try:
        with open(log_path, "r", encoding="utf-8", errors="replace") as f:
            content = f.read()
    except OSError:
        return None
    matches = re.findall(r"FTP server listening on (ftp://[^\s]+)", content)
    return matches[-1] if matches else None


def _is_ftp_transient_error(exc: Exception) -> bool:
    if isinstance(exc, (EOFError, BrokenPipeError, ConnectionResetError, ConnectionAbortedError, TimeoutError)):
        return True
    if isinstance(exc, ftplib.error_temp):
        return str(exc).startswith("421")
    if isinstance(exc, OSError):
        return True
    return False


def _parse_mlst_response(resp: str) -> Dict[str, str]:
    for line in resp.splitlines():
        if line.startswith(" "):
            facts_str, _, _ = line[1:].partition(" ")
            facts: Dict[str, str] = {}
            for item in facts_str.split(";"):
                if not item or "=" not in item:
                    continue
                k, _, v = item.partition("=")
                facts[k.strip().lower()] = v.strip()
            return facts
    raise ValueError(f"Unexpected MLST response format: {resp!r}")


def _parse_list_line(line: str) -> Optional[Tuple[str, Dict[str, str]]]:
    parts = line.rstrip("\r\n").split(None, 8)
    if len(parts) < 9:
        return None
    perms = parts[0]
    name = parts[8]
    if name in (".", ".."):
        return None
    facts: Dict[str, str] = {"type": "dir" if perms.startswith("d") else "file"}
    if not perms.startswith("d") and parts[4].isdigit():
        facts["size"] = parts[4]
    return name, facts


class VncFtpClient:
    """Direct remote file I/O client over TigerVNC's embedded -FtpServer bridge."""

    def __init__(
        self,
        host: str,
        port: int,
        username: str = "",
        password: str = "",
        timeout: float = 30.0,
    ) -> None:
        self.host = host
        self.port = int(port)
        self.username = username
        self.password = password
        self.timeout = float(timeout)
        self._ftp: Optional[ftplib.FTP] = None
        self._last_activity = 0.0
        self._is_binary = False
        self._features: List[str] = []

    def close(self) -> None:
        ftp = self._ftp
        self._ftp = None
        self._is_binary = False
        if ftp is not None:
            try:
                ftp.quit()
            except Exception:
                try:
                    ftp.close()
                except Exception:
                    pass

    def _reset(self) -> None:
        ftp = self._ftp
        self._ftp = None
        self._is_binary = False
        if ftp is not None:
            try:
                ftp.close()
            except Exception:
                pass

    def _ensure_connected(self) -> ftplib.FTP:
        if self._ftp is not None:
            if (time.monotonic() - self._last_activity) > 5.0:
                try:
                    self._ftp.voidcmd("NOOP")
                    self._last_activity = time.monotonic()
                except (*ftplib.all_errors, OSError, EOFError):
                    self._reset()

        if self._ftp is None:
            ftp = ftplib.FTP(timeout=self.timeout, encoding="utf-8")
            try:
                ftp.connect(self.host, self.port, timeout=self.timeout)
                ftp.set_pasv(True)
                ftp.login(user=(self.username or "agent"), passwd=(self.password or ""))
                try:
                    ftp.sendcmd("OPTS UTF8 ON")
                except ftplib.error_perm:
                    pass
                ftp.voidcmd("TYPE I")
            except Exception:
                try:
                    ftp.close()
                except Exception:
                    pass
                raise
            self._ftp = ftp
            self._is_binary = True
            self._last_activity = time.monotonic()

        return self._ftp

    def _ensure_binary(self, ftp: ftplib.FTP) -> None:
        if not self._is_binary:
            ftp.voidcmd("TYPE I")
            self._is_binary = True

    def _run_with_retry(self, fn: Any, idempotent: bool = True) -> Any:
        attempts = 2 if idempotent else 1
        for attempt in range(attempts):
            ftp = self._ensure_connected()
            try:
                res = fn(ftp)
                self._last_activity = time.monotonic()
                return res
            except (*ftplib.all_errors, OSError, EOFError) as exc:
                if _is_ftp_transient_error(exc):
                    self._reset()
                    if attempt + 1 < attempts:
                        continue
                raise

    def probe(self) -> Dict[str, Any]:
        def _do(ftp: ftplib.FTP) -> Dict[str, Any]:
            features: List[str] = []
            try:
                feat_resp = ftp.sendcmd("FEAT")
                for line in feat_resp.splitlines()[1:-1]:
                    cleaned = line.strip()
                    if cleaned:
                        features.append(cleaned)
            except ftplib.all_errors:
                pass
            self._features = features
            try:
                root_list = self._list_dir_impl(ftp, "/", DEFAULT_FILE_LIST_MAX_ENTRIES)
                return {
                    "connected": True,
                    "supported": True,
                    "features": features,
                    "root_entries": root_list["entries"],
                }
            except ftplib.error_perm as exc:
                msg = str(exc)
                if "not supported" in msg.lower():
                    return {
                        "connected": True,
                        "supported": False,
                        "features": features,
                        "reason": msg,
                    }
                raise

        return self._run_with_retry(_do, idempotent=True)

    def _list_dir_impl(self, ftp: ftplib.FTP, norm: str, limit: int) -> Dict[str, Any]:
        entries: List[Dict[str, Any]] = []
        try:
            raw_items = list(ftp.mlsd(norm))
            self._is_binary = False
        except ftplib.error_perm as exc:
            code = str(exc)[:3]
            if code not in ("500", "502"):
                raise
            lines: List[str] = []
            ftp.retrlines(f"LIST {norm}", lines.append)
            self._is_binary = False
            raw_items = []
            for line in lines:
                parsed = _parse_list_line(line)
                if parsed is not None:
                    raw_items.append(parsed)

        for name, facts in raw_items:
            ftype_raw = str(facts.get("type", "")).lower()
            if ftype_raw in ("cdir", "pdir") or name in (".", ".."):
                continue
            is_dir = ftype_raw == "dir"
            full_path = f"/{name}" if norm == "/" else f"{norm}/{name}"
            item: Dict[str, Any] = {
                "name": name,
                "path": full_path,
                "type": "dir" if is_dir else "file",
            }
            if not is_dir and "size" in facts:
                try:
                    item["size"] = int(facts["size"])
                except ValueError:
                    pass
            if "modify" in facts and facts["modify"]:
                item["modified"] = facts["modify"]
            entries.append(item)

        entries.sort(key=lambda e: (0 if e["type"] == "dir" else 1, e["name"].lower(), e["name"]))
        sliced = entries[:limit]
        return {
            "status": "ok",
            "path": norm,
            "entries": sliced,
            "returned_entries": len(sliced),
            "total_entries": len(entries),
            "truncated": len(entries) > limit,
        }

    def list_dir(
        self,
        path: str = "/",
        max_entries: int = DEFAULT_FILE_LIST_MAX_ENTRIES,
    ) -> Dict[str, Any]:
        norm = normalize_remote_path(path or "/")
        if isinstance(max_entries, bool):
            raise ValueError("max_entries must be an integer")
        limit = max(1, min(int(max_entries), MAX_FILE_LIST_ENTRIES))
        return self._run_with_retry(
            lambda ftp: self._list_dir_impl(ftp, norm, limit),
            idempotent=True,
        )

    def stat(self, path: str) -> Dict[str, Any]:
        norm = normalize_remote_path(path)
        if norm == "/":
            return {"status": "ok", "path": "/", "exists": True, "type": "dir"}

        def _do(ftp: ftplib.FTP) -> Dict[str, Any]:
            try:
                resp = ftp.sendcmd(f"MLST {norm}")
            except ftplib.error_perm as exc:
                msg = str(exc).lower()
                if msg.startswith("550") and any(
                    tok in msg for tok in ("not found", "cannot find", "no such", "does not exist")
                ):
                    return {"status": "ok", "path": norm, "exists": False}
                raise

            facts = _parse_mlst_response(resp)
            is_dir = str(facts.get("type", "")).lower() == "dir"
            res: Dict[str, Any] = {
                "status": "ok",
                "path": norm,
                "exists": True,
                "type": "dir" if is_dir else "file",
            }
            if not is_dir and "size" in facts:
                try:
                    res["size"] = int(facts["size"])
                except ValueError:
                    pass
            if "modify" in facts and facts["modify"]:
                res["modified"] = facts["modify"]
            return res

        return self._run_with_retry(_do, idempotent=True)

    def read_file(
        self,
        path: str,
        encoding: str = "utf-8",
        as_base64: bool = False,
        offset: int = 0,
        max_bytes: int = DEFAULT_FILE_READ_MAX_BYTES,
    ) -> Dict[str, Any]:
        norm = normalize_remote_path(path)
        if norm == "/" or is_windows_drive_root(norm):
            raise ValueError(f"Cannot read directory/root path as a file: {norm}")
        if isinstance(offset, bool) or isinstance(max_bytes, bool):
            raise ValueError("offset and max_bytes must be integers")
        offset = int(offset)
        max_bytes = int(max_bytes)
        if offset < 0:
            raise ValueError("offset must be >= 0")
        if max_bytes < 1:
            raise ValueError("max_bytes must be >= 1")
        limit = min(max_bytes, MAX_FILE_READ_BYTES)
        if not as_base64:
            codecs.lookup(encoding)

        st = self.stat(norm)
        if not st.get("exists"):
            raise FileNotFoundError(f"Remote file not found: {norm}")
        if st.get("type") == "dir":
            raise IsADirectoryError(f"Remote path is a directory: {norm}")
        total_size = st.get("size")
        if total_size is not None and offset >= int(total_size):
            empty_res: Dict[str, Any] = {
                "status": "ok",
                "path": norm,
                "offset": offset,
                "bytes_read": 0,
                "total_size": int(total_size),
                "truncated": False,
                "eof": True,
            }
            if as_base64:
                empty_res["encoding"] = "base64"
                empty_res["content_base64"] = ""
            else:
                empty_res["encoding"] = encoding
                empty_res["content"] = ""
            return empty_res

        def _do(ftp: ftplib.FTP) -> Tuple[bytes, bool]:
            self._ensure_binary(ftp)
            conn = ftp.transfercmd(f"RETR {norm}", rest=(offset if offset > 0 else None))
            buf = bytearray()
            try:
                while len(buf) <= limit:
                    want = min(65536, limit + 1 - len(buf))
                    chunk = conn.recv(want)
                    if not chunk:
                        break
                    buf.extend(chunk)
            finally:
                try:
                    conn.close()
                except OSError:
                    pass

            truncated_local = len(buf) > limit
            try:
                ftp.voidresp()
            except ftplib.error_temp as exc:
                if truncated_local and str(exc).startswith("426"):
                    pass
                else:
                    raise
            except (EOFError, OSError, TimeoutError):
                self._reset()
                if not truncated_local:
                    raise

            return bytes(buf[:limit]), truncated_local

        raw, truncated = self._run_with_retry(_do, idempotent=True)
        eof = not truncated
        if total_size is not None and (offset + len(raw)) >= int(total_size):
            eof = True

        result: Dict[str, Any] = {
            "status": "ok",
            "path": norm,
            "offset": offset,
            "bytes_read": len(raw),
            "truncated": truncated,
            "eof": eof,
        }
        if total_size is not None:
            result["total_size"] = int(total_size)

        if as_base64:
            result["encoding"] = "base64"
            result["content_base64"] = base64.b64encode(raw).decode("ascii")
        else:
            result["encoding"] = encoding
            try:
                result["content"] = raw.decode(encoding)
            except UnicodeDecodeError:
                result["content"] = raw.decode(encoding, errors="replace")
                result["had_decode_errors"] = True
                result["note"] = (
                    "Some bytes could not be decoded cleanly; retry with as_base64=true for binary files."
                )
        return result

    def _is_existing_dir(self, norm_path: str) -> bool:
        try:
            self.list_dir(norm_path, max_entries=1)
            return True
        except Exception:
            return False

    def mkdir(self, path: str, parents: bool = True) -> Dict[str, Any]:
        norm = normalize_remote_path(path)
        if norm == "/" or is_windows_drive_root(norm):
            raise ValueError(f"Cannot create root/drive directory: {norm}")

        created: List[str] = []
        if parents:
            segs = norm.strip("/").split("/")
            cur = ""
            for seg in segs:
                cur = f"{cur}/{seg}"
                if is_windows_drive_root(cur):
                    continue
                st = self.stat(cur)
                if st.get("exists"):
                    if st.get("type") != "dir":
                        raise FileExistsError(f"Path exists and is not a directory: {cur}")
                    continue
                try:
                    self._run_with_retry(lambda ftp, target=cur: ftp.mkd(target), idempotent=False)
                    created.append(cur)
                except ftplib.error_perm as exc:
                    msg = str(exc).lower()
                    if "exist" in msg and self._is_existing_dir(cur):
                        continue
                    raise
        else:
            self._run_with_retry(lambda ftp: ftp.mkd(norm), idempotent=False)
            created.append(norm)

        return {"status": "ok", "path": norm, "created": created}

    def write_file(
        self,
        path: str,
        content: Optional[str] = None,
        content_base64: Optional[str] = None,
        encoding: str = "utf-8",
        append: bool = False,
        create_parents: bool = False,
    ) -> Dict[str, Any]:
        norm = normalize_remote_path(path)
        if norm == "/" or is_windows_drive_root(norm):
            raise ValueError(f"Cannot write to directory/root path: {norm}")
        if (content is None) == (content_base64 is None):
            raise ValueError("Provide exactly one of 'content' or 'content_base64'")

        if content_base64 is not None:
            if not isinstance(content_base64, str):
                raise ValueError("content_base64 must be a string")
            raw = base64.b64decode(content_base64, validate=True)
        else:
            codecs.lookup(encoding)
            raw = str(content).encode(encoding)

        if len(raw) > MAX_FILE_WRITE_BYTES:
            raise ValueError(
                f"Write payload ({len(raw)} bytes) exceeds MAX_FILE_WRITE_BYTES ({MAX_FILE_WRITE_BYTES})"
            )

        if create_parents:
            parent, _ = split_remote_path(norm)
            if parent != "/" and not is_windows_drive_root(parent):
                self.mkdir(parent, parents=True)

        ftp_cmd = f"APPE {norm}" if append else f"STOR {norm}"

        def _do(ftp: ftplib.FTP) -> None:
            ftp.storbinary(ftp_cmd, io.BytesIO(raw))
            self._is_binary = True

        self._run_with_retry(_do, idempotent=(not append))
        return {
            "status": "ok",
            "path": norm,
            "bytes_written": len(raw),
            "mode": "append" if append else "overwrite",
        }

    def delete(self, path: str, is_dir: bool = False) -> Dict[str, Any]:
        norm = normalize_remote_path(path)
        if norm == "/" or is_windows_drive_root(norm):
            raise ValueError(f"Cannot delete root/drive path: {norm}")

        def _do(ftp: ftplib.FTP) -> None:
            if is_dir:
                ftp.rmd(norm)
            else:
                ftp.delete(norm)

        self._run_with_retry(_do, idempotent=False)
        return {
            "status": "ok",
            "path": norm,
            "deleted_type": "dir" if is_dir else "file",
        }

    def rename(self, from_path: str, to_path: str) -> Dict[str, Any]:
        norm_from = normalize_remote_path(from_path)
        norm_to = normalize_remote_path(to_path)
        if norm_from == "/" or is_windows_drive_root(norm_from):
            raise ValueError(f"Cannot rename root/drive path: {norm_from}")
        if norm_to == "/" or is_windows_drive_root(norm_to):
            raise ValueError(f"Invalid target path: {norm_to}")

        self._run_with_retry(
            lambda ftp: ftp.rename(norm_from, norm_to),
            idempotent=False,
        )
        return {
            "status": "ok",
            "from_path": norm_from,
            "to_path": norm_to,
        }


class VncDesktopExecutor:
    """Executes Gemini ENVIRONMENT_DESKTOP actions against TigerVNC's AgentSocket."""

    def __init__(self, client: AgentClient, ftp_client: Optional[VncFtpClient] = None) -> None:
        self.client = client
        self.ftp_client = ftp_client
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

    def pause_agent(self) -> None:
        resp, _ = self.client.request("PAUSE_AGENT")
        if not resp.get("ok"):
            raise RuntimeError(f"PAUSE_AGENT failed: {resp}")

    def resume_agent(self) -> None:
        resp, _ = self.client.request("RESUME_AGENT")
        if not resp.get("ok"):
            raise RuntimeError(f"RESUME_AGENT failed: {resp}")

    def clipboard_get(self) -> Dict[str, Any]:
        resp, _ = self.client.request("CLIPBOARD_GET")
        if not resp.get("ok"):
            raise RuntimeError(f"CLIPBOARD_GET failed: {resp}")
        return {
            "status": "ok",
            "available": bool(resp.get("available")),
            "text": str(resp.get("text", "")),
        }

    def clipboard_set(self, text: str) -> Dict[str, Any]:
        resp, _ = self.client.request("CLIPBOARD_SET", text=text)
        if not resp.get("ok"):
            if resp.get("error") == "agent_paused":
                raise AgentPausedError(resp.get("message", "Agent input paused by operator"))
            raise RuntimeError(f"CLIPBOARD_SET failed: {resp}")
        return {
            "status": "ok",
            "bytes": int(resp.get("bytes", len(text.encode("utf-8")))),
        }

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

    def _hotkey_syms(self, keysyms: List[int]) -> None:
        for ks in keysyms:
            self._key(ks, True)
            time.sleep(0.04)
        time.sleep(0.06)
        for ks in reversed(keysyms):
            self._key(ks, False)
            time.sleep(0.04)

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

        if fname == "clipboard_set":
            text = str(args.get("text", ""))
            paste = bool(args.get("paste", False))
            clip_res = self.clipboard_set(text)
            if paste:
                time.sleep(0.05)
                self._hotkey_syms([0xFFE3, ord("v")])
            clip_res["pasted"] = paste
            return clip_res

        if fname == "paste_text_at":
            px = denormalize_x(args["x"], self.width)
            py = denormalize_y(args["y"], self.height)
            text = str(args.get("text", ""))
            clear_existing = bool(args.get("clear_existing", False))
            press_enter = bool(args.get("press_enter", False))
            self._click_px(px, py, btn_bit=1, count=1)
            time.sleep(0.10)
            if clear_existing:
                self._hotkey_syms([0xFFE3, ord("a")])
                time.sleep(0.05)
                self._hotkey_syms([0xFF08])
                time.sleep(0.05)
            clip_res = self.clipboard_set(text)
            time.sleep(0.05)
            self._hotkey_syms([0xFFE3, ord("v")])
            if press_enter:
                time.sleep(0.05)
                self._hotkey_syms([0xFF0D])
            return {
                "status": "ok",
                "pixel_x": px,
                "pixel_y": py,
                "chars_pasted": len(text),
                "bytes": clip_res["bytes"],
                "clear_existing": clear_existing,
                "press_enter": press_enter,
            }

        if fname == "click_and_type":
            px = denormalize_x(args["x"], self.width)
            py = denormalize_y(args["y"], self.height)
            text = str(args.get("text", ""))
            clear_existing = bool(args.get("clear_existing", False))
            press_enter = bool(args.get("press_enter", False))
            self._click_px(px, py, btn_bit=1, count=1)
            time.sleep(0.10)
            if clear_existing:
                self._hotkey_syms([0xFFE3, ord("a")])
                time.sleep(0.05)
                self._hotkey_syms([0xFF08])
                time.sleep(0.05)
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


class InteractionsApiError(RuntimeError):
    """Structured error from the Gemini Interactions REST API."""

    def __init__(self, status_code: Optional[int], raw_body: str) -> None:
        self.status_code = status_code
        self.raw_body = raw_body or ""
        self.error_message = self.raw_body.strip()
        self.error_code = ""

        try:
            parsed = json.loads(self.raw_body)
        except (json.JSONDecodeError, TypeError):
            parsed = None

        if isinstance(parsed, dict):
            err_obj = parsed.get("error") if isinstance(parsed.get("error"), dict) else parsed
            msg = err_obj.get("message")
            code = err_obj.get("code") or err_obj.get("status")
            if isinstance(msg, str) and msg.strip():
                self.error_message = msg.strip()
            if isinstance(code, (str, int)):
                self.error_code = str(code).strip()

        if status_code is not None:
            summary = f"Interactions API HTTP {status_code}: {self.error_message or self.error_code or 'Unknown error'}"
        else:
            summary = f"Interactions API error: {self.error_message or 'Unknown error'}"
        super().__init__(summary)

    @property
    def is_safety_blocked(self) -> bool:
        hay = f"{self.error_message} {self.error_code}".lower()
        if any(tok in hay for tok in ("safety", "harmful", "policy violation", "prohibited_content", "content_filter")):
            return True
        if "blocked" in hay and any(tok in hay for tok in ("safety", "harmful", "policy", "content")):
            return True
        return False

    @property
    def is_auth_error(self) -> bool:
        return self.status_code in (401, 403)

    @property
    def is_previous_interaction_error(self) -> bool:
        if self.is_safety_blocked or self.is_auth_error:
            return False
        if self.status_code in (429, 500, 502, 503, 504) or self.status_code is None:
            return False
        if self.status_code == 404:
            return True
        if self.status_code == 400:
            msg = self.error_message.lower()
            if "previous_interaction" in msg:
                return True
            if "interaction" in msg and any(
                tok in msg for tok in ("not found", "expired", "invalid", "unknown", "requires_action", "pending")
            ):
                return True
        return False


class InteractionsClient:
    """Thin REST client for Gemini v1beta Interactions API with computer_use."""

    def __init__(
        self,
        api_key: str,
        model: str,
        tools: List[Dict[str, Any]],
        system_instruction: str,
        generation_config: Optional[Dict[str, Any]] = None,
    ) -> None:
        self.api_key = api_key
        self.model = model
        self.tools = tools
        self.system_instruction = system_instruction
        self.default_generation_config: Dict[str, Any] = dict(generation_config or {})

    def create(
        self,
        input_payload: Any,
        previous_interaction_id: Optional[str] = None,
        thinking_level: Optional[str] = None,
    ) -> Dict[str, Any]:
        body: Dict[str, Any] = {
            "model": self.model,
            "input": input_payload,
            "tools": self.tools,
            "system_instruction": self.system_instruction,
        }
        if previous_interaction_id:
            body["previous_interaction_id"] = previous_interaction_id

        gen_cfg = dict(self.default_generation_config)
        if thinking_level is not None and thinking_level != "default":
            gen_cfg["thinking_level"] = thinking_level
        if gen_cfg:
            body["generation_config"] = gen_cfg

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
                    raw_resp = resp.read().decode("utf-8")
                try:
                    return json.loads(raw_resp)
                except json.JSONDecodeError as e:
                    raise InteractionsApiError(None, f"Invalid JSON response from Interactions API: {e}") from None
            except urllib.error.HTTPError as e:
                err_text = e.read().decode("utf-8", errors="replace")
                api_err = InteractionsApiError(e.code, err_text)
                if e.code in (429, 500, 502, 503, 504) and attempt < 2:
                    last_err = api_err
                    time.sleep(1.5 * (2**attempt))
                    continue
                raise api_err from None
            except (urllib.error.URLError, TimeoutError, OSError) as e:
                if attempt < 2:
                    last_err = e
                    time.sleep(1.5 * (2**attempt))
                    continue
                raise InteractionsApiError(None, f"Network error: {e}") from None
        if isinstance(last_err, InteractionsApiError):
            raise last_err
        raise InteractionsApiError(None, f"Interactions API failed after retries: {last_err}")


def parse_excluded_actions(raw_values: Optional[List[str]]) -> List[str]:
    excluded: List[str] = []
    seen = set()
    for item in raw_values or []:
        for part in str(item).split(","):
            name = part.strip()
            if not name or name in seen:
                continue
            seen.add(name)
            if name not in PREDEFINED_DESKTOP_FUNCTIONS:
                print(
                    f"  [WARN] Excluded action {name!r} is not in PREDEFINED_DESKTOP_FUNCTIONS; "
                    "forwarding as configured."
                )
            excluded.append(name)
    return excluded


def build_tools_config(
    strict_safety: bool,
    excluded_actions: List[str],
    enable_custom_tools: bool,
    enable_file_tools: bool = False,
) -> List[Dict[str, Any]]:
    cu_cfg: Dict[str, Any] = {
        "type": "computer_use",
        "environment": "desktop",
        "enable_prompt_injection_detection": True,
    }
    if excluded_actions:
        cu_cfg["excluded_predefined_functions"] = list(excluded_actions)
    if not strict_safety:
        cu_cfg["disabled_safety_policies"] = ["data_modification"]

    tools: List[Dict[str, Any]] = [cu_cfg]
    if enable_custom_tools:
        tools.extend(copy.deepcopy(CUSTOM_FUNCTION_DECLARATIONS))
    if enable_file_tools:
        tools.extend(copy.deepcopy(FILE_FUNCTION_DECLARATIONS))
    return tools


def is_sandboxed_local_target(vnc_host: str, socket_path: Optional[str] = None) -> bool:
    if socket_path:
        return True
    raw = (vnc_host or "").strip().lower()
    if not raw:
        return False
    if raw.startswith("["):
        host = raw[1:].split("]", 1)[0]
    elif raw == "::1":
        host = "::1"
    elif raw.startswith(":"):
        host = "localhost"
    else:
        host = raw.split(":", 1)[0]
    return host in ("localhost", "127.0.0.1", "::1")


def handle_safety_decision(
    safety_decision: Dict[str, Any],
    auto_confirm: bool,
    executor: Optional[VncDesktopExecutor] = None,
    collaborate_mode: bool = False,
    poll_interval: float = 0.25,
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

        stdin_enabled = bool(sys.stdin is not None and sys.stdin.isatty())
        if collaborate_mode and executor is not None:
            print("  -> Confirm via vncviewer note ('/allow' or '/deny') or terminal below:")
            if stdin_enabled:
                print("  Allow this action? [y/N]: ", end="", flush=True)
            try:
                while True:
                    try:
                        st = executor.get_status()
                    except (EOFError, ConnectionError, BrokenPipeError, OSError):
                        print("\n  -> Viewer disconnected during confirmation; halting execution.")
                        return "TERMINATE", False
                    if not st.get("connected"):
                        print("\n  -> VNC connection closed during confirmation; halting execution.")
                        return "TERMINATE", False
                    if int(st.get("pending_user_messages", 0)) > 0:
                        notes, _, _ = executor.poll_user_messages()
                        for raw_note in notes:
                            cleaned = str(raw_note).strip().lower()
                            if cleaned in ("/allow", "allow", "/yes", "yes", "y"):
                                print("\n  -> Confirmed via vncviewer note.")
                                return "ALLOW", True
                            if cleaned in ("/deny", "deny", "/no", "no", "n", "/exit", "/quit"):
                                print("\n  -> Denied via vncviewer note; halting execution.")
                                return "TERMINATE", False
                            if cleaned:
                                print(
                                    f"\n  [COLLAB] Received note {raw_note!r} while awaiting confirmation; "
                                    "send '/allow' or '/deny'."
                                )
                    if stdin_enabled:
                        readable, _, _ = select.select([sys.stdin], [], [], poll_interval)
                        if readable:
                            line = sys.stdin.readline()
                            if line == "":
                                print("\n  -> Stdin EOF; halting execution.")
                                return "TERMINATE", False
                            ans = line.strip().lower()
                            if ans in ("y", "yes", "/yes", "/allow", "allow"):
                                return "ALLOW", True
                            print("  -> Denied; halting execution.")
                            return "TERMINATE", False
                    else:
                        time.sleep(poll_interval)
            except KeyboardInterrupt:
                print("\n  -> Interrupted during confirmation; halting execution.")
                return "TERMINATE", False

        if stdin_enabled:
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
    viewer_log_path: Optional[str] = None
    ftp_client: Optional[VncFtpClient] = None
    spawned_ftp = False
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
            if not args.no_ftp and not args.ftp_url:
                supports_ftp_flag = vncviewer_supports_ftp(args.vncviewer_bin)
                if args.ftp and not supports_ftp_flag:
                    raise RuntimeError(
                        f"vncviewer binary {args.vncviewer_bin!r} does not support -FtpServer "
                        "(pass --ftp-url or build from draft-full/draft-ftp)."
                    )
                if supports_ftp_flag:
                    cmd.extend(
                        [
                            "-FtpServer",
                            "-FtpBindAddress",
                            args.ftp_bind_address,
                            "-FtpPort",
                            str(args.ftp_port),
                            "-FtpProtocol",
                            args.ftp_protocol,
                        ]
                    )
                    if args.ftp_username:
                        cmd.extend(["-FtpUsername", args.ftp_username])
                    if args.ftp_password:
                        cmd.extend(["-FtpPassword", args.ftp_password])
                    spawned_ftp = True
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

        raw_base_thinking = str(getattr(args, "thinking_level", "low") or "low")
        base_thinking_level: Optional[str] = None if raw_base_thinking == "default" else raw_base_thinking
        raw_rec_thinking = str(getattr(args, "recovery_thinking_level", "high") or "high")
        recovery_thinking_level: Optional[str] = None if raw_rec_thinking == "off" else raw_rec_thinking
        excluded_actions = parse_excluded_actions(getattr(args, "exclude_actions", None))
        enable_custom_tools = not bool(getattr(args, "no_custom_tools", False))

        with AgentClient(socket_path, timeout=15.0) as vnc_client:
            executor = VncDesktopExecutor(vnc_client)
            status = executor.wait_ready(timeout=15.0)
            sandboxed = is_sandboxed_local_target(args.vnc_host, args.socket)
            endpoint_desc = f"socket={args.socket}" if args.socket else f"host={args.vnc_host}"
            print(
                f"Connected to VNC desktop: {status['width']}x{status['height']} "
                f"({status.get('desktop_name', '')})"
            )

            ftp_active = False
            ftp_display_url = ""
            system_instruction = SYSTEM_INSTRUCTION
            if not args.no_ftp:
                ftp_url: Optional[str] = args.ftp_url
                if not ftp_url:
                    if status.get("ftp_running") and status.get("ftp_url"):
                        ftp_url = str(status["ftp_url"])
                    elif spawned_ftp:
                        ftp_url = discover_ftp_url_from_log(viewer_log_path)
                        if not ftp_url:
                            ftp_deadline = time.monotonic() + 2.0
                            while time.monotonic() < ftp_deadline:
                                status = executor.get_status()
                                if status.get("ftp_running") and status.get("ftp_url"):
                                    ftp_url = str(status["ftp_url"])
                                    break
                                ftp_url = discover_ftp_url_from_log(viewer_log_path)
                                if ftp_url:
                                    break
                                time.sleep(0.1)

                if ftp_url:
                    if status.get("ftp_running") and not status.get("ftp_ready", True):
                        ready_deadline = time.monotonic() + 2.0
                        while time.monotonic() < ready_deadline:
                            status = executor.get_status()
                            if status.get("ftp_ready") or not status.get("ftp_running"):
                                break
                            time.sleep(0.1)

                    ftp_host, ftp_port_num, url_user, url_pass = parse_ftp_url(ftp_url)
                    ftp_user = args.ftp_username if args.ftp_username else (url_user or "")
                    ftp_pass = args.ftp_password if args.ftp_password else (url_pass or "")
                    ftp_client = VncFtpClient(
                        host=ftp_host,
                        port=ftp_port_num,
                        username=ftp_user,
                        password=ftp_pass,
                        timeout=args.ftp_timeout,
                    )
                    probe_deadline = time.monotonic() + 2.0
                    probe_res: Optional[Dict[str, Any]] = None
                    last_probe_err: Optional[Exception] = None
                    while True:
                        try:
                            probe_res = ftp_client.probe()
                            break
                        except Exception as exc:
                            last_probe_err = exc
                            ftp_client._reset()
                            if time.monotonic() >= probe_deadline:
                                break
                            time.sleep(0.15)

                    if probe_res is None:
                        ftp_client.close()
                        ftp_client = None
                        if args.ftp:
                            raise RuntimeError(
                                f"Failed to probe FTP bridge at {redact_ftp_url(ftp_url)}: {last_probe_err}"
                            )
                        print(f"  [WARN] FTP bridge probe failed ({last_probe_err}); direct file tools disabled.")
                    elif probe_res.get("supported"):
                        ftp_active = True
                        ftp_display_url = redact_ftp_url(ftp_url)
                        executor.ftp_client = ftp_client
                        system_instruction = SYSTEM_INSTRUCTION + "\n" + FTP_SYSTEM_INSTRUCTION
                        root_entries = probe_res.get("root_entries") or []
                        if root_entries:
                            summary = ", ".join(
                                f"{e['path']} ({e['type']})" for e in root_entries[:20]
                            )
                            system_instruction += f"\nDetected remote root entries: {summary}\n"
                    else:
                        ftp_client.close()
                        ftp_client = None
                        print(
                            "[INFO] Remote VNC server does not support RFB file transfer; "
                            "direct file tools disabled."
                        )
                elif args.ftp:
                    raise RuntimeError(
                        "Direct FTP file tools requested (--ftp) but no FTP bridge was detected "
                        "(pass --ftp-url or connect to a vncviewer running with -FtpServer)."
                    )

            enable_file_tools = ftp_active and (enable_custom_tools or bool(args.ftp))
            tools = build_tools_config(
                strict_safety=bool(args.strict_safety),
                excluded_actions=excluded_actions,
                enable_custom_tools=enable_custom_tools,
                enable_file_tools=enable_file_tools,
            )

            llm = InteractionsClient(
                api_key=api_key,
                model=args.model,
                tools=tools,
                system_instruction=system_instruction,
                generation_config=({"thinking_level": base_thinking_level} if base_thinking_level else None),
            )

            ftp_banner = f"on ({ftp_display_url})" if ftp_active else "off"
            print(
                f"Session config: model={args.model} | thinking={raw_base_thinking} "
                f"(recovery={raw_rec_thinking}) | custom_tools={'on' if enable_custom_tools else 'off'} "
                f"| file_tools={'on' if enable_file_tools else 'off'} | ftp={ftp_banner} "
                f"| excluded={excluded_actions or 'none'} | prompt_injection_detection=on "
                f"| strict_safety={'on' if args.strict_safety else 'off'} | target={endpoint_desc} "
                f"({'local/sandboxed' if sandboxed else 'NON-LOOPBACK'})"
            )
            if not sandboxed:
                print(
                    "  [WARN] Target VNC host is non-loopback; Gemini Computer Use recommends running inside an "
                    "isolated VM/container sandbox to reduce UI drift and limit host blast radius."
                )

            collaborate_mode = bool(args.collaborate or status.get("collaborate"))
            stay_interactive = collaborate_mode and not bool(getattr(args, "one_shot", False))

            def select_thinking_level(reason: Optional[str]) -> Optional[str]:
                if reason and recovery_thinking_level is not None and recovery_thinking_level != base_thinking_level:
                    print(
                        f"  [THINKING] Escalating thinking_level {raw_base_thinking} -> "
                        f"{recovery_thinking_level} (reason={reason})"
                    )
                    return recovery_thinking_level
                return base_thinking_level

            def format_collab_prompt(next_text: str, human_delta: int) -> str:
                if human_delta > 0:
                    return (
                        f"[Human collaborator performed {human_delta} desktop action(s) before this instruction; "
                        f"inspect the updated screenshot.]\n{next_text}"
                    )
                return next_text

            def handle_api_error(err: InteractionsApiError, interaction_id: Optional[str]) -> str:
                if err.is_safety_blocked:
                    print(f"\n[SAFETY BLOCKED] Gemini API rejected this request: {err.error_message}")
                    print("  Please modify/rephrase your instruction or perform the action manually in vncviewer.")
                    wait_intent = summarize_for_overlay(
                        f"Blocked: {err.error_message}",
                        fallback="Request blocked by safety filter - modify input and retry",
                    )
                    status_label = "safety_blocked"
                else:
                    print(f"\n[API ERROR] {err}")
                    wait_intent = summarize_for_overlay(
                        f"API error: {err.error_message}",
                        fallback="API error - retry or send new instruction",
                    )
                    status_label = "api_error"

                if trace_file:
                    entry: Dict[str, Any] = {
                        "turn": global_turn,
                        "task": task_index,
                        "status": status_label,
                        "http_status": err.status_code,
                        "error_code": err.error_code,
                        "error": err.error_message,
                    }
                    if interaction_id:
                        entry["interaction_id"] = interaction_id
                    trace_file.write(json.dumps(entry, ensure_ascii=False) + "\n")
                    trace_file.flush()
                return wait_intent

            def start_user_turn(
                prompt_text: str,
                png_bytes: bytes,
                prev_id: Optional[str],
                thinking_level: Optional[str],
            ) -> Dict[str, Any]:
                nonlocal last_completed_interaction_id
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
                        return llm.create(
                            input_payload=payload,
                            previous_interaction_id=prev_id,
                            thinking_level=thinking_level,
                        )
                    except RuntimeError as e:
                        should_retry_fresh = False
                        if isinstance(e, InteractionsApiError):
                            should_retry_fresh = e.is_previous_interaction_error
                        else:
                            msg_lower = str(e).lower()
                            if (
                                "http 404" in msg_lower
                                or "not found" in msg_lower
                                or "previous_interaction" in msg_lower
                            ) and "safety" not in msg_lower and "harmful" not in msg_lower:
                                should_retry_fresh = True
                        if not should_retry_fresh:
                            raise
                        print(f"  [COLLAB] Starting fresh LLM session (previous interaction unavailable: {e})")
                        last_completed_interaction_id = None
                return llm.create(
                    input_payload=payload,
                    previous_interaction_id=None,
                    thinking_level=thinking_level,
                )

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
                        current_prompt = format_collab_prompt(next_text, human_delta)
                    else:
                        raise ValueError("--prompt must not be empty in non-collaborative mode")

                task_index += 1
                consecutive_no_change_turns = 0
                init_thinking_lv = select_thinking_level(None)
                init_png, _ = executor.capture_png(settle_delay=0.2, max_wait=1.0)
                if log_dir:
                    init_name = "turn_00_initial.png" if task_index == 1 else f"task_{task_index:02d}_initial.png"
                    with open(os.path.join(log_dir, init_name), "wb") as f:
                        f.write(init_png)

                print(f"Goal: {current_prompt}")
                executor.set_overlay("thinking", "thinking", "Analyzing screen and planning next action...")
                try:
                    interaction = start_user_turn(
                        current_prompt,
                        init_png,
                        last_completed_interaction_id,
                        thinking_level=init_thinking_lv,
                    )
                except InteractionsApiError as e:
                    current_prompt = None
                    wait_intent = handle_api_error(e, None)
                    if not stay_interactive or e.is_auth_error:
                        executor.set_overlay("done", "error", wait_intent)
                        return 2
                    nxt = executor.wait_for_next_instruction(
                        viewer_proc=viewer_proc,
                        overlay_intent=wait_intent,
                    )
                    if nxt is None:
                        return 0
                    next_text, human_delta = nxt
                    current_prompt = format_collab_prompt(next_text, human_delta)
                    continue
                current_prompt = None

                task_completed = False
                task_halted = False
                wait_intent = "Awaiting next instruction"
                turn_thinking_lv = init_thinking_lv

                for task_turn in range(1, args.max_turns + 1):
                    global_turn += 1
                    print(
                        f"\n--- Turn {global_turn} "
                        f"(task={task_index}.{task_turn}, interaction={interaction.get('id')}, "
                        f"status={interaction.get('status')}, thinking={turn_thinking_lv or 'default'}) ---"
                    )
                    steps = interaction.get("steps", [])
                    function_calls = [s for s in steps if s.get("type") == "function_call"]

                    if not function_calls:
                        final_text = extract_model_text(interaction)
                        last_completed_interaction_id = interaction.get("id")
                        consecutive_no_change_turns = 0
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
                                        "thinking_level": turn_thinking_lv,
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

                    if len(function_calls) > 1:
                        print(f"  [BATCH] Executing {len(function_calls)} actions:")
                        for b_idx, b_fc in enumerate(function_calls, 1):
                            b_intent = str((b_fc.get("arguments") or {}).get("intent", "N/A"))
                            print(f"    {b_idx}. {b_fc.get('name', '')}: {b_intent}")

                    halted_reason: Optional[str] = None
                    results: List[Tuple[str, str, Dict[str, Any], bool]] = []
                    turn_log_actions: List[Dict[str, Any]] = []
                    collab_notes: List[str] = []
                    collaborator_intervened = False
                    batch_had_error = False
                    batch_had_resize = False
                    any_modifying_action = False
                    any_frame_changed = False
                    any_file_mutating_success = False

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
                                    results.append((rem_name, rem_id, cancel_res, True))
                                    turn_log_actions.append({"name": rem_name, "args": rem_fc.get("arguments") or {}, "result": cancel_res})
                                break

                        action_result: Dict[str, Any] = {}
                        safety_audit: Optional[Dict[str, Any]] = None
                        safety_decision = fc_args.get("safety_decision")
                        if isinstance(safety_decision, dict):
                            decision_str = str(safety_decision.get("decision", "")).lower()
                            if decision_str == "require_confirmation":
                                executor.set_overlay(
                                    "waiting_confirmation",
                                    fname,
                                    str(safety_decision.get("explanation", "Safety confirmation required")),
                                )
                            s_status, ack = handle_safety_decision(
                                safety_decision,
                                args.auto_confirm,
                                executor=executor,
                                collaborate_mode=collaborate_mode,
                            )
                            safety_audit = {
                                "decision": decision_str,
                                "status": s_status,
                                "acknowledged": ack,
                                "explanation": str(safety_decision.get("explanation", "")),
                            }
                            if s_status != "ALLOW":
                                halted_reason = f"Safety decision {s_status}: {safety_decision.get('explanation', '')}"
                                action_result = {"error": halted_reason}
                                batch_had_error = True
                                results.append((fname, call_id, action_result, True))
                                turn_log_actions.append(
                                    {"name": fname, "args": fc_args, "result": action_result, "safety_decision": safety_audit}
                                )
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
                            elif fname in ("click", "double_click", "triple_click", "mouse_down", "type", "paste_text_at", "click_and_type"):
                                bmask = 1
                            pulse = (fname != "move")

                        hud_intent = (
                            f"[{idx + 1}/{len(function_calls)}] {intent}"
                            if len(function_calls) > 1
                            else intent
                        )
                        executor.set_overlay(
                            state="acting",
                            action=fname,
                            intent=hud_intent,
                            x=px,
                            y=py,
                            end_x=ex,
                            end_y=ey,
                            pulse=pulse,
                            button_mask=bmask,
                        )
                        if action_delay > 0:
                            time.sleep(action_delay)

                        is_file_mutating = fname in FILE_MUTATING_FUNCTIONS
                        is_gui_modifying = (fname not in READ_ONLY_FUNCTIONS) and (not is_file_mutating)
                        pre_status: Dict[str, Any] = executor.get_status() if is_gui_modifying else {}

                        try:
                            exec_res = executor.execute(fname, fc_args, collaborate_mode=collaborate_mode)
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
                                results.append((fname, call_id, action_result, True))
                                log_entry: Dict[str, Any] = {"name": fname, "args": fc_args, "result": action_result}
                                if safety_audit is not None:
                                    log_entry["safety_decision"] = safety_audit
                                turn_log_actions.append(log_entry)
                                for rem_fc in function_calls[idx + 1 :]:
                                    rem_name = str(rem_fc.get("name", ""))
                                    rem_id = str(rem_fc.get("id", ""))
                                    cancel_res = {"status": "paused_for_human_takeover"}
                                    results.append((rem_name, rem_id, cancel_res, True))
                                    turn_log_actions.append({"name": rem_name, "args": rem_fc.get("arguments") or {}, "result": cancel_res})
                                break
                            halted_reason = str(e)
                            action_result = {"error": halted_reason}
                            batch_had_error = True
                            results.append((fname, call_id, action_result, True))
                            log_entry = {"name": fname, "args": fc_args, "result": action_result}
                            if safety_audit is not None:
                                log_entry["safety_decision"] = safety_audit
                            turn_log_actions.append(log_entry)
                            break
                        except Exception as e:
                            print(f"  [WARN] Error executing {fname}: {e}")
                            action_result["error"] = str(e)

                        explicit_attach = bool(action_result.pop("_attach_screenshot", False))
                        if "error" in action_result:
                            batch_had_error = True
                            item_needs_screenshot = True
                        else:
                            if is_gui_modifying:
                                verify = executor.verify_action_effect(pre_status)
                                action_result["verification"] = verify
                                any_modifying_action = True
                                if verify["frame_changed"]:
                                    any_frame_changed = True
                                if verify["resized"]:
                                    batch_had_resize = True
                            elif is_file_mutating:
                                any_file_mutating_success = True
                            item_needs_screenshot = (
                                explicit_attach
                                or collaborator_intervened
                                or (fname not in ALL_CUSTOM_FUNCTION_NAMES)
                            )

                        results.append((fname, call_id, action_result, item_needs_screenshot))
                        log_entry = {"name": fname, "args": fc_args, "result": action_result}
                        if safety_audit is not None:
                            log_entry["safety_decision"] = safety_audit
                        turn_log_actions.append(log_entry)

                        if idx + 1 < len(function_calls):
                            if batch_had_error:
                                for rem_fc in function_calls[idx + 1 :]:
                                    rem_name = str(rem_fc.get("name", ""))
                                    rem_id = str(rem_fc.get("id", ""))
                                    skip_res: Dict[str, Any] = {
                                        "status": "skipped_after_batch_abort",
                                        "reason": f"Prior action {fname} failed: {action_result['error']}",
                                    }
                                    results.append((rem_name, rem_id, skip_res, True))
                                    turn_log_actions.append(
                                        {"name": rem_name, "args": rem_fc.get("arguments") or {}, "result": skip_res}
                                    )
                                break
                            if batch_had_resize:
                                for rem_fc in function_calls[idx + 1 :]:
                                    rem_name = str(rem_fc.get("name", ""))
                                    rem_id = str(rem_fc.get("id", ""))
                                    skip_res = {
                                        "status": "skipped_after_desktop_resize",
                                        "new_size": [executor.width, executor.height],
                                    }
                                    results.append((rem_name, rem_id, skip_res, True))
                                    turn_log_actions.append(
                                        {"name": rem_name, "args": rem_fc.get("arguments") or {}, "result": skip_res}
                                    )
                                break

                    drained_notes, _, _ = executor.poll_user_messages()
                    collab_notes.extend(drained_notes)
                    if collab_notes and results:
                        existing_notes = results[0][2].get("user_notes")
                        if isinstance(existing_notes, list):
                            existing_notes.extend(collab_notes)
                        else:
                            results[0][2]["user_notes"] = collab_notes
                        print(f"  [COLLAB] Delivering user notes to agent: {collab_notes}")
                    if collaborator_intervened:
                        print("  [COLLAB] Re-syncing model with post-takeover screenshot.")

                    need_capture = (
                        any(ns for _, _, _, ns in results)
                        or batch_had_error
                        or batch_had_resize
                        or collaborator_intervened
                    )
                    if need_capture:
                        post_png, post_hdr = executor.capture_png(settle_delay=0.45, max_wait=1.2)
                        post_b64: Optional[str] = base64.b64encode(post_png).decode("ascii")
                        if log_dir:
                            with open(os.path.join(log_dir, f"turn_{global_turn:02d}.png"), "wb") as f:
                                f.write(post_png)
                            with open(os.path.join(log_dir, "final.png"), "wb") as f:
                                f.write(post_png)
                    else:
                        post_hdr = executor.get_status()
                        post_b64 = None

                    if any_file_mutating_success:
                        consecutive_no_change_turns = 0
                    elif any_modifying_action:
                        if any_frame_changed:
                            consecutive_no_change_turns = 0
                        else:
                            consecutive_no_change_turns += 1

                    escalation_reason: Optional[str] = None
                    if batch_had_error:
                        escalation_reason = "action_error"
                    elif batch_had_resize:
                        escalation_reason = "desktop_resized"
                    elif collaborator_intervened:
                        escalation_reason = "human_takeover"
                    elif consecutive_no_change_turns >= 2:
                        escalation_reason = "stalled_ui"

                    next_thinking_lv = select_thinking_level(escalation_reason) if not halted_reason else turn_thinking_lv

                    if trace_file:
                        trace_entry: Dict[str, Any] = {
                            "turn": global_turn,
                            "task": task_index,
                            "interaction_id": interaction.get("id"),
                            "actions": turn_log_actions,
                            "frame_seq": post_hdr.get("frame_seq"),
                            "captured_screenshot": need_capture,
                            "thinking_level": turn_thinking_lv,
                            "halted_reason": halted_reason,
                            "user_notes": collab_notes,
                        }
                        if escalation_reason:
                            trace_entry["escalated_reason"] = escalation_reason
                            trace_entry["next_thinking_level"] = next_thinking_lv
                        trace_file.write(json.dumps(trace_entry, ensure_ascii=False) + "\n")
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

                    last_img_idx: Optional[int] = None
                    if post_b64 is not None and results:
                        img_indices = [i for i, (_, _, _, ns) in enumerate(results) if ns]
                        last_img_idx = img_indices[-1] if img_indices else len(results) - 1

                    function_responses = []
                    for i, (fname, call_id, res, _) in enumerate(results):
                        res_blocks: List[Dict[str, Any]] = [{"type": "text", "text": json.dumps(res)}]
                        if post_b64 is not None and i == last_img_idx:
                            res_blocks.append({"type": "image", "data": post_b64, "mime_type": "image/png"})
                        function_responses.append(
                            {
                                "type": "function_result",
                                "name": fname,
                                "call_id": call_id,
                                "result": res_blocks,
                            }
                        )

                    executor.set_overlay("thinking", "thinking", "Analyzing screen and planning next action...")
                    try:
                        interaction = llm.create(
                            input_payload=function_responses,
                            previous_interaction_id=interaction["id"],
                            thinking_level=next_thinking_lv,
                        )
                        turn_thinking_lv = next_thinking_lv
                    except InteractionsApiError as e:
                        wait_intent = handle_api_error(e, interaction.get("id"))
                        if not stay_interactive or e.is_auth_error:
                            executor.set_overlay("done", "error", wait_intent)
                            return 2
                        task_halted = True
                        last_completed_interaction_id = None
                        break

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
                current_prompt = format_collab_prompt(next_text, human_delta)

    except KeyboardInterrupt:
        print("\n[COLLAB] Interrupted; ending session.")
        return 0
    except (EOFError, ConnectionError, BrokenPipeError) as e:
        if collaborate_mode:
            print(f"\n[COLLAB] Viewer disconnected ({e}); ending session.")
            return 0
        raise

    finally:
        if ftp_client is not None:
            ftp_client.close()
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
        "--thinking-level",
        choices=THINKING_LEVELS,
        default="low",
        help="Baseline generation_config.thinking_level for routine automation turns (default: low)",
    )
    parser.add_argument(
        "--recovery-thinking-level",
        choices=RECOVERY_THINKING_LEVELS,
        default="high",
        help="Adaptive thinking_level used on error/resize/stalled-UI recovery turns (default: high; 'off' disables escalation)",
    )
    parser.add_argument(
        "--exclude-actions",
        action="append",
        default=None,
        help=(
            "Built-in computer_use actions to disable via excluded_predefined_functions "
            "(comma-separated or repeatable, e.g. --exclude-actions drag_and_drop,type)"
        ),
    )
    parser.add_argument(
        "--no-custom-tools",
        action="store_true",
        help="Disable registering custom deterministic VNC function declarations alongside computer_use",
    )
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
    parser.add_argument(
        "--ftp",
        action="store_true",
        help="Require and enable direct remote file I/O tools via TigerVNC -FtpServer",
    )
    parser.add_argument(
        "--no-ftp",
        action="store_true",
        help="Disable direct remote file I/O tools and do not spawn vncviewer with -FtpServer",
    )
    parser.add_argument(
        "--ftp-url",
        default=None,
        help="Explicit FTP bridge URL (e.g. ftp://127.0.0.1:2121) for direct remote file I/O",
    )
    parser.add_argument(
        "--ftp-port",
        type=int,
        default=0,
        help="TCP port for spawned vncviewer -FtpPort (default: 0 = auto)",
    )
    parser.add_argument(
        "--ftp-bind-address",
        default="127.0.0.1",
        help="Bind address for spawned vncviewer -FtpBindAddress (default: 127.0.0.1)",
    )
    parser.add_argument(
        "--ftp-username",
        default="",
        help="Username for authenticating with the TigerVNC -FtpServer bridge",
    )
    parser.add_argument(
        "--ftp-password",
        default="",
        help="Password for authenticating with the TigerVNC -FtpServer bridge",
    )
    parser.add_argument(
        "--ftp-protocol",
        choices=FTP_PROTOCOLS,
        default="Auto",
        help="Preferred RFB file transfer protocol passed to vncviewer -FtpProtocol (default: Auto)",
    )
    parser.add_argument(
        "--ftp-timeout",
        type=float,
        default=30.0,
        help="Socket timeout in seconds for direct FTP file operations (default: 30.0)",
    )
    args = parser.parse_args()
    if args.ftp and args.no_ftp:
        parser.error("--ftp and --no-ftp cannot be used together")
    return run_agent(args)


if __name__ == "__main__":
    sys.exit(main())
