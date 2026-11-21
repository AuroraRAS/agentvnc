#!/usr/bin/env python3
"""Unit and live integration tests for direct agent file I/O over TigerVNC -FtpServer."""

import argparse
import base64
import ftplib
import os
import posixpath
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from typing import Any, Dict, List, Optional, Tuple

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

from test_agent_bridge import AgentClient  # noqa: E402
import vnc_computer_use_agent as agent_mod  # noqa: E402


class MockFtpServer:
    """In-memory mock FTP server matching TigerVNC FtpServer semantics for unit tests."""

    def __init__(
        self,
        username: str = "",
        password: str = "",
        unsupported: bool = False,
    ) -> None:
        self.username = username
        self.password = password
        self.unsupported = unsupported
        self.files: Dict[str, bytes] = {}
        self.dirs: set = {"/", "/C:", "/C:/Temp"}
        self.files["/C:/Temp/hello.txt"] = b"Hello mock world!\n"
        self.lock = threading.Lock()
        self.drop_next_command = False
        self.retr_aborted_count = 0
        self.connection_count = 0

        self._listen_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listen_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listen_sock.bind(("127.0.0.1", 0))
        self._listen_sock.listen(16)
        self._listen_sock.settimeout(0.2)
        self.port = self._listen_sock.getsockname()[1]
        self._stop_event = threading.Event()
        self._threads: List[threading.Thread] = []
        self._accept_thread = threading.Thread(target=self._accept_loop, daemon=True)
        self._accept_thread.start()

    @property
    def url(self) -> str:
        return f"ftp://127.0.0.1:{self.port}"

    def stop(self) -> None:
        self._stop_event.set()
        try:
            self._listen_sock.close()
        except OSError:
            pass
        self._accept_thread.join(timeout=2.0)
        for t in self._threads:
            t.join(timeout=1.0)

    def _accept_loop(self) -> None:
        while not self._stop_event.is_set():
            try:
                conn, _ = self._listen_sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with self.lock:
                self.connection_count += 1
            t = threading.Thread(target=self._handle_client, args=(conn,), daemon=True)
            self._threads.append(t)
            t.start()

    @staticmethod
    def _send_line(sock: socket.socket, text: str) -> None:
        sock.sendall((text + "\r\n").encode("utf-8"))

    def _resolve_ci_dir(self, norm_dir: str) -> Optional[str]:
        if norm_dir in self.dirs:
            return norm_dir
        return next((d for d in self.dirs if d.lower() == norm_dir.lower()), None)

    def _list_entries(self, norm_dir: str) -> Optional[List[Tuple[str, bool, int]]]:
        with self.lock:
            resolved_dir = self._resolve_ci_dir(norm_dir)
            if resolved_dir is None:
                return None
            out: List[Tuple[str, bool, int]] = []
            prefix = "/" if resolved_dir == "/" else resolved_dir + "/"
            seen = set()
            for d in self.dirs:
                if d == resolved_dir or not d.startswith(prefix):
                    continue
                rest = d[len(prefix) :]
                if "/" not in rest and rest not in seen:
                    seen.add(rest)
                    out.append((rest, True, 0))
            for fpath, data in self.files.items():
                if not fpath.startswith(prefix):
                    continue
                rest = fpath[len(prefix) :]
                if "/" not in rest and rest not in seen:
                    seen.add(rest)
                    out.append((rest, False, len(data)))
            return out

    def _stat_entry(self, norm_path: str) -> Tuple[Optional[bool], Optional[int], bool]:
        """Match FtpServer: list parent dir (CI-resolved) and exact-match base name."""
        if norm_path == "/":
            return True, 0, True
        parent = posixpath.dirname(norm_path) or "/"
        base = posixpath.basename(norm_path)
        entries = self._list_entries(parent)
        if entries is None:
            return None, None, False
        for name, is_dir, size in entries:
            if name == base:
                return is_dir, size, True
        return None, None, True

    def _handle_client(self, ctrl: socket.socket) -> None:
        ctrl.settimeout(2.0)
        pasv_sock: Optional[socket.socket] = None
        cwd = "/"
        pending_user = ""
        authed = False
        rest_offset = 0
        rnfr_path: Optional[str] = None

        def close_pasv() -> None:
            nonlocal pasv_sock
            if pasv_sock is not None:
                try:
                    pasv_sock.close()
                except OSError:
                    pass
                pasv_sock = None

        def accept_data() -> Optional[socket.socket]:
            nonlocal pasv_sock
            if pasv_sock is None:
                return None
            try:
                pasv_sock.settimeout(2.0)
                dsock, _ = pasv_sock.accept()
                return dsock
            except OSError:
                return None
            finally:
                close_pasv()

        try:
            self._send_line(ctrl, "220 TigerVNC Mock FTP Ready")
            buf = b""
            while not self._stop_event.is_set():
                while b"\n" not in buf:
                    try:
                        chunk = ctrl.recv(4096)
                    except socket.timeout:
                        if self._stop_event.is_set():
                            return
                        continue
                    if not chunk:
                        return
                    buf += chunk
                raw_line, buf = buf.split(b"\n", 1)
                line = raw_line.decode("utf-8", errors="replace").rstrip("\r\n")
                if not line:
                    continue
                parts = line.split(" ", 1)
                cmd = parts[0].upper()
                arg = parts[1] if len(parts) > 1 else ""

                with self.lock:
                    if self.drop_next_command and cmd not in ("USER", "PASS", "OPTS", "TYPE", "FEAT"):
                        self.drop_next_command = False
                        return

                if cmd != "RNTO":
                    saved_rnfr = rnfr_path
                    rnfr_path = None
                else:
                    saved_rnfr = rnfr_path
                    rnfr_path = None

                if cmd == "NOOP":
                    self._send_line(ctrl, "200 NOOP ok.")
                elif cmd == "QUIT":
                    self._send_line(ctrl, "221 Goodbye.")
                    return
                elif cmd == "FEAT":
                    self._send_line(
                        ctrl,
                        "211-Features:\r\n MDTM\r\n REST STREAM\r\n SIZE\r\n"
                        " MLST type*;size*;modify*;\r\n MLSD\r\n UTF8\r\n211 End",
                    )
                elif cmd == "OPTS":
                    if arg.upper() == "UTF8 ON":
                        self._send_line(ctrl, "200 Always in UTF8 mode.")
                    else:
                        self._send_line(ctrl, "501 Option not understood.")
                elif cmd == "USER":
                    pending_user = arg
                    self._send_line(ctrl, "331 Please specify the password.")
                elif cmd == "PASS":
                    if self.username and pending_user != self.username:
                        self._send_line(ctrl, "530 Login incorrect.")
                    elif self.password and arg != self.password:
                        self._send_line(ctrl, "530 Login incorrect.")
                    else:
                        authed = True
                        self._send_line(ctrl, "230 Login successful.")
                elif not authed:
                    self._send_line(ctrl, "530 Please login with USER and PASS.")
                elif cmd == "TYPE":
                    self._send_line(ctrl, f"200 Switching to {arg} mode.")
                elif cmd == "PWD":
                    self._send_line(ctrl, f'257 "{cwd}" is the current directory')
                elif cmd == "PASV":
                    close_pasv()
                    pasv_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    pasv_sock.bind(("127.0.0.1", 0))
                    pasv_sock.listen(1)
                    p = pasv_sock.getsockname()[1]
                    self._send_line(ctrl, f"227 Entering Passive Mode (127,0,0,1,{p >> 8},{p & 0xFF}).")
                elif cmd == "REST":
                    rest_offset = int(arg)
                    self._send_line(ctrl, f"350 Restart position accepted ({rest_offset}).")
                elif self.unsupported:
                    close_pasv()
                    rest_offset = 0
                    self._send_line(ctrl, "550 File transfer is not supported by the VNC server.")
                elif cmd == "MLST":
                    target = agent_mod.normalize_remote_path(arg if arg else cwd)
                    is_dir, size, parent_ok = self._stat_entry(target)
                    if is_dir is None:
                        if not parent_ok:
                            self._send_line(ctrl, "550 Directory unavailable.")
                        else:
                            self._send_line(ctrl, "550 File not found.")
                    elif is_dir:
                        self._send_line(
                            ctrl,
                            f"250-Listing {target}\r\n type=dir;modify=20261122010000; {target}\r\n250 End",
                        )
                    else:
                        self._send_line(
                            ctrl,
                            f"250-Listing {target}\r\n type=file;size={size};modify=20261122010000; {target}\r\n250 End",
                        )
                elif cmd == "MLSD":
                    target = agent_mod.normalize_remote_path(arg if arg else cwd)
                    dsock = accept_data()
                    if dsock is None:
                        self._send_line(ctrl, "425 Use PASV first.")
                        continue
                    entries = self._list_entries(target)
                    if entries is None:
                        dsock.close()
                        self._send_line(ctrl, "550 Directory not found.")
                        continue
                    self._send_line(ctrl, "150 Here comes the directory listing.")
                    lines = []
                    for name, is_d, sz in entries:
                        if is_d:
                            lines.append(f"type=dir;modify=20261122010000; {name}\r\n")
                        else:
                            lines.append(f"type=file;size={sz};modify=20261122010000; {name}\r\n")
                    dsock.sendall("".join(lines).encode("utf-8"))
                    dsock.close()
                    self._send_line(ctrl, "226 Transfer complete.")
                elif cmd == "RETR":
                    off = rest_offset
                    rest_offset = 0
                    target = agent_mod.normalize_remote_path(arg)
                    dsock = accept_data()
                    if dsock is None:
                        self._send_line(ctrl, "425 Use PASV first.")
                        continue
                    with self.lock:
                        data = self.files.get(target)
                    if data is None:
                        dsock.close()
                        self._send_line(ctrl, "550 File not found.")
                        continue
                    self._send_line(ctrl, "150 Opening BINARY mode data connection.")
                    aborted = False
                    pos = min(off, len(data))
                    try:
                        while pos < len(data):
                            chunk = data[pos : pos + 4096]
                            dsock.sendall(chunk)
                            pos += len(chunk)
                            time.sleep(0.001)
                    except OSError:
                        aborted = True
                    finally:
                        try:
                            dsock.close()
                        except OSError:
                            pass
                    if aborted:
                        with self.lock:
                            self.retr_aborted_count += 1
                        self._send_line(ctrl, "426 Connection closed; transfer aborted.")
                    else:
                        self._send_line(ctrl, "226 Transfer complete.")
                elif cmd in ("STOR", "APPE"):
                    rest_offset = 0
                    target = agent_mod.normalize_remote_path(arg)
                    dsock = accept_data()
                    if dsock is None:
                        self._send_line(ctrl, "425 Use PASV first.")
                        continue
                    parent = posixpath.dirname(target) or "/"
                    with self.lock:
                        parent_ok = parent in self.dirs
                    if not parent_ok or target in self.dirs:
                        dsock.close()
                        self._send_line(ctrl, "550 Cannot write target path.")
                        continue
                    self._send_line(ctrl, "150 Ok to send data.")
                    chunks = []
                    while True:
                        dchunk = dsock.recv(65536)
                        if not dchunk:
                            break
                        chunks.append(dchunk)
                    dsock.close()
                    payload = b"".join(chunks)
                    with self.lock:
                        if cmd == "APPE" and target in self.files:
                            self.files[target] = self.files[target] + payload
                        else:
                            self.files[target] = payload
                    self._send_line(ctrl, "226 Transfer complete.")
                elif cmd == "MKD":
                    target = agent_mod.normalize_remote_path(arg)
                    parent = posixpath.dirname(target) or "/"
                    base = posixpath.basename(target)
                    with self.lock:
                        resolved_parent = self._resolve_ci_dir(parent)
                        ci_dir = self._resolve_ci_dir(target)
                        if ci_dir is not None:
                            self._send_line(ctrl, "550 Directory already exists")
                        elif target in self.files or resolved_parent is None:
                            self._send_line(ctrl, "550 Cannot create directory.")
                        else:
                            canonical = f"/{base}" if resolved_parent == "/" else f"{resolved_parent}/{base}"
                            self.dirs.add(canonical)
                            self._send_line(ctrl, f'257 "{canonical}" created')
                elif cmd == "DELE":
                    target = agent_mod.normalize_remote_path(arg)
                    with self.lock:
                        if target not in self.files:
                            self._send_line(ctrl, "550 File not found.")
                        else:
                            del self.files[target]
                            self._send_line(ctrl, "250 Command successful.")
                elif cmd == "RMD":
                    target = agent_mod.normalize_remote_path(arg)
                    with self.lock:
                        if target not in self.dirs or target in ("/", "/C:"):
                            self._send_line(ctrl, "550 Directory not found.")
                        else:
                            prefix = target + "/"
                            nonempty = any(d.startswith(prefix) for d in self.dirs) or any(
                                f.startswith(prefix) for f in self.files
                            )
                            if nonempty:
                                self._send_line(ctrl, "550 Directory not empty.")
                            else:
                                self.dirs.remove(target)
                                self._send_line(ctrl, "250 Command successful.")
                elif cmd == "RNFR":
                    target = agent_mod.normalize_remote_path(arg)
                    with self.lock:
                        exists = (target in self.files) or (target in self.dirs)
                    if not exists:
                        self._send_line(ctrl, "550 File not found.")
                    else:
                        rnfr_path = target
                        self._send_line(ctrl, "350 Ready for RNTO.")
                elif cmd == "RNTO":
                    if not saved_rnfr:
                        self._send_line(ctrl, "503 RNFR required first.")
                        continue
                    target = agent_mod.normalize_remote_path(arg)
                    with self.lock:
                        if saved_rnfr in self.files:
                            self.files[target] = self.files.pop(saved_rnfr)
                            self._send_line(ctrl, "250 Command successful.")
                        elif saved_rnfr in self.dirs:
                            self.dirs.remove(saved_rnfr)
                            self.dirs.add(target)
                            self._send_line(ctrl, "250 Command successful.")
                        else:
                            self._send_line(ctrl, "550 Source vanished.")
                else:
                    self._send_line(ctrl, f"502 Command {cmd} not implemented.")
        finally:
            close_pasv()
            try:
                ctrl.close()
            except OSError:
                pass


class FakeAgentClient:
    """Minimal AgentClient stub for VncDesktopExecutor unit tests."""

    def __init__(self, view_only: bool = False, agent_paused: bool = False) -> None:
        self.view_only = view_only
        self.agent_paused = agent_paused

    def request(self, cmd: str, payload: bytes = b"", **fields: Any) -> Tuple[Dict[str, Any], bytes]:
        if cmd == "GET_STATUS":
            return (
                {
                    "ok": True,
                    "connected": True,
                    "ready": True,
                    "width": 1024,
                    "height": 768,
                    "desktop_name": "mock",
                    "frame_seq": 1,
                    "damage_count": 1,
                    "resize_count": 0,
                    "updating": False,
                    "has_clipboard": False,
                    "view_only": self.view_only,
                    "agent_paused": self.agent_paused,
                },
                b"",
            )
        return ({"ok": True}, b"")


class AgentFileIoUnitTests(unittest.TestCase):
    def test_normalize_remote_path(self) -> None:
        self.assertEqual(agent_mod.normalize_remote_path(""), "/")
        self.assertEqual(agent_mod.normalize_remote_path("/"), "/")
        self.assertEqual(agent_mod.normalize_remote_path("/a/b/../c/./"), "/a/c")
        self.assertEqual(agent_mod.normalize_remote_path("/../../x"), "/x")
        self.assertEqual(
            agent_mod.normalize_remote_path(r"c:\Users\alice\Desktop\my file.txt"),
            "/C:/Users/alice/Desktop/my file.txt",
        )
        self.assertEqual(agent_mod.normalize_remote_path("/c:/"), "/C:")
        self.assertTrue(agent_mod.is_windows_drive_root("/C:"))
        self.assertFalse(agent_mod.is_windows_drive_root("/C:/Temp"))
        self.assertEqual(agent_mod.split_remote_path("/C:/Temp/a.txt"), ("/C:/Temp", "a.txt"))
        self.assertEqual(agent_mod.split_remote_path("/C:"), ("/", "C:"))
        for bad in ("/a\r\nb", "/a\nb", "/a\x00b"):
            with self.assertRaises(ValueError):
                agent_mod.normalize_remote_path(bad)
        with self.assertRaises(ValueError):
            agent_mod.normalize_remote_path(123)  # type: ignore[arg-type]

    def test_parse_and_redact_ftp_url(self) -> None:
        self.assertEqual(
            agent_mod.parse_ftp_url("ftp://127.0.0.1:2121"),
            ("127.0.0.1", 2121, None, None),
        )
        self.assertEqual(
            agent_mod.parse_ftp_url("ftp://[::1]"),
            ("::1", 2121, None, None),
        )
        self.assertEqual(
            agent_mod.parse_ftp_url("ftp://alice:s%40cret@127.0.0.1:4555"),
            ("127.0.0.1", 4555, "alice", "s@cret"),
        )
        self.assertEqual(
            agent_mod.redact_ftp_url("ftp://alice:secret123@127.0.0.1:2121"),
            "ftp://alice:***@127.0.0.1:2121",
        )
        self.assertEqual(
            agent_mod.redact_ftp_url("ftp://[::1]:2121"),
            "ftp://[::1]:2121",
        )
        with self.assertRaises(ValueError):
            agent_mod.parse_ftp_url("http://127.0.0.1:2121")

    def test_discover_ftp_url_from_log(self) -> None:
        with tempfile.NamedTemporaryFile("w", encoding="utf-8", delete=False) as tf:
            tf.write("CConn: Connected\nFtpServer: FTP server listening on ftp://127.0.0.1:41234\n")
            path = tf.name
        try:
            self.assertEqual(agent_mod.discover_ftp_url_from_log(path), "ftp://127.0.0.1:41234")
            self.assertIsNone(agent_mod.discover_ftp_url_from_log(path + ".missing"))
        finally:
            os.unlink(path)

    def test_schema_and_tool_config(self) -> None:
        forbidden = {"default", "anyOf", "oneOf", "allOf", "additionalProperties"}
        allowed_types = {"string", "integer", "boolean"}

        for decl in agent_mod.FILE_FUNCTION_DECLARATIONS:
            self.assertEqual(decl["type"], "function")
            self.assertTrue(decl["name"].startswith("file_"))
            params = decl["parameters"]
            self.assertEqual(params["type"], "object")
            self.assertFalse(forbidden.intersection(params.keys()))
            for pname, pdef in params["properties"].items():
                self.assertIn(pdef["type"], allowed_types, f"{decl['name']}.{pname}")
                self.assertFalse(forbidden.intersection(pdef.keys()), f"{decl['name']}.{pname}")

        tools_no_ftp = agent_mod.build_tools_config(
            strict_safety=False, excluded_actions=[], enable_custom_tools=True, enable_file_tools=False
        )
        names_no_ftp = {t["name"] for t in tools_no_ftp if t.get("type") == "function"}
        self.assertFalse(agent_mod.FILE_FUNCTION_NAMES.intersection(names_no_ftp))

        tools_with_ftp = agent_mod.build_tools_config(
            strict_safety=False, excluded_actions=[], enable_custom_tools=True, enable_file_tools=True
        )
        names_with_ftp = {t["name"] for t in tools_with_ftp if t.get("type") == "function"}
        self.assertTrue(agent_mod.FILE_FUNCTION_NAMES.issubset(names_with_ftp))

        self.assertTrue(agent_mod.FILE_READ_ONLY_FUNCTIONS.issubset(agent_mod.READ_ONLY_FUNCTIONS))
        self.assertTrue(agent_mod.FILE_FUNCTION_NAMES.issubset(agent_mod.ALL_CUSTOM_FUNCTION_NAMES))

    def test_ftp_client_operations_and_recovery(self) -> None:
        srv = MockFtpServer(username="u1", password="p1")
        try:
            client = agent_mod.VncFtpClient("127.0.0.1", srv.port, username="u1", password="p1", timeout=5.0)
            try:
                pr = client.probe()
                self.assertTrue(pr["supported"])
                self.assertEqual(pr["root_entries"][0]["path"], "/C:")

                # list_dir sorting + truncation
                client.mkdir("/C:/Temp/sub_dir")
                client.write_file("/C:/Temp/b.txt", content="B")
                client.write_file("/C:/Temp/a.txt", content="A")
                listing = client.list_dir("/C:/Temp", max_entries=2)
                self.assertEqual(listing["total_entries"], 4)
                self.assertEqual(listing["returned_entries"], 2)
                self.assertTrue(listing["truncated"])
                self.assertEqual(listing["entries"][0]["type"], "dir")
                self.assertEqual(listing["entries"][0]["name"], "sub_dir")

                # stat cases
                self.assertTrue(client.stat("/")["exists"])
                self.assertEqual(client.stat("/C:/Temp")["type"], "dir")
                self.assertFalse(client.stat("/C:/Temp/missing.txt")["exists"])

                # read UTF-8, empty, non-UTF-8 decode warning, base64
                client.write_file("/C:/Temp/utf8.txt", content="Line 1\nHéllo ✓\n")
                r_utf8 = client.read_file("/C:/Temp/utf8.txt")
                self.assertEqual(r_utf8["content"], "Line 1\nHéllo ✓\n")
                self.assertTrue(r_utf8["eof"])
                self.assertFalse(r_utf8["truncated"])

                client.write_file("/C:/Temp/empty.txt", content="")
                r_empty = client.read_file("/C:/Temp/empty.txt")
                self.assertEqual(r_empty["content"], "")
                self.assertEqual(r_empty["bytes_read"], 0)
                self.assertTrue(r_empty["eof"])

                # append + create_parents
                client.write_file("/C:/Temp/utf8.txt", content="Line 3", append=True)
                self.assertEqual(client.read_file("/C:/Temp/utf8.txt")["content"], "Line 1\nHéllo ✓\nLine 3")

                client.write_file(
                    "/C:/Temp/deep/nested/file.txt",
                    content="deep!",
                    create_parents=True,
                )
                self.assertEqual(client.read_file("/C:/Temp/deep/nested/file.txt")["content"], "deep!")

                # Windows-style case-insensitive existing parent directory (e.g. WINDOWS vs Windows)
                with srv.lock:
                    srv.dirs.add("/C:/WINDOWS")
                    srv.dirs.add("/C:/WINDOWS/Temp")
                mk_ci = client.mkdir("/C:/Windows/Temp/ci_subdir", parents=True)
                self.assertEqual(mk_ci["created"], ["/C:/Windows/Temp/ci_subdir"])
                self.assertTrue(client.stat("/C:/WINDOWS/Temp/ci_subdir")["exists"])

                # binary + chunked read with early close 426 recovery on same connection
                blob = bytes((i * 13 + 7) % 256 for i in range(40000))
                client.write_file(
                    "/C:/Temp/blob.bin",
                    content_base64=base64.b64encode(blob).decode("ascii"),
                )
                r_bad_txt = client.read_file("/C:/Temp/blob.bin", max_bytes=128)
                self.assertTrue(r_bad_txt["truncated"])
                self.assertTrue(r_bad_txt.get("had_decode_errors"))
                self.assertIn("as_base64=true", r_bad_txt["note"])

                # Subsequent read on same connection after early close 426
                r_chunk1 = client.read_file("/C:/Temp/blob.bin", as_base64=True, offset=0, max_bytes=1024)
                self.assertTrue(r_chunk1["truncated"])
                self.assertFalse(r_chunk1["eof"])
                self.assertEqual(base64.b64decode(r_chunk1["content_base64"]), blob[:1024])

                r_chunk2 = client.read_file("/C:/Temp/blob.bin", as_base64=True, offset=1024, max_bytes=2048)
                self.assertTrue(r_chunk2["truncated"])
                self.assertEqual(base64.b64decode(r_chunk2["content_base64"]), blob[1024:3072])

                r_eof = client.read_file("/C:/Temp/blob.bin", as_base64=True, offset=len(blob), max_bytes=512)
                self.assertEqual(r_eof["bytes_read"], 0)
                self.assertTrue(r_eof["eof"])

                # rename & delete
                client.rename("/C:/Temp/a.txt", "/C:/Temp/a_renamed.txt")
                self.assertFalse(client.stat("/C:/Temp/a.txt")["exists"])
                self.assertEqual(client.read_file("/C:/Temp/a_renamed.txt")["content"], "A")
                client.delete("/C:/Temp/a_renamed.txt")
                self.assertFalse(client.stat("/C:/Temp/a_renamed.txt")["exists"])
                client.delete("/C:/Temp/sub_dir", is_dir=True)
                self.assertFalse(client.stat("/C:/Temp/sub_dir")["exists"])

                # Drop control connection and confirm automatic reconnect on idempotent call
                with srv.lock:
                    srv.drop_next_command = True
                    prev_conns = srv.connection_count
                r_reconnect = client.read_file("/C:/Temp/hello.txt")
                self.assertEqual(r_reconnect["content"], "Hello mock world!\n")
                with srv.lock:
                    self.assertGreater(srv.connection_count, prev_conns)
            finally:
                client.close()
        finally:
            srv.stop()

    def test_ftp_client_unsupported(self) -> None:
        srv = MockFtpServer(unsupported=True)
        try:
            client = agent_mod.VncFtpClient("127.0.0.1", srv.port, timeout=5.0)
            try:
                pr = client.probe()
                self.assertTrue(pr["connected"])
                self.assertFalse(pr["supported"])
            finally:
                client.close()
        finally:
            srv.stop()

    def test_executor_dispatch_pause_and_view_only(self) -> None:
        srv = MockFtpServer()
        try:
            ftp_client = agent_mod.VncFtpClient("127.0.0.1", srv.port, timeout=5.0)
            fake_agent = FakeAgentClient()
            executor = agent_mod.VncDesktopExecutor(fake_agent, ftp_client=ftp_client)  # type: ignore[arg-type]

            try:
                # Write, mkdir (parents=None defaults to True), read, list, stat, rename, delete
                res_mk = executor.execute("file_mkdir", {"path": "/C:/Temp/ex/nested", "parents": None})
                self.assertEqual(res_mk["status"], "ok")
                self.assertEqual(res_mk["created"], ["/C:/Temp/ex", "/C:/Temp/ex/nested"])

                res_wr = executor.execute(
                    "file_write",
                    {"path": "/C:/Temp/ex/nested/note.txt", "content": "ok"},
                )
                self.assertEqual(res_wr["bytes_written"], 2)

                res_rd = executor.execute("file_read", {"path": "/C:/Temp/ex/nested/note.txt"})
                self.assertEqual(res_rd["content"], "ok")

                res_rn = executor.execute(
                    "file_rename",
                    {"from_path": "/C:/Temp/ex/nested/note.txt", "to_path": "/C:/Temp/ex/nested/note2.txt"},
                )
                self.assertEqual(res_rn["status"], "ok")

                res_ls = executor.execute("file_list", {"path": "/C:/Temp/ex/nested"})
                self.assertEqual([e["name"] for e in res_ls["entries"]], ["note2.txt"])

                res_st = executor.execute("file_stat", {"path": "/C:/Temp/ex/nested/note2.txt"})
                self.assertTrue(res_st["exists"])

                res_del = executor.execute("file_delete", {"path": "/C:/Temp/ex/nested/note2.txt"})
                self.assertEqual(res_del["deleted_type"], "file")

                # Pause enforcement: mutating raises AgentPausedError, read-only still allowed
                fake_agent.agent_paused = True
                self.assertEqual(executor.execute("file_list", {"path": "/"})["status"], "ok")
                self.assertTrue(executor.execute("file_stat", {"path": "/C:/Temp/hello.txt"})["exists"])
                self.assertEqual(
                    executor.execute("file_read", {"path": "/C:/Temp/hello.txt"})["content"],
                    "Hello mock world!\n",
                )
                for mut_cmd, mut_args in (
                    ("file_write", {"path": "/C:/Temp/x.txt", "content": "x"}),
                    ("file_mkdir", {"path": "/C:/Temp/xdir"}),
                    ("file_delete", {"path": "/C:/Temp/hello.txt"}),
                    ("file_rename", {"from_path": "/C:/Temp/hello.txt", "to_path": "/C:/Temp/h2.txt"}),
                ):
                    with self.assertRaises(agent_mod.AgentPausedError):
                        executor.execute(mut_cmd, mut_args)

                # ViewOnly enforcement
                fake_agent.agent_paused = False
                fake_agent.view_only = True
                res_vo = executor.execute("file_write", {"path": "/C:/Temp/x.txt", "content": "x"})
                self.assertIn("error", res_vo)

                # Unavailable FTP bridge
                no_ftp_exec = agent_mod.VncDesktopExecutor(fake_agent)  # type: ignore[arg-type]
                res_unavail = no_ftp_exec.execute("file_list", {"path": "/"})
                self.assertIn("error", res_unavail)
            finally:
                ftp_client.close()
        finally:
            srv.stop()


def run_live_tests(vnc_host: str, vncviewer_bin: str) -> int:
    if not os.path.exists(vncviewer_bin):
        raise FileNotFoundError(f"vncviewer binary not found: {vncviewer_bin}")
    if not agent_mod.vncviewer_supports_ftp(vncviewer_bin):
        raise RuntimeError(f"{vncviewer_bin} does not support -FtpServer")

    temp_dir = tempfile.mkdtemp(prefix="tigervnc_live_ftp_agent_")
    sock_path = os.path.join(temp_dir, "agent.sock")
    log_path = os.path.join(temp_dir, "vncviewer.log")
    env = os.environ.copy()
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)

    cmd = [
        vncviewer_bin,
        "-Shared",
        "-Headless",
        "-AgentSocket",
        sock_path,
        "-FtpServer",
        "-FtpBindAddress",
        "127.0.0.1",
        "-FtpPort",
        "0",
        vnc_host,
    ]
    print(f"[LIVE] Starting vncviewer -> {vnc_host}")
    viewer_log = open(log_path, "w", encoding="utf-8")
    proc = subprocess.Popen(cmd, env=env, stdout=viewer_log, stderr=subprocess.STDOUT)

    try:
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            if os.path.exists(sock_path):
                break
            if proc.poll() is not None:
                raise RuntimeError(f"vncviewer exited prematurely ({proc.returncode}); check {log_path}")
            time.sleep(0.1)
        if not os.path.exists(sock_path):
            raise TimeoutError(f"Timed out waiting for AgentSocket {sock_path}")

        with AgentClient(sock_path, timeout=15.0) as ac:
            executor = agent_mod.VncDesktopExecutor(ac)
            status = executor.wait_ready(timeout=15.0)
            print(f"[LIVE] Ready: {status['width']}x{status['height']} ({status.get('desktop_name', '')})")

            # Wait briefly for FTP bridge URL via GET_STATUS or log
            ftp_url: Optional[str] = None
            ftp_deadline = time.monotonic() + 5.0
            while time.monotonic() < ftp_deadline:
                status = executor.get_status()
                log_url = agent_mod.discover_ftp_url_from_log(log_path)
                if "ftp_running" in status:
                    if status.get("ftp_running") and status.get("ftp_url") and status.get("ftp_ready"):
                        ftp_url = str(status["ftp_url"])
                        assert status.get("ftp_supported") is True, f"Unexpected ftp_supported: {status}"
                        assert status.get("ftp_protocol") in ("Tight2", "Tight13", "Ultra"), status
                        if log_url:
                            assert log_url == ftp_url, f"Log URL {log_url} != GET_STATUS {ftp_url}"
                        print(
                            f"[PASS] GET_STATUS FTP discovery: url={ftp_url} "
                            f"protocol={status.get('ftp_protocol')}"
                        )
                        break
                elif log_url:
                    ftp_url = log_url
                    print(f"[PASS] Log FTP discovery: url={ftp_url}")
                    break
                time.sleep(0.1)

            if not ftp_url:
                raise RuntimeError(f"Failed to discover FTP URL; status={status}")

            host, port, user, passwd = agent_mod.parse_ftp_url(ftp_url)
            ftp_client = agent_mod.VncFtpClient(host, port, username=(user or ""), password=(passwd or ""), timeout=30.0)
            executor.ftp_client = ftp_client
            created_files: List[str] = []
            created_dirs: List[str] = []

            try:
                probe_res = ftp_client.probe()
                assert probe_res["supported"] is True, f"RFB file transfer unsupported: {probe_res}"

                # 1. Root listing & nonexistent stat
                root_res = executor.execute("file_list", {"path": "/"})
                assert root_res["status"] == "ok" and len(root_res["entries"]) > 0, root_res
                root_names = [e["name"] for e in root_res["entries"]]
                print(f"[PASS] file_list('/') -> {root_names}")

                if "C:" in root_names:
                    base_parent = "/C:/Windows/Temp"
                    st_temp = executor.execute("file_stat", {"path": base_parent})
                    if not st_temp.get("exists"):
                        base_parent = "/C:"
                else:
                    base_parent = "/tmp"

                test_root = f"{base_parent}/tigervnc_agent_test_{os.getpid()}_{int(time.time())}"
                missing_res = executor.execute("file_stat", {"path": f"{test_root}/does_not_exist.txt"})
                assert missing_res["exists"] is False, missing_res
                print("[PASS] file_stat on nonexistent path -> exists=False")

                # 2. Nested mkdir
                nested_dir = f"{test_root}/sub/deep"
                mk_res = executor.execute("file_mkdir", {"path": nested_dir, "parents": True})
                assert mk_res["status"] == "ok", mk_res
                created_dirs.extend([nested_dir, f"{test_root}/sub", test_root])
                st_dir = executor.execute("file_stat", {"path": nested_dir})
                assert st_dir["exists"] is True and st_dir["type"] == "dir", st_dir
                print(f"[PASS] file_mkdir + file_stat dir -> {nested_dir}")

                # 3. UTF-8 text write & read
                utf8_path = f"{nested_dir}/utf8_note.txt"
                utf8_text = "Line 1: ASCII\nLine 2: Unicode café ✓ 日本語\n"
                wr1 = executor.execute("file_write", {"path": utf8_path, "content": utf8_text})
                assert wr1["status"] == "ok", wr1
                created_files.append(utf8_path)
                rd1 = executor.execute("file_read", {"path": utf8_path})
                assert rd1["content"] == utf8_text and rd1["eof"] is True, rd1
                print("[PASS] file_write / file_read UTF-8 text")

                # 4. Empty file write & read
                empty_path = f"{test_root}/empty.txt"
                wr_empty = executor.execute("file_write", {"path": empty_path, "content": ""})
                assert wr_empty["bytes_written"] == 0, wr_empty
                created_files.append(empty_path)
                rd_empty = executor.execute("file_read", {"path": empty_path})
                assert rd_empty["bytes_read"] == 0 and rd_empty["eof"] is True and rd_empty["content"] == "", rd_empty
                print("[PASS] file_write / file_read empty file")

                # 5. Append
                append_suffix = "Appended line 3!\n"
                wr_app = executor.execute(
                    "file_write",
                    {"path": utf8_path, "content": append_suffix, "append": True},
                )
                assert wr_app["mode"] == "append", wr_app
                rd_app = executor.execute("file_read", {"path": utf8_path})
                assert rd_app["content"] == (utf8_text + append_suffix), rd_app
                print("[PASS] file_write(append=True) / file_read")

                # 6. Large binary (> 200 KiB), early-close truncated read + 426 recovery + offset read + full read
                big_blob = bytes(((i * 37 + 11) & 0xFF) for i in range(220 * 1024))
                big_path = f"{test_root}/big_payload.bin"
                wr_big = executor.execute(
                    "file_write",
                    {
                        "path": big_path,
                        "content_base64": base64.b64encode(big_blob).decode("ascii"),
                    },
                )
                assert wr_big["bytes_written"] == len(big_blob), wr_big
                created_files.append(big_path)

                rd_part1 = executor.execute(
                    "file_read",
                    {"path": big_path, "as_base64": True, "offset": 0, "max_bytes": 1024},
                )
                assert rd_part1["truncated"] is True and rd_part1["eof"] is False, rd_part1
                assert base64.b64decode(rd_part1["content_base64"]) == big_blob[:1024]

                rd_part2 = executor.execute(
                    "file_read",
                    {"path": big_path, "as_base64": True, "offset": 1024, "max_bytes": 2048},
                )
                assert rd_part2["truncated"] is True, rd_part2
                assert base64.b64decode(rd_part2["content_base64"]) == big_blob[1024:3072]

                rd_full = executor.execute(
                    "file_read",
                    {"path": big_path, "as_base64": True, "offset": 0, "max_bytes": 300000},
                )
                assert rd_full["truncated"] is False and rd_full["eof"] is True, rd_full
                assert base64.b64decode(rd_full["content_base64"]) == big_blob
                print(f"[PASS] large binary ({len(big_blob)} bytes), truncated early-close + offset + full read")

                # 7. file_write with create_parents=True
                parent_file = f"{test_root}/auto_p1/auto_p2/child.txt"
                wr_cp = executor.execute(
                    "file_write",
                    {"path": parent_file, "content": "created parents", "create_parents": True},
                )
                assert wr_cp["status"] == "ok", wr_cp
                created_files.append(parent_file)
                created_dirs.insert(0, f"{test_root}/auto_p1")
                created_dirs.insert(0, f"{test_root}/auto_p1/auto_p2")
                assert executor.execute("file_read", {"path": parent_file})["content"] == "created parents"
                print("[PASS] file_write(create_parents=True)")

                # 8. file_rename
                renamed_path = f"{test_root}/auto_p1/auto_p2/child_renamed.txt"
                rn_res = executor.execute(
                    "file_rename",
                    {"from_path": parent_file, "to_path": renamed_path},
                )
                assert rn_res["status"] == "ok", rn_res
                created_files.remove(parent_file)
                created_files.append(renamed_path)
                assert executor.execute("file_stat", {"path": parent_file})["exists"] is False
                assert executor.execute("file_read", {"path": renamed_path})["content"] == "created parents"
                print("[PASS] file_rename")

                # 9. PAUSE_AGENT enforcement
                executor.pause_agent()
                try:
                    rd_paused = executor.execute("file_read", {"path": renamed_path})
                    assert rd_paused["content"] == "created parents"
                    try:
                        executor.execute("file_write", {"path": renamed_path, "content": "blocked"})
                        raise AssertionError("Expected AgentPausedError on file_write while paused")
                    except agent_mod.AgentPausedError:
                        pass
                finally:
                    executor.resume_agent()
                print("[PASS] PAUSE_AGENT / RESUME_AGENT with file_read & file_write")

            finally:
                # 10. Clean up all remote test files and directories
                for fpath in list(created_files):
                    try:
                        del_res = executor.execute("file_delete", {"path": fpath, "is_dir": False})
                        assert del_res["status"] == "ok", del_res
                    except Exception as exc:
                        print(f"[WARN] Cleanup failed for file {fpath}: {exc}")
                for dpath in created_dirs:
                    try:
                        del_res = executor.execute("file_delete", {"path": dpath, "is_dir": True})
                        assert del_res["status"] == "ok", del_res
                    except Exception as exc:
                        print(f"[WARN] Cleanup failed for dir {dpath}: {exc}")
                st_clean = executor.execute("file_stat", {"path": test_root})
                assert st_clean["exists"] is False, f"Cleanup left test_root behind: {st_clean}"
                print(f"[PASS] file_delete cleanup verified ({test_root} removed)")
                ftp_client.close()
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        viewer_log.close()
        shutil.rmtree(temp_dir, ignore_errors=True)

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Test TigerVNC direct agent file I/O")
    parser.add_argument("--live", action="store_true", help="Run live integration test against a VNC server")
    parser.add_argument("--vnc-host", default="localhost:5901", help="Target VNC host:port for --live")
    parser.add_argument(
        "--vncviewer-bin",
        default=os.path.join(REPO_ROOT, "build", "vncviewer", "vncviewer"),
        help="Path to vncviewer binary for --live",
    )
    args, remaining = parser.parse_known_args()

    if not args.live:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(AgentFileIoUnitTests)
        result = unittest.TextTestRunner(verbosity=2).run(suite)
        return 0 if result.wasSuccessful() else 1

    return run_live_tests(args.vnc_host, args.vncviewer_bin)


if __name__ == "__main__":
    sys.exit(main())
