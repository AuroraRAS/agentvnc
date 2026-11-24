"""Direct remote file I/O and streaming transfer client over TigerVNC's -FtpServer."""

import base64
import codecs
import ftplib
import hashlib
import io
import os
import re
import subprocess
import tempfile
import threading
import time
import urllib.parse
from typing import Any, Dict, List, Optional, Tuple

from .constants import (
    DEFAULT_FILE_LIST_MAX_ENTRIES,
    DEFAULT_FILE_READ_MAX_BYTES,
    MAX_FILE_LIST_ENTRIES,
    MAX_FILE_READ_BYTES,
    MAX_FILE_WRITE_BYTES,
    MAX_TRANSFER_BYTES,
)


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
        self._lock = threading.RLock()

    def close(self) -> None:
        with self._lock:
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
        with self._lock:
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
        with self._lock:
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
        with self._lock:
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

        with self._lock:
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

        with self._lock:
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

        with self._lock:
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

    def download_to_path(
        self,
        remote_path: str,
        local_abs_path: str,
        max_bytes: int = MAX_TRANSFER_BYTES,
    ) -> Dict[str, Any]:
        """Stream-download a remote file to local_abs_path atomically with SHA-256."""
        norm = normalize_remote_path(remote_path)
        if norm == "/" or is_windows_drive_root(norm):
            raise ValueError(f"Cannot download directory/root path: {norm}")

        with self._lock:
            st = self.stat(norm)
            if not st.get("exists"):
                raise FileNotFoundError(f"Remote file not found: {norm}")
            if st.get("type") == "dir":
                raise IsADirectoryError(f"Remote path is a directory: {norm}")
            reported_size = st.get("size")
            if reported_size is not None and int(reported_size) > max_bytes:
                raise ValueError(
                    f"Remote file size ({reported_size} bytes) exceeds max transfer limit ({max_bytes} bytes)"
                )

            parent_dir = os.path.dirname(os.path.abspath(local_abs_path))
            os.makedirs(parent_dir, exist_ok=True)

            def _do(ftp: ftplib.FTP) -> Tuple[int, str]:
                self._ensure_binary(ftp)
                fd, tmp_path = tempfile.mkstemp(prefix=".dl_", dir=parent_dir)
                os.close(fd)
                conn = None
                total = 0
                digest = hashlib.sha256()
                try:
                    conn = ftp.transfercmd(f"RETR {norm}")
                    with open(tmp_path, "wb") as out_f:
                        while True:
                            chunk = conn.recv(65536)
                            if not chunk:
                                break
                            total += len(chunk)
                            if total > max_bytes:
                                raise ValueError(
                                    f"Downloaded bytes exceeded max transfer limit ({max_bytes} bytes)"
                                )
                            digest.update(chunk)
                            out_f.write(chunk)
                    try:
                        conn.close()
                    except OSError:
                        pass
                    conn = None
                    ftp.voidresp()
                    os.replace(tmp_path, local_abs_path)
                    return total, digest.hexdigest()
                except Exception:
                    if conn is not None:
                        try:
                            conn.close()
                        except OSError:
                            pass
                    try:
                        if os.path.exists(tmp_path):
                            os.unlink(tmp_path)
                    except OSError:
                        pass
                    raise

            total_bytes, sha256_hex = self._run_with_retry(_do, idempotent=True)
            return {
                "status": "ok",
                "remote_path": norm,
                "bytes_transferred": total_bytes,
                "sha256": sha256_hex,
            }

    def upload_from_path(
        self,
        local_abs_path: str,
        remote_path: str,
        create_parents: bool = False,
        max_bytes: int = MAX_TRANSFER_BYTES,
    ) -> Dict[str, Any]:
        """Stream-upload a local file at local_abs_path to remote_path via FTP STOR."""
        norm = normalize_remote_path(remote_path)
        if norm == "/" or is_windows_drive_root(norm):
            raise ValueError(f"Cannot upload to directory/root path: {norm}")
        if not os.path.exists(local_abs_path):
            raise FileNotFoundError(f"Local file not found: {local_abs_path}")
        if not os.path.isfile(local_abs_path):
            raise IsADirectoryError(f"Local path is not a regular file: {local_abs_path}")
        size = os.path.getsize(local_abs_path)
        if size > max_bytes:
            raise ValueError(f"Local file size ({size} bytes) exceeds max transfer limit ({max_bytes} bytes)")

        digest = hashlib.sha256()
        with open(local_abs_path, "rb") as hash_f:
            while True:
                blk = hash_f.read(65536)
                if not blk:
                    break
                digest.update(blk)
        sha256_hex = digest.hexdigest()

        with self._lock:
            if create_parents:
                parent, _ = split_remote_path(norm)
                if parent != "/" and not is_windows_drive_root(parent):
                    self.mkdir(parent, parents=True)

            def _do(ftp: ftplib.FTP) -> None:
                with open(local_abs_path, "rb") as in_f:
                    ftp.storbinary(f"STOR {norm}", in_f, blocksize=65536)
                self._is_binary = True

            self._run_with_retry(_do, idempotent=True)
            return {
                "status": "ok",
                "remote_path": norm,
                "bytes_transferred": size,
                "sha256": sha256_hex,
            }
