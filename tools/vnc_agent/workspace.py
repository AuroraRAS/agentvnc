"""Shared local workspace, persistent memory, and remote host transfer manager."""

import base64
import codecs
import datetime
import fnmatch
import json
import os
import shutil
import tempfile
import threading
from typing import Any, Dict, List, Optional, Tuple, Union

from .constants import (
    DEFAULT_FILE_LIST_MAX_ENTRIES,
    DEFAULT_FILE_READ_MAX_BYTES,
    MAX_FILE_LIST_ENTRIES,
    MAX_FILE_READ_BYTES,
    MAX_FILE_WRITE_BYTES,
    MAX_TRANSFER_BYTES,
)
from .ftp_client import VncFtpClient


INTERNAL_DIR_NAMES = frozenset({".coord", ".agent"})
DEFAULT_MEMORY_HEADER = (
    "# Shared Agent Memory\n\n"
    "Persistent notes and findings shared between the Computer Use Agent and Research Assistant.\n"
)


def utc_now_iso() -> str:
    """Return an ISO-8601 UTC timestamp string."""
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def atomic_write_bytes(abs_path: str, data: bytes) -> None:
    """Write bytes atomically via a temporary file in the target directory."""
    parent = os.path.dirname(os.path.abspath(abs_path))
    os.makedirs(parent, exist_ok=True)
    fd, tmp_path = tempfile.mkstemp(prefix=".tmp_", dir=parent)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp_path, abs_path)
    except Exception:
        try:
            if os.path.exists(tmp_path):
                os.unlink(tmp_path)
        except OSError:
            pass
        raise


def atomic_write_json(abs_path: str, payload: Any) -> None:
    """Serialize payload as UTF-8 JSON and write atomically."""
    raw = (json.dumps(payload, indent=2, ensure_ascii=False) + "\n").encode("utf-8")
    atomic_write_bytes(abs_path, raw)


def _is_internal_rel(rel_path: str) -> bool:
    if not rel_path or rel_path == ".":
        return False
    first = rel_path.split("/", 1)[0]
    return first in INTERNAL_DIR_NAMES


class LocalWorkspace:
    """Sandboxed local workspace shared by the Computer Use Agent and Research Assistant."""

    def __init__(self, root_dir: str) -> None:
        if not isinstance(root_dir, str) or not root_dir.strip():
            raise ValueError("Workspace root_dir must be a non-empty string")
        abs_root = os.path.abspath(root_dir)
        os.makedirs(abs_root, exist_ok=True)
        self.root = os.path.realpath(abs_root)
        self._lock = threading.RLock()

        self.memory_dir = os.path.join(self.root, "memory")
        self.coord_dir = os.path.join(self.root, ".coord")
        self.agent_dir = os.path.join(self.root, ".agent")
        self.memory_md_path = os.path.join(self.memory_dir, "MEMORY.md")
        self.memory_entries_path = os.path.join(self.memory_dir, "entries.json")

        os.makedirs(self.memory_dir, exist_ok=True)
        os.makedirs(self.coord_dir, exist_ok=True)
        os.makedirs(os.path.join(self.coord_dir, "inbox", "computer_use"), exist_ok=True)
        os.makedirs(os.path.join(self.coord_dir, "inbox", "assistant"), exist_ok=True)
        os.makedirs(os.path.join(self.coord_dir, "archive"), exist_ok=True)
        os.makedirs(self.agent_dir, exist_ok=True)

        if not os.path.exists(self.memory_md_path):
            atomic_write_bytes(self.memory_md_path, DEFAULT_MEMORY_HEADER.encode("utf-8"))
        if not os.path.exists(self.memory_entries_path):
            atomic_write_json(self.memory_entries_path, {"version": 1, "entries": {}})

    def resolve_path(
        self,
        rel_path: str,
        allow_root: bool = False,
        allow_internal: bool = False,
    ) -> Tuple[str, str]:
        """Resolve a user/agent path safely inside self.root, blocking escapes."""
        if not isinstance(rel_path, str):
            raise ValueError("Workspace path must be a string")
        if "\x00" in rel_path or "\r" in rel_path or "\n" in rel_path:
            raise ValueError("Workspace path contains invalid control characters")

        raw = rel_path.strip().replace("\\", "/")
        if raw in ("", ".", "./", "/"):
            if not allow_root:
                raise ValueError("Path refers to workspace root; specify a relative file or directory path")
            return self.root, "."

        if os.path.isabs(raw):
            real_candidate = os.path.realpath(raw)
            if os.path.commonpath([self.root, real_candidate]) != self.root:
                raise PermissionError(f"Path escapes local workspace: {rel_path!r}")
            rel_candidate = os.path.relpath(real_candidate, self.root).replace("\\", "/")
            if rel_candidate == ".":
                if not allow_root:
                    raise ValueError("Path refers to workspace root")
                return self.root, "."
            raw = rel_candidate

        parts: List[str] = []
        for seg in raw.lstrip("/").split("/"):
            if not seg or seg == ".":
                continue
            if seg == "..":
                if not parts:
                    raise PermissionError(f"Path traversal outside workspace is not allowed: {rel_path!r}")
                parts.pop()
                continue
            parts.append(seg)

        if not parts:
            if not allow_root:
                raise ValueError("Path refers to workspace root")
            return self.root, "."

        # Check each prefix step on disk so no intermediate symlink can point outside root.
        cur = self.root
        for seg in parts:
            cur = os.path.join(cur, seg)
            if os.path.islink(cur) or os.path.exists(cur):
                real_step = os.path.realpath(cur)
                if os.path.commonpath([self.root, real_step]) != self.root:
                    raise PermissionError(f"Symlink escapes local workspace: {rel_path!r}")

        abs_target = os.path.realpath(os.path.join(self.root, *parts))
        if os.path.commonpath([self.root, abs_target]) != self.root:
            raise PermissionError(f"Resolved path escapes local workspace: {rel_path!r}")

        canon_rel = os.path.relpath(abs_target, self.root).replace("\\", "/")
        if canon_rel == ".":
            if not allow_root:
                raise ValueError("Path refers to workspace root")
            return self.root, "."

        if not allow_internal and _is_internal_rel(canon_rel):
            raise PermissionError(f"Access to internal workspace path {canon_rel!r} is restricted")

        return abs_target, canon_rel

    def _entry_info(self, abs_path: str, rel_path: str) -> Dict[str, Any]:
        st = os.stat(abs_path)
        is_dir = os.path.isdir(abs_path)
        item: Dict[str, Any] = {
            "name": os.path.basename(abs_path) if rel_path != "." else ".",
            "path": rel_path,
            "type": "dir" if is_dir else "file",
            "modified": datetime.datetime.fromtimestamp(st.st_mtime, tz=datetime.timezone.utc).strftime(
                "%Y-%m-%dT%H:%M:%SZ"
            ),
        }
        if not is_dir:
            item["size"] = int(st.st_size)
        return item

    def list_dir(
        self,
        path: str = ".",
        recursive: bool = False,
        max_entries: int = DEFAULT_FILE_LIST_MAX_ENTRIES,
    ) -> Dict[str, Any]:
        if isinstance(max_entries, bool):
            raise ValueError("max_entries must be an integer")
        limit = max(1, min(int(max_entries), MAX_FILE_LIST_ENTRIES))

        with self._lock:
            abs_dir, rel_dir = self.resolve_path(path or ".", allow_root=True, allow_internal=True)
            if not os.path.exists(abs_dir):
                raise FileNotFoundError(f"Workspace directory not found: {rel_dir}")
            if not os.path.isdir(abs_dir):
                raise NotADirectoryError(f"Workspace path is not a directory: {rel_dir}")

            entries: List[Dict[str, Any]] = []
            hide_internal = rel_dir == "."

            if recursive:
                for cur_root, dirnames, filenames in os.walk(abs_dir, followlinks=False):
                    if hide_internal:
                        dirnames[:] = [d for d in dirnames if d not in INTERNAL_DIR_NAMES]
                    for dname in dirnames:
                        d_abs = os.path.join(cur_root, dname)
                        d_rel = os.path.relpath(d_abs, self.root).replace("\\", "/")
                        entries.append(self._entry_info(d_abs, d_rel))
                    for fname in filenames:
                        f_abs = os.path.join(cur_root, fname)
                        f_rel = os.path.relpath(f_abs, self.root).replace("\\", "/")
                        entries.append(self._entry_info(f_abs, f_rel))
            else:
                with os.scandir(abs_dir) as it:
                    for de in it:
                        if hide_internal and de.name in INTERNAL_DIR_NAMES:
                            continue
                        sub_rel = de.name if rel_dir == "." else f"{rel_dir}/{de.name}"
                        entries.append(self._entry_info(de.path, sub_rel))

            entries.sort(key=lambda e: (0 if e["type"] == "dir" else 1, e["path"].lower(), e["path"]))
            sliced = entries[:limit]
            return {
                "status": "ok",
                "path": rel_dir,
                "entries": sliced,
                "returned_entries": len(sliced),
                "total_entries": len(entries),
                "truncated": len(entries) > limit,
            }

    def stat(self, path: str) -> Dict[str, Any]:
        with self._lock:
            abs_path, rel_path = self.resolve_path(path, allow_root=True, allow_internal=True)
            if not os.path.exists(abs_path):
                return {"status": "ok", "path": rel_path, "exists": False}
            info = self._entry_info(abs_path, rel_path)
            info["exists"] = True
            return info

    def read_file(
        self,
        path: str,
        encoding: str = "utf-8",
        as_base64: bool = False,
        offset: int = 0,
        max_bytes: int = DEFAULT_FILE_READ_MAX_BYTES,
    ) -> Dict[str, Any]:
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
            abs_path, rel_path = self.resolve_path(path, allow_root=False, allow_internal=True)
            if not os.path.exists(abs_path):
                raise FileNotFoundError(f"Workspace file not found: {rel_path}")
            if os.path.isdir(abs_path):
                raise IsADirectoryError(f"Workspace path is a directory: {rel_path}")

            total_size = int(os.path.getsize(abs_path))
            if offset >= total_size:
                empty_res: Dict[str, Any] = {
                    "status": "ok",
                    "path": rel_path,
                    "offset": offset,
                    "bytes_read": 0,
                    "total_size": total_size,
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

            with open(abs_path, "rb") as f:
                if offset > 0:
                    f.seek(offset)
                raw = f.read(limit + 1)

        truncated = len(raw) > limit
        chunk = raw[:limit]
        eof = (not truncated) and ((offset + len(chunk)) >= total_size)
        result: Dict[str, Any] = {
            "status": "ok",
            "path": rel_path,
            "offset": offset,
            "bytes_read": len(chunk),
            "total_size": total_size,
            "truncated": truncated,
            "eof": eof,
        }
        if as_base64:
            result["encoding"] = "base64"
            result["content_base64"] = base64.b64encode(chunk).decode("ascii")
        else:
            result["encoding"] = encoding
            try:
                result["content"] = chunk.decode(encoding)
            except UnicodeDecodeError:
                result["content"] = chunk.decode(encoding, errors="replace")
                result["had_decode_errors"] = True
                result["note"] = (
                    "Some bytes could not be decoded cleanly; retry with as_base64=true for binary files."
                )
        return result

    def write_file(
        self,
        path: str,
        content: Optional[str] = None,
        content_base64: Optional[str] = None,
        encoding: str = "utf-8",
        append: bool = False,
        create_parents: bool = True,
    ) -> Dict[str, Any]:
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
            abs_path, rel_path = self.resolve_path(path, allow_root=False, allow_internal=False)
            if os.path.isdir(abs_path):
                raise IsADirectoryError(f"Workspace path is a directory: {rel_path}")
            parent = os.path.dirname(abs_path)
            if create_parents:
                os.makedirs(parent, exist_ok=True)
            elif not os.path.isdir(parent):
                raise FileNotFoundError(f"Parent directory does not exist for {rel_path}")

            if append:
                with open(abs_path, "ab") as f:
                    f.write(raw)
                    f.flush()
                    os.fsync(f.fileno())
            else:
                atomic_write_bytes(abs_path, raw)

            return {
                "status": "ok",
                "path": rel_path,
                "bytes_written": len(raw),
                "mode": "append" if append else "overwrite",
            }

    def mkdir(self, path: str, parents: bool = True) -> Dict[str, Any]:
        with self._lock:
            abs_path, rel_path = self.resolve_path(path, allow_root=False, allow_internal=False)
            if os.path.exists(abs_path):
                if not os.path.isdir(abs_path):
                    raise FileExistsError(f"Workspace path exists and is not a directory: {rel_path}")
                return {"status": "ok", "path": rel_path, "created": False}
            if parents:
                os.makedirs(abs_path, exist_ok=True)
            else:
                os.mkdir(abs_path)
            return {"status": "ok", "path": rel_path, "created": True}

    def delete(self, path: str, recursive: bool = False) -> Dict[str, Any]:
        with self._lock:
            abs_path, rel_path = self.resolve_path(path, allow_root=False, allow_internal=False)
            if not os.path.exists(abs_path):
                raise FileNotFoundError(f"Workspace path not found: {rel_path}")
            if os.path.isdir(abs_path):
                if recursive:
                    shutil.rmtree(abs_path)
                else:
                    os.rmdir(abs_path)
                return {"status": "ok", "path": rel_path, "deleted_type": "dir"}
            os.unlink(abs_path)
            return {"status": "ok", "path": rel_path, "deleted_type": "file"}

    def rename(self, from_path: str, to_path: str) -> Dict[str, Any]:
        with self._lock:
            abs_from, rel_from = self.resolve_path(from_path, allow_root=False, allow_internal=False)
            abs_to, rel_to = self.resolve_path(to_path, allow_root=False, allow_internal=False)
            if not os.path.exists(abs_from):
                raise FileNotFoundError(f"Source workspace path not found: {rel_from}")
            os.makedirs(os.path.dirname(abs_to), exist_ok=True)
            os.replace(abs_from, abs_to)
            return {"status": "ok", "from_path": rel_from, "to_path": rel_to}

    def search_files(
        self,
        query: str,
        path: str = ".",
        glob_pattern: Optional[str] = None,
        max_matches: int = 50,
    ) -> Dict[str, Any]:
        if not isinstance(query, str) or not query:
            raise ValueError("query must be a non-empty string")
        if isinstance(max_matches, bool):
            raise ValueError("max_matches must be an integer")
        limit = max(1, min(int(max_matches), 500))
        q_lower = query.lower()

        with self._lock:
            abs_dir, rel_dir = self.resolve_path(path or ".", allow_root=True, allow_internal=False)
            if not os.path.exists(abs_dir):
                raise FileNotFoundError(f"Workspace search path not found: {rel_dir}")
            if not os.path.isdir(abs_dir):
                raise NotADirectoryError(f"Workspace search path is not a directory: {rel_dir}")

            matches: List[Dict[str, Any]] = []
            truncated = False

            for cur_root, dirnames, filenames in os.walk(abs_dir, followlinks=False):
                dirnames[:] = sorted(d for d in dirnames if d not in INTERNAL_DIR_NAMES)
                for fname in sorted(filenames):
                    if glob_pattern and not fnmatch.fnmatch(fname, glob_pattern):
                        continue
                    f_abs = os.path.join(cur_root, fname)
                    f_rel = os.path.relpath(f_abs, self.root).replace("\\", "/")

                    if q_lower in f_rel.lower():
                        matches.append({"path": f_rel, "match_type": "filename", "line_number": 0, "snippet": f_rel})
                        if len(matches) >= limit:
                            truncated = True
                            break

                    try:
                        if os.path.getsize(f_abs) > MAX_FILE_READ_BYTES:
                            continue
                        with open(f_abs, "rb") as f:
                            raw = f.read()
                        if b"\x00" in raw:
                            continue
                        text = raw.decode("utf-8")
                    except (OSError, UnicodeDecodeError):
                        continue

                    for idx, line in enumerate(text.splitlines(), 1):
                        if q_lower in line.lower():
                            matches.append(
                                {
                                    "path": f_rel,
                                    "match_type": "content",
                                    "line_number": idx,
                                    "snippet": line.strip()[:240],
                                }
                            )
                            if len(matches) >= limit:
                                truncated = True
                                break
                    if truncated:
                        break
                if truncated:
                    break

            return {
                "status": "ok",
                "query": query,
                "path": rel_dir,
                "matches": matches,
                "total_matches": len(matches),
                "truncated": truncated,
            }

    # --- Persistent Memory Management ---

    def _load_entries_unlocked(self) -> Dict[str, Dict[str, Any]]:
        if not os.path.exists(self.memory_entries_path):
            return {}
        try:
            with open(self.memory_entries_path, "r", encoding="utf-8") as f:
                data = json.load(f)
            entries = data.get("entries") if isinstance(data, dict) else None
            if isinstance(entries, dict):
                return dict(entries)
        except (OSError, json.JSONDecodeError):
            pass
        return {}

    def _save_entries_unlocked(self, entries: Dict[str, Dict[str, Any]]) -> None:
        atomic_write_json(self.memory_entries_path, {"version": 1, "entries": entries})

    @staticmethod
    def _normalize_tags(tags: Optional[Union[str, List[str]]]) -> List[str]:
        if tags is None:
            return []
        if isinstance(tags, str):
            raw_list = tags.split(",")
        elif isinstance(tags, list):
            raw_list = [str(t) for t in tags]
        else:
            raise ValueError("tags must be a comma-separated string or list")
        out: List[str] = []
        seen = set()
        for item in raw_list:
            cleaned = item.strip()
            if cleaned and cleaned not in seen:
                seen.add(cleaned)
                out.append(cleaned)
        return out

    def memory_list(self, tag: Optional[str] = None, limit: int = 100) -> Dict[str, Any]:
        if isinstance(limit, bool):
            raise ValueError("limit must be an integer")
        cap = max(1, min(int(limit), 1000))
        tag_filter = tag.strip().lower() if isinstance(tag, str) and tag.strip() else None

        with self._lock:
            entries = self._load_entries_unlocked()
            items: List[Dict[str, Any]] = []
            for key, entry in sorted(entries.items()):
                etags = [str(t) for t in entry.get("tags", [])]
                if tag_filter and tag_filter not in [t.lower() for t in etags]:
                    continue
                items.append(
                    {
                        "key": key,
                        "tags": etags,
                        "author": entry.get("author", ""),
                        "updated_at": entry.get("updated_at", ""),
                        "preview": str(entry.get("value", ""))[:160],
                    }
                )
            sliced = items[:cap]
            return {
                "status": "ok",
                "entries": sliced,
                "total_entries": len(items),
                "truncated": len(items) > cap,
                "memory_md_path": "memory/MEMORY.md",
            }

    def memory_read(self, key: Optional[str] = None) -> Dict[str, Any]:
        with self._lock:
            if key is None or not str(key).strip():
                with open(self.memory_md_path, "r", encoding="utf-8", errors="replace") as f:
                    md_text = f.read()
                entries = self._load_entries_unlocked()
                index = [
                    {
                        "key": k,
                        "tags": v.get("tags", []),
                        "author": v.get("author", ""),
                        "updated_at": v.get("updated_at", ""),
                    }
                    for k, v in sorted(entries.items())
                ]
                return {
                    "status": "ok",
                    "memory_md": md_text,
                    "keys": index,
                }

            norm_key = str(key).strip()
            if norm_key in ("MEMORY.md", "memory/MEMORY.md"):
                with open(self.memory_md_path, "r", encoding="utf-8", errors="replace") as f:
                    md_text = f.read()
                return {"status": "ok", "key": "MEMORY.md", "exists": True, "value": md_text}

            entries = self._load_entries_unlocked()
            if norm_key not in entries:
                return {"status": "ok", "key": norm_key, "exists": False}
            entry = dict(entries[norm_key])
            entry["status"] = "ok"
            entry["key"] = norm_key
            entry["exists"] = True
            return entry

    def memory_write(
        self,
        key: str,
        value: str,
        tags: Optional[Union[str, List[str]]] = None,
        author: str = "computer_use",
        append: bool = False,
    ) -> Dict[str, Any]:
        if not isinstance(key, str) or not key.strip():
            raise ValueError("memory key must be a non-empty string")
        norm_key = key.strip()
        val_str = str(value)
        norm_tags = self._normalize_tags(tags)
        now = utc_now_iso()

        with self._lock:
            if norm_key in ("MEMORY.md", "memory/MEMORY.md"):
                if append and os.path.exists(self.memory_md_path):
                    with open(self.memory_md_path, "r", encoding="utf-8", errors="replace") as f:
                        prev = f.read()
                    sep = "" if (not prev or prev.endswith("\n")) else "\n"
                    new_text = f"{prev}{sep}{val_str}"
                    if not new_text.endswith("\n"):
                        new_text += "\n"
                else:
                    new_text = val_str if val_str.endswith("\n") else (val_str + "\n")
                atomic_write_bytes(self.memory_md_path, new_text.encode("utf-8"))
                return {
                    "status": "ok",
                    "key": "MEMORY.md",
                    "mode": "append" if append else "overwrite",
                    "bytes": len(new_text.encode("utf-8")),
                    "updated_at": now,
                }

            entries = self._load_entries_unlocked()
            prev_entry = entries.get(norm_key)
            if append and prev_entry is not None:
                prev_val = str(prev_entry.get("value", ""))
                sep = "" if (not prev_val or prev_val.endswith("\n")) else "\n"
                final_val = f"{prev_val}{sep}{val_str}"
                merged_tags = list(prev_entry.get("tags", []))
                for t in norm_tags:
                    if t not in merged_tags:
                        merged_tags.append(t)
            else:
                final_val = val_str
                merged_tags = norm_tags if norm_tags else (list(prev_entry.get("tags", [])) if prev_entry else [])

            entries[norm_key] = {
                "value": final_val,
                "tags": merged_tags,
                "author": author,
                "created_at": prev_entry.get("created_at", now) if prev_entry else now,
                "updated_at": now,
            }
            self._save_entries_unlocked(entries)
            return {
                "status": "ok",
                "key": norm_key,
                "tags": merged_tags,
                "author": author,
                "mode": "append" if append else "overwrite",
                "updated_at": now,
            }

    def memory_delete(self, key: str) -> Dict[str, Any]:
        if not isinstance(key, str) or not key.strip():
            raise ValueError("memory key must be a non-empty string")
        norm_key = key.strip()
        if norm_key in ("MEMORY.md", "memory/MEMORY.md"):
            raise ValueError("Cannot delete MEMORY.md; overwrite it with memory_write instead")

        with self._lock:
            entries = self._load_entries_unlocked()
            existed = norm_key in entries
            if existed:
                del entries[norm_key]
                self._save_entries_unlocked(entries)
            return {"status": "ok", "key": norm_key, "deleted": existed}

    def memory_search(
        self,
        query: str,
        tag: Optional[str] = None,
        limit: int = 20,
    ) -> Dict[str, Any]:
        if not isinstance(query, str) or not query.strip():
            raise ValueError("query must be a non-empty string")
        if isinstance(limit, bool):
            raise ValueError("limit must be an integer")
        cap = max(1, min(int(limit), 200))
        q_lower = query.lower()
        tag_filter = tag.strip().lower() if isinstance(tag, str) and tag.strip() else None

        with self._lock:
            results: List[Dict[str, Any]] = []
            if not tag_filter and os.path.exists(self.memory_md_path):
                with open(self.memory_md_path, "r", encoding="utf-8", errors="replace") as f:
                    for idx, line in enumerate(f, 1):
                        if q_lower in line.lower():
                            results.append(
                                {
                                    "source": "MEMORY.md",
                                    "key": "MEMORY.md",
                                    "line_number": idx,
                                    "snippet": line.strip()[:240],
                                }
                            )
                            if len(results) >= cap:
                                return {"status": "ok", "query": query, "matches": results, "truncated": True}

            entries = self._load_entries_unlocked()
            for key, entry in sorted(entries.items()):
                etags = [str(t) for t in entry.get("tags", [])]
                if tag_filter and tag_filter not in [t.lower() for t in etags]:
                    continue
                val_str = str(entry.get("value", ""))
                hay = f"{key}\n{' '.join(etags)}\n{val_str}".lower()
                if q_lower in hay:
                    results.append(
                        {
                            "source": "entries.json",
                            "key": key,
                            "tags": etags,
                            "author": entry.get("author", ""),
                            "updated_at": entry.get("updated_at", ""),
                            "value": val_str[:400],
                        }
                    )
                    if len(results) >= cap:
                        return {"status": "ok", "query": query, "matches": results, "truncated": True}

            return {"status": "ok", "query": query, "matches": results, "truncated": False}

    # --- Remote Host <-> Local Workspace Transfers ---

    def download_from_remote(
        self,
        ftp_client: VncFtpClient,
        remote_path: str,
        local_path: str,
        overwrite: bool = True,
    ) -> Dict[str, Any]:
        with self._lock:
            abs_local, rel_local = self.resolve_path(local_path, allow_root=False, allow_internal=False)
            if os.path.isdir(abs_local):
                raise IsADirectoryError(f"Local destination is a directory: {rel_local}")
            if not overwrite and os.path.exists(abs_local):
                raise FileExistsError(f"Local file already exists and overwrite=false: {rel_local}")

        res = ftp_client.download_to_path(
            remote_path=remote_path,
            local_abs_path=abs_local,
            max_bytes=MAX_TRANSFER_BYTES,
        )
        res["local_path"] = rel_local
        return res

    def upload_to_remote(
        self,
        ftp_client: VncFtpClient,
        local_path: str,
        remote_path: str,
        create_parents: bool = True,
    ) -> Dict[str, Any]:
        with self._lock:
            abs_local, rel_local = self.resolve_path(local_path, allow_root=False, allow_internal=True)
            if not os.path.exists(abs_local):
                raise FileNotFoundError(f"Local workspace file not found: {rel_local}")
            if not os.path.isfile(abs_local):
                raise IsADirectoryError(f"Local workspace path is not a regular file: {rel_local}")

        res = ftp_client.upload_from_path(
            local_abs_path=abs_local,
            remote_path=remote_path,
            create_parents=create_parents,
            max_bytes=MAX_TRANSFER_BYTES,
        )
        res["local_path"] = rel_local
        return res
