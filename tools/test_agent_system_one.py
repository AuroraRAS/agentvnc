#!/usr/bin/env python3
"""Unit and end-to-end tests for TypeSafe System One integration (client, 6-state FSM, breakers, runner)."""

import argparse
import base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import os
import shutil
import socket
import struct
import sys
import tempfile
import threading
import time
import unittest
from typing import Any, Callable, Dict, List, Optional, Tuple
from unittest import mock

from PIL import Image, ImageDraw

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

from test_agent_workspace_assistant import MockGeminiBackend, start_mock_gemini_server  # noqa: E402
import vnc_computer_use_agent as agent_mod  # noqa: E402
from vnc_agent import (  # noqa: E402
    CB_AMBIGUITY,
    CB_BATCH_DRIFT,
    CB_BUDGET_EXCEEDED,
    CB_INTERRUPT,
    CB_SAFETY_GUARD,
    CB_STALL,
    FSM_STATES,
    FrameSnapshot,
    SYSTEM_ONE_FUNCTION_DECLARATIONS,
    SYSTEM_ONE_FUNCTION_NAMES,
    SystemOneApiError,
    SystemOneClient,
    SystemOneHostEngine,
    VncDesktopExecutor,
    adjudicate_predicate,
    build_tools_config,
    compute_pixel_diff_ratio,
    compute_ssim,
    load_typesafe_api_key,
    normalize_predicate,
    prepare_image_for_system_one,
    try_load_typesafe_api_key,
    validate_host_action,
)


def make_rgb_frame(
    width: int = 800,
    height: int = 600,
    bg: Tuple[int, int, int] = (235, 235, 235),
    rects: Optional[List[Tuple[int, int, int, int, Tuple[int, int, int]]]] = None,
    frame_seq: int = 1,
) -> FrameSnapshot:
    img = Image.new("RGB", (width, height), bg)
    if rects:
        draw = ImageDraw.Draw(img)
        for x0, y0, x1, y1, color in rects:
            draw.rectangle([x0, y0, x1, y1], fill=color)
    return FrameSnapshot(
        width=width,
        height=height,
        rgb_bytes=img.tobytes(),
        frame_seq=frame_seq,
        timestamp=float(frame_seq),
    )


class FakeFsmExecutor:
    """In-memory desktop executor stub with deterministic frame sequences and input tracking."""

    def __init__(self, frames: List[FrameSnapshot]) -> None:
        self._frames = list(frames)
        self._idx = 0
        self.width = frames[0].width
        self.height = frames[0].height
        self.actions: List[Dict[str, Any]] = []
        self.released_count = 0
        self.held_keys: List[int] = []
        self.held_buttons: int = 0
        self.agent_paused = False
        self.on_action_cb: Optional[Callable[[Dict[str, Any], "FakeFsmExecutor"], None]] = None

    def current_frame(self) -> FrameSnapshot:
        return self._frames[min(self._idx, len(self._frames) - 1)]

    def advance_frame(self) -> None:
        if self._idx + 1 < len(self._frames):
            self._idx += 1

    def capture_rgb_frame(self) -> FrameSnapshot:
        f = self.current_frame()
        self.width = f.width
        self.height = f.height
        return f

    def get_status(self) -> Dict[str, Any]:
        f = self.current_frame()
        return {
            "ok": True,
            "connected": True,
            "ready": True,
            "width": f.width,
            "height": f.height,
            "frame_seq": f.frame_seq,
            "agent_paused": self.agent_paused,
        }

    def execute_host_action(self, action: Dict[str, Any]) -> Dict[str, Any]:
        self.actions.append(dict(action))
        if self.on_action_cb is not None:
            self.on_action_cb(action, self)
        else:
            self.advance_frame()
        return {"status": "ok", "type": action["type"]}

    def release_all_inputs(self) -> None:
        self.released_count += 1
        self.held_keys.clear()
        self.held_buttons = 0


class ScriptedSystemOneClient:
    """Mock SystemOneClient recording requests and returning scripted responses."""

    def __init__(self, responder: Callable[[int, Dict[str, Any], Dict[str, Any], Optional[Dict[str, Tuple[bytes, str]]]], Dict[str, Any]]) -> None:
        self.responder = responder
        self.calls: List[Dict[str, Any]] = []

    def query(
        self,
        state: Any,
        questions: Dict[str, Dict[str, Any]],
        image_assets: Optional[Dict[str, Tuple[bytes, str]]] = None,
        model: Optional[str] = None,
        timeout: Optional[float] = None,
    ) -> Dict[str, Any]:
        call_idx = len(self.calls)
        self.calls.append(
            {
                "state": state,
                "questions": questions,
                "asset_keys": list((image_assets or {}).keys()),
                "asset_sizes": {k: len(v[0]) for k, v in (image_assets or {}).items()},
                "model": model,
            }
        )
        return {"model": model or "jev-latest", "answers": self.responder(call_idx, state, questions, image_assets)}


class MockTypeSafeServer:
    """In-memory mock HTTP server for TypeSafe /v1/systemone and /v1/models."""

    def __init__(self, handler_fn: Callable[[Dict[str, Any], Dict[str, str], Dict[str, bytes]], Tuple[int, Dict[str, Any]]]) -> None:
        self.handler_fn = handler_fn
        self.requests: List[Dict[str, Any]] = []

        outer = self

        class _Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, format: str, *args: Any) -> None:  # noqa: A002
                return

            def _send_json(self, code: int, payload: Any) -> None:
                raw = json.dumps(payload).encode("utf-8")
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

            def do_GET(self) -> None:  # noqa: N802
                if self.path == "/v1/models":
                    self._send_json(200, {"models": [{"name": "jev-latest"}]})
                    return
                self._send_json(404, {"detail": "not found"})

            def do_POST(self) -> None:  # noqa: N802
                length = int(self.headers.get("Content-Length", "0") or "0")
                raw_body = self.rfile.read(length) if length > 0 else b""
                ctype = self.headers.get("Content-Type", "")
                hdrs = {k.lower(): v for k, v in self.headers.items()}
                req_json: Dict[str, Any] = {}
                parts_bin: Dict[str, bytes] = {}
                if ctype.startswith("multipart/form-data"):
                    boundary = ctype.split("boundary=", 1)[1].strip().encode("ascii")
                    for seg in raw_body.split(b"--" + boundary):
                        if not seg or seg in (b"--\r\n", b"--"):
                            continue
                        if b"\r\n\r\n" not in seg:
                            continue
                        head_raw, body_raw = seg.split(b"\r\n\r\n", 1)
                        body_clean = body_raw[:-2] if body_raw.endswith(b"\r\n") else body_raw
                        head_text = head_raw.decode("utf-8", errors="replace").strip()
                        first_line = head_text.splitlines()[0] if head_text else ""
                        name = ""
                        for token in first_line.split(";"):
                            token = token.strip()
                            if token.startswith("name="):
                                name = token.split("=", 1)[1].strip('"')
                        if name == "request":
                            req_json = json.loads(body_clean.decode("utf-8"))
                        elif name:
                            parts_bin[name] = body_clean
                else:
                    req_json = json.loads(raw_body.decode("utf-8"))

                outer.requests.append({"headers": hdrs, "body": req_json, "parts": list(parts_bin.keys())})
                code, resp_obj = outer.handler_fn(req_json, hdrs, parts_bin)
                self._send_json(code, resp_obj)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base_url = f"http://127.0.0.1:{self.server.server_port}/v1/systemone"

    def stop(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2.0)


class FakeClock:
    def __init__(self) -> None:
        self.now = 100.0
        self.sleeps: List[float] = []

    def monotonic(self) -> float:
        return self.now

    def sleep(self, secs: float) -> None:
        self.sleeps.append(secs)
        self.now += secs


class SystemOneUnitTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="tigervnc_s1_test_")

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_tool_schemas_and_build_tools_config(self) -> None:
        forbidden = {"default", "anyOf", "oneOf", "allOf", "additionalProperties"}
        allowed_types = {"string", "integer", "number", "boolean", "array", "object"}
        for decl in SYSTEM_ONE_FUNCTION_DECLARATIONS:
            self.assertEqual(decl["type"], "function")
            params = decl["parameters"]
            self.assertEqual(params["type"], "object")
            self.assertFalse(forbidden.intersection(params.keys()), decl["name"])
            for pname, pdef in params["properties"].items():
                self.assertIn(pdef["type"], allowed_types, f"{decl['name']}.{pname}")
                self.assertFalse(forbidden.intersection(pdef.keys()), f"{decl['name']}.{pname}")
            for req in params.get("required", []):
                self.assertIn(req, params["properties"])

        default_tools = build_tools_config(False, [], enable_custom_tools=True)
        default_names = {t["name"] for t in default_tools if t.get("type") == "function"}
        self.assertFalse(SYSTEM_ONE_FUNCTION_NAMES.intersection(default_names))

        s1_tools = build_tools_config(False, [], enable_custom_tools=True, enable_system_one_tools=True)
        s1_names = {t["name"] for t in s1_tools if t.get("type") == "function"}
        self.assertTrue(SYSTEM_ONE_FUNCTION_NAMES.issubset(s1_names))

    def test_key_loading_and_image_prep_mapping(self) -> None:
        key_path = os.path.join(self.tmp, "ts.key")
        with open(key_path, "w", encoding="utf-8") as f:
            f.write("ts_live_secret_123\n")
        self.assertEqual(load_typesafe_api_key(key_path), "ts_live_secret_123")
        self.assertIsNone(try_load_typesafe_api_key(os.path.join(self.tmp, "missing.key")))

        # Oversized 1920x1080 -> downscaled <= 1568
        big = make_rgb_frame(1920, 1080)
        prep_big = prepare_image_for_system_one(big)
        self.assertLessEqual(prep_big.out_width, 1568)
        self.assertLessEqual(prep_big.out_height, 1568)
        self.assertGreaterEqual(prep_big.out_width, 280)
        self.assertGreaterEqual(prep_big.out_height, 280)

        # Tiny 40x30 crop -> upscaled/padded >= 280x280 and round-trips box accurately
        prep_small = prepare_image_for_system_one(big, [200, 300, 240, 350])
        self.assertGreaterEqual(prep_small.out_width, 280)
        self.assertGreaterEqual(prep_small.out_height, 280)
        mapped = prep_small.map_box_to_screen_bbox({"top": 0.2, "bottom": 0.8, "left": 0.2, "right": 0.8})
        self.assertGreaterEqual(mapped[0], 200)
        self.assertLessEqual(mapped[2], 240)
        self.assertGreaterEqual(mapped[1], 300)
        self.assertLessEqual(mapped[3], 350)

        # Extreme aspect ratio 1200x30 strip -> padded onto >= 280x280 canvas
        prep_strip = prepare_image_for_system_one(big, [100, 100, 130, 900])
        self.assertGreaterEqual(prep_strip.out_width, 280)
        self.assertGreaterEqual(prep_strip.out_height, 280)
        with mock.patch("vnc_agent.system_one_client.Image", None), mock.patch("vnc_agent.system_one_client.np", None):
            prep_fallback = prepare_image_for_system_one(big)
        self.assertEqual((prep_fallback.out_width, prep_fallback.out_height), (prep_big.out_width, prep_big.out_height))

    def test_malformed_oracle_answers_cannot_match(self) -> None:
        negative = normalize_predicate({"type": "noul", "instructions": "Is the gate closed?", "expected_value": False}, "read_only")
        for answer in ({}, {"noul": None}, {"noul": False}, {"noul": -0.1}, {"noul": float("nan")}):
            self.assertEqual(adjudicate_predicate(negative, answer, "read_only").zone, "gray")
        self.assertEqual(adjudicate_predicate(negative, {"noul": 0.0}, "read_only").zone, "pass")

        choice = normalize_predicate(
            {"type": "choice", "instructions": "Status?", "criteria": {"ready": "Ready", "loading": "Loading"},
             "target_choices": ["ready"]},
            "read_only",
        )
        self.assertEqual(adjudicate_predicate(choice, {"choice": "unexpected", "confidence": 1}, "read_only").zone, "gray")
        self.assertEqual(adjudicate_predicate(choice, {"choice": "loading", "confidence": 0.2}, "read_only").zone, "gray")

        tightened = normalize_predicate({"type": "noul", "instructions": "Ready?", "min_confidence": 0.95}, "read_only")
        self.assertEqual(adjudicate_predicate(tightened, {"noul": 0.9}, "read_only", prev_two_frame_hits=1).zone, "gray")

        frame = make_rgb_frame(800, 50)
        prepared = prepare_image_for_system_one(frame)
        with self.assertRaises(ValueError):
            prepared.map_box_to_screen_bbox({"top": 0.01, "bottom": 0.02, "left": 0.2, "right": 0.8})

    def test_failed_key_and_button_releases_can_be_retried(self) -> None:
        calls = []
        reject_release = False

        def request(command, **args):
            calls.append((command, args))
            held = args.get("down") or args.get("button_mask")
            if reject_release and not held:
                return {"ok": False, "error": "agent_paused"}, b""
            return {"ok": True}, b""

        executor = VncDesktopExecutor(mock.Mock(request=request))
        executor._pointer(20, 30, 1)
        executor._key(0xFF51, True)
        reject_release = True
        executor.release_all_inputs()
        reject_release = False
        executor.release_all_inputs()
        self.assertEqual(calls[-2:], [
            ("POINTER_EVENT", {"x": 20, "y": 30, "button_mask": 0}),
            ("KEY_EVENT", {"keysym": 0xFF51, "down": False}),
        ])
        count = len(calls)
        executor.release_all_inputs()
        self.assertEqual(len(calls), count)

    def test_pixel_diff_and_ssim_metrics(self) -> None:
        f1 = make_rgb_frame(400, 300, bg=(235, 235, 235), rects=[(50, 50, 150, 150, (200, 20, 20))], frame_seq=1)
        f2 = make_rgb_frame(400, 300, bg=(235, 235, 235), rects=[(50, 50, 150, 150, (200, 20, 20))], frame_seq=2)
        # Textured contrasting scene so SSIM drops well below 0.50
        stripe_rects = [(x, 0, x + 10, 300, (15, 15, 30) if (x // 10) % 2 == 0 else (245, 245, 245)) for x in range(0, 400, 10)]
        f3 = make_rgb_frame(400, 300, bg=(20, 20, 20), rects=stripe_rects, frame_seq=3)

        self.assertEqual(compute_pixel_diff_ratio(f1, f2), 0.0)
        self.assertEqual(compute_ssim(f1, f2), 1.0)
        self.assertGreater(compute_pixel_diff_ratio(f1, f3), 0.8)
        self.assertLess(compute_ssim(f1, f3), 0.50)

    def test_rest_client_multipart_json_and_retry(self) -> None:
        attempts = {"count": 0}

        def handler(req: Dict[str, Any], hdrs: Dict[str, str], parts: Dict[str, bytes]) -> Tuple[int, Dict[str, Any]]:
            attempts["count"] += 1
            if attempts["count"] == 1:
                return 503, {"detail": {"error_type": "overloaded", "message": "try again"}}
            if req.get("state", {}).get("trigger_400"):
                return 400, {"detail": {"error_type": "api_usage_error", "message": "Invalid image dimensions."}}
            return 200, {
                "model": "jev-latest",
                "answers": {"ok_q": {"type": "noul", "noul": 0.94}},
            }

        srv = MockTypeSafeServer(handler)
        try:
            client = SystemOneClient(api_key="ts_test_key", base_url=srv.base_url, use_multipart=True)
            img_bytes = make_rgb_frame(320, 320).to_png_bytes()
            resp = client.query(
                state={"frame": "ref1"},
                questions={"ok_q": {"type": "noul", "instructions": "Ready?"}},
                image_assets={"ref1": (img_bytes, "png")},
            )
            self.assertEqual(resp["answers"]["ok_q"]["noul"], 0.94)
            self.assertEqual(srv.requests[-1]["headers"]["authorization"], "Bearer ts_test_key")
            self.assertEqual(srv.requests[-1]["headers"]["typesafe-danger-zone-images"], "true")
            self.assertEqual(srv.requests[-1]["parts"], ["img_0"])

            client_json = SystemOneClient(api_key="ts_test_key", base_url=srv.base_url, use_multipart=False)
            resp2 = client_json.query(
                state={"frame": "ref1"},
                questions={"ok_q": {"type": "noul", "instructions": "Ready?"}},
                image_assets={"ref1": (img_bytes, "png")},
            )
            self.assertEqual(resp2["answers"]["ok_q"]["noul"], 0.94)
            self.assertIn("data_base64", srv.requests[-1]["body"]["assets"]["ref1"])

            with self.assertRaises(SystemOneApiError) as ctx:
                client_json.query(
                    state={"trigger_400": True},
                    questions={"ok_q": {"type": "noul", "instructions": "Ready?"}},
                )
            self.assertEqual(ctx.exception.status_code, 400)
            self.assertEqual(ctx.exception.error_type, "api_usage_error")
        finally:
            srv.stop()

    def test_s0_validation_floors_unreadable_and_closed_set(self) -> None:
        # Callers can tighten min_confidence, never loosen below system floor (0.90 reversible)
        p_loosen = normalize_predicate(
            {"type": "noul", "instructions": "Saved?", "min_confidence": 0.40},
            risk_tier="reversible",
        )
        self.assertEqual(p_loosen.tau_high, 0.90)
        p_tight = normalize_predicate(
            {"type": "noul", "instructions": "Saved?", "min_confidence": 0.96},
            risk_tier="reversible",
        )
        self.assertEqual(p_tight.tau_high, 0.96)

        # Choice auto-appends "unreadable"
        p_choice = normalize_predicate(
            {"type": "choice", "instructions": "Status?", "criteria": {"ok": "Done", "err": "Failed"}},
            risk_tier="read_only",
        )
        assert p_choice.criteria is not None
        self.assertIn("unreadable", p_choice.criteria)

        # Reject non-closed-set HostAction or script keys
        with self.assertRaises(ValueError):
            validate_host_action({"type": "execute_script", "script": "rm -rf /"})
        with self.assertRaises(ValueError):
            validate_host_action({"type": "click", "x": 100, "y": 200, "script": "bad"})
        act = validate_host_action({"type": "wheel_scroll", "x": 500, "y": 500, "direction": "down"})
        self.assertEqual(act["type"], "wheel_scroll")

    def test_wait_until_diff_gate_heartbeat_stable_and_transient(self) -> None:
        base = make_rgb_frame(640, 480, bg=(240, 240, 240), frame_seq=1)
        changed = make_rgb_frame(
            640, 480, bg=(240, 240, 240), rects=[(200, 150, 440, 260, (20, 180, 40))], frame_seq=2
        )
        # 8 identical frames (triggers diff gate skips + 1 heartbeat at 6 skips), then 2 changed frames
        frames = [base] * 8 + [changed, changed]
        idx_holder = {"i": 0}

        class PollExecutor(FakeFsmExecutor):
            def capture_rgb_frame(self) -> FrameSnapshot:
                f = frames[min(idx_holder["i"], len(frames) - 1)]
                idx_holder["i"] += 1
                return f

        exec_stub = PollExecutor(frames)
        clock = FakeClock()

        def responder(call_idx: int, state: Dict[str, Any], questions: Dict[str, Any], assets: Any) -> Dict[str, Any]:
            # First two S1 calls (initial + heartbeat) happen on base frame -> False
            if call_idx < 2:
                return {"condition": {"type": "noul", "noul": 0.12}}
            return {"condition": {"type": "noul", "noul": 0.93}}

        engine = SystemOneHostEngine(exec_stub, ScriptedSystemOneClient(responder), sleep_fn=clock.sleep, monotonic_fn=clock.monotonic)
        res = engine.execute_tool(
            "wait_until",
            {
                "condition": {"type": "noul", "instructions": "Is the green success toast visible?"},
                "watch_roi": [250, 250, 600, 750],
                "poll_interval_ms": 200,
                "timeout_ms": 999999,  # tests hard cap clamping to 180000
                "diff_gate_threshold": 0.005,
                "heartbeat_every_n": 6,
                "require_stable_frames": 2,
                "capture_transient": True,
                "risk_tier": "read_only",
            },
        )
        self.assertEqual(res["status"], "matched")
        self.assertIsNone(res["breaker_code"])
        # Initial (1) + 1 heartbeat after 6 skips (1) + 2 changed stable frames (2) = 4 S1 calls across 10 captures!
        self.assertEqual(res["metrics"]["s1_calls"], 4)
        self.assertEqual(res["visual_evidence"], ["final_fullscreen", "capture_transient"])
        self.assertEqual(len(res["_visual_evidence_images"]), 2)
        self.assertLessEqual(len(res["outcome"]["recent_confidences"]), 3)
        self.assertEqual(exec_stub.released_count, 1)

    def test_wait_until_two_frame_080_and_timeout_breaker(self) -> None:
        base = make_rgb_frame(400, 300, frame_seq=1)
        f2 = make_rgb_frame(400, 300, rects=[(20, 20, 380, 280, (200, 200, 200))], frame_seq=2)
        exec_stub = FakeFsmExecutor([base, f2])
        clock = FakeClock()

        # read_only allows 2 consecutive frames >= 0.80 to pass
        def responder_two_frame(call_idx: int, state: Dict[str, Any], questions: Dict[str, Any], assets: Any) -> Dict[str, Any]:
            qkey = next(iter(questions.keys()))
            return {qkey: {"type": "noul", "noul": 0.82}}

        engine = SystemOneHostEngine(exec_stub, ScriptedSystemOneClient(responder_two_frame), sleep_fn=clock.sleep, monotonic_fn=clock.monotonic)
        res = engine.execute_tool(
            "wait_until",
            {
                "condition": {"type": "noul", "instructions": "Done?"},
                "diff_gate_threshold": 0.0,
                "poll_interval_ms": 200,
                "risk_tier": "read_only",
            },
        )
        self.assertEqual(res["status"], "matched")
        self.assertEqual(res["metrics"]["s1_calls"], 2)

        # Explicit timeout -> CB_BUDGET_EXCEEDED
        exec_to = FakeFsmExecutor([base])
        clock_to = FakeClock()
        engine_to = SystemOneHostEngine(
            exec_to,
            ScriptedSystemOneClient(lambda *_: {"condition": {"type": "noul", "noul": 0.1}}),
            sleep_fn=clock_to.sleep,
            monotonic_fn=clock_to.monotonic,
        )
        res_to = engine_to.execute_tool(
            "wait_until",
            {"condition": {"type": "noul", "instructions": "Done?"}, "poll_interval_ms": 500, "timeout_ms": 900},
        )
        self.assertEqual(res_to["status"], "timeout")
        self.assertEqual(res_to["breaker_code"], CB_BUDGET_EXCEEDED)
        self.assertEqual(res_to["metrics"]["elapsed_ms"], 900)

        clock_late = FakeClock()

        def answer_after_deadline(*_):
            clock_late.sleep(0.2)
            return {"condition": {"noul": 0.99}}

        late = SystemOneHostEngine(
            FakeFsmExecutor([base]), ScriptedSystemOneClient(answer_after_deadline),
            sleep_fn=clock_late.sleep, monotonic_fn=clock_late.monotonic,
        ).run_wait_until({"condition": {"type": "noul", "instructions": "Done?"}, "timeout_ms": 100})
        self.assertEqual(late["breaker_code"], CB_BUDGET_EXCEEDED)

    def test_gray_zone_retry_and_unreadable_breaker(self) -> None:
        base = make_rgb_frame(640, 480, frame_seq=1)
        exec_stub = FakeFsmExecutor([base])
        clock = FakeClock()

        # Retry succeeds (>= tau_high=0.85)
        def responder_recover(call_idx: int, state: Dict[str, Any], questions: Dict[str, Any], assets: Any) -> Dict[str, Any]:
            qkey = next(iter(questions.keys()))
            if call_idx == 0:
                return {qkey: {"type": "noul", "noul": 0.75}}
            return {qkey: {"type": "noul", "noul": 0.91}}

        engine = SystemOneHostEngine(exec_stub, ScriptedSystemOneClient(responder_recover), sleep_fn=clock.sleep, monotonic_fn=clock.monotonic)
        res = engine.execute_tool("wait_until", {"condition": {"type": "noul", "instructions": "Ready?"}})
        self.assertEqual(res["status"], "matched")
        self.assertEqual(res["metrics"]["s1_calls"], 2)

        # "unreadable" even at confidence 0.99 -> 1x retry -> still unreadable -> CB_AMBIGUITY
        exec_unr = FakeFsmExecutor([base])
        clock_unr = FakeClock()
        engine_unr = SystemOneHostEngine(
            exec_unr,
            ScriptedSystemOneClient(
                lambda _i, _s, questions, _a: {
                    next(iter(questions.keys())): {
                        "type": "choice",
                        "choice": "unreadable",
                        "confidence": 0.99,
                        "probabilities": {"unreadable": 0.99, "ready": 0.01},
                    }
                }
            ),
            sleep_fn=clock_unr.sleep,
            monotonic_fn=clock_unr.monotonic,
        )
        res_unr = engine_unr.execute_tool(
            "wait_until",
            {
                "condition": {
                    "type": "choice",
                    "instructions": "State?",
                    "criteria": {"ready": "Ready", "loading": "Loading"},
                    "target_choices": ["ready"],
                }
            },
        )
        self.assertEqual(res_unr["breaker_code"], CB_AMBIGUITY)
        self.assertEqual(res_unr["visual_evidence"], ["final_fullscreen", "annotated_roi_crop"])

    def test_loop_until_fallback_stall_on_match_and_geometry_guards(self) -> None:
        f0 = make_rgb_frame(800, 600, bg=(240, 240, 240), rects=[(50, 50, 200, 100, (100, 100, 100))], frame_seq=1)
        f1 = make_rgb_frame(800, 600, bg=(240, 240, 240), rects=[(50, 150, 200, 200, (100, 100, 100))], frame_seq=2)
        f2_target = make_rgb_frame(800, 600, bg=(240, 240, 240), rects=[(320, 240, 480, 320, (20, 160, 20))], frame_seq=3)
        f3_post_click = make_rgb_frame(800, 600, bg=(240, 240, 240), rects=[(320, 240, 480, 320, (20, 60, 220))], frame_seq=4)

        # Sequence: f0 -> (wheel_scroll) -> f0 (stall -> fallback PageDown) -> f1 -> f2_target -> (click) -> f3_post_click
        step_frames = [f0, f0, f1, f2_target, f3_post_click]
        exec_stub = FakeFsmExecutor(step_frames)
        clock = FakeClock()

        def responder(call_idx: int, state: Dict[str, Any], questions: Dict[str, Any], assets: Any) -> Dict[str, Any]:
            if "confirm_noul" in questions:
                return {
                    "confirm_noul": {"type": "noul", "noul": 0.95},
                    "confirm_box": {"type": "box", "box": {"top": 0.2, "bottom": 0.8, "left": 0.2, "right": 0.8}, "confidence": 0.92},
                }
            if call_idx < 3:
                return {
                    "stop": {"type": "noul", "noul": 0.05},
                    "target_box": {"type": "box", "box": {"top": 0.1, "bottom": 0.2, "left": 0.1, "right": 0.2}, "confidence": 0.30},
                }
            return {
                "stop": {"type": "noul", "noul": 0.96},
                "target_box": {"type": "box", "box": {"top": 0.40, "bottom": 0.533, "left": 0.40, "right": 0.60}, "confidence": 0.94},
            }

        engine = SystemOneHostEngine(exec_stub, ScriptedSystemOneClient(responder), sleep_fn=clock.sleep, monotonic_fn=clock.monotonic)
        res = engine.execute_tool(
            "loop_until",
            {
                "stop_predicate": {"type": "noul", "instructions": "Is the green Save button visible?"},
                "step_action": {"type": "wheel_scroll", "x": 500, "y": 500, "direction": "down"},
                "fallback_action": {"type": "key_press", "key": "Page_Down"},
                "on_match": {"mode": "click_target_box", "target_description": "green Save button"},
                "risk_tier": "reversible",
                "max_steps": 10,
            },
        )
        self.assertEqual(res["status"], "matched")
        self.assertIsNone(res["breaker_code"])
        # Action 0: wheel_scroll, Action 1: fallback key_press Page_Down, Action 2: fallback key_press Page_Down, Action 3: on_match click
        self.assertEqual([a["type"] for a in exec_stub.actions], ["wheel_scroll", "key_press", "key_press", "click"])
        self.assertEqual(exec_stub.released_count, 1)

        # Post-fallback consecutive stalls -> CB_STALL
        exec_stall = FakeFsmExecutor([f0, f0, f0, f0, f0])
        engine_stall = SystemOneHostEngine(
            exec_stall,
            ScriptedSystemOneClient(lambda *_: {"stop": {"type": "noul", "noul": 0.1}}),
            sleep_fn=clock.sleep,
            monotonic_fn=clock.monotonic,
        )
        res_stall = engine_stall.execute_tool(
            "loop_until",
            {
                "stop_predicate": {"type": "noul", "instructions": "Found?"},
                "step_action": {"type": "wheel_scroll", "x": 500, "y": 500, "direction": "down"},
                "fallback_action": {"type": "key_press", "key": "Page_Down"},
                "max_stalls": 2,
                "max_steps": 10,
            },
        )
        self.assertEqual(res_stall["breaker_code"], CB_STALL)
        self.assertEqual([a["type"] for a in exec_stall.actions], ["wheel_scroll", "key_press", "key_press"])

        exec_small_change = FakeFsmExecutor([f0, f0])
        engine_small_change = SystemOneHostEngine(
            exec_small_change,
            ScriptedSystemOneClient(lambda idx, *_: {"stop": {"noul": 0.05 if idx == 0 else 0.99}}),
            sleep_fn=clock.sleep, monotonic_fn=clock.monotonic,
        )
        result = engine_small_change.run_loop_until({
            "stop_predicate": {"type": "noul", "instructions": "Found?"},
            "step_action": {"type": "key_press", "key": "Down"},
            "max_stalls": 1,
        })
        self.assertEqual(result["status"], "matched")
        self.assertEqual(len(exec_small_change.actions), 1)

        # Geometry failure (< 6px degenerate box) -> CB_AMBIGUITY with zero click
        exec_geom = FakeFsmExecutor([f0])
        def bad_geom_responder(call_idx: int, state: Dict[str, Any], questions: Dict[str, Any], assets: Any) -> Dict[str, Any]:
            return {
                "stop": {"type": "noul", "noul": 0.96},
                "target_box": {"type": "box", "box": {"top": 0.50, "bottom": 0.502, "left": 0.50, "right": 0.502}, "confidence": 0.95},
            }
        engine_geom = SystemOneHostEngine(exec_geom, ScriptedSystemOneClient(bad_geom_responder), sleep_fn=clock.sleep, monotonic_fn=clock.monotonic)
        res_geom = engine_geom.execute_tool(
            "loop_until",
            {
                "stop_predicate": {"type": "noul", "instructions": "Button visible?"},
                "step_action": {"type": "key_press", "key": "Down"},
                "on_match": {"mode": "click_target_box", "target_description": "Apply button"},
                "risk_tier": "reversible",
            },
        )
        self.assertEqual(res_geom["breaker_code"], CB_AMBIGUITY)
        self.assertEqual(exec_geom.actions, [])

    def test_interrupt_and_safety_guard_breakers(self) -> None:
        f0 = make_rgb_frame(640, 480, bg=(240, 240, 240), frame_seq=1)
        stripe_rects = [(x, 0, x + 12, 480, (10, 10, 20) if (x // 12) % 2 == 0 else (245, 245, 245)) for x in range(0, 640, 12)]
        f_scene_drop = make_rgb_frame(640, 480, bg=(10, 10, 10), rects=stripe_rects, frame_seq=2)
        clock = FakeClock()

        # 1. Irreversible auto-click hard-blocked in S0_INIT_VALIDATE -> CB_SAFETY_GUARD
        exec_irr = FakeFsmExecutor([f0])
        engine_irr = SystemOneHostEngine(exec_irr, ScriptedSystemOneClient(lambda *_: {}), sleep_fn=clock.sleep, monotonic_fn=clock.monotonic)
        res_irr = engine_irr.execute_tool(
            "loop_until",
            {
                "stop_predicate": {"type": "noul", "instructions": "Is Delete Account visible?"},
                "step_action": {"type": "key_press", "key": "Down"},
                "on_match": {"mode": "click_target_box", "target_description": "Delete Account"},
                "risk_tier": "irreversible",
            },
        )
        self.assertEqual(res_irr["breaker_code"], CB_SAFETY_GUARD)
        self.assertEqual(exec_irr.actions, [])
        self.assertEqual(res_irr["metrics"]["s1_calls"], 0)
        self.assertEqual(res_irr["fsm_states"][-1], "S5_HANDOFF_TO_S2")
        nav_guard = engine_irr.run_loop_until({
            "stop_predicate": {"type": "noul", "instructions": "Is the report visible?"},
            "step_action": {"type": "click", "x": 500, "y": 500},
            "risk_tier": "irreversible",
        })
        self.assertEqual(nav_guard["breaker_code"], CB_SAFETY_GUARD)
        self.assertEqual(exec_irr.actions, [])

        # 2. Safety keyword guardrail on reversible tier -> CB_SAFETY_GUARD
        exec_kw = FakeFsmExecutor([f0])
        engine_kw = SystemOneHostEngine(
            exec_kw,
            ScriptedSystemOneClient(
                lambda *_: {
                    "stop": {"type": "noul", "noul": 0.96},
                    "target_box": {"type": "box", "box": {"top": 0.4, "bottom": 0.5, "left": 0.4, "right": 0.6}, "confidence": 0.95},
                }
            ),
            sleep_fn=clock.sleep,
            monotonic_fn=clock.monotonic,
        )
        res_kw = engine_kw.execute_tool(
            "loop_until",
            {
                "stop_predicate": {"type": "noul", "instructions": "Confirm purchase button visible?"},
                "step_action": {"type": "key_press", "key": "Down"},
                "on_match": {"mode": "click_target_box", "target_description": "Confirm Purchase button"},
                "risk_tier": "reversible",
            },
        )
        self.assertEqual(res_kw["breaker_code"], CB_SAFETY_GUARD)
        self.assertEqual(exec_kw.actions, [])

        # 3. Interrupt predicate >= 0.78 -> CB_INTERRUPT
        exec_intr = FakeFsmExecutor([f0])
        engine_intr = SystemOneHostEngine(
            exec_intr,
            ScriptedSystemOneClient(lambda *_: {"stop": {"type": "noul", "noul": 0.1}, "interrupt_0": {"type": "noul", "noul": 0.81}}),
            sleep_fn=clock.sleep,
            monotonic_fn=clock.monotonic,
        )
        res_intr = engine_intr.execute_tool(
            "loop_until",
            {
                "stop_predicate": {"type": "noul", "instructions": "Target visible?"},
                "step_action": {"type": "key_press", "key": "Down"},
                "interrupt_predicates": [{"type": "noul", "instructions": "Is a modal error popup blocking?"}],
            },
        )
        self.assertEqual(res_intr["breaker_code"], CB_INTERRUPT)
        self.assertEqual(exec_intr.actions, [])

        # 4. Full-screen SSIM < 0.50 -> CB_INTERRUPT
        exec_ssim = FakeFsmExecutor([f0, f_scene_drop])
        engine_ssim = SystemOneHostEngine(
            exec_ssim,
            ScriptedSystemOneClient(lambda *_: {"stop": {"type": "noul", "noul": 0.1}}),
            sleep_fn=clock.sleep,
            monotonic_fn=clock.monotonic,
        )
        res_ssim = engine_ssim.execute_tool(
            "loop_until",
            {
                "stop_predicate": {"type": "noul", "instructions": "Target visible?"},
                "step_action": {"type": "key_press", "key": "Down"},
            },
        )
        self.assertEqual(res_ssim["breaker_code"], CB_INTERRUPT)

    def test_loop_interrupts_on_the_gray_zone_retry_frame(self) -> None:
        executor = FakeFsmExecutor([make_rgb_frame()])
        clock = FakeClock()

        def respond(_, state, questions, assets):
            if "retry_pred" in questions:
                interrupt_question = questions["interrupt_0"]["instructions"]
                self.assertIn("`interrupt_view_0`", interrupt_question)
                return {"retry_pred": {"noul": 0.99}, "interrupt_0": {"noul": 0.99}}
            return {"stop": {"noul": 0.75}, "interrupt_0": {"noul": 0.01}}

        engine = SystemOneHostEngine(
            executor, ScriptedSystemOneClient(respond),
            sleep_fn=clock.sleep, monotonic_fn=clock.monotonic,
        )
        result = engine.run_loop_until({
            "stop_predicate": {"type": "noul", "instructions": "Ready?"},
            "step_action": {"type": "key_press", "key": "Down"},
            "on_match": {"mode": "action", "action": {"type": "click", "x": 500, "y": 500}},
            "interrupt_predicates": [{"type": "noul", "instructions": "Is the queue paused?", "roi": [100, 100, 200, 400]}],
        })
        self.assertEqual(result["breaker_code"], CB_INTERRUPT)
        self.assertIn("during retry", result["detail"])
        self.assertEqual(executor.actions, [])

    def test_batch_apply_parallel_eval_and_drift_breakers(self) -> None:
        # 2x2 grid on 800x600 screen, 2 items match -> 1 S1 call evaluates all 4 slots in parallel
        f0 = make_rgb_frame(800, 600, bg=(245, 245, 245), frame_seq=1)
        f1 = make_rgb_frame(800, 600, bg=(245, 245, 245), rects=[(100, 100, 200, 200, (10, 180, 10))], frame_seq=2)
        f2 = make_rgb_frame(800, 600, bg=(245, 245, 245), rects=[(500, 400, 600, 500, (10, 180, 10))], frame_seq=3)

        exec_stub = FakeFsmExecutor([f0, f1, f2])
        clock = FakeClock()

        def responder(call_idx: int, state: Dict[str, Any], questions: Dict[str, Any], assets: Any) -> Dict[str, Any]:
            if "confirm_noul" in questions:
                return {"confirm_noul": {"type": "noul", "noul": 0.96}}
            # First call evaluates all 4 slots in ONE request
            self.assertEqual(len(questions), 4)
            return {
                "eval_slot_0": {"type": "noul", "noul": 0.95},
                "eval_slot_1": {"type": "noul", "noul": 0.05},
                "eval_slot_2": {"type": "noul", "noul": 0.10},
                "eval_slot_3": {"type": "noul", "noul": 0.94},
            }

        engine = SystemOneHostEngine(exec_stub, ScriptedSystemOneClient(responder), sleep_fn=clock.sleep, monotonic_fn=clock.monotonic)
        res = engine.execute_tool(
            "batch_apply",
            {
                "item_predicate": {"type": "noul", "instructions": "Is this checkbox unchecked?"},
                "item_action": "click_relative",
                "locate_mode": "fixed_slots",
                "slot_grid": {"rows": 2, "cols": 2},
                "target_description": "unchecked item",
                "risk_tier": "reversible",
                "verify_after_action": True,
            },
        )
        self.assertEqual(res["status"], "completed")
        self.assertIsNone(res["breaker_code"])
        self.assertEqual(res["outcome"]["batch_summary"]["items_evaluated"], 4)
        self.assertEqual(res["outcome"]["batch_summary"]["items_matched"], 2)
        self.assertEqual(res["outcome"]["batch_summary"]["mutations_executed"], 2)
        # 1 batched page eval + 2 20%-crop confirmations = 3 S1 calls
        self.assertEqual(res["metrics"]["s1_calls"], 3)

        # 2 consecutive zero-delta clicks -> CB_BATCH_DRIFT
        exec_nodelta = FakeFsmExecutor([f0, f0, f0])
        engine_nodelta = SystemOneHostEngine(
            exec_nodelta,
            ScriptedSystemOneClient(responder),
            sleep_fn=clock.sleep,
            monotonic_fn=clock.monotonic,
        )
        res_nodelta = engine_nodelta.execute_tool(
            "batch_apply",
            {
                "item_predicate": {"type": "noul", "instructions": "Is this checkbox unchecked?"},
                "item_action": "click_relative",
                "locate_mode": "fixed_slots",
                "slot_grid": {"rows": 2, "cols": 2},
                "target_description": "unchecked item",
                "risk_tier": "reversible",
                "verify_after_action": True,
            },
        )
        self.assertEqual(res_nodelta["breaker_code"], CB_BATCH_DRIFT)

        # Error ratio > 0.25 -> CB_BATCH_DRIFT
        exec_err = FakeFsmExecutor([f0])
        def err_responder(call_idx: int, state: Dict[str, Any], questions: Dict[str, Any], assets: Any) -> Dict[str, Any]:
            if "retry_pred" in questions:
                return {"retry_pred": {"type": "noul", "noul": 0.72}}
            return {
                "eval_slot_0": {"type": "noul", "noul": 0.72},
                "eval_slot_1": {"type": "noul", "noul": 0.72},
                "eval_slot_2": {"type": "noul", "noul": 0.05},
                "eval_slot_3": {"type": "noul", "noul": 0.05},
            }
        engine_err = SystemOneHostEngine(exec_err, ScriptedSystemOneClient(err_responder), sleep_fn=clock.sleep, monotonic_fn=clock.monotonic)
        res_err = engine_err.execute_tool(
            "batch_apply",
            {
                "item_predicate": {"type": "noul", "instructions": "Checked?"},
                "item_action": "noop_collect",
                "locate_mode": "fixed_slots",
                "slot_grid": {"rows": 2, "cols": 2},
                "max_error_ratio": 0.25,
            },
        )
        self.assertEqual(res_err["breaker_code"], CB_BATCH_DRIFT)

    def test_batch_returns_before_clicking_a_reordered_slot(self) -> None:
        original = make_rgb_frame()
        reordered = make_rgb_frame(rects=[(400, 0, 799, 599, (0, 0, 0))], frame_seq=2)
        executor = FakeFsmExecutor([original, reordered])
        clock = FakeClock()

        def respond(_, state, questions, assets):
            return {name: {"noul": 0.99} for name in questions}

        engine = SystemOneHostEngine(
            executor, ScriptedSystemOneClient(respond),
            sleep_fn=clock.sleep, monotonic_fn=clock.monotonic,
        )
        result = engine.run_batch_apply({
            "item_predicate": {"type": "noul", "instructions": "Is the item eligible?"},
            "item_action": "click_relative",
            "slot_grid": {"rows": 1, "cols": 2},
        })
        self.assertEqual(result["breaker_code"], CB_BATCH_DRIFT)
        self.assertEqual(result["outcome"]["batch_summary"]["mutations_executed"], 1)
        self.assertEqual(len(executor.actions), 1)

    def test_end_to_end_run_agent_with_system_one(self) -> None:
        # Spin up a tiny Unix domain socket stub speaking AgentSocket JSON framing
        sock_path = os.path.join(self.tmp, "agent.sock")
        frame = make_rgb_frame(640, 480, bg=(235, 235, 235), rects=[(200, 160, 440, 320, (30, 190, 60))], frame_seq=1)
        stop_event = threading.Event()

        def sock_server() -> None:
            srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            srv.bind(sock_path)
            srv.listen(4)
            srv.settimeout(0.2)
            while not stop_event.is_set():
                try:
                    conn, _ = srv.accept()
                except socket.timeout:
                    continue
                except OSError:
                    break
                with conn:
                    while not stop_event.is_set():
                        try:
                            hdr_len_b = conn.recv(8)
                        except OSError:
                            break
                        if len(hdr_len_b) < 8:
                            break
                        hlen, plen = struct.unpack("!II", hdr_len_b)
                        hraw = conn.recv(hlen)
                        if plen:
                            conn.recv(plen)
                        req = json.loads(hraw.decode("utf-8"))
                        cmd = req.get("cmd")
                        if cmd == "GET_STATUS":
                            rh = {
                                "id": req["id"],
                                "ok": True,
                                "connected": True,
                                "ready": True,
                                "width": frame.width,
                                "height": frame.height,
                                "desktop_name": "stub",
                                "frame_seq": 1,
                                "damage_count": 1,
                                "resize_count": 0,
                                "updating": False,
                                "has_clipboard": False,
                            }
                            rb = json.dumps(rh).encode("utf-8")
                            conn.sendall(struct.pack("!II", len(rb), 0) + rb)
                        elif cmd == "GET_FRAME":
                            rh = {
                                "id": req["id"],
                                "ok": True,
                                "changed": True,
                                "width": frame.width,
                                "height": frame.height,
                                "frame_seq": 1,
                            }
                            rb = json.dumps(rh).encode("utf-8")
                            conn.sendall(struct.pack("!II", len(rb), len(frame.rgb_bytes)) + rb + frame.rgb_bytes)
                        else:
                            rh = {"id": req["id"], "ok": True}
                            rb = json.dumps(rh).encode("utf-8")
                            conn.sendall(struct.pack("!II", len(rb), 0) + rb)
            srv.close()

        t = threading.Thread(target=sock_server, daemon=True)
        t.start()

        gemini_backend = MockGeminiBackend()
        gemini_srv = start_mock_gemini_server(gemini_backend)
        ts_srv = MockTypeSafeServer(
            lambda req, hdrs, parts: (
                200,
                {
                    "model": "jev-latest",
                    "answers": {
                        "condition": {"type": "noul", "noul": 0.72},
                        "retry_pred": {"type": "noul", "noul": 0.72},
                    },
                },
            )
        )

        def scripted_gemini(req_body: Dict[str, Any]) -> Tuple[int, Dict[str, Any]]:
            turn_num = len(gemini_backend.interaction_requests)
            if turn_num == 1:
                return 200, {
                    "id": "int_1",
                    "status": "requires_action",
                    "outputs": [
                        {
                            "type": "function_call",
                            "id": "call_1",
                            "name": "wait_until",
                            "arguments": {
                                "condition": {"type": "noul", "instructions": "Is the badge ready?"},
                                "settle_delay_ms": 0,
                                "intent": "Wait for badge",
                            },
                        },
                        {
                            "type": "function_call",
                            "id": "call_2",
                            "name": "click",
                            "arguments": {"x": 500, "y": 500},
                        },
                    ],
                }
            return 200, {
                "id": "int_2",
                "status": "completed",
                "outputs": [{"type": "text", "text": "Escalated ambiguity cleanly."}],
            }

        gemini_backend.scripted_handler = scripted_gemini

        gemini_key = os.path.join(self.tmp, "gemini.key")
        ts_key = os.path.join(self.tmp, "typesafe.key")
        with open(gemini_key, "w", encoding="utf-8") as f:
            f.write("gem_test_key\n")
        with open(ts_key, "w", encoding="utf-8") as f:
            f.write("ts_test_key\n")
        log_dir = os.path.join(self.tmp, "logs")

        args = argparse.Namespace(
            prompt="Wait for badge",
            socket=sock_path,
            vnc_host="localhost:5900",
            vncviewer_bin="vncviewer",
            legacy_clipboard_encoding=None,
            api_key_file=gemini_key,
            model="gemini-4-hydrogen",
            max_turns=5,
            log_dir=log_dir,
            workspace=os.path.join(self.tmp, "ws"),
            assistant_model="gemini-3.8-flash",
            assistant_thinking_level="default",
            assistant_max_turns=4,
            no_assistant=True,
            no_web_search=True,
            file_search_store=None,
            thinking_level="low",
            recovery_thinking_level="high",
            exclude_actions=None,
            no_custom_tools=False,
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
            ftp_timeout=10.0,
            typesafe_key_file=ts_key,
            system_one_model="jev-latest",
            system_one_url=ts_srv.base_url,
            system_one=True,
            no_system_one=False,
        )

        try:
            with mock.patch("vnc_agent.interactions.INTERACTIONS_URL", f"http://127.0.0.1:{gemini_srv.server_port}/v1beta/interactions"), \
                 mock.patch("vnc_agent.runner.InteractionsClient", side_effect=lambda **kw: agent_mod.InteractionsClient(base_url=f"http://127.0.0.1:{gemini_srv.server_port}/v1beta/interactions", **kw)):
                rc = agent_mod.run_agent(args)
            self.assertEqual(rc, 0)
            self.assertEqual(len(gemini_backend.interaction_requests), 2)

            # Turn 2 sends function_result with 2 visual_evidence images and escalated thinking_level="high"
            req2 = gemini_backend.interaction_requests[1]
            self.assertEqual(req2.get("generation_config", {}).get("thinking_level"), "high")
            fres = req2["input"][0]
            self.assertEqual(fres["name"], "wait_until")
            img_blocks = [b for b in fres["result"] if b.get("type") == "image"]
            self.assertEqual(len(img_blocks), 2)
            txt_payload = json.loads(fres["result"][0]["text"])
            self.assertEqual(txt_payload["breaker_code"], CB_AMBIGUITY)
            self.assertNotIn("_visual_evidence_images", txt_payload)
            skipped = json.loads(req2["input"][1]["result"][0]["text"])
            self.assertEqual(skipped["status"], "skipped_pending_visual_verification")
        finally:
            stop_event.set()
            t.join(timeout=2.0)
            gemini_srv.shutdown()
            gemini_srv.server_close()
            ts_srv.stop()


if __name__ == "__main__":
    unittest.main()
