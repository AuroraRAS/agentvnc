"""Asynchronous marker-based coordination between Computer Use Agent and Research Assistant."""

import json
import os
import re
import threading
import time
import uuid
from typing import Any, Callable, Dict, List, Optional, Tuple, Union

from .constants import (
    AGENT_ASSISTANT,
    AGENT_COMPUTER_USE,
    VALID_AGENTS,
    VALID_MESSAGE_KINDS,
    VALID_PRIORITIES,
    VALID_STATUS_STATES,
)
from .workspace import LocalWorkspace, atomic_write_bytes, atomic_write_json, utc_now_iso


COORD_PROTOCOL_DOC = """# Dual-Agent Marker Coordination Protocol (`.coord/`)

Both `computer_use` (desktop VNC agent) and `assistant` (Web Search + Gemini File Search specialist)
share this local workspace and coordinate asynchronously via atomic marker files in `.coord/`.

## Marker Files

1. **Status Markers:** `.coord/STATUS.<agent>.marker`
   JSON: `{"agent": str, "state": str, "task": str, "detail": str, "updated_at": str, "seq": int}`
   Valid states: `idle`, `working`, `waiting_peer`, `waiting_user`, `sleeping`, `paused`, `done`, `error`.

2. **Wakeup Markers:** `.coord/WAKE.<agent>.marker`
   JSON: `{"from": str, "to": str, "reason": str, "message_id": str|null, "priority": "normal"|"urgent", "created_at": str, "seq": int}`
   Created to wake the target agent up from sleep/idle/wait and deleted when consumed.

3. **Message Markers:**
   - Unread: `.coord/inbox/<recipient>/MSG_<seq:06d>_<id>.marker.json`
   - Read/Archived: `.coord/archive/MSG_<seq:06d>_<id>.marker.json`
   JSON: `{"id": str, "seq": int, "from": str, "to": str, "kind": str, "subject": str, "body": str, "attachments": [str], "in_reply_to": str|null, "priority": str, "created_at": str, "status": "unread"|"read"}`
"""

_MSG_SEQ_RE = re.compile(r"^MSG_(\d+)_")


def _validate_agent(agent: str) -> str:
    if agent not in VALID_AGENTS:
        raise ValueError(f"Unknown agent {agent!r}; expected one of {VALID_AGENTS}")
    return agent


def _parse_attachments(attachments: Optional[Union[str, List[str]]]) -> List[str]:
    if attachments is None:
        return []
    if isinstance(attachments, str):
        parts = attachments.split(",")
    elif isinstance(attachments, list):
        parts = [str(p) for p in attachments]
    else:
        raise ValueError("attachments must be a comma-separated string or list of paths")
    out: List[str] = []
    for item in parts:
        cleaned = item.strip()
        if cleaned and cleaned not in out:
            out.append(cleaned)
    return out


class WorkspaceCoordinator:
    """Manages `.coord/` status, wake, and inbox/archive marker files plus wakeup events."""

    def __init__(self, workspace: LocalWorkspace) -> None:
        self.workspace = workspace
        self.coord_dir = workspace.coord_dir
        self.inbox_dirs = {
            AGENT_COMPUTER_USE: os.path.join(self.coord_dir, "inbox", AGENT_COMPUTER_USE),
            AGENT_ASSISTANT: os.path.join(self.coord_dir, "inbox", AGENT_ASSISTANT),
        }
        self.archive_dir = os.path.join(self.coord_dir, "archive")
        self.protocol_path = os.path.join(self.coord_dir, "PROTOCOL.md")

        for d in self.inbox_dirs.values():
            os.makedirs(d, exist_ok=True)
        os.makedirs(self.archive_dir, exist_ok=True)
        if not os.path.exists(self.protocol_path):
            atomic_write_bytes(self.protocol_path, COORD_PROTOCOL_DOC.encode("utf-8"))

        self._lock = threading.RLock()
        self._wake_events = {
            AGENT_COMPUTER_USE: threading.Event(),
            AGENT_ASSISTANT: threading.Event(),
        }
        self._stop_event = threading.Event()
        self._seq = self._discover_max_seq()

        for agent in VALID_AGENTS:
            if not os.path.exists(self._status_path(agent)):
                init_state = "idle" if agent == AGENT_COMPUTER_USE else "sleeping"
                self.set_status(agent, init_state, task="initialized")
            if os.path.exists(self._wake_path(agent)) or self._list_inbox_files_unlocked(agent):
                self._wake_events[agent].set()

    def _discover_max_seq(self) -> int:
        max_seq = 0
        scan_dirs = [self.archive_dir, *self.inbox_dirs.values()]
        for d in scan_dirs:
            try:
                for name in os.listdir(d):
                    m = _MSG_SEQ_RE.match(name)
                    if m:
                        max_seq = max(max_seq, int(m.group(1)))
            except OSError:
                pass
        for agent in VALID_AGENTS:
            for path in (self._status_path(agent), self._wake_path(agent)):
                data = self._read_json_safe(path)
                if isinstance(data, dict) and isinstance(data.get("seq"), int):
                    max_seq = max(max_seq, int(data["seq"]))
        return max_seq

    def _next_seq_unlocked(self) -> int:
        self._seq += 1
        return self._seq

    @staticmethod
    def peer_of(agent: str) -> str:
        _validate_agent(agent)
        return AGENT_ASSISTANT if agent == AGENT_COMPUTER_USE else AGENT_COMPUTER_USE

    def _status_path(self, agent: str) -> str:
        _validate_agent(agent)
        return os.path.join(self.coord_dir, f"STATUS.{agent}.marker")

    def _wake_path(self, agent: str) -> str:
        _validate_agent(agent)
        return os.path.join(self.coord_dir, f"WAKE.{agent}.marker")

    @staticmethod
    def _read_json_safe(path: str) -> Optional[Dict[str, Any]]:
        if not os.path.exists(path):
            return None
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
            return data if isinstance(data, dict) else None
        except (OSError, json.JSONDecodeError):
            return None

    def _list_inbox_files_unlocked(self, agent: str) -> List[str]:
        _validate_agent(agent)
        d = self.inbox_dirs[agent]
        try:
            names = [n for n in os.listdir(d) if n.endswith(".marker.json")]
        except OSError:
            return []
        names.sort()
        return [os.path.join(d, n) for n in names]

    def set_status(self, agent: str, state: str, task: str = "", detail: str = "") -> Dict[str, Any]:
        _validate_agent(agent)
        norm_state = str(state).strip().lower()
        if norm_state not in VALID_STATUS_STATES:
            raise ValueError(f"Invalid status state {state!r}; expected one of {VALID_STATUS_STATES}")

        with self._lock:
            seq = self._next_seq_unlocked()
            payload = {
                "agent": agent,
                "state": norm_state,
                "task": str(task or ""),
                "detail": str(detail or ""),
                "updated_at": utc_now_iso(),
                "seq": seq,
            }
            atomic_write_json(self._status_path(agent), payload)
            return payload

    def get_status(self, agent: Optional[str] = None) -> Dict[str, Any]:
        with self._lock:
            targets = (_validate_agent(agent),) if agent is not None else VALID_AGENTS
            agents_info: Dict[str, Any] = {}
            for name in targets:
                status_doc = self._read_json_safe(self._status_path(name)) or {
                    "agent": name,
                    "state": "unknown",
                    "task": "",
                    "detail": "",
                }
                wake_doc = self._read_json_safe(self._wake_path(name))
                unread = len(self._list_inbox_files_unlocked(name))
                agents_info[name] = {
                    "status": status_doc,
                    "wake_pending": wake_doc is not None,
                    "wake_marker": wake_doc,
                    "unread_messages": unread,
                }
            return {
                "status": "ok",
                "coord_dir": ".coord",
                "agents": agents_info,
            }

    def wake_peer(
        self,
        sender: str,
        recipient: str,
        reason: str,
        message_id: Optional[str] = None,
        priority: str = "normal",
    ) -> Dict[str, Any]:
        _validate_agent(sender)
        _validate_agent(recipient)
        norm_prio = str(priority or "normal").strip().lower()
        if norm_prio not in VALID_PRIORITIES:
            raise ValueError(f"Invalid priority {priority!r}; expected one of {VALID_PRIORITIES}")

        with self._lock:
            seq = self._next_seq_unlocked()
            wake_doc: Dict[str, Any] = {
                "from": sender,
                "to": recipient,
                "reason": str(reason or "wake requested"),
                "message_id": message_id,
                "priority": norm_prio,
                "created_at": utc_now_iso(),
                "seq": seq,
            }
            atomic_write_json(self._wake_path(recipient), wake_doc)
            self._wake_events[recipient].set()
            return {"status": "ok", "wake_marker": wake_doc}

    def send_message(
        self,
        sender: str,
        recipient: str,
        subject: str,
        body: str,
        kind: str = "request",
        attachments: Optional[Union[str, List[str]]] = None,
        in_reply_to: Optional[str] = None,
        wake: bool = True,
        priority: str = "normal",
    ) -> Dict[str, Any]:
        _validate_agent(sender)
        _validate_agent(recipient)
        norm_kind = str(kind or "request").strip().lower()
        if norm_kind not in VALID_MESSAGE_KINDS:
            raise ValueError(f"Invalid message kind {kind!r}; expected one of {VALID_MESSAGE_KINDS}")
        norm_prio = str(priority or "normal").strip().lower()
        if norm_prio not in VALID_PRIORITIES:
            raise ValueError(f"Invalid priority {priority!r}; expected one of {VALID_PRIORITIES}")
        if not isinstance(subject, str) or not subject.strip():
            raise ValueError("Message subject must be a non-empty string")

        att_list = _parse_attachments(attachments)
        with self._lock:
            seq = self._next_seq_unlocked()
            msg_id = f"msg_{seq:06d}_{uuid.uuid4().hex[:8]}"
            filename = f"MSG_{seq:06d}_{msg_id.rsplit('_', 1)[-1]}.marker.json"
            msg_path = os.path.join(self.inbox_dirs[recipient], filename)
            msg_doc: Dict[str, Any] = {
                "id": msg_id,
                "seq": seq,
                "from": sender,
                "to": recipient,
                "kind": norm_kind,
                "subject": subject.strip(),
                "body": str(body),
                "attachments": att_list,
                "in_reply_to": in_reply_to.strip() if isinstance(in_reply_to, str) and in_reply_to.strip() else None,
                "priority": norm_prio,
                "created_at": utc_now_iso(),
                "status": "unread",
            }
            atomic_write_json(msg_path, msg_doc)
            wake_res: Optional[Dict[str, Any]] = None
            if wake:
                wake_res = self.wake_peer(
                    sender=sender,
                    recipient=recipient,
                    reason=f"{norm_kind}: {subject.strip()}",
                    message_id=msg_id,
                    priority=norm_prio,
                )["wake_marker"]
            return {
                "status": "ok",
                "message": msg_doc,
                "marker_file": os.path.relpath(msg_path, self.workspace.root).replace("\\", "/"),
                "woke_peer": bool(wake),
                "wake_marker": wake_res,
            }

    def has_wake(self, agent: str) -> bool:
        _validate_agent(agent)
        with self._lock:
            if self._wake_events[agent].is_set():
                return True
            if os.path.exists(self._wake_path(agent)):
                return True
            return bool(self._list_inbox_files_unlocked(agent))

    def peek_wake(self, agent: str) -> Optional[Dict[str, Any]]:
        _validate_agent(agent)
        with self._lock:
            return self._read_json_safe(self._wake_path(agent))

    def consume_wake(self, agent: str) -> Optional[Dict[str, Any]]:
        _validate_agent(agent)
        with self._lock:
            wpath = self._wake_path(agent)
            wake_doc = self._read_json_safe(wpath)
            if os.path.exists(wpath):
                try:
                    os.unlink(wpath)
                except OSError:
                    pass
            self._wake_events[agent].clear()
            return wake_doc

    def unread_count(self, agent: str) -> int:
        _validate_agent(agent)
        with self._lock:
            return len(self._list_inbox_files_unlocked(agent))

    def peek_unread_summaries(self, agent: str, limit: int = 10) -> List[Dict[str, Any]]:
        _validate_agent(agent)
        with self._lock:
            summaries: List[Dict[str, Any]] = []
            for path in self._list_inbox_files_unlocked(agent)[: max(1, int(limit))]:
                doc = self._read_json_safe(path)
                if doc is None:
                    continue
                summaries.append(
                    {
                        "id": doc.get("id"),
                        "from": doc.get("from"),
                        "kind": doc.get("kind"),
                        "subject": doc.get("subject"),
                        "priority": doc.get("priority"),
                        "created_at": doc.get("created_at"),
                        "attachments": doc.get("attachments", []),
                    }
                )
            return summaries

    def read_messages(
        self,
        agent: str,
        include_read: bool = False,
        mark_read: bool = True,
        limit: int = 20,
    ) -> Dict[str, Any]:
        _validate_agent(agent)
        if isinstance(limit, bool):
            raise ValueError("limit must be an integer")
        cap = max(1, min(int(limit), 200))

        with self._lock:
            inbox_paths = self._list_inbox_files_unlocked(agent)
            unread_msgs: List[Dict[str, Any]] = []
            for path in inbox_paths[:cap]:
                doc = self._read_json_safe(path)
                if doc is None:
                    continue
                if mark_read:
                    doc["status"] = "read"
                    doc["read_at"] = utc_now_iso()
                    arch_path = os.path.join(self.archive_dir, os.path.basename(path))
                    atomic_write_json(arch_path, doc)
                    try:
                        os.unlink(path)
                    except OSError:
                        pass
                unread_msgs.append(doc)

            wake_consumed: Optional[Dict[str, Any]] = None
            if mark_read and not self._list_inbox_files_unlocked(agent):
                wake_consumed = self.consume_wake(agent)

            archived_msgs: List[Dict[str, Any]] = []
            if include_read:
                try:
                    arch_names = sorted(
                        [n for n in os.listdir(self.archive_dir) if n.endswith(".marker.json")],
                        reverse=True,
                    )
                except OSError:
                    arch_names = []
                for name in arch_names:
                    doc = self._read_json_safe(os.path.join(self.archive_dir, name))
                    if doc and doc.get("to") == agent:
                        archived_msgs.append(doc)
                        if len(archived_msgs) >= cap:
                            break

            return {
                "status": "ok",
                "agent": agent,
                "messages": unread_msgs,
                "unread_remaining": len(self._list_inbox_files_unlocked(agent)),
                "archived_messages": archived_msgs,
                "wake_consumed": wake_consumed,
            }

    def wait_for_wake(
        self,
        agent: str,
        timeout: float = 30.0,
        poll_interval: float = 0.2,
        check_interrupt: Optional[Callable[[], Optional[Dict[str, Any]]]] = None,
    ) -> Dict[str, Any]:
        """Sleep until WAKE/inbox marker arrives, stop is signaled, or check_interrupt returns a dict."""
        _validate_agent(agent)
        deadline = time.monotonic() + max(0.0, float(timeout))
        step = max(0.05, min(float(poll_interval), 0.5))

        while True:
            if self._stop_event.is_set():
                return {"woken": False, "stopped": True}

            if self.has_wake(agent):
                return {"woken": True, "stopped": False}

            if check_interrupt is not None:
                interrupted = check_interrupt()
                if interrupted is not None:
                    return {"woken": False, "stopped": False, "interrupted": interrupted}

            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return {"woken": False, "stopped": False, "timed_out": True}

            self._wake_events[agent].wait(timeout=min(step, remaining))

    def shutdown(self) -> None:
        self._stop_event.set()
        for ev in self._wake_events.values():
            ev.set()
