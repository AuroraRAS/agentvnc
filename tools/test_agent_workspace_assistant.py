#!/usr/bin/env python3
"""Offline unit and end-to-end tests for local workspace, memory, transfers, File Search, and async assistant."""

import argparse
import base64
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import shutil
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock
import urllib.parse
from typing import Any, Dict, List, Optional

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

from test_agent_file_io import FakeAgentClient, MockFtpServer  # noqa: E402
import vnc_computer_use_agent as agent_mod  # noqa: E402
from vnc_agent import (  # noqa: E402
    AGENT_ASSISTANT,
    AGENT_COMPUTER_USE,
    ASSISTANT_FILE_SEARCH_DECLARATIONS,
    ASSISTANT_SYSTEM_INSTRUCTION,
    COORDINATION_FUNCTION_DECLARATIONS,
    COORDINATION_SYSTEM_INSTRUCTION,
    FileSearchClient,
    FileSearchStoreRegistry,
    LocalWorkspace,
    REMOTE_TRANSFER_FUNCTION_DECLARATIONS,
    ResearchAssistantWorker,
    SYSTEM_INSTRUCTION,
    WORKSPACE_FUNCTION_DECLARATIONS,
    WorkspaceCoordinator,
    extract_citations,
    extract_server_tool_events,
)


class MockGeminiBackend:
    """In-memory mock HTTP backend for Gemini Interactions and File Search v1beta REST APIs."""

    def __init__(self, fail_combined_tools: bool = False) -> None:
        self.fail_combined_tools = fail_combined_tools
        self.lock = threading.RLock()
        self.stores: Dict[str, Dict[str, Any]] = {}
        self.documents: Dict[str, Dict[str, Any]] = {}
        self.operations: Dict[str, Dict[str, Any]] = {}
        self.upload_sessions: Dict[str, Dict[str, Any]] = {}
        self.interaction_requests: List[Dict[str, Any]] = []
        self.file_search_requests: List[Dict[str, Any]] = []
        self.api_keys_seen: List[str] = []
        self.interaction_revisions_seen: List[str] = []
        self._seq = 0
        self.scripted_handler: Optional[Any] = None

    def next_id(self, prefix: str) -> str:
        with self.lock:
            self._seq += 1
            return f"{prefix}_{self._seq}"


def start_mock_gemini_server(backend: MockGeminiBackend) -> ThreadingHTTPServer:
    class _Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, format: str, *args: Any) -> None:  # noqa: A002
            return

        def _send_json(self, code: int, payload: Any, headers: Optional[Dict[str, str]] = None) -> None:
            raw = json.dumps(payload).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            if headers:
                for k, v in headers.items():
                    self.send_header(k, v)
            self.end_headers()
            self.wfile.write(raw)

        def _read_body(self) -> bytes:
            length = int(self.headers.get("Content-Length", "0") or "0")
            return self.rfile.read(length) if length > 0 else b""

        def do_GET(self) -> None:  # noqa: N802
            parsed = urllib.parse.urlsplit(self.path)
            path = parsed.path
            qs = urllib.parse.parse_qs(parsed.query)
            api_key = self.headers.get("x-goog-api-key", "")
            with backend.lock:
                backend.api_keys_seen.append(api_key)
                backend.file_search_requests.append({"method": "GET", "path": path, "qs": qs})

                if path == "/v1beta/fileSearchStores":
                    self._send_json(200, {"fileSearchStores": list(backend.stores.values())})
                    return

                if path.startswith("/v1beta/fileSearchStores/") and path.endswith("/documents"):
                    store_name = path[len("/v1beta/") : -len("/documents")]
                    docs = [d for d in backend.documents.values() if d["name"].startswith(store_name + "/documents/")]
                    self._send_json(200, {"documents": docs})
                    return

                if path.startswith("/v1beta/fileSearchStores/") and "/operations/" in path:
                    op_name = path[len("/v1beta/") :]
                    op = backend.operations.get(op_name)
                    if not op:
                        self._send_json(404, {"error": {"code": 404, "message": "Operation not found"}})
                        return
                    polls = op.pop("_remaining_polls", 0)
                    if polls > 0:
                        op["_remaining_polls"] = polls - 1
                        resp_op = {"name": op_name, "done": False}
                    else:
                        op["done"] = True
                        resp_op = {k: v for k, v in op.items() if not k.startswith("_")}
                    self._send_json(200, resp_op)
                    return

                if path.startswith("/v1beta/fileSearchStores/") and "/documents/" in path:
                    doc_name = path[len("/v1beta/") :]
                    doc = backend.documents.get(doc_name)
                    if not doc:
                        self._send_json(404, {"error": {"code": 404, "message": "Document not found"}})
                        return
                    self._send_json(200, doc)
                    return

                if path.startswith("/v1beta/fileSearchStores/"):
                    store_name = path[len("/v1beta/") :]
                    st = backend.stores.get(store_name)
                    if not st:
                        self._send_json(404, {"error": {"code": 404, "message": "Store not found"}})
                        return
                    self._send_json(200, st)
                    return

            self._send_json(404, {"error": {"code": 404, "message": f"Unhandled GET {path}"}})

        def do_DELETE(self) -> None:  # noqa: N802
            parsed = urllib.parse.urlsplit(self.path)
            path = parsed.path
            api_key = self.headers.get("x-goog-api-key", "")
            with backend.lock:
                backend.api_keys_seen.append(api_key)
                backend.file_search_requests.append({"method": "DELETE", "path": path, "query": parsed.query})
                if "/documents/" in path and path.startswith("/v1beta/fileSearchStores/"):
                    doc_name = path[len("/v1beta/") :]
                    if doc_name not in backend.documents:
                        self._send_json(404, {"error": {"code": 404, "message": "Document not found"}})
                        return
                    del backend.documents[doc_name]
                    self._send_json(200, {})
                    return
                if path.startswith("/v1beta/fileSearchStores/"):
                    store_name = path[len("/v1beta/") :]
                    if store_name not in backend.stores:
                        self._send_json(404, {"error": {"code": 404, "message": "Store not found"}})
                        return
                    del backend.stores[store_name]
                    for k in [dk for dk in backend.documents if dk.startswith(store_name + "/documents/")]:
                        del backend.documents[k]
                    self._send_json(200, {})
                    return
            self._send_json(404, {"error": {"code": 404, "message": f"Unhandled DELETE {path}"}})

        def do_POST(self) -> None:  # noqa: N802
            parsed = urllib.parse.urlsplit(self.path)
            path = parsed.path
            raw_body = self._read_body()
            api_key = self.headers.get("x-goog-api-key", "")

            if path == "/v1beta/interactions":
                req_json = json.loads(raw_body.decode("utf-8"))
                with backend.lock:
                    backend.api_keys_seen.append(api_key)
                    backend.interaction_revisions_seen.append(self.headers.get("Api-Revision", ""))
                    backend.interaction_requests.append(req_json)
                    tools = req_json.get("tools", [])
                    has_builtin = any(t.get("type") in ("google_search", "file_search") for t in tools)
                    has_func = any(t.get("type") == "function" for t in tools)
                    if backend.fail_combined_tools and has_builtin and has_func:
                        self._send_json(
                            400,
                            {
                                "error": {
                                    "code": 400,
                                    "message": "Built-in google_search/file_search tool combination alongside function tools is not supported",
                                }
                            },
                        )
                        return
                    handler = backend.scripted_handler
                if handler is not None:
                    code, resp_obj = handler(req_json)
                    self._send_json(code, resp_obj)
                    return
                self._send_json(
                    200,
                    {
                        "id": backend.next_id("int"),
                        "status": "completed",
                        "steps": [
                            {
                                "type": "model_output",
                                "content": [{"type": "text", "text": "Mock response"}],
                            }
                        ],
                    },
                )
                return

            if path == "/v1beta/fileSearchStores":
                req_json = json.loads(raw_body.decode("utf-8"))
                sid = backend.next_id("store")
                store_name = f"fileSearchStores/{sid}"
                store_obj = {"name": store_name, "displayName": req_json.get("displayName", sid)}
                if "embeddingModel" in req_json:
                    store_obj["embeddingModel"] = req_json["embeddingModel"]
                with backend.lock:
                    backend.api_keys_seen.append(api_key)
                    backend.stores[store_name] = store_obj
                    backend.file_search_requests.append({"method": "POST", "path": path, "body": req_json})
                self._send_json(200, store_obj)
                return

            if path.startswith("/upload/v1beta/fileSearchStores/") and path.endswith(":uploadToFileSearchStore"):
                store_name = path[len("/upload/v1beta/") : -len(":uploadToFileSearchStore")]
                meta = json.loads(raw_body.decode("utf-8"))
                up_id = backend.next_id("upload")
                with backend.lock:
                    backend.api_keys_seen.append(api_key)
                    backend.upload_sessions[up_id] = {
                        "store_name": store_name,
                        "meta": meta,
                        "headers": dict(self.headers.items()),
                    }
                    backend.file_search_requests.append(
                        {"method": "POST", "path": path, "meta": meta, "protocol": self.headers.get("X-Goog-Upload-Protocol")}
                    )
                host, port = self.server.server_address[:2]
                upload_url = f"http://{host}:{port}/upload/session/{up_id}"
                self._send_json(200, {}, headers={"X-Goog-Upload-URL": upload_url})
                return

            if path.startswith("/upload/session/"):
                up_id = path[len("/upload/session/") :]
                with backend.lock:
                    sess = backend.upload_sessions.get(up_id)
                    if not sess:
                        self._send_json(404, {"error": {"code": 404, "message": "Upload session not found"}})
                        return
                    store_name = sess["store_name"]
                    meta = sess["meta"]
                    doc_id = backend.next_id("doc")
                    op_id = backend.next_id("op")
                    doc_name = f"{store_name}/documents/{doc_id}"
                    op_name = f"{store_name}/upload/operations/{op_id}"
                    backend.documents[doc_name] = {
                        "name": doc_name,
                        "displayName": meta.get("displayName", doc_id),
                        "customMetadata": meta.get("customMetadata", []),
                        "chunkingConfig": meta.get("chunkingConfig"),
                        "sizeBytes": len(raw_body),
                    }
                    backend.operations[op_name] = {
                        "name": op_name,
                        "done": False,
                        "_remaining_polls": 1,
                        "response": {"documentName": doc_name},
                    }
                    backend.file_search_requests.append(
                        {"method": "POST", "path": path, "bytes": len(raw_body), "doc": doc_name}
                    )
                self._send_json(200, {"name": op_name, "done": False})
                return

            self._send_json(404, {"error": {"code": 404, "message": f"Unhandled POST {path}"}})

    srv = ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    return srv


class WorkspaceAndAssistantTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="tigervnc_ws_test_")

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_tool_schemas_and_build_tools_config(self) -> None:
        forbidden = {"default", "anyOf", "oneOf", "allOf", "additionalProperties"}
        allowed_types = {"string", "integer", "boolean"}
        groups = [
            WORKSPACE_FUNCTION_DECLARATIONS,
            REMOTE_TRANSFER_FUNCTION_DECLARATIONS,
            COORDINATION_FUNCTION_DECLARATIONS,
            ASSISTANT_FILE_SEARCH_DECLARATIONS,
        ]
        for group in groups:
            for decl in group:
                self.assertEqual(decl["type"], "function")
                params = decl["parameters"]
                self.assertEqual(params["type"], "object")
                self.assertFalse(forbidden.intersection(params.keys()), decl["name"])
                for pname, pdef in params["properties"].items():
                    self.assertIn(pdef["type"], allowed_types, f"{decl['name']}.{pname}")
                    self.assertFalse(forbidden.intersection(pdef.keys()), f"{decl['name']}.{pname}")
                for req in params.get("required", []):
                    self.assertIn(req, params["properties"], f"{decl['name']} missing required {req}")

        default_tools = agent_mod.build_tools_config(
            strict_safety=False,
            excluded_actions=[],
            enable_custom_tools=True,
            enable_file_tools=False,
        )
        default_names = {t["name"] for t in default_tools if t.get("type") == "function"}
        self.assertEqual(default_tools[0]["environment"], "desktop")
        self.assertNotIn("excluded_predefined_functions", default_tools[0])
        self.assertNotIn("workspace_read", default_names)
        self.assertNotIn("peer_send_message", default_names)

        full_tools = agent_mod.build_tools_config(
            strict_safety=False,
            excluded_actions=[],
            enable_custom_tools=True,
            enable_file_tools=True,
            enable_workspace_tools=True,
            enable_remote_transfer_tools=True,
            enable_coord_tools=True,
        )
        full_names = {t["name"] for t in full_tools if t.get("type") == "function"}
        for expected in ("workspace_read", "memory_write", "workspace_download_from_remote", "peer_send_message"):
            self.assertIn(expected, full_names)

    def test_local_workspace_security_crud_and_memory(self) -> None:
        ws_dir = os.path.join(self.tmp, "ws")
        ws = LocalWorkspace(ws_dir)

        # Security: traversal, outside absolute path, symlink escape, internal dir write protection
        with self.assertRaises(PermissionError):
            ws.resolve_path("../outside.txt")
        with self.assertRaises(PermissionError):
            ws.resolve_path("/etc/passwd")
        with self.assertRaises(PermissionError):
            ws.resolve_path(".coord/hack.marker", allow_internal=False)
        with self.assertRaises(PermissionError):
            ws.write_file(".coord/hack.txt", content="bad")

        outside_file = os.path.join(self.tmp, "secret.txt")
        with open(outside_file, "w", encoding="utf-8") as f:
            f.write("secret")
        symlink_path = os.path.join(ws.root, "escape_link")
        os.symlink(outside_file, symlink_path)
        with self.assertRaises(PermissionError):
            ws.read_file("escape_link")
        os.symlink("missing.txt", os.path.join(ws.root, "dangling_link"))
        self.assertEqual(ws.search_files("secret")["matches"], [])
        for recursive in (False, True):
            names = {entry["name"] for entry in ws.list_dir(recursive=recursive)["entries"]}
            self.assertTrue({"escape_link", "dangling_link"}.isdisjoint(names))

        # CRUD + chunked + base64 + search
        wr = ws.write_file("notes/todo.md", content="Line 1: alpha\nLine 2: beta\n", create_parents=True)
        self.assertEqual(wr["mode"], "overwrite")
        ws.write_file("notes/todo.md", content="Line 3: gamma\n", append=True)
        rd = ws.read_file("notes/todo.md")
        self.assertIn("Line 3: gamma", rd["content"])

        blob = bytes((i * 19 + 3) % 256 for i in range(4096))
        ws.write_file("bin/data.bin", content_base64=base64.b64encode(blob).decode("ascii"))
        rd_c1 = ws.read_file("bin/data.bin", as_base64=True, offset=0, max_bytes=1024)
        self.assertTrue(rd_c1["truncated"])
        self.assertEqual(base64.b64decode(rd_c1["content_base64"]), blob[:1024])
        rd_c2 = ws.read_file("bin/data.bin", as_base64=True, offset=1024, max_bytes=4096)
        self.assertTrue(rd_c2["eof"])
        self.assertEqual(base64.b64decode(rd_c2["content_base64"]), blob[1024:])

        listing_root = ws.list_dir(".")
        root_names = {e["name"] for e in listing_root["entries"]}
        self.assertNotIn(".coord", root_names)
        self.assertNotIn(".agent", root_names)
        self.assertIn("notes", root_names)

        search_res = ws.search_files("gamma", glob_pattern="*.md", max_matches=1)
        self.assertEqual(search_res["total_matches"], 1)
        self.assertFalse(search_res["truncated"])
        self.assertEqual(search_res["matches"][0]["path"], "notes/todo.md")

        ws.rename("notes/todo.md", "notes/done.md")
        self.assertFalse(ws.stat("notes/todo.md")["exists"])
        self.assertTrue(ws.stat("notes/done.md")["exists"])
        ws.delete("notes", recursive=True)
        self.assertFalse(ws.stat("notes")["exists"])

        # Memory CRUD + reload persistence
        ws.memory_write("MEMORY.md", "## Findings\n- TigerVNC FTP works.\n", append=True)
        ws.memory_write("api_endpoint", "https://example.invalid/v1", tags="config,api", author=AGENT_ASSISTANT)
        ws.memory_write("api_endpoint", "\nhttps://example.invalid/v2", tags="v2", append=True)

        ws2 = LocalWorkspace(ws_dir)
        mem_all = ws2.memory_read()
        self.assertIn("TigerVNC FTP works.", mem_all["memory_md"])
        mem_key = ws2.memory_read("api_endpoint")
        self.assertTrue(mem_key["exists"])
        self.assertEqual(mem_key["tags"], ["config", "api", "v2"])
        self.assertIn("v2", mem_key["value"])
        self.assertEqual(len(ws2.memory_list(tag="api")["entries"]), 1)
        self.assertGreaterEqual(len(ws2.memory_search("TigerVNC")["matches"]), 1)
        self.assertTrue(ws2.memory_delete("api_endpoint")["deleted"])

    def test_invalid_memory_state_is_preserved(self) -> None:
        ws = LocalWorkspace(os.path.join(self.tmp, "ws"))
        invalid_index = b"incomplete JSON"
        with open(ws.memory_entries_path, "wb") as index:
            index.write(invalid_index)
        with self.assertRaisesRegex(ValueError, "Invalid workspace memory index"):
            ws.memory_write("key", "value")
        with open(ws.memory_entries_path, "rb") as index:
            self.assertEqual(index.read(), invalid_index)

        linked_workspace = os.path.join(self.tmp, "linked-workspace")
        os.mkdir(linked_workspace)
        os.symlink(ws.memory_dir, os.path.join(linked_workspace, "memory"))
        with self.assertRaises(PermissionError):
            LocalWorkspace(linked_workspace)

    def test_remote_workspace_transfers_and_pause(self) -> None:
        ws = LocalWorkspace(os.path.join(self.tmp, "ws"))
        srv = MockFtpServer()
        try:
            ftp_client = agent_mod.VncFtpClient("127.0.0.1", srv.port, timeout=5.0)
            fake_agent = FakeAgentClient()
            executor = agent_mod.VncDesktopExecutor(fake_agent, ftp_client=ftp_client, workspace=ws)  # type: ignore[arg-type]
            try:
                big_blob = bytes((i * 29 + 5) & 0xFF for i in range(180 * 1024))
                expected_sha = hashlib.sha256(big_blob).hexdigest()
                with srv.lock:
                    srv.files["/C:/Temp/remote_big.bin"] = big_blob

                dl_res = executor.execute(
                    "workspace_download_from_remote",
                    {"remote_path": "/C:/Temp/remote_big.bin", "local_path": "downloads/big.bin"},
                )
                self.assertEqual(dl_res["bytes_transferred"], len(big_blob))
                self.assertEqual(dl_res["sha256"], expected_sha)

                with self.assertRaises(FileExistsError):
                    ws.download_from_remote(ftp_client, "/C:/Temp/remote_big.bin", "downloads/big.bin", overwrite=False)

                late_file = os.path.join(ws.root, "downloads", "late.txt")
                original_stat = ftp_client.stat

                def create_destination_during_transfer(remote_path: str) -> Dict[str, Any]:
                    with open(late_file, "w", encoding="utf-8") as f:
                        f.write("keep this file")
                    return original_stat(remote_path)

                with mock.patch.object(ftp_client, "stat", side_effect=create_destination_during_transfer):
                    with self.assertRaises(FileExistsError):
                        ws.download_from_remote(ftp_client, "/C:/Temp/hello.txt", "downloads/late.txt", overwrite=False)
                self.assertEqual(ws.read_file("downloads/late.txt")["content"], "keep this file")
                self.assertFalse(any(name.startswith(".dl_") for name in os.listdir(os.path.dirname(late_file))))

                up_res = executor.execute(
                    "workspace_upload_to_remote",
                    {
                        "local_path": "downloads/big.bin",
                        "remote_path": "/C:/Temp/uploaded_dir/out.bin",
                        "create_parents": True,
                    },
                )
                self.assertEqual(up_res["bytes_transferred"], len(big_blob))
                self.assertEqual(up_res["sha256"], expected_sha)
                with srv.lock:
                    self.assertEqual(srv.files["/C:/Temp/uploaded_dir/out.bin"], big_blob)

                # Pause enforcement: download allowed, upload raises AgentPausedError
                fake_agent.agent_paused = True
                dl2 = executor.execute(
                    "workspace_download_from_remote",
                    {"remote_path": "/C:/Temp/hello.txt", "local_path": "downloads/hello.txt"},
                )
                self.assertEqual(dl2["status"], "ok")
                with self.assertRaises(agent_mod.AgentPausedError):
                    executor.execute(
                        "workspace_upload_to_remote",
                        {"local_path": "downloads/hello.txt", "remote_path": "/C:/Temp/hello2.txt"},
                    )

                # ViewOnly enforcement
                fake_agent.agent_paused = False
                fake_agent.view_only = True
                vo_res = executor.execute(
                    "workspace_upload_to_remote",
                    {"local_path": "downloads/hello.txt", "remote_path": "/C:/Temp/hello2.txt"},
                )
                self.assertIn("error", vo_res)
            finally:
                ftp_client.close()
        finally:
            srv.stop()

    def test_coordinator_markers_and_async_wake(self) -> None:
        ws = LocalWorkspace(os.path.join(self.tmp, "ws"))
        coord = WorkspaceCoordinator(ws)

        self.assertTrue(os.path.exists(os.path.join(ws.coord_dir, "PROTOCOL.md")))
        self.assertTrue(os.path.exists(os.path.join(ws.coord_dir, "STATUS.computer_use.marker")))
        self.assertTrue(os.path.exists(os.path.join(ws.coord_dir, "STATUS.assistant.marker")))

        wake_result: Dict[str, Any] = {}

        def _waiter() -> None:
            wake_result.update(coord.wait_for_wake(AGENT_ASSISTANT, timeout=5.0))

        t = threading.Thread(target=_waiter)
        t.start()
        time.sleep(0.1)

        sent = coord.send_message(
            sender=AGENT_COMPUTER_USE,
            recipient=AGENT_ASSISTANT,
            subject="Search release notes",
            body="Find latest info",
            attachments="notes/a.txt,notes/b.txt",
            wake=True,
        )
        t.join(timeout=3.0)
        self.assertTrue(wake_result.get("woken"))
        self.assertTrue(os.path.exists(os.path.join(ws.coord_dir, "WAKE.assistant.marker")))
        self.assertTrue(os.path.exists(os.path.join(ws.root, sent["marker_file"])))

        inbox = coord.read_messages(AGENT_ASSISTANT, mark_read=True)
        self.assertEqual(len(inbox["messages"]), 1)
        self.assertEqual(inbox["messages"][0]["attachments"], ["notes/a.txt", "notes/b.txt"])
        self.assertFalse(os.path.exists(os.path.join(ws.coord_dir, "WAKE.assistant.marker")))
        self.assertEqual(coord.unread_count(AGENT_ASSISTANT), 0)

        arch = coord.read_messages(AGENT_ASSISTANT, include_read=True, mark_read=False)
        self.assertEqual(len(arch["archived_messages"]), 1)

    def test_inbox_only_messages_do_not_start_background_research(self) -> None:
        ws = LocalWorkspace(os.path.join(self.tmp, "ws"))
        coord = WorkspaceCoordinator(ws)
        coord.send_message(AGENT_COMPUTER_USE, AGENT_ASSISTANT, "Queued", "Wait for the wake signal", wake=False)
        self.assertTrue(coord.wait_for_wake(AGENT_ASSISTANT, timeout=0)["woken"])
        coord = WorkspaceCoordinator(ws)
        assistant = ResearchAssistantWorker(
            "test-key", ws, coord, FileSearchClient("test-key"), FileSearchStoreRegistry(ws), max_turns=1,
        )
        self.assertTrue(coord.wait_for_wake(AGENT_ASSISTANT, timeout=0, include_inbox=False)["timed_out"])

        coord.wake_peer(AGENT_COMPUTER_USE, AGENT_ASSISTANT, "Start now")
        replies = [
            {"id": "turn-1", "steps": [{"type": "function_call", "id": "call-1", "name": "workspace_list"}]},
            {"id": "turn-2", "steps": [{"type": "model_output", "content": [{"type": "text", "text": "Research complete"}]}]},
        ]
        with mock.patch.object(assistant, "_create_interaction", side_effect=replies) as interaction:
            assistant._process_wakeup()
        self.assertEqual(interaction.call_count, 2)
        self.assertIn("Start now", interaction.call_args_list[0].kwargs["input_payload"][0]["text"])
        messages = coord.read_messages(AGENT_COMPUTER_USE)["messages"]
        self.assertEqual([message["body"] for message in messages], ["Research complete"])

    def test_file_search_client_and_registry(self) -> None:
        backend = MockGeminiBackend()
        srv = start_mock_gemini_server(backend)
        try:
            host, port = srv.server_address[:2]
            api_base = f"http://{host}:{port}/v1beta"
            upload_base = f"http://{host}:{port}/upload/v1beta"
            ws = LocalWorkspace(os.path.join(self.tmp, "ws"))
            ws.write_file("docs/manual.md", content="# TigerVNC Manual\nAgentSocket docs.\n")
            abs_manual, _ = ws.resolve_path("docs/manual.md")

            client = FileSearchClient(api_key="test-key-123", api_base=api_base, upload_base=upload_base)
            registry = FileSearchStoreRegistry(ws)
            assistant = ResearchAssistantWorker("test-key-123", ws, WorkspaceCoordinator(ws), client, registry)
            failed_attach = assistant._execute_tool("file_search_attach_store", {"store_name": "unavailable"})
            self.assertIn("error", failed_attach)
            self.assertEqual(registry.list_attached(), [])

            store = client.create_store("TigerVNC Docs", embedding_model="models/gemini-embedding-001")
            store_name = store["name"]
            registry.register_store(store_name, display_name=store["displayName"], set_default=True)
            self.assertEqual(registry.get_default_store(), store_name)
            native_search = [tool for tool in assistant._build_tools() if tool["type"] != "function"]
            self.assertEqual(native_search, [{"type": "file_search", "file_search_store_names": [store_name]}])

            op = client.upload_file(
                store_name=store_name,
                local_abs_path=abs_manual,
                display_name="manual.md",
                custom_metadata=[{"key": "topic", "stringValue": "tigervnc"}],
                chunking_config={"whiteSpaceConfig": {"maxTokensPerChunk": 200, "maxOverlapTokens": 20}},
                wait=True,
                poll_interval=0.05,
            )
            self.assertTrue(op["done"])
            self.assertIn("/upload/operations/", op["name"])

            docs = client.list_documents(store_name)
            self.assertEqual(len(docs["documents"]), 1)
            doc_name = docs["documents"][0]["name"]
            self.assertEqual(docs["documents"][0]["displayName"], "manual.md")
            self.assertEqual(client.get_document(doc_name)["name"], doc_name)

            client.delete_document(doc_name, force=True)
            self.assertEqual(client.list_documents(store_name)["documents"], [])

            client.delete_store(store_name, force=True)
            registry.remove_store(store_name)
            self.assertEqual(registry.list_attached(), [])
            self.assertTrue(all(k == "test-key-123" for k in backend.api_keys_seen))
        finally:
            srv.shutdown()
            srv.server_close()

    def test_end_to_end_async_multi_agent_and_tool_fallback(self) -> None:
        backend = MockGeminiBackend(fail_combined_tools=True)
        turn_counter = {"n": 0}

        def _scripted(req: Dict[str, Any]) -> Any:
            tools = req.get("tools", [])
            # Dedicated web_search_query / file_search_query call
            if len(tools) == 1 and tools[0].get("type") in ("google_search", "file_search"):
                t_type = tools[0]["type"]
                if t_type == "google_search":
                    return (
                        200,
                        {
                            "id": backend.next_id("web"),
                            "status": "completed",
                            "steps": [
                                {
                                    "type": "model_output",
                                    "content": [
                                        {
                                            "type": "text",
                                            "text": "Web result: TigerVNC uses RFB protocol.",
                                            "annotations": [{"type": "url_citation", "url": "https://tigervnc.org"}],
                                        }
                                    ],
                                }
                            ],
                        },
                    )
                return (
                    200,
                    {
                        "id": backend.next_id("fs"),
                        "status": "completed",
                        "steps": [
                            {
                                "type": "model_output",
                                "content": [
                                    {
                                        "type": "text",
                                        "text": "Indexed doc states: target port is 5901.",
                                        "annotations": [{"type": "file_citation", "filename": "remote_notes.txt"}],
                                    }
                                ],
                            }
                        ],
                    },
                )

            turn_counter["n"] += 1
            step_num = turn_counter["n"]
            if step_num == 1:
                return (
                    200,
                    {
                        "id": backend.next_id("asst"),
                        "status": "requires_action",
                        "steps": [
                            {
                                "type": "function_call",
                                "id": "c1",
                                "name": "file_search_upload_file",
                                "arguments": {"local_path": "inbox/remote_notes.txt"},
                            },
                            {
                                "type": "function_call",
                                "id": "c2",
                                "name": "web_search_query",
                                "arguments": {"query": "TigerVNC RFB protocol"},
                            },
                        ],
                    },
                )
            if step_num == 2:
                return (
                    200,
                    {
                        "id": backend.next_id("asst"),
                        "status": "requires_action",
                        "steps": [
                            {
                                "type": "function_call",
                                "id": "c3",
                                "name": "file_search_query",
                                "arguments": {"query": "What port is in remote_notes.txt?"},
                            },
                            {
                                "type": "function_call",
                                "id": "c4",
                                "name": "workspace_write",
                                "arguments": {
                                    "path": "reports/summary.md",
                                    "content": "# Report\nPort: 5901\nRef: https://tigervnc.org\n",
                                },
                            },
                            {
                                "type": "function_call",
                                "id": "c5",
                                "name": "memory_write",
                                "arguments": {"key": "target_port", "value": "5901", "tags": "vnc"},
                            },
                        ],
                    },
                )
            return (
                200,
                {
                    "id": backend.next_id("asst"),
                    "status": "requires_action" if step_num == 3 else "completed",
                    "steps": (
                        [
                            {
                                "type": "function_call",
                                "id": "c6",
                                "name": "peer_send_message",
                                "arguments": {
                                    "subject": "Research complete",
                                    "body": "Wrote reports/summary.md and stored target_port=5901.",
                                    "kind": "response",
                                    "attachments": "reports/summary.md",
                                    "wake": True,
                                },
                            }
                        ]
                        if step_num == 3
                        else [
                            {
                                "type": "model_output",
                                "content": [{"type": "text", "text": "Done and notified computer_use."}],
                            }
                        ]
                    ),
                },
            )

        backend.scripted_handler = _scripted
        srv = start_mock_gemini_server(backend)
        ftp_srv = MockFtpServer()
        try:
            with ftp_srv.lock:
                ftp_srv.files["/C:/Temp/remote_notes.txt"] = b"Target VNC port is 5901.\n"

            host, port = srv.server_address[:2]
            api_base = f"http://{host}:{port}/v1beta"
            upload_base = f"http://{host}:{port}/upload/v1beta"
            interactions_url = f"{api_base}/interactions"

            ws = LocalWorkspace(os.path.join(self.tmp, "ws"))
            coord = WorkspaceCoordinator(ws)
            fs_client = FileSearchClient(api_key="test-key", api_base=api_base, upload_base=upload_base)
            registry = FileSearchStoreRegistry(ws)

            assistant = ResearchAssistantWorker(
                api_key="test-key",
                workspace=ws,
                coordinator=coord,
                file_search_client=fs_client,
                store_registry=registry,
                interactions_url=interactions_url,
                log_dir=self.tmp,
                poll_interval=0.05,
            )
            assistant.start()

            ftp_client = agent_mod.VncFtpClient("127.0.0.1", ftp_srv.port, timeout=5.0)
            fake_agent = FakeAgentClient()
            executor = agent_mod.VncDesktopExecutor(
                fake_agent,  # type: ignore[arg-type]
                ftp_client=ftp_client,
                workspace=ws,
                coordinator=coord,
            )
            try:
                # 1. Computer Use downloads file from remote host into shared local workspace
                dl = executor.execute(
                    "workspace_download_from_remote",
                    {"remote_path": "/C:/Temp/remote_notes.txt", "local_path": "inbox/remote_notes.txt"},
                )
                self.assertEqual(dl["status"], "ok")

                # 2. Computer Use sends async message + wakes assistant, then sleeps in peer_wait_for_message
                executor.execute(
                    "peer_send_message",
                    {
                        "subject": "Index and research remote_notes.txt",
                        "body": "Upload inbox/remote_notes.txt to File Search, search web, and summarize.",
                        "attachments": "inbox/remote_notes.txt",
                        "wake": True,
                    },
                )
                wait_res = executor.execute("peer_wait_for_message", {"timeout_seconds": 10})
                self.assertEqual(wait_res["status"], "woken")
                self.assertEqual(len(wait_res["messages"]), 1)
                self.assertEqual(wait_res["messages"][0]["subject"], "Research complete")
                self.assertEqual(wait_res["messages"][0]["attachments"], ["reports/summary.md"])

                # 3. Verify assistant created workspace report, shared memory, File Search store, and fell back cleanly
                self.assertFalse(assistant._use_native_builtin_tools)
                self.assertEqual(ws.memory_read("target_port")["value"], "5901")
                self.assertIn("Port: 5901", ws.read_file("reports/summary.md")["content"])
                self.assertEqual(len(registry.list_attached()), 1)

                # 4. Verify wait_for_next_instruction wakes up when assistant sends a follow-up wake marker
                def _late_wake() -> None:
                    time.sleep(0.15)
                    coord.send_message(
                        sender=AGENT_ASSISTANT,
                        recipient=AGENT_COMPUTER_USE,
                        subject="Follow-up alert",
                        body="New update ready",
                        kind="update",
                        wake=True,
                    )

                wt = threading.Thread(target=_late_wake)
                wt.start()
                nxt = executor.wait_for_next_instruction(poll_interval=0.05, use_stdin=False)
                wt.join(timeout=2.0)
                self.assertIsNotNone(nxt)
                self.assertIn("Follow-up alert", nxt[0])
            finally:
                assistant.stop(timeout=3.0)
                ftp_client.close()
        finally:
            ftp_srv.stop()
            srv.shutdown()
            srv.server_close()

    def test_run_agent_batches(self) -> None:
        key_file = os.path.join(self.tmp, "gemini.key")
        with open(key_file, "w", encoding="utf-8") as f:
            f.write("test-key")
        log_dir = os.path.join(self.tmp, "logs")
        ws_dir = os.path.join(self.tmp, "ws")

        overlay_calls: List[Dict[str, Any]] = []
        pointer_calls: List[Dict[str, Any]] = []
        key_calls: List[Dict[str, Any]] = []
        inputs: List[Any] = []
        interaction_configs: List[Dict[str, Any]] = []

        class _StubAgentClient:
            def __init__(self, socket_path: str, timeout: float = 15.0) -> None:
                self.frame_seq = 1
                self.damage_count = 1

            def __enter__(self) -> "_StubAgentClient":
                return self

            def __exit__(self, exc_type: Any, exc: Any, tb: Any) -> None:
                return None

            def request(self, cmd: str, payload: bytes = b"", **fields: Any) -> Any:
                if cmd == "GET_STATUS":
                    return (
                        {
                            "ok": True,
                            "connected": True,
                            "ready": True,
                            "width": 1000,
                            "height": 500,
                            "desktop_name": "stub",
                            "frame_seq": self.frame_seq,
                            "damage_count": self.damage_count,
                            "resize_count": 0,
                            "updating": False,
                            "has_clipboard": False,
                            "view_only": False,
                            "agent_paused": False,
                            "pending_user_messages": 0,
                        },
                        b"",
                    )
                if cmd == "GET_FRAME":
                    return ({"ok": True, "changed": True, "width": 1000, "height": 500}, b"\x00" * (1000 * 500 * 3))
                if cmd == "AGENT_OVERLAY":
                    overlay_calls.append(dict(fields))
                    return ({"ok": True}, b"")
                if cmd == "POINTER_EVENT":
                    pointer_calls.append(dict(fields))
                    self.frame_seq += 1
                    self.damage_count += 1
                    return ({"ok": True}, b"")
                if cmd == "KEY_EVENT":
                    key_calls.append(dict(fields))
                    return ({"ok": True}, b"")
                if cmd == "POLL_USER_MESSAGES":
                    return ({"ok": True, "messages": [], "human_action_count": 0, "agent_paused": False}, b"")
                return ({"ok": True}, b"")

        responses = [
            {
                "id": "int_1",
                "status": "requires_action",
                "steps": [
                    {
                        "type": "function_call",
                        "id": "call_1",
                        "name": "click",
                        "arguments": {"x": 500, "y": 250, "intent": "Click center-ish"},
                    },
                    {
                        "type": "function_call",
                        "id": "call_2",
                        "name": "drag_and_drop",
                        "arguments": {
                            "start_x": 100,
                            "start_y": 200,
                            "end_x": 400,
                            "end_y": 600,
                            "intent": "Drag widget",
                        },
                    },
                ],
            },
            {
                "id": "int_2",
                "status": "completed",
                "steps": [
                    {
                        "type": "model_output",
                        "content": [{"type": "text", "text": "Clicked and dragged."}],
                    }
                ],
            },
        ]

        class _StubInteractionsClient:
            def __init__(self, *args: Any, **kwargs: Any) -> None:
                self._idx = 0
                interaction_configs.append(kwargs)

            def create(self, *args: Any, **kwargs: Any) -> Dict[str, Any]:
                inputs.append(kwargs["input_payload"])
                resp = responses[self._idx]
                self._idx += 1
                return resp

        ns = argparse.Namespace(
            prompt="Click and drag",
            socket=os.path.join(self.tmp, "stub.sock"),
            vnc_host="localhost:5900",
            vncviewer_bin="vncviewer",
            api_key_file=key_file,
            model="gemini-4-hydrogen",
            max_turns=1,
            log_dir=log_dir,
            workspace=ws_dir,
            assistant_model="gemini-2.5-flash",
            assistant_thinking_level="default",
            assistant_max_turns=4,
            no_assistant=True,
            no_web_search=True,
            file_search_store=None,
            thinking_level="low",
            recovery_thinking_level="high",
            exclude_actions=None,
            no_custom_tools=False,
            no_system_one=True,
            auto_confirm=False,
            strict_safety=False,
            windowed=False,
            collaborate=False,
            one_shot=True,
            action_delay=0.0,
            ftp=False,
            no_ftp=True,
            ftp_url=None,
            ftp_port=0,
            ftp_bind_address="127.0.0.1",
            ftp_username="",
            ftp_password="",
            ftp_protocol="Auto",
            ftp_timeout=5.0,
        )

        with mock.patch("vnc_agent.runner.AgentClient", _StubAgentClient), mock.patch(
            "vnc_agent.runner.InteractionsClient", _StubInteractionsClient
        ):
            rc = agent_mod.run_agent(ns)

        self.assertEqual(rc, 0)
        acting = [c for c in overlay_calls if c.get("state") == "acting"]
        self.assertEqual(len(acting), 2)
        self.assertEqual((acting[0]["action"], acting[0]["x"], acting[0]["y"]), ("click", 500, 125))
        self.assertEqual(
            (acting[1]["action"], acting[1]["x"], acting[1]["y"], acting[1]["end_x"], acting[1]["end_y"]),
            ("drag_and_drop", 100, 100, 400, 300),
        )
        self.assertGreater(len(pointer_calls), 0)

        responses[0]["steps"] = [
            {"type": "function_call", "id": "paste", "name": "paste_text_at",
             "arguments": {"x": 100, "y": 100, "text": "日本語", "press_enter": True}},
            {"type": "function_call", "id": "submit", "name": "press_key",
             "arguments": {"key": "Enter"}},
        ]
        with mock.patch("vnc_agent.runner.AgentClient", _StubAgentClient), mock.patch(
            "vnc_agent.runner.InteractionsClient", _StubInteractionsClient
        ):
            self.assertEqual(agent_mod.run_agent(ns), 0)
        paste, submit = inputs[-1]
        self.assertEqual(paste["call_id"], "paste")
        self.assertEqual(paste["result"][1]["type"], "image")
        self.assertFalse(json.loads(paste["result"][0]["text"])["press_enter"])
        self.assertEqual(submit["call_id"], "submit")
        self.assertEqual(json.loads(submit["result"][0]["text"])["status"], "skipped_pending_visual_verification")
        self.assertNotIn(0xFF0D, [key["keysym"] for key in key_calls])

        for environment, calls, expected_exclusions, expected_keys in (
            (
                "browser",
                [("navigate", {"url": "https://example.com"}), ("go_back", {}), ("go_forward", {})],
                ["navigate"],
                [0x1008FF26, 0x1008FF27],
            ),
            (
                "mobile",
                [("open_app", {"app_name": "Clock"}), ("list_apps", {}), ("go_back", {}),
                 ("long_press", {"x": 250, "y": 500, "seconds": 0.1})],
                ["open_app", "list_apps"],
                [0x1008FF26],
            ),
        ):
            with self.subTest(environment=environment):
                ns.environment = environment
                ns.exclude_actions = [",".join(["press_key", expected_exclusions[0]]), "press_key"]
                pointer_calls.clear()
                key_calls.clear()
                calls = [("press_key", {"key": "Enter"}), *calls]
                responses[0]["steps"] = [
                    {"type": "function_call", "id": name, "name": name, "arguments": arguments}
                    for name, arguments in calls
                ]
                with mock.patch("vnc_agent.runner.AgentClient", _StubAgentClient), mock.patch(
                    "vnc_agent.runner.InteractionsClient", _StubInteractionsClient
                ):
                    self.assertEqual(agent_mod.run_agent(ns), 0)

                config = interaction_configs[-1]
                self.assertEqual(config["tools"][0]["environment"], environment)
                self.assertEqual(config["tools"][0]["excluded_predefined_functions"], ["press_key", *expected_exclusions])
                self.assertIn("address bar" if environment == "browser" else "app drawer", config["system_instruction"])
                self.assertEqual([result["call_id"] for result in inputs[-1]], [name for name, _ in calls])
                skipped = {"press_key", *expected_exclusions}
                for result in inputs[-1]:
                    status = json.loads(result["result"][0]["text"])["status"]
                    self.assertEqual(status, "skipped" if result["name"] in skipped else "ok")
                self.assertEqual(
                    key_calls,
                    [{"keysym": keysym, "down": down} for keysym in expected_keys for down in (True, False)],
                )
                self.assertEqual(pointer_calls, [
                    {"x": 250, "y": 250, "button_mask": mask} for mask in (0, 1, 0)
                ] if environment == "mobile" else [])

    def test_interactions_reports_failed_response(self) -> None:
        backend = MockGeminiBackend()
        backend.scripted_handler = lambda req: (200, {"status": "failed", "error": {"message": "Request failed"}})
        srv = start_mock_gemini_server(backend)
        try:
            host, port = srv.server_address[:2]
            client = agent_mod.InteractionsClient(
                "test-key", "test-model", [], "", base_url=f"http://{host}:{port}/v1beta/interactions"
            )
            with self.assertRaisesRegex(agent_mod.InteractionsApiError, "Request failed"):
                client.create("Hello")
            self.assertEqual(backend.interaction_revisions_seen, ["2026-05-20"])
        finally:
            srv.shutdown()
            srv.server_close()

    def test_system_instruction_guardrails(self) -> None:
        self.assertIn("Visual Verification Before Form Commit/Close", SYSTEM_INSTRUCTION)
        self.assertIn("Verify exact pasted text before saving", SYSTEM_INSTRUCTION)
        self.assertIn("Iterative Consultation", COORDINATION_SYSTEM_INSTRUCTION)
        self.assertIn("Proactively use", ASSISTANT_SYSTEM_INSTRUCTION)

    def test_non_ascii_paste_attaches_screenshot(self) -> None:
        fake_agent = FakeAgentClient()
        executor = agent_mod.VncDesktopExecutor(fake_agent)  # type: ignore[arg-type]
        executor.width = 1000
        executor.height = 500

        res_ascii = executor.execute("paste_text_at", {"x": 100, "y": 100, "text": "Hello ASCII"})
        self.assertNotIn("_attach_screenshot", res_ascii)

        res_utf8 = executor.execute("paste_text_at", {"x": 100, "y": 100, "text": "令和6年度白色申告"})
        self.assertTrue(res_utf8.get("_attach_screenshot"))
        self.assertFalse(res_utf8["text_verified"])

        res_clip_no_paste = executor.execute("clipboard_set", {"text": "札幌東税務署", "paste": False})
        self.assertNotIn("_attach_screenshot", res_clip_no_paste)

        res_clip_ascii = executor.execute("clipboard_set", {"text": "plain", "paste": True})
        self.assertNotIn("_attach_screenshot", res_clip_ascii)

        res_clip_utf8 = executor.execute("clipboard_set", {"text": "札幌東税務署", "paste": True})
        self.assertTrue(res_clip_utf8.get("_attach_screenshot"))
        self.assertFalse(res_clip_utf8["text_verified"])

    def test_legacy_clipboard_encoding_is_used_only_for_legacy_transfer(self) -> None:
        agent = mock.Mock()
        executor = agent_mod.VncDesktopExecutor(agent, legacy_clipboard_encoding="cp936")
        title = "Café expense reconciliation"
        legacy_text = title.encode("cp936").decode("latin-1")

        for supports_utf8 in (False, True):
            with self.subTest(supports_utf8=supports_utf8):
                agent.request.reset_mock()
                agent.request.side_effect = [({"ok": True, "clipboard_utf8": supports_utf8}, b""), ({"ok": True}, b"")]
                executor.clipboard_set(title)
                agent.request.assert_called_with("CLIPBOARD_SET", text=title if supports_utf8 else legacy_text)

        agent.request.reset_mock()
        agent.request.side_effect = None
        agent.request.return_value = ({"ok": True, "clipboard_utf8": False}, b"")
        with self.assertRaisesRegex(ValueError, "cannot represent"):
            executor.execute("paste_text_at", {"x": 100, "y": 100, "text": "☕", "clear_existing": True})
        self.assertEqual(agent.request.call_args_list, [mock.call("GET_STATUS")])

    def test_clipboard_verification_compares_fresh_copy(self) -> None:
        agent = mock.Mock()
        executor = agent_mod.VncDesktopExecutor(agent, legacy_clipboard_encoding="cp936")
        title = "Café expense reconciliation"

        def legacy_reply(seq: int, text: str) -> Any:
            return ({"ok": True, "available": True, "legacy": True, "sequence": seq,
                     "text": text.encode("cp936").decode("latin-1")}, b"")

        original = legacy_reply(5, title)
        set_clipboard = ({"ok": True}, b"")
        agent.request.side_effect = [original, set_clipboard, original, legacy_reply(6, "Caf?expense reconciliation")]
        with mock.patch.object(executor, "_hotkey_syms") as copy_selection:
            result = executor.execute("clipboard_verify_selection", {"expected_text": title})
        copy_selection.assert_called_once_with([0xFFE3, ord("c")])
        self.assertFalse(result["matches"])
        self.assertIn("differs", result["error"])

        agent.request.side_effect = [original, set_clipboard, legacy_reply(6, "test-marker"), legacy_reply(7, title + "\r\n")]
        with mock.patch.object(executor, "_hotkey_syms"), mock.patch("vnc_agent.executor.uuid.uuid4") as uuid4:
            uuid4.return_value.hex = "test-marker"
            result = executor.clipboard_verify_selection(title + "\n")
        self.assertEqual(result, {"status": "ok", "matches": True})

        agent.request.side_effect = [original, set_clipboard, original]
        with mock.patch.object(executor, "_hotkey_syms"), mock.patch(
            "vnc_agent.executor.time.monotonic", side_effect=[0, 0.1, 1.6]
        ):
            result = executor.clipboard_verify_selection(title)
        self.assertIn("no fresh clipboard update", result["error"])

    def test_text_macros_clear_legacy_filename_field(self) -> None:
        executor = agent_mod.VncDesktopExecutor(FakeAgentClient())  # type: ignore[arg-type]
        executor.width, executor.height = 1024, 768
        for action in ("click_and_type", "paste_text_at"):
            with self.subTest(action=action), mock.patch.object(executor, "_hotkey_syms") as hotkey:
                executor.execute(action, {"x": 430, "y": 688, "text": "report.txt", "clear_existing": True})
                self.assertEqual(hotkey.call_args_list[:3], [
                    mock.call([0xFFE3, 0xFF50]),
                    mock.call([0xFFE3, 0xFFE1, 0xFF57]),
                    mock.call([0xFF08]),
                ])

    def test_extract_server_tool_events_and_citations(self) -> None:
        interaction = {
            "id": "int_ground",
            "steps": [
                {"type": "google_search_call", "id": "gs_1", "queries": ["札幌市白石区 税務署"]},
                {
                    "type": "google_search_result",
                    "id": "gs_1",
                    "results": [{"url": "https://www.nta.go.jp/about/organization/sapporo/", "title": "札幌国税局"}],
                },
                {
                    "type": "model_output",
                    "content": [
                        {
                            "type": "text",
                            "text": "札幌東税務署です。",
                            "annotations": [{"type": "url_citation", "url": "https://www.nta.go.jp"}],
                        }
                    ],
                },
            ],
            "outputs": [
                {"type": "file_search_call", "id": "fs_1", "query": "capital"},
                {"type": "file_search_result", "id": "fs_1", "results": [{"filename": "bs.pdf"}]},
            ],
            "groundingMetadata": {
                "webSearchQueries": ["札幌市白石区 税務署"],
                "groundingChunks": [{"web": {"uri": "https://www.nta.go.jp", "title": "NTA"}}],
            },
        }
        events = extract_server_tool_events(interaction)
        event_types = [e.get("type") for e in events]
        self.assertIn("google_search_call", event_types)
        self.assertIn("google_search_result", event_types)
        self.assertIn("file_search_call", event_types)
        self.assertIn("file_search_result", event_types)
        self.assertIn("grounding_metadata", event_types)

        cits = extract_citations(interaction)
        self.assertEqual(len(cits), 4)
        cit_types = [c.get("type") for c in cits]
        self.assertIn("url_citation", cit_types)
        self.assertIn("google_search_result", cit_types)
        self.assertIn("file_search_result", cit_types)
        self.assertTrue(any(c.get("web", {}).get("uri") == "https://www.nta.go.jp" for c in cits))

    def test_assistant_trace_logs_server_tool_events_and_citations(self) -> None:
        backend = MockGeminiBackend(fail_combined_tools=False)
        step_counter = {"n": 0}

        def _scripted(req: Dict[str, Any]) -> Any:
            step_counter["n"] += 1
            if step_counter["n"] == 1:
                return (
                    200,
                    {
                        "id": backend.next_id("asst"),
                        "status": "requires_action",
                        "steps": [
                            {"type": "google_search_call", "id": "gs_1", "queries": ["白石区 税務署"]},
                            {
                                "type": "google_search_result",
                                "id": "gs_1",
                                "results": [{"url": "https://www.nta.go.jp/higashi"}],
                            },
                            {
                                "type": "function_call",
                                "id": "fc_1",
                                "name": "peer_send_message",
                                "arguments": {
                                    "subject": "Verified tax office",
                                    "body": "札幌東税務署",
                                    "kind": "response",
                                    "wake": True,
                                },
                            },
                        ],
                    },
                )
            return (
                200,
                {
                    "id": backend.next_id("asst"),
                    "status": "completed",
                    "steps": [
                        {
                            "type": "model_output",
                            "content": [
                                {
                                    "type": "text",
                                    "text": "Confirmed 札幌東税務署.",
                                    "annotations": [{"type": "url_citation", "url": "https://www.nta.go.jp/higashi"}],
                                }
                            ],
                        }
                    ],
                },
            )

        backend.scripted_handler = _scripted
        srv = start_mock_gemini_server(backend)
        try:
            host, port = srv.server_address[:2]
            api_base = f"http://{host}:{port}/v1beta"
            upload_base = f"http://{host}:{port}/upload/v1beta"
            ws = LocalWorkspace(os.path.join(self.tmp, "ws"))
            coord = WorkspaceCoordinator(ws)
            fs_client = FileSearchClient(api_key="test-key", api_base=api_base, upload_base=upload_base)
            registry = FileSearchStoreRegistry(ws)

            assistant = ResearchAssistantWorker(
                api_key="test-key",
                workspace=ws,
                coordinator=coord,
                file_search_client=fs_client,
                store_registry=registry,
                interactions_url=f"{api_base}/interactions",
                log_dir=self.tmp,
                poll_interval=0.05,
            )
            assistant.start()
            try:
                coord.send_message(
                    sender=AGENT_COMPUTER_USE,
                    recipient=AGENT_ASSISTANT,
                    subject="Verify tax office",
                    body="Which tax office covers Shiroishi-ku?",
                    wake=True,
                )
                wait_res = coord.wait_for_wake(AGENT_COMPUTER_USE, timeout=5.0)
                self.assertTrue(wait_res.get("woken"))
                deadline = time.monotonic() + 2.0
                trace_path = os.path.join(self.tmp, "assistant_trace.jsonl")
                entries: List[Dict[str, Any]] = []
                while time.monotonic() < deadline:
                    if os.path.exists(trace_path):
                        with open(trace_path, "r", encoding="utf-8") as f:
                            entries = [json.loads(line) for line in f if line.strip()]
                        if len(entries) >= 2:
                            break
                    time.sleep(0.05)

                self.assertGreaterEqual(len(entries), 2)
                self.assertTrue(entries[0]["native_builtin_tools"])
                ev_types = [ev.get("type") for ev in entries[0]["server_tool_events"]]
                self.assertIn("google_search_call", ev_types)
                self.assertIn("google_search_result", ev_types)
                self.assertEqual(len(entries[0]["citations"]), 1)
                self.assertEqual(entries[1]["citations"][0]["type"], "url_citation")
            finally:
                assistant.stop(timeout=3.0)
        finally:
            srv.shutdown()
            srv.server_close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
