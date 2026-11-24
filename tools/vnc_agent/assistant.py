"""Asynchronous Research Assistant worker specializing in Web Search and Gemini File Search."""

import copy
import json
import os
import threading
import time
from typing import Any, Dict, List, Optional

from .constants import (
    AGENT_ASSISTANT,
    AGENT_COMPUTER_USE,
    ASSISTANT_FILE_SEARCH_DECLARATIONS,
    ASSISTANT_FILE_SEARCH_FUNCTION_NAMES,
    ASSISTANT_SYSTEM_INSTRUCTION,
    COORDINATION_FUNCTION_DECLARATIONS,
    COORDINATION_FUNCTION_NAMES,
    DEFAULT_ASSISTANT_MODEL,
    DEFAULT_FILE_LIST_MAX_ENTRIES,
    DEFAULT_FILE_READ_MAX_BYTES,
    INTERACTIONS_URL,
    WORKSPACE_FUNCTION_DECLARATIONS,
    WORKSPACE_FUNCTION_NAMES,
)
from .coordination import WorkspaceCoordinator
from .file_search import FileSearchClient, FileSearchStoreRegistry, normalize_store_name
from .interactions import (
    InteractionsApiError,
    InteractionsClient,
    extract_citations,
    extract_function_calls,
    extract_model_text,
    extract_server_tool_events,
)
from .workspace import LocalWorkspace


def _parse_csv_list(raw: Optional[str]) -> List[str]:
    if not raw or not isinstance(raw, str):
        return []
    return [part.strip() for part in raw.split(",") if part.strip()]


class ResearchAssistantWorker:
    """Background worker that wakes on `.coord/` markers to perform Web Search & File Search."""

    def __init__(
        self,
        api_key: str,
        workspace: LocalWorkspace,
        coordinator: WorkspaceCoordinator,
        file_search_client: FileSearchClient,
        store_registry: FileSearchStoreRegistry,
        model: str = DEFAULT_ASSISTANT_MODEL,
        thinking_level: Optional[str] = None,
        max_turns: int = 12,
        enable_web_search: bool = True,
        log_dir: Optional[str] = None,
        interactions_url: str = INTERACTIONS_URL,
        poll_interval: float = 0.25,
    ) -> None:
        self.api_key = api_key
        self.workspace = workspace
        self.coordinator = coordinator
        self.file_search_client = file_search_client
        self.store_registry = store_registry
        self.model = model
        self.thinking_level = thinking_level
        self.max_turns = max(1, int(max_turns))
        self.enable_web_search = bool(enable_web_search)
        self.log_dir = log_dir
        self.interactions_url = interactions_url
        self.poll_interval = float(poll_interval)

        self._use_native_builtin_tools = True
        self._last_interaction_id: Optional[str] = None
        self._stop_event = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._trace_lock = threading.Lock()
        self._trace_path = os.path.join(log_dir, "assistant_trace.jsonl") if log_dir else None

        gen_cfg = {"thinking_level": thinking_level} if (thinking_level and thinking_level != "default") else None
        self._llm = InteractionsClient(
            api_key=api_key,
            model=model,
            tools=[],
            system_instruction=ASSISTANT_SYSTEM_INSTRUCTION,
            generation_config=gen_cfg,
            base_url=interactions_url,
            timeout=90.0,
        )

    def _log_trace(self, entry: Dict[str, Any]) -> None:
        if not self._trace_path:
            return
        try:
            with self._trace_lock:
                os.makedirs(os.path.dirname(self._trace_path), exist_ok=True)
                with open(self._trace_path, "a", encoding="utf-8") as f:
                    f.write(json.dumps(entry, ensure_ascii=False) + "\n")
        except OSError:
            pass

    def _build_tools(self) -> List[Dict[str, Any]]:
        tools: List[Dict[str, Any]] = []
        if self._use_native_builtin_tools:
            if self.enable_web_search:
                tools.append({"type": "google_search"})
            attached_stores = self.store_registry.list_attached()
            if attached_stores:
                tools.append({"type": "file_search", "file_search_store_names": attached_stores})
        tools.extend(copy.deepcopy(WORKSPACE_FUNCTION_DECLARATIONS))
        tools.extend(copy.deepcopy(COORDINATION_FUNCTION_DECLARATIONS))
        tools.extend(copy.deepcopy(ASSISTANT_FILE_SEARCH_DECLARATIONS))
        return tools

    def _create_interaction(
        self,
        input_payload: Any,
        previous_interaction_id: Optional[str],
    ) -> Dict[str, Any]:
        tools = self._build_tools()
        try:
            return self._llm.create(
                input_payload=input_payload,
                previous_interaction_id=previous_interaction_id,
                thinking_level=self.thinking_level,
                tools_override=tools,
            )
        except InteractionsApiError as exc:
            if exc.is_tool_combination_error and self._use_native_builtin_tools:
                print(
                    "  [ASSISTANT] Model rejected combined built-in + function tools; "
                    "falling back to web_search_query / file_search_query helpers."
                )
                self._use_native_builtin_tools = False
                return self._llm.create(
                    input_payload=input_payload,
                    previous_interaction_id=previous_interaction_id,
                    thinking_level=self.thinking_level,
                    tools_override=self._build_tools(),
                )
            if previous_interaction_id and exc.is_previous_interaction_error:
                self._last_interaction_id = None
                return self._llm.create(
                    input_payload=input_payload,
                    previous_interaction_id=None,
                    thinking_level=self.thinking_level,
                    tools_override=self._build_tools(),
                )
            raise

    def _run_dedicated_search(
        self,
        query: str,
        tool_spec: Dict[str, Any],
        instruction: str,
    ) -> Dict[str, Any]:
        client = InteractionsClient(
            api_key=self.api_key,
            model=self.model,
            tools=[tool_spec],
            system_instruction=instruction,
            base_url=self.interactions_url,
            timeout=90.0,
        )
        resp = client.create(
            input_payload=[{"type": "text", "text": query}],
            previous_interaction_id=None,
            thinking_level=self.thinking_level,
            tools_override=[tool_spec],
        )
        return {
            "status": "ok",
            "query": query,
            "answer": extract_model_text(resp),
            "citations": extract_citations(resp),
            "server_tool_events": extract_server_tool_events(resp),
            "interaction_id": resp.get("id"),
        }

    def _execute_workspace_or_memory(self, fname: str, args: Dict[str, Any]) -> Dict[str, Any]:
        if fname == "workspace_list":
            return self.workspace.list_dir(
                path="." if args.get("path") is None else str(args["path"]),
                recursive=bool(args.get("recursive", False)),
                max_entries=(
                    DEFAULT_FILE_LIST_MAX_ENTRIES
                    if args.get("max_entries") is None
                    else int(args["max_entries"])
                ),
            )
        if fname == "workspace_stat":
            return self.workspace.stat(str(args["path"]))
        if fname == "workspace_read":
            return self.workspace.read_file(
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
        if fname == "workspace_write":
            return self.workspace.write_file(
                path=str(args["path"]),
                content=args.get("content"),
                content_base64=args.get("content_base64"),
                encoding=str(args.get("encoding") or "utf-8"),
                append=bool(args.get("append", False)),
                create_parents=(True if args.get("create_parents") is None else bool(args["create_parents"])),
            )
        if fname == "workspace_mkdir":
            return self.workspace.mkdir(
                path=str(args["path"]),
                parents=(True if args.get("parents") is None else bool(args["parents"])),
            )
        if fname == "workspace_delete":
            return self.workspace.delete(
                path=str(args["path"]),
                recursive=bool(args.get("recursive", False)),
            )
        if fname == "workspace_rename":
            return self.workspace.rename(
                from_path=str(args["from_path"]),
                to_path=str(args["to_path"]),
            )
        if fname == "workspace_search":
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
            return self.workspace.memory_write(
                key=str(args["key"]),
                value=str(args["value"]),
                tags=args.get("tags"),
                author=AGENT_ASSISTANT,
                append=bool(args.get("append", False)),
            )
        if fname == "memory_delete":
            return self.workspace.memory_delete(key=str(args["key"]))
        if fname == "memory_search":
            return self.workspace.memory_search(
                query=str(args["query"]),
                tag=args.get("tag"),
                limit=20 if args.get("limit") is None else int(args["limit"]),
            )
        return {"error": f"Unsupported workspace tool: {fname}"}

    def _execute_coordination(self, fname: str, args: Dict[str, Any]) -> Dict[str, Any]:
        if fname == "peer_get_status":
            return self.coordinator.get_status()
        if fname == "peer_set_status":
            return {
                "status": "ok",
                "marker": self.coordinator.set_status(
                    agent=AGENT_ASSISTANT,
                    state=str(args["state"]),
                    task=str(args.get("task") or ""),
                    detail=str(args.get("detail") or ""),
                ),
            }
        if fname == "peer_send_message":
            return self.coordinator.send_message(
                sender=AGENT_ASSISTANT,
                recipient=AGENT_COMPUTER_USE,
                subject=str(args["subject"]),
                body=str(args["body"]),
                kind=str(args.get("kind") or "response"),
                attachments=args.get("attachments"),
                in_reply_to=args.get("in_reply_to"),
                wake=(True if args.get("wake") is None else bool(args["wake"])),
                priority=str(args.get("priority") or "normal"),
            )
        if fname == "peer_read_messages":
            return self.coordinator.read_messages(
                agent=AGENT_ASSISTANT,
                include_read=bool(args.get("include_read", False)),
                mark_read=(True if args.get("mark_read") is None else bool(args["mark_read"])),
                limit=20 if args.get("limit") is None else int(args["limit"]),
            )
        if fname == "peer_wake":
            return self.coordinator.wake_peer(
                sender=AGENT_ASSISTANT,
                recipient=AGENT_COMPUTER_USE,
                reason=str(args["reason"]),
                priority=str(args.get("priority") or "normal"),
            )
        if fname == "peer_wait_for_message":
            raw_to = 30 if args.get("timeout_seconds") is None else int(args["timeout_seconds"])
            secs = float(max(1, min(raw_to, 120)))
            self.coordinator.set_status(
                AGENT_ASSISTANT,
                "waiting_peer",
                task=str(args.get("intent") or "Waiting for Computer Use Agent"),
            )
            res = self.coordinator.wait_for_wake(AGENT_ASSISTANT, timeout=secs)
            self.coordinator.set_status(AGENT_ASSISTANT, "working", task="Processing peer message")
            if res.get("woken"):
                inbox = self.coordinator.read_messages(AGENT_ASSISTANT, mark_read=True)
                return {
                    "status": "woken",
                    "messages": inbox.get("messages", []),
                    "wake_consumed": inbox.get("wake_consumed"),
                }
            return {"status": "stopped" if res.get("stopped") else "timeout", "waited_seconds": secs}
        return {"error": f"Unsupported coordination tool: {fname}"}

    def _execute_file_or_web_search(self, fname: str, args: Dict[str, Any]) -> Dict[str, Any]:
        if fname == "file_search_create_store":
            store = self.file_search_client.create_store(
                display_name=str(args["display_name"]),
                embedding_model=args.get("embedding_model"),
            )
            store_name = str(store.get("name", ""))
            if store_name:
                self.store_registry.register_store(
                    store_name,
                    display_name=str(store.get("displayName", args["display_name"])),
                    set_default=True,
                )
            return {"status": "ok", "store": store, "attached_stores": self.store_registry.list_attached()}

        if fname == "file_search_list_stores":
            page_size = 20 if args.get("page_size") is None else int(args["page_size"])
            remote = self.file_search_client.list_stores(page_size=page_size)
            return {
                "status": "ok",
                "remote_stores": remote.get("fileSearchStores", []),
                "registry": self.store_registry.get_snapshot(),
            }

        if fname == "file_search_attach_store":
            norm = normalize_store_name(str(args["store_name"]))
            store_info: Dict[str, Any] = {"name": norm}
            try:
                store_info = self.file_search_client.get_store(norm)
            except Exception:
                pass
            reg = self.store_registry.register_store(
                norm,
                display_name=str(store_info.get("displayName", "")),
                set_default=True,
            )
            return {"status": "ok", "store": store_info, "registry": reg}

        if fname == "file_search_delete_store":
            norm = normalize_store_name(str(args["store_name"]))
            force = True if args.get("force") is None else bool(args["force"])
            self.file_search_client.delete_store(norm, force=force)
            reg = self.store_registry.remove_store(norm)
            return {"status": "ok", "deleted": norm, "registry": reg}

        if fname == "file_search_upload_file":
            abs_local, rel_local = self.workspace.resolve_path(
                str(args["local_path"]),
                allow_root=False,
                allow_internal=True,
            )
            raw_store = args.get("store_name")
            if raw_store and str(raw_store).strip():
                store_name = normalize_store_name(str(raw_store))
                self.store_registry.register_store(store_name)
            else:
                store_name = self.store_registry.get_default_store() or ""
                if not store_name:
                    created = self.file_search_client.create_store(display_name="vnc-workspace-store")
                    store_name = str(created["name"])
                    self.store_registry.register_store(
                        store_name,
                        display_name=str(created.get("displayName", "vnc-workspace-store")),
                        set_default=True,
                    )

            custom_meta: Optional[List[Dict[str, Any]]] = None
            raw_meta = args.get("custom_metadata_json")
            if raw_meta and str(raw_meta).strip():
                parsed_meta = json.loads(str(raw_meta))
                if not isinstance(parsed_meta, list):
                    raise ValueError("custom_metadata_json must decode to a JSON array")
                custom_meta = parsed_meta

            chunking_cfg: Optional[Dict[str, Any]] = None
            max_tok = args.get("max_tokens_per_chunk")
            max_ov = args.get("max_overlap_tokens")
            if max_tok is not None or max_ov is not None:
                ws_cfg: Dict[str, int] = {}
                if max_tok is not None:
                    ws_cfg["maxTokensPerChunk"] = int(max_tok)
                if max_ov is not None:
                    ws_cfg["maxOverlapTokens"] = int(max_ov)
                chunking_cfg = {"whiteSpaceConfig": ws_cfg}

            op = self.file_search_client.upload_file(
                store_name=store_name,
                local_abs_path=abs_local,
                display_name=args.get("display_name"),
                mime_type=args.get("mime_type"),
                custom_metadata=custom_meta,
                chunking_config=chunking_cfg,
                wait=True,
            )
            return {
                "status": "ok",
                "local_path": rel_local,
                "store_name": store_name,
                "operation": op,
            }

        if fname == "file_search_list_documents":
            raw_store = args.get("store_name")
            store_name = (
                normalize_store_name(str(raw_store))
                if (raw_store and str(raw_store).strip())
                else (self.store_registry.get_default_store() or "")
            )
            if not store_name:
                return {"error": "No File Search store specified or attached."}
            page_size = 20 if args.get("page_size") is None else int(args["page_size"])
            res = self.file_search_client.list_documents(store_name, page_size=page_size)
            return {"status": "ok", "store_name": store_name, "documents": res.get("documents", [])}

        if fname == "file_search_delete_document":
            force = True if args.get("force") is None else bool(args["force"])
            self.file_search_client.delete_document(str(args["document_name"]), force=force)
            return {"status": "ok", "deleted": str(args["document_name"])}

        if fname == "file_search_query":
            raw_stores = _parse_csv_list(args.get("store_names"))
            stores = [normalize_store_name(s) for s in raw_stores] if raw_stores else self.store_registry.list_attached()
            if not stores:
                return {"error": "No File Search stores attached. Create or attach a store and upload files first."}
            tool_spec: Dict[str, Any] = {"type": "file_search", "file_search_store_names": stores}
            mf = args.get("metadata_filter")
            if mf and str(mf).strip():
                tool_spec["metadata_filter"] = str(mf).strip()
            return self._run_dedicated_search(
                query=str(args["query"]),
                tool_spec=tool_spec,
                instruction="Answer the user's question accurately using File Search and cite the retrieved documents.",
            )

        if fname == "web_search_query":
            if not self.enable_web_search:
                return {"error": "Web search is disabled (--no-web-search)."}
            return self._run_dedicated_search(
                query=str(args["query"]),
                tool_spec={"type": "google_search"},
                instruction="Answer the query using Google Search and include clear source citations.",
            )

        return {"error": f"Unsupported File Search / Web Search tool: {fname}"}

    def _execute_tool(self, fname: str, fc_args: Dict[str, Any]) -> Dict[str, Any]:
        try:
            if fname in WORKSPACE_FUNCTION_NAMES:
                return self._execute_workspace_or_memory(fname, fc_args)
            if fname in COORDINATION_FUNCTION_NAMES:
                return self._execute_coordination(fname, fc_args)
            if fname in ASSISTANT_FILE_SEARCH_FUNCTION_NAMES:
                return self._execute_file_or_web_search(fname, fc_args)
            return {"error": f"Unknown tool for assistant: {fname}"}
        except Exception as exc:
            return {"error": str(exc)}

    def _format_wake_prompt(
        self,
        messages: List[Dict[str, Any]],
        wake_marker: Optional[Dict[str, Any]],
    ) -> str:
        attached = self.store_registry.list_attached()
        lines = [
            "You were woken up by the Computer Use Agent via `.coord/` markers.",
            f"Shared workspace root: {self.workspace.root}",
            f"Attached Gemini File Search stores: {attached if attached else 'none'}",
        ]
        if wake_marker:
            lines.append(f"Wake marker: {json.dumps(wake_marker, ensure_ascii=False)}")
        if messages:
            lines.append("Unread inbox messages:")
            for msg in messages:
                lines.append(json.dumps(msg, ensure_ascii=False))
        lines.append(
            "Complete the requested research / File Search / workspace work, update shared memory if useful, "
            "and reply via `peer_send_message(..., wake=true)` to wake the Computer Use Agent."
        )
        return "\n\n".join(lines)

    def _process_wakeup(self) -> None:
        inbox = self.coordinator.read_messages(AGENT_ASSISTANT, mark_read=True, limit=20)
        messages = inbox.get("messages", [])
        wake_marker = inbox.get("wake_consumed")
        if wake_marker is None:
            wake_marker = self.coordinator.consume_wake(AGENT_ASSISTANT)
        if not messages and wake_marker is None:
            return

        first_subject = messages[0].get("subject", "research task") if messages else wake_marker.get("reason", "wake")
        self.coordinator.set_status(AGENT_ASSISTANT, "working", task=str(first_subject)[:120])
        prompt_text = self._format_wake_prompt(messages, wake_marker)
        print(f"  [ASSISTANT] Woken up (messages={len(messages)}, task={first_subject!r})")

        sent_peer_message = False
        try:
            interaction = self._create_interaction(
                input_payload=[{"type": "text", "text": prompt_text}],
                previous_interaction_id=self._last_interaction_id,
            )

            for turn_idx in range(1, self.max_turns + 1):
                if self._stop_event.is_set():
                    break
                calls = extract_function_calls(interaction)
                server_events = extract_server_tool_events(interaction)
                citations = extract_citations(interaction)
                if server_events:
                    event_types = [str(ev.get("type", "unknown")) for ev in server_events]
                    print(f"  [ASSISTANT] Server tools: {event_types}")

                if not calls:
                    final_text = extract_model_text(interaction)
                    self._last_interaction_id = interaction.get("id")
                    self._log_trace(
                        {
                            "turn": turn_idx,
                            "interaction_id": interaction.get("id"),
                            "native_builtin_tools": self._use_native_builtin_tools,
                            "attached_stores": self.store_registry.list_attached(),
                            "server_tool_events": server_events,
                            "final_text": final_text,
                            "citations": citations,
                        }
                    )
                    if not sent_peer_message:
                        reply_body = final_text or "(Assistant finished with no text output.)"
                        if citations:
                            reply_body += "\n\nSources/Citations:\n" + json.dumps(citations, ensure_ascii=False, indent=2)
                        reply_to = messages[-1].get("id") if messages else None
                        self.coordinator.send_message(
                            sender=AGENT_ASSISTANT,
                            recipient=AGENT_COMPUTER_USE,
                            subject=f"Re: {first_subject}",
                            body=reply_body,
                            kind="response",
                            in_reply_to=reply_to,
                            wake=True,
                        )
                    break

                fn_results: List[Dict[str, Any]] = []
                turn_actions: List[Dict[str, Any]] = []
                for fc in calls:
                    fname = str(fc.get("name", ""))
                    call_id = str(fc.get("id", ""))
                    fc_args = fc.get("arguments") or {}
                    print(f"  [ASSISTANT] -> Tool: {fname}")
                    res = self._execute_tool(fname, fc_args)
                    if fname == "peer_send_message" and res.get("status") == "ok":
                        sent_peer_message = True
                    turn_actions.append({"name": fname, "args": fc_args, "result": res})
                    fn_results.append(
                        {
                            "type": "function_result",
                            "name": fname,
                            "call_id": call_id,
                            "result": [{"type": "text", "text": json.dumps(res, ensure_ascii=False)}],
                        }
                    )

                self._log_trace(
                    {
                        "turn": turn_idx,
                        "interaction_id": interaction.get("id"),
                        "native_builtin_tools": self._use_native_builtin_tools,
                        "attached_stores": self.store_registry.list_attached(),
                        "server_tool_events": server_events,
                        "citations": citations,
                        "actions": turn_actions,
                    }
                )
                interaction = self._create_interaction(
                    input_payload=fn_results,
                    previous_interaction_id=interaction.get("id"),
                )
            else:
                if not sent_peer_message:
                    self.coordinator.send_message(
                        sender=AGENT_ASSISTANT,
                        recipient=AGENT_COMPUTER_USE,
                        subject=f"Re: {first_subject} (turn limit reached)",
                        body=f"Research Assistant reached max_turns ({self.max_turns}) while working on {first_subject!r}.",
                        kind="update",
                        in_reply_to=(messages[-1].get("id") if messages else None),
                        wake=True,
                    )
        except Exception as exc:
            err_msg = f"Research Assistant failed on {first_subject!r}: {exc}"
            print(f"  [ASSISTANT ERROR] {err_msg}")
            self._log_trace({"error": err_msg})
            self.coordinator.set_status(AGENT_ASSISTANT, "error", task=str(first_subject)[:120], detail=str(exc))
            self.coordinator.send_message(
                sender=AGENT_ASSISTANT,
                recipient=AGENT_COMPUTER_USE,
                subject=f"Error: {first_subject}",
                body=err_msg,
                kind="alert",
                in_reply_to=(messages[-1].get("id") if messages else None),
                wake=True,
                priority="urgent",
            )
            return

        if not self._stop_event.is_set():
            self.coordinator.set_status(AGENT_ASSISTANT, "sleeping", task="Awaiting wakeup marker")

    def _run_loop(self) -> None:
        self.coordinator.set_status(AGENT_ASSISTANT, "sleeping", task="Awaiting wakeup marker")
        while not self._stop_event.is_set():
            wait_res = self.coordinator.wait_for_wake(
                AGENT_ASSISTANT,
                timeout=0.5,
                poll_interval=self.poll_interval,
            )
            if self._stop_event.is_set() or wait_res.get("stopped"):
                break
            if wait_res.get("woken"):
                self._process_wakeup()
        self.coordinator.set_status(AGENT_ASSISTANT, "done", task="Assistant stopped")

    def start(self) -> None:
        if self._thread is not None and self._thread.is_alive():
            return
        self._stop_event.clear()
        self._thread = threading.Thread(target=self._run_loop, name="ResearchAssistantWorker", daemon=True)
        self._thread.start()

    def stop(self, timeout: float = 5.0) -> None:
        self._stop_event.set()
        self.coordinator.shutdown()
        if self._thread is not None:
            self._thread.join(timeout=timeout)
            self._thread = None
