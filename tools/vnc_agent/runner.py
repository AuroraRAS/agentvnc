"""Main CLI runner, tool builder, and Computer Use Agent loop."""

import argparse
import base64
import copy
import json
import os
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from typing import Any, Dict, List, Optional, Tuple

from test_agent_bridge import AgentClient

from .assistant import ResearchAssistantWorker
from .constants import (
    AGENT_COMPUTER_USE,
    ALL_CUSTOM_FUNCTION_NAMES,
    COORDINATION_FUNCTION_DECLARATIONS,
    COORDINATION_SYSTEM_INSTRUCTION,
    CUSTOM_FUNCTION_DECLARATIONS,
    DEFAULT_ASSISTANT_MODEL,
    DEFAULT_COMPUTER_USE_ENVIRONMENT,
    DEFAULT_MODEL,
    DEFAULT_SYSTEM_ONE_MODEL,
    DEFAULT_VNCVIEWER,
    ENVIRONMENT_SYSTEM_INSTRUCTIONS,
    FILE_FUNCTION_DECLARATIONS,
    FTP_PROTOCOLS,
    FTP_SYSTEM_INSTRUCTION,
    PREDEFINED_FUNCTIONS_BY_ENVIRONMENT,
    READ_ONLY_FUNCTIONS,
    RECOVERY_THINKING_LEVELS,
    REMOTE_MUTATING_FUNCTIONS,
    REMOTE_TRANSFER_FUNCTION_DECLARATIONS,
    REPO_ROOT,
    SYSTEM_INSTRUCTION,
    SYSTEM_ONE_FUNCTION_DECLARATIONS,
    SYSTEM_ONE_FUNCTION_NAMES,
    SYSTEM_ONE_SYSTEM_INSTRUCTION,
    THINKING_LEVELS,
    TYPESAFE_SYSTEM_ONE_URL,
    VNC_UNAVAILABLE_FUNCTIONS,
    WORKSPACE_FUNCTION_DECLARATIONS,
    WORKSPACE_SYSTEM_INSTRUCTION,
)
from .coordination import WorkspaceCoordinator
from .executor import (
    AgentPausedError,
    VncDesktopExecutor,
    denormalize_x,
    denormalize_y,
    parse_legacy_clipboard_encoding,
    summarize_for_overlay,
)
from .file_search import FileSearchClient, FileSearchStoreRegistry
from .ftp_client import (
    VncFtpClient,
    discover_ftp_url_from_log,
    parse_ftp_url,
    redact_ftp_url,
    vncviewer_supports_ftp,
)
from .interactions import (
    InteractionsApiError,
    InteractionsClient,
    extract_function_calls,
    extract_model_text,
    extract_usage,
    load_api_key,
)
from .system_one_client import (
    SystemOneClient,
    load_typesafe_api_key,
    try_load_typesafe_api_key,
)
from .system_one_fsm import SystemOneHostEngine
from .workspace import LocalWorkspace


def parse_excluded_actions(
    raw_values: Optional[List[str]],
    environment: str = DEFAULT_COMPUTER_USE_ENVIRONMENT,
) -> List[str]:
    predefined_actions = PREDEFINED_FUNCTIONS_BY_ENVIRONMENT[environment]
    excluded: List[str] = []
    seen = set()
    for item in raw_values or []:
        for part in str(item).split(","):
            name = part.strip()
            if not name or name in seen:
                continue
            seen.add(name)
            if name not in predefined_actions:
                print(
                    f"  [WARN] Excluded action {name!r} is not documented for the {environment} environment; "
                    "forwarding as configured."
                )
            excluded.append(name)
    return excluded


def build_tools_config(
    strict_safety: bool,
    excluded_actions: List[str],
    enable_custom_tools: bool,
    enable_file_tools: bool = False,
    enable_workspace_tools: bool = False,
    enable_remote_transfer_tools: bool = False,
    enable_coord_tools: bool = False,
    enable_system_one_tools: bool = False,
    environment: str = DEFAULT_COMPUTER_USE_ENVIRONMENT,
) -> List[Dict[str, Any]]:
    cu_cfg: Dict[str, Any] = {
        "type": "computer_use",
        "environment": environment,
        "enable_prompt_injection_detection": True,
    }
    unavailable_actions = (
        name for name in PREDEFINED_FUNCTIONS_BY_ENVIRONMENT[environment] if name in VNC_UNAVAILABLE_FUNCTIONS
    )
    exclusions = list(dict.fromkeys([*excluded_actions, *unavailable_actions]))
    if exclusions:
        cu_cfg["excluded_predefined_functions"] = exclusions
    if not strict_safety:
        cu_cfg["disabled_safety_policies"] = ["data_modification"]

    tools: List[Dict[str, Any]] = [cu_cfg]
    if enable_custom_tools:
        tools.extend(copy.deepcopy(CUSTOM_FUNCTION_DECLARATIONS))
    if enable_file_tools:
        tools.extend(copy.deepcopy(FILE_FUNCTION_DECLARATIONS))
    if enable_workspace_tools:
        tools.extend(copy.deepcopy(WORKSPACE_FUNCTION_DECLARATIONS))
    if enable_remote_transfer_tools:
        tools.extend(copy.deepcopy(REMOTE_TRANSFER_FUNCTION_DECLARATIONS))
    if enable_coord_tools:
        tools.extend(copy.deepcopy(COORDINATION_FUNCTION_DECLARATIONS))
    if enable_system_one_tools:
        tools.extend(copy.deepcopy(SYSTEM_ONE_FUNCTION_DECLARATIONS))
    return tools


def trace_action_args(name: str, args: Dict[str, Any]) -> Dict[str, Any]:
    fields = args.get("fields")
    if name != "reconcile_ui_state" or not isinstance(fields, list):
        return args
    return {
        **args,
        "fields": [
            {**field, "desired_value": "<redacted>"}
            if isinstance(field, dict)
            and str(field.get("control_type", "")).strip().lower() == "secret_text"
            and "desired_value" in field
            else field
            for field in fields
        ],
    }


def system_one_model_result(result: Dict[str, Any]) -> Dict[str, Any]:
    """Keep the visual handoff and field outcomes in context, with diagnostics in the trace."""
    compact = {key: value for key, value in result.items() if key not in ("fsm_states", "metrics", "outcome")}
    outcome = result.get("outcome")
    if outcome:
        model_outcome = {
            key: value for key, value in outcome.items()
            if value is not None and key not in ("recent_confidences", "reconcile_summary")
        }
        summary = outcome.get("reconcile_summary")
        if summary is not None:
            model_outcome["reconcile_summary"] = {
                "fields": [
                    {key: value for key, value in field.items()
                     if value is not None and key not in ("attempts", "control_type", "desired_value")
                     and (key != "reason" or field.get("breaker_code"))}
                    for field in summary["fields"]
                ],
                "conflicting_fields": summary["conflicting_fields"],
            }
        compact["outcome"] = model_outcome
    return compact


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


def run_agent(args: argparse.Namespace) -> int:
    environment = getattr(args, "environment", DEFAULT_COMPUTER_USE_ENVIRONMENT)
    predefined_actions = PREDEFINED_FUNCTIONS_BY_ENVIRONMENT[environment]
    excluded_actions = parse_excluded_actions(getattr(args, "exclude_actions", None), environment)
    skipped_actions = {
        name: f"This action is unavailable in the {environment} Computer Use environment."
        for actions in PREDEFINED_FUNCTIONS_BY_ENVIRONMENT.values()
        for name in actions
        if name not in predefined_actions
    }
    skipped_actions.update({
        name: "This action was disabled with --exclude-actions."
        for name in excluded_actions
        if name in predefined_actions
    })
    skipped_actions.update(VNC_UNAVAILABLE_FUNCTIONS)

    api_key = load_api_key(args.api_key_file)
    log_dir = os.path.abspath(args.log_dir) if args.log_dir else None
    if log_dir:
        os.makedirs(log_dir, exist_ok=True)
    trace_file = open(os.path.join(log_dir, "trace.jsonl"), "w", encoding="utf-8") if log_dir else None

    raw_workspace = getattr(args, "workspace", None)
    if raw_workspace:
        workspace_root = os.path.abspath(raw_workspace)
    elif log_dir:
        workspace_root = os.path.join(log_dir, "workspace")
    else:
        workspace_root = os.path.join(os.getcwd(), "vnc_agent_workspace")

    workspace = LocalWorkspace(workspace_root)
    coordinator = WorkspaceCoordinator(workspace)
    store_registry = FileSearchStoreRegistry(workspace)
    for store_arg in getattr(args, "file_search_store", None) or []:
        if str(store_arg).strip():
            store_registry.register_store(str(store_arg).strip(), set_default=False)

    enable_custom_tools = not bool(getattr(args, "no_custom_tools", False))
    assistant_enabled = enable_custom_tools and not bool(getattr(args, "no_assistant", False))
    assistant_worker: Optional[ResearchAssistantWorker] = None
    if assistant_enabled:
        raw_asst_thinking = str(getattr(args, "assistant_thinking_level", "default") or "default")
        asst_thinking: Optional[str] = None if raw_asst_thinking == "default" else raw_asst_thinking
        fs_client = FileSearchClient(api_key=api_key)
        assistant_worker = ResearchAssistantWorker(
            api_key=api_key,
            workspace=workspace,
            coordinator=coordinator,
            file_search_client=fs_client,
            store_registry=store_registry,
            model=str(getattr(args, "assistant_model", DEFAULT_ASSISTANT_MODEL) or DEFAULT_ASSISTANT_MODEL),
            thinking_level=asst_thinking,
            max_turns=int(getattr(args, "assistant_max_turns", 12) or 12),
            enable_web_search=not bool(getattr(args, "no_web_search", False)),
            log_dir=log_dir,
        )

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

        with AgentClient(socket_path, timeout=15.0) as vnc_client:
            executor = VncDesktopExecutor(
                vnc_client,
                workspace=workspace,
                coordinator=coordinator if assistant_enabled else None,
                legacy_clipboard_encoding=getattr(args, "legacy_clipboard_encoding", None),
            )
            status = executor.wait_ready(timeout=15.0)
            sandboxed = is_sandboxed_local_target(args.vnc_host, args.socket)
            endpoint_desc = f"socket={args.socket}" if args.socket else f"host={args.vnc_host}"
            print(
                f"Connected to VNC desktop: {status['width']}x{status['height']} "
                f"({status.get('desktop_name', '')})"
            )

            ftp_active = False
            ftp_display_url = ""
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
                    else:
                        ftp_client.close()
                        ftp_client = None
                        if args.ftp:
                            raise RuntimeError(
                                "Direct FTP file tools requested (--ftp), but the remote "
                                "VNC server does not support RFB file transfer."
                            )
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
            enable_workspace_tools = enable_custom_tools
            enable_remote_transfer_tools = ftp_active and enable_workspace_tools
            enable_coord_tools = assistant_enabled

            no_s1 = bool(getattr(args, "no_system_one", False))
            require_s1 = bool(getattr(args, "system_one", False))
            s1_key_file = getattr(args, "typesafe_key_file", None)
            s1_model = str(getattr(args, "system_one_model", DEFAULT_SYSTEM_ONE_MODEL) or DEFAULT_SYSTEM_ONE_MODEL)
            s1_url = str(getattr(args, "system_one_url", TYPESAFE_SYSTEM_ONE_URL) or TYPESAFE_SYSTEM_ONE_URL)
            s1_key: Optional[str] = None
            if (enable_custom_tools and not no_s1) or require_s1:
                s1_key = load_typesafe_api_key(s1_key_file) if require_s1 else try_load_typesafe_api_key(s1_key_file)

            system_one_engine: Optional[SystemOneHostEngine] = None
            if s1_key:
                s1_client = SystemOneClient(api_key=s1_key, model=s1_model, base_url=s1_url)
                system_one_engine = SystemOneHostEngine(executor=executor, s1_client=s1_client)
                executor.system_one_engine = system_one_engine
            enable_system_one_tools = system_one_engine is not None

            instruction_parts = [SYSTEM_INSTRUCTION]
            if environment in ENVIRONMENT_SYSTEM_INSTRUCTIONS:
                instruction_parts.append(ENVIRONMENT_SYSTEM_INSTRUCTIONS[environment])
            if enable_file_tools:
                instruction_parts.append(FTP_SYSTEM_INSTRUCTION)
            if enable_workspace_tools:
                instruction_parts.append(WORKSPACE_SYSTEM_INSTRUCTION)
            if enable_coord_tools:
                instruction_parts.append(COORDINATION_SYSTEM_INSTRUCTION)
            if enable_system_one_tools:
                instruction_parts.append(SYSTEM_ONE_SYSTEM_INSTRUCTION)
            system_instruction = "\n".join(instruction_parts)

            tools = build_tools_config(
                strict_safety=bool(args.strict_safety),
                excluded_actions=excluded_actions,
                enable_custom_tools=enable_custom_tools,
                enable_file_tools=enable_file_tools,
                enable_workspace_tools=enable_workspace_tools,
                enable_remote_transfer_tools=enable_remote_transfer_tools,
                enable_coord_tools=enable_coord_tools,
                enable_system_one_tools=enable_system_one_tools,
                environment=environment,
            )

            llm = InteractionsClient(
                api_key=api_key,
                model=args.model,
                tools=tools,
                system_instruction=system_instruction,
                generation_config=({"thinking_level": base_thinking_level} if base_thinking_level else None),
            )

            ftp_banner = f"on ({ftp_display_url})" if ftp_active else "off"
            asst_banner = (
                f"on ({assistant_worker.model}, web={'off' if args.no_web_search else 'on'}, "
                f"stores={len(store_registry.list_attached())})"
                if assistant_worker is not None
                else "off"
            )
            s1_banner = f"on ({s1_model})" if enable_system_one_tools else "off"
            print(
                f"Session config: model={args.model} | environment={environment} | thinking={raw_base_thinking} "
                f"(recovery={raw_rec_thinking}) | custom_tools={'on' if enable_custom_tools else 'off'} "
                f"| file_tools={'on' if enable_file_tools else 'off'} | ftp={ftp_banner} "
                f"| workspace={workspace.root} | assistant={asst_banner} | system_one={s1_banner} "
                f"| excluded={tools[0].get('excluded_predefined_functions', 'none')} | prompt_injection_detection=on "
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
            if assistant_worker is not None:
                assistant_worker.start()

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

            def handle_api_error(err: InteractionsApiError, interaction_id: Optional[str], wall_elapsed_s: float) -> str:
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

                coordinator.set_status(AGENT_COMPUTER_USE, "error", task=status_label, detail=err.error_message)
                if trace_file:
                    entry: Dict[str, Any] = {
                        "turn": global_turn,
                        "task": task_index,
                        "status": status_label,
                        "http_status": err.status_code,
                        "error_code": err.error_code,
                        "error": err.error_message,
                        "wall_elapsed_s": wall_elapsed_s,
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
                        coordinator.set_status(AGENT_COMPUTER_USE, "waiting_user", task="Awaiting instruction")
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
                task_start_mono = time.monotonic()
                task_usage_totals: Dict[str, int] = {
                    "api_calls": 0,
                    "usage_reports": 0,
                    "total_tokens": 0,
                    "total_input_tokens": 0,
                    "total_cached_tokens": 0,
                    "total_output_tokens": 0,
                    "total_thought_tokens": 0,
                    "total_tool_use_tokens": 0,
                }

                def _record_turn_usage(u: Optional[Dict[str, Any]]) -> None:
                    task_usage_totals["api_calls"] += 1
                    if not isinstance(u, dict):
                        return
                    task_usage_totals["usage_reports"] += 1
                    for k in (
                        "total_tokens",
                        "total_input_tokens",
                        "total_cached_tokens",
                        "total_output_tokens",
                        "total_thought_tokens",
                        "total_tool_use_tokens",
                    ):
                        val = u.get(k)
                        if isinstance(val, (int, float)) and not isinstance(val, bool):
                            task_usage_totals[k] += int(val)

                def _format_usage_summary(wall_s: float) -> str:
                    t = task_usage_totals
                    return (
                        f"[TOKENS] api_calls={t['api_calls']} usage_reports={t['usage_reports']} total={t['total_tokens']} "
                        f"(in={t['total_input_tokens']}, cached={t['total_cached_tokens']}, "
                        f"out={t['total_output_tokens']}, thought={t['total_thought_tokens']}, "
                        f"tool={t['total_tool_use_tokens']}) | wall={wall_s:.2f}s"
                    )

                consecutive_no_change_turns = 0
                previous_s1_breaker: Optional[str] = None
                init_thinking_lv = select_thinking_level(None)
                init_png, _ = executor.capture_png(settle_delay=0.2, max_wait=1.0)
                if log_dir:
                    init_name = "turn_00_initial.png" if task_index == 1 else f"task_{task_index:02d}_initial.png"
                    with open(os.path.join(log_dir, init_name), "wb") as f:
                        f.write(init_png)

                print(f"Goal: {current_prompt}")
                coordinator.set_status(
                    AGENT_COMPUTER_USE,
                    "working",
                    task=summarize_for_overlay(current_prompt, fallback="Working on task"),
                )
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
                    elapsed = round(time.monotonic() - task_start_mono, 3)
                    wait_intent = handle_api_error(e, None, elapsed)
                    print(_format_usage_summary(elapsed))
                    if not stay_interactive or e.is_auth_error:
                        executor.set_overlay("done", "error", wait_intent)
                        return 2
                    coordinator.set_status(AGENT_COMPUTER_USE, "waiting_user", task=wait_intent)
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

                for task_turn in range(1, args.max_turns + 2):
                    turn_usage = extract_usage(interaction)
                    _record_turn_usage(turn_usage)
                    wall_elapsed_s = round(time.monotonic() - task_start_mono, 3)
                    function_calls = extract_function_calls(interaction)
                    if task_turn > args.max_turns and function_calls:
                        if trace_file:
                            max_entry: Dict[str, Any] = {
                                "turn": global_turn + 1,
                                "task": task_index,
                                "interaction_id": interaction.get("id"),
                                "status": "max_turns_reached",
                                "wall_elapsed_s": wall_elapsed_s,
                            }
                            if turn_usage is not None:
                                max_entry["usage"] = turn_usage
                            trace_file.write(json.dumps(max_entry, ensure_ascii=False) + "\n")
                            trace_file.flush()
                        break
                    global_turn += 1
                    print(
                        f"\n--- Turn {global_turn} "
                        f"(task={task_index}.{task_turn}, interaction={interaction.get('id')}, "
                        f"status={interaction.get('status')}, thinking={turn_thinking_lv or 'default'}) ---"
                    )
                    if not function_calls:
                        final_text = extract_model_text(interaction)
                        last_completed_interaction_id = interaction.get("id")
                        consecutive_no_change_turns = 0
                        print(f"Agent finished:\n{final_text}")
                        print(_format_usage_summary(wall_elapsed_s))
                        final_png, _ = executor.capture_png(settle_delay=0.2, max_wait=0.8)
                        if log_dir:
                            with open(os.path.join(log_dir, "final.png"), "wb") as f:
                                f.write(final_png)
                            if task_index > 1:
                                with open(os.path.join(log_dir, f"task_{task_index:02d}_final.png"), "wb") as f:
                                    f.write(final_png)
                        if trace_file:
                            done_entry: Dict[str, Any] = {
                                "turn": global_turn,
                                "task": task_index,
                                "status": interaction.get("status"),
                                "thinking_level": turn_thinking_lv,
                                "final_text": final_text,
                                "wall_elapsed_s": wall_elapsed_s,
                            }
                            if turn_usage is not None:
                                done_entry["usage"] = turn_usage
                            trace_file.write(json.dumps(done_entry, ensure_ascii=False) + "\n")
                            trace_file.flush()
                        if not stay_interactive:
                            coordinator.set_status(AGENT_COMPUTER_USE, "done", task="Task completed")
                            executor.set_overlay("done", "done", "Task completed")
                            return 0
                        coordinator.set_status(AGENT_COMPUTER_USE, "idle", task="Task completed")
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
                    s1_evidence_map: Dict[int, List[Tuple[str, bytes]]] = {}
                    s1_breaker_reason: Optional[str] = None
                    turn_log_actions: List[Dict[str, Any]] = []
                    collab_notes: List[str] = []
                    collaborator_intervened = False
                    batch_had_error = False
                    batch_had_resize = False
                    any_modifying_action = False
                    any_frame_changed = False
                    any_file_mutating_success = False
                    checked_peer_inbox_this_turn = False

                    for idx, fc in enumerate(function_calls):
                        fname = str(fc.get("name", ""))
                        call_id = str(fc.get("id", ""))
                        fc_args = fc.get("arguments") or {}
                        intent = str(fc_args.get("intent", "N/A"))
                        print(f"  -> Action: {fname}({{...}}) | Intent: {intent}")
                        if fname in ("peer_read_messages", "peer_wait_for_message"):
                            checked_peer_inbox_this_turn = True

                        if collaborate_mode:
                            st_now = executor.get_status()
                            if st_now.get("agent_paused"):
                                coordinator.set_status(AGENT_COMPUTER_USE, "paused", task="Human collaborator takeover")
                                human_delta, notes = executor.wait_until_resumed()
                                coordinator.set_status(AGENT_COMPUTER_USE, "working", task="Resumed after takeover")
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

                        if fname in skipped_actions:
                            reason = skipped_actions[fname]
                            print(f"  [SKIP] {fname}: {reason}")
                            action_result.update(status="skipped", reason=reason)
                            results.append((fname, call_id, action_result, False))
                            skip_log_entry = {"name": fname, "args": fc_args, "result": action_result}
                            if safety_audit is not None:
                                skip_log_entry["safety_decision"] = safety_audit
                            turn_log_actions.append(skip_log_entry)
                            continue

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
                            elif fname in (
                                "click", "double_click", "triple_click", "mouse_down", "long_press",
                                "type", "paste_text_at", "click_and_type",
                            ):
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

                        is_remote_file_mutating = fname in REMOTE_MUTATING_FUNCTIONS
                        is_s1_tool = fname in SYSTEM_ONE_FUNCTION_NAMES
                        is_gui_modifying = (
                            fname not in READ_ONLY_FUNCTIONS and not is_remote_file_mutating and not is_s1_tool
                        )
                        pre_status: Dict[str, Any] = executor.get_status() if is_gui_modifying else {}

                        try:
                            exec_res = executor.execute(fname, fc_args, collaborate_mode=collaborate_mode)
                            action_result.update(exec_res)
                        except AgentPausedError as e:
                            if collaborate_mode:
                                coordinator.set_status(AGENT_COMPUTER_USE, "paused", task="Human collaborator takeover")
                                human_delta, notes = executor.wait_until_resumed()
                                coordinator.set_status(AGENT_COMPUTER_USE, "working", task="Resumed after takeover")
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
                        s1_evidence = action_result.pop("_visual_evidence_images", None)
                        if isinstance(s1_evidence, list) and s1_evidence:
                            s1_evidence_map[len(results)] = s1_evidence[:2]
                        if "error" in action_result:
                            batch_had_error = True
                            item_needs_screenshot = not bool(s1_evidence)
                        else:
                            if is_s1_tool:
                                if action_result.get("breaker_code"):
                                    s1_breaker_reason = f"system_one_{str(action_result['breaker_code']).lower()}"
                                elif action_result.get("status") == "error":
                                    s1_breaker_reason = "system_one_error"
                                item_needs_screenshot = not bool(s1_evidence)
                            else:
                                if is_gui_modifying:
                                    verify = executor.verify_action_effect(pre_status)
                                    action_result["verification"] = verify
                                    any_modifying_action = True
                                    if verify["frame_changed"]:
                                        any_frame_changed = True
                                    if verify["resized"]:
                                        batch_had_resize = True
                                elif is_remote_file_mutating:
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
                            if batch_had_resize or explicit_attach or is_s1_tool:
                                if batch_had_resize:
                                    skip_res = {
                                        "status": "skipped_after_desktop_resize",
                                        "new_size": [executor.width, executor.height],
                                    }
                                elif is_s1_tool:
                                    skip_res = {
                                        "status": "skipped_pending_visual_verification",
                                        "note": f"Inspect the {fname} result and visual evidence before continuing.",
                                    }
                                else:
                                    skip_res = {
                                        "status": "skipped_pending_visual_verification",
                                        "note": "Verify the pasted text in the screenshot before continuing.",
                                    }
                                for rem_fc in function_calls[idx + 1 :]:
                                    rem_name = str(rem_fc.get("name", ""))
                                    rem_id = str(rem_fc.get("id", ""))
                                    results.append((rem_name, rem_id, dict(skip_res), False))
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

                    if assistant_enabled and results and not checked_peer_inbox_this_turn:
                        if coordinator.has_wake(AGENT_COMPUTER_USE):
                            wake_doc = coordinator.peek_wake(AGENT_COMPUTER_USE)
                            unread = coordinator.unread_count(AGENT_COMPUTER_USE)
                            summaries = coordinator.peek_unread_summaries(AGENT_COMPUTER_USE, limit=5)
                            results[0][2]["peer_notification"] = {
                                "from": "assistant",
                                "wake_marker": wake_doc,
                                "unread_count": unread,
                                "unread_summaries": summaries,
                                "note": "Call peer_read_messages() to read full message(s) from Research Assistant.",
                            }
                            print(f"  [PEER] Notifying computer_use of {unread} unread assistant message(s).")

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
                    elif s1_breaker_reason and (
                        s1_breaker_reason == previous_s1_breaker
                        or s1_breaker_reason == "system_one_error"
                    ):
                        escalation_reason = s1_breaker_reason
                    elif consecutive_no_change_turns >= 2:
                        escalation_reason = "stalled_ui"

                    next_thinking_lv = select_thinking_level(escalation_reason) if not halted_reason else turn_thinking_lv
                    previous_s1_breaker = s1_breaker_reason

                    if trace_file:
                        trace_entry: Dict[str, Any] = {
                            "turn": global_turn,
                            "task": task_index,
                            "interaction_id": interaction.get("id"),
                            "actions": [
                                {**action, "args": trace_action_args(action["name"], action["args"])}
                                for action in turn_log_actions
                            ],
                            "frame_seq": post_hdr.get("frame_seq"),
                            "captured_screenshot": need_capture,
                            "thinking_level": turn_thinking_lv,
                            "halted_reason": halted_reason,
                            "user_notes": collab_notes,
                            "wall_elapsed_s": wall_elapsed_s,
                        }
                        if turn_usage is not None:
                            trace_entry["usage"] = turn_usage
                        if s1_breaker_reason:
                            trace_entry["system_one_breaker"] = s1_breaker_reason
                        if escalation_reason:
                            trace_entry["escalated_reason"] = escalation_reason
                            trace_entry["next_thinking_level"] = next_thinking_lv
                        trace_file.write(json.dumps(trace_entry, ensure_ascii=False) + "\n")
                        trace_file.flush()

                    if halted_reason:
                        print(f"Agent halted: {halted_reason}")
                        print(_format_usage_summary(wall_elapsed_s))
                        coordinator.set_status(AGENT_COMPUTER_USE, "error", task="Halted", detail=halted_reason)
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
                        if fname in SYSTEM_ONE_FUNCTION_NAMES:
                            result_text = json.dumps(system_one_model_result(res), separators=(",", ":"))
                        else:
                            result_text = json.dumps(res)
                        res_blocks: List[Dict[str, Any]] = [{"type": "text", "text": result_text}]
                        if i in s1_evidence_map:
                            for ev_idx, (ev_label, ev_png) in enumerate(s1_evidence_map[i][:2]):
                                if log_dir:
                                    ev_path = os.path.join(log_dir, f"turn_{global_turn:02d}_s1_{i}_{ev_idx}_{ev_label}.png")
                                    with open(ev_path, "wb") as f:
                                        f.write(ev_png)
                                    if ev_label == "final_fullscreen" and post_b64 is None:
                                        with open(os.path.join(log_dir, "final.png"), "wb") as f:
                                            f.write(ev_png)
                                res_blocks.append(
                                    {
                                        "type": "image",
                                        "data": base64.b64encode(ev_png).decode("ascii"),
                                        "mime_type": "image/png",
                                    }
                                )
                        elif post_b64 is not None and i == last_img_idx:
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
                        elapsed = round(time.monotonic() - task_start_mono, 3)
                        wait_intent = handle_api_error(e, interaction.get("id"), elapsed)
                        print(_format_usage_summary(elapsed))
                        if not stay_interactive or e.is_auth_error:
                            executor.set_overlay("done", "error", wait_intent)
                            return 2
                        task_halted = True
                        last_completed_interaction_id = None
                        break

                if not task_completed and not task_halted:
                    print(_format_usage_summary(round(time.monotonic() - task_start_mono, 3)))
                    if not stay_interactive:
                        print(f"Reached turn limit ({args.max_turns}) before task completion.")
                        return 1
                    print(f"Reached per-task turn limit ({args.max_turns}); returning to collaborative wait.")
                    last_completed_interaction_id = None
                    wait_intent = f"Turn limit ({args.max_turns}) reached - awaiting next instruction"

                coordinator.set_status(AGENT_COMPUTER_USE, "waiting_user", task=wait_intent)
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
        coordinator.shutdown()
        if assistant_worker is not None:
            assistant_worker.stop(timeout=5.0)
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
        description="Run a Gemini Computer Use agent over TigerVNC AgentSocket."
    )
    parser.add_argument(
        "--prompt",
        help=(
            "Task instruction for the agent (pass '' with --collaborate to start waiting for user input). "
            "Defaults to creating hello.txt on desktop or describing the screen in browser and mobile modes."
        ),
    )
    parser.add_argument(
        "--socket",
        default=None,
        help="Existing vncviewer -AgentSocket Unix domain socket path (if omitted, spawns headless vncviewer)",
    )
    parser.add_argument("--vnc-host", default="localhost:5900", help="Target VNC server host:port")
    parser.add_argument(
        "--environment",
        choices=PREDEFINED_FUNCTIONS_BY_ENVIRONMENT,
        default=DEFAULT_COMPUTER_USE_ENVIRONMENT,
        help=(
            "Computer Use profile for the remote VNC session (default: %(default)s). "
            "Browser control uses on-screen UI and VNC keys. Mobile targets Android over VNC."
        ),
    )
    parser.add_argument("--vncviewer-bin", default=DEFAULT_VNCVIEWER, help="Path to built vncviewer binary")
    parser.add_argument(
        "--legacy-clipboard-encoding",
        help="Override Latin-1 for legacy VNC clipboard transfers (e.g. cp936 on a Chinese Windows XP server).",
    )
    parser.add_argument(
        "--api-key-file",
        default=os.path.join(REPO_ROOT, "gemini.key"),
        help="Path to Gemini API key file",
    )
    parser.add_argument("--model", default=DEFAULT_MODEL, help="Gemini model name supporting computer_use")
    parser.add_argument("--max-turns", type=int, default=20, help="Maximum agent tool turns per task")
    parser.add_argument("--log-dir", default="/tmp/vnc_agent_run", help="Directory for trace.jsonl and PNG snapshots")
    parser.add_argument(
        "--workspace",
        default=None,
        help="Shared local workspace directory (default: <log-dir>/workspace)",
    )
    parser.add_argument(
        "--assistant-model",
        default=DEFAULT_ASSISTANT_MODEL,
        help=f"Gemini model for the async Web/File Search assistant (default: {DEFAULT_ASSISTANT_MODEL})",
    )
    parser.add_argument(
        "--assistant-thinking-level",
        choices=THINKING_LEVELS,
        default="default",
        help="Optional thinking_level for the Research Assistant (default: default)",
    )
    parser.add_argument(
        "--assistant-max-turns",
        type=int,
        default=12,
        help="Maximum tool turns per wakeup for the Research Assistant (default: 12)",
    )
    parser.add_argument(
        "--no-assistant",
        action="store_true",
        help="Disable the asynchronous Web Search & File Search Research Assistant",
    )
    parser.add_argument(
        "--no-web-search",
        action="store_true",
        help="Disable Google Web Search on the Research Assistant (keep File Search & workspace tools)",
    )
    parser.add_argument(
        "--file-search-store",
        action="append",
        default=None,
        help="Existing Gemini File Search store ID/name to pre-attach to the shared workspace (repeatable)",
    )
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
            "Built-in computer_use actions to disable for the selected environment "
            "(comma-separated or repeatable, e.g. --exclude-actions drag_and_drop,type)"
        ),
    )
    parser.add_argument(
        "--no-custom-tools",
        action="store_true",
        help="Disable custom function tools and the Research Assistant. Use --ftp to enable direct remote file tools explicitly.",
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
    parser.add_argument(
        "--typesafe-key-file",
        default=os.path.join(REPO_ROOT, "typesafe.key"),
        help="Path to TypeSafe System One API key file (default: <repo>/typesafe.key)",
    )
    parser.add_argument(
        "--system-one-model",
        default=DEFAULT_SYSTEM_ONE_MODEL,
        help=f"TypeSafe System One model ID (default: {DEFAULT_SYSTEM_ONE_MODEL})",
    )
    parser.add_argument(
        "--system-one-url",
        default=TYPESAFE_SYSTEM_ONE_URL,
        help=f"TypeSafe System One endpoint URL (default: {TYPESAFE_SYSTEM_ONE_URL})",
    )
    parser.add_argument(
        "--system-one",
        action="store_true",
        help="Require TypeSafe System One host tools (wait_until, loop_until, batch_apply, reconcile_ui_state)",
    )
    parser.add_argument(
        "--no-system-one",
        action="store_true",
        help="Disable TypeSafe System One host tools even if typesafe.key is present",
    )
    args = parser.parse_args()
    if args.prompt is None:
        if args.environment == DEFAULT_COMPUTER_USE_ENVIRONMENT:
            args.prompt = (
                "Create a new text document on the desktop named hello.txt containing the text "
                "'Hello from TigerVNC Computer Use Agent!' and make sure it is saved and visible on the desktop."
            )
        else:
            args.prompt = "Describe what is currently visible on the screen."
    if args.legacy_clipboard_encoding:
        try:
            args.legacy_clipboard_encoding = parse_legacy_clipboard_encoding(args.legacy_clipboard_encoding)
        except ValueError as exc:
            parser.error(f"--legacy-clipboard-encoding: {exc}")
    if args.ftp and args.no_ftp:
        parser.error("--ftp and --no-ftp cannot be used together")
    if args.system_one and args.no_system_one:
        parser.error("--system-one and --no-system-one cannot be used together")
    return run_agent(args)
