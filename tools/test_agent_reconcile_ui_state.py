#!/usr/bin/env python3
"""Unit and end-to-end tests for L2 composite ``reconcile_ui_state`` controller."""

import argparse
import json
import os
import shutil
import socket
import struct
import sys
import threading
import unittest
from typing import Any, Callable, Dict, List, Optional, Sequence, Tuple
from unittest import mock

from PIL import Image, ImageDraw

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

from test_agent_system_one import (  # noqa: E402
    FakeClock,
    FakeFsmExecutor,
    MockTypeSafeServer,
    ScriptedSystemOneClient,
    make_rgb_frame,
)
from test_agent_workspace_assistant import MockGeminiBackend, start_mock_gemini_server  # noqa: E402
import vnc_computer_use_agent as agent_mod  # noqa: E402
from vnc_agent import (  # noqa: E402
    CB_AMBIGUITY,
    CB_BUDGET_EXCEEDED,
    CB_DEPENDENCY_BLOCKED,
    CB_INTERRUPT,
    CB_OSCILLATION,
    CB_SAFETY_GUARD,
    CB_VALIDATION_ERROR,
    FrameSnapshot,
    SYSTEM_ONE_FUNCTION_DECLARATIONS,
    SYSTEM_ONE_FUNCTION_NAMES,
    SystemOneHostEngine,
    VALID_FIELD_STATES,
    VncDesktopExecutor,
    build_tools_config,
    check_reconcile_safety_guard,
    compute_ssim_excluding_roi,
    extract_roi_text_ocr,
    normalize_reconcile_fields,
)


class ReconcileFakeExecutor(FakeFsmExecutor):
    """Fake executor with programmable clipboard, keystroke typing, and frame updates."""

    def __init__(self, frames: List[FrameSnapshot]) -> None:
        super().__init__(frames)
        self.clipboard_read_enabled = True
        self.clipboard_paste_works = True
        self.clipboard_values: List[Tuple[bool, str]] = []
        self.typed_texts: List[str] = []
        self.pasted_texts: List[str] = []
        self.copy_calls = 0
        self.paste_calls = 0
        self.type_calls = 0
        self.clipboard_cleared_count = 0
        self.last_clipboard_text: Optional[str] = None
        self.on_clipboard_paste_cb: Optional[Callable[[str, "ReconcileFakeExecutor"], None]] = None
        self.on_type_cb: Optional[Callable[[str, "ReconcileFakeExecutor"], None]] = None

    def copy_focused_text(self, timeout: float = 1.0) -> Tuple[bool, str]:
        self.copy_calls += 1
        if not self.clipboard_read_enabled:
            return False, ""
        if self.clipboard_values:
            return self.clipboard_values.pop(0)
        return True, self.last_clipboard_text or ""

    def overwrite_focused_text_clipboard(self, text: str) -> bool:
        self.paste_calls += 1
        self.pasted_texts.append(text)
        if not self.clipboard_paste_works:
            return False
        self.last_clipboard_text = text
        if self.on_clipboard_paste_cb is not None:
            self.on_clipboard_paste_cb(text, self)
        else:
            self.advance_frame()
        return True

    def type_focused_text(self, text: str) -> None:
        self.type_calls += 1
        self.typed_texts.append(text)
        self.last_clipboard_text = text
        if self.on_type_cb is not None:
            self.on_type_cb(text, self)
        else:
            self.advance_frame()

    def clear_clipboard_secret(self) -> None:
        self.clipboard_cleared_count += 1
        self.last_clipboard_text = ""


class ReconcileUiStateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = os.path.join("/tmp", f"tigervnc_reconcile_test_{os.getpid()}_{id(self)}")
        os.makedirs(self.tmp, exist_ok=True)

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    # 1. Schema, DAG waves, secret_text constraint, HostAction validation, max_reconcile_passes cap
    def test_schema_validation_dag_waves_and_constraints(self) -> None:
        self.assertIn("reconcile_ui_state", SYSTEM_ONE_FUNCTION_NAMES)
        forbidden = {"default", "anyOf", "oneOf", "allOf", "additionalProperties"}
        allowed_types = {"string", "integer", "number", "boolean", "array", "object"}
        decl = next(d for d in SYSTEM_ONE_FUNCTION_DECLARATIONS if d["name"] == "reconcile_ui_state")
        params = decl["parameters"]
        self.assertFalse(forbidden.intersection(params.keys()))
        for pname, pdef in params["properties"].items():
            self.assertIn(pdef["type"], allowed_types, pname)
            self.assertFalse(forbidden.intersection(pdef.keys()), pname)

        tools = build_tools_config(False, [], enable_custom_tools=True, enable_system_one_tools=True)
        self.assertIn("reconcile_ui_state", {t["name"] for t in tools if t.get("type") == "function"})

        # Topological DAG wave grouping
        specs, waves = normalize_reconcile_fields(
            [
                {"id": "c", "control_type": "text", "locator_prompt": "C", "desired_value": "v", "depends_on": ["b"]},
                {"id": "a", "control_type": "checkbox", "locator_prompt": "A", "desired_value": True},
                {"id": "b", "control_type": "dropdown", "locator_prompt": "B", "desired_value": "X", "depends_on": ["a"]},
                {"id": "d", "control_type": "toggle", "locator_prompt": "D", "desired_value": False},
            ]
        )
        self.assertEqual([s.field_id for s in specs], ["c", "a", "b", "d"])
        self.assertEqual(waves, [["a", "d"], ["b"], ["c"]])

        # Cycle rejection
        with self.assertRaises(ValueError):
            normalize_reconcile_fields(
                [
                    {"id": "x", "control_type": "checkbox", "locator_prompt": "X", "desired_value": True, "depends_on": ["y"]},
                    {"id": "y", "control_type": "checkbox", "locator_prompt": "Y", "desired_value": True, "depends_on": ["x"]},
                ]
            )

        # Missing dependency ID rejection
        with self.assertRaises(ValueError):
            normalize_reconcile_fields(
                [{"id": "x", "control_type": "checkbox", "locator_prompt": "X", "desired_value": True, "depends_on": ["missing"]}]
            )

        # secret_text rejects exact/semantic match_mode
        with self.assertRaises(ValueError):
            normalize_reconcile_fields(
                [{"id": "pw", "control_type": "secret_text", "locator_prompt": "Password", "desired_value": "s3cr3t", "match_mode": "exact"}]
            )

        # Reject non-closed-set blur_action / scroll_action
        exec_stub = ReconcileFakeExecutor([make_rgb_frame()])
        engine = SystemOneHostEngine(exec_stub, ScriptedSystemOneClient(lambda *_: {}))
        with self.assertRaises(ValueError):
            engine.execute_tool(
                "reconcile_ui_state",
                {
                    "fields": [{"id": "a", "control_type": "checkbox", "locator_prompt": "A", "desired_value": True}],
                    "blur_action": {"type": "eval", "script": "bad()"},
                },
            )

    # 2. Outside-ROI SSIM & local OCR
    def test_outside_roi_ssim_and_local_ocr(self) -> None:
        base = make_rgb_frame(400, 300, bg=(240, 240, 240), rects=[(50, 50, 150, 150, (40, 40, 40))])
        inside_edit = make_rgb_frame(
            400, 300, bg=(240, 240, 240), rects=[(50, 50, 150, 150, (200, 20, 20)), (160, 160, 210, 210, (10, 120, 220))]
        )
        # Edit strictly inside [500, 350, 750, 600] -> pixel box approx (140..240, 150..225)
        edit_in_roi = make_rgb_frame(
            400, 300, bg=(240, 240, 240), rects=[(50, 50, 150, 150, (40, 40, 40)), (160, 160, 210, 210, (10, 120, 220))]
        )
        # Box covering (140..240, 150..225) in 0..1000: y=[500..750], x=[350..600]
        self.assertEqual(
            compute_ssim_excluding_roi(base, edit_in_roi, container_roi=[0, 0, 1000, 1000], exclude_roi=[500, 350, 750, 600]),
            1.0,
        )
        # Unannounced outside shift drops outside SSIM < 0.95
        shift_stripes = [(x, 0, x + 10, 140, (20, 20, 20) if (x // 10) % 2 == 0 else (230, 230, 230)) for x in range(0, 400, 10)]
        outside_shift = make_rgb_frame(
            400, 300, bg=(240, 240, 240), rects=[(50, 50, 150, 150, (40, 40, 40)), *shift_stripes]
        )
        self.assertLess(
            compute_ssim_excluding_roi(base, outside_shift, container_roi=[0, 0, 1000, 1000], exclude_roi=[500, 350, 750, 600]),
            0.95,
        )
        # Pure-Python fallback check
        with mock.patch("vnc_agent.system_one_client.np", None):
            self.assertEqual(
                compute_ssim_excluding_roi(base, edit_in_roi, container_roi=[0, 0, 1000, 1000], exclude_roi=[500, 350, 750, 600]),
                1.0,
            )
            self.assertLess(
                compute_ssim_excluding_roi(base, outside_shift, container_roi=[0, 0, 1000, 1000], exclude_roi=[500, 350, 750, 600]),
                0.95,
            )

        # Missing tesseract binary gracefully returns None
        with mock.patch("vnc_agent.system_one_client.shutil.which", return_value=None):
            self.assertIsNone(extract_roi_text_ocr(base, [100, 100, 200, 400]))

        # Live smoke test when tesseract is installed
        if shutil.which("tesseract"):
            img = Image.new("RGB", (400, 120), (255, 255, 255))
            draw = ImageDraw.Draw(img)
            draw.text((40, 40), "4200", fill=(0, 0, 0))
            snap = FrameSnapshot(400, 120, img.tobytes(), frame_seq=1)
            ocr_out = extract_roi_text_ocr(snap, [0, 0, 1000, 1000])
            self.assertIsNotNone(ocr_out)

    # 3. Safety guard hard-blocking
    def test_safety_guard_blocks_unsafe_fields_without_input(self) -> None:
        base = make_rgb_frame(800, 600)
        unsafe_cases = [
            {"id": "auto_save", "control_type": "toggle", "locator_prompt": "Auto-save toggle", "desired_value": True, "risk_level": "irreversible"},
            {"id": "submit_btn", "control_type": "checkbox", "locator_prompt": "Final Submit Form", "desired_value": True},
            {"id": "pay_now", "control_type": "checkbox", "locator_prompt": "Pay Now button", "desired_value": True},
            {"id": "del_acct", "control_type": "checkbox", "locator_prompt": "Delete Account", "desired_value": True},
        ]
        for bad_field in unsafe_cases:
            exec_stub = ReconcileFakeExecutor([base])
            client = ScriptedSystemOneClient(lambda *_: {})
            engine = SystemOneHostEngine(exec_stub, client)
            res = engine.execute_tool(
                "reconcile_ui_state",
                {
                    "fields": [
                        bad_field,
                        {
                            "id": "child",
                            "control_type": "text",
                            "locator_prompt": "Child note",
                            "desired_value": "ok",
                            "depends_on": [bad_field["id"]],
                        },
                    ]
                },
            )
            self.assertEqual(res["status"], "blocked")
            self.assertEqual(res["breaker_code"], CB_SAFETY_GUARD)
            self.assertEqual(res["metrics"]["s1_calls"], 0)
            self.assertEqual(exec_stub.actions, [])
            self.assertEqual(res["visual_evidence"], ["final_fullscreen", "annotated_roi_crop"])
            rep = {f["id"]: f for f in res["outcome"]["reconcile_summary"]["fields"]}
            self.assertEqual(rep[bad_field["id"]]["status"], "uncertain")
            self.assertEqual(rep["child"]["status"], "skipped_dependency")

    # 4. DAG waves + batched binary observation, idempotent skip, pixel-diff + choice verify, disabled block
    def test_binary_controls_dag_batch_idempotent_and_disabled(self) -> None:
        # Frame 0: cb1 checked (already_matched), cb2 unchecked (needs click), cb3 disabled
        # Frame 1: after clicking cb2 -> cb2 pixel change + choice=checked
        f0 = make_rgb_frame(800, 600, bg=(240, 240, 240), rects=[(80, 120, 160, 180, (20, 20, 20))], frame_seq=1)
        f1 = make_rgb_frame(
            800, 600, bg=(240, 240, 240), rects=[(80, 120, 160, 180, (20, 20, 20)), (480, 120, 560, 180, (10, 180, 30))], frame_seq=2
        )
        exec_stub = ReconcileFakeExecutor([f0, f1])
        clock = FakeClock()

        def responder(call_idx: int, state: Dict[str, Any], questions: Dict[str, Any], assets: Any) -> Dict[str, Any]:
            if "confirm_noul" in questions:
                return {
                    "confirm_noul": {"type": "noul", "noul": 0.96},
                    "confirm_box": {"type": "box", "box": {"top": 0.2, "bottom": 0.8, "left": 0.2, "right": 0.8}, "confidence": 0.94},
                }
            out: Dict[str, Any] = {}
            if "eval_cb1" in questions:
                out["eval_cb1"] = {"type": "choice", "choice": "checked", "confidence": 0.95}
            if "box_cb1" in questions:
                out["box_cb1"] = {"type": "box", "box": {"top": 0.2, "bottom": 0.7, "left": 0.2, "right": 0.7}, "confidence": 0.94}
            if "eval_cb2" in questions:
                cb2_state = "unchecked" if len(exec_stub.actions) == 0 else "checked"
                out["eval_cb2"] = {"type": "choice", "choice": cb2_state, "confidence": 0.96}
            if "box_cb2" in questions:
                out["box_cb2"] = {"type": "box", "box": {"top": 0.2, "bottom": 0.7, "left": 0.2, "right": 0.7}, "confidence": 0.94}
            if "eval_cb3" in questions:
                out["eval_cb3"] = {"type": "choice", "choice": "disabled", "confidence": 0.95}
            if "box_cb3" in questions:
                out["box_cb3"] = {"type": "box", "box": {"top": 0.2, "bottom": 0.7, "left": 0.2, "right": 0.7}, "confidence": 0.94}
            if "eval_child_ok" in questions:
                out["eval_child_ok"] = {"type": "choice", "choice": "checked", "confidence": 0.96}
            if "box_child_ok" in questions:
                out["box_child_ok"] = {"type": "box", "box": {"top": 0.2, "bottom": 0.7, "left": 0.2, "right": 0.7}, "confidence": 0.94}
            return out

        engine = SystemOneHostEngine(exec_stub, ScriptedSystemOneClient(responder), sleep_fn=clock.sleep, monotonic_fn=clock.monotonic)
        res = engine.execute_tool(
            "reconcile_ui_state",
            {
                "fields": [
                    {"id": "cb1", "control_type": "checkbox", "locator_prompt": "CB1", "roi": [100, 50, 350, 300], "desired_value": True},
                    {"id": "cb2", "control_type": "toggle", "locator_prompt": "CB2", "roi": [100, 550, 350, 800], "desired_value": True},
                    {"id": "cb3", "control_type": "radio", "locator_prompt": "CB3", "roi": [400, 50, 600, 300], "desired_value": True, "required": True},
                    {"id": "child_ok", "control_type": "checkbox", "locator_prompt": "Child OK", "roi": [400, 550, 600, 800], "desired_value": True, "depends_on": ["cb2"]},
                    {"id": "child_blocked", "control_type": "checkbox", "locator_prompt": "Child Blocked", "roi": [650, 550, 850, 800], "desired_value": True, "depends_on": ["cb3"]},
                ],
                "settle_delay_ms": 0,
            },
        )
        self.assertEqual(res["breaker_code"], CB_DEPENDENCY_BLOCKED)
        self.assertEqual(res["status"], "dependency_blocked")
        # Only 1 click (on cb2); cb1 was skipped idempotently, cb3 blocked as disabled, child_blocked never clicked
        self.assertEqual(len(exec_stub.actions), 1)
        # Only 1 image (final_fullscreen) since no failed_validation, uncertain, or CB_SAFETY_GUARD!
        self.assertEqual(res["visual_evidence"], ["final_fullscreen"])
        rep = {f["id"]: f for f in res["outcome"]["reconcile_summary"]["fields"]}
        self.assertEqual(rep["cb1"]["status"], "already_matched")
        self.assertEqual(rep["cb2"]["status"], "reconciled")
        self.assertEqual(rep["cb3"]["status"], "skipped_dependency")
        self.assertEqual(rep["child_ok"]["status"], "already_matched")
        self.assertEqual(rep["child_blocked"]["status"], "skipped_dependency")

    # 5. triggers_relayout -> wait_until + invalidate downstream cache + missing downstream CB_DEPENDENCY_BLOCKED
    def test_relayout_invalidation_and_missing_downstream_control(self) -> None:
        f0 = make_rgb_frame(800, 600, bg=(240, 240, 240), frame_seq=1)
        f1 = make_rgb_frame(800, 600, bg=(240, 240, 240), rects=[(60, 40, 120, 80, (20, 160, 40))], frame_seq=2)
        f2_settled = make_rgb_frame(
            800, 600, bg=(240, 240, 240), rects=[(60, 40, 120, 80, (20, 160, 40)), (100, 350, 200, 400, (10, 10, 10))], frame_seq=3
        )
        exec_stub = ReconcileFakeExecutor([f0, f1, f2_settled])
        clock = FakeClock()
        wait_calls: List[Dict[str, Any]] = []

        def responder(call_idx: int, state: Dict[str, Any], questions: Dict[str, Any], assets: Any) -> Dict[str, Any]:
            if "condition" in questions:
                wait_calls.append(dict(questions))
                return {"condition": {"type": "noul", "noul": 0.95}}
            if "confirm_noul" in questions:
                return {
                    "confirm_noul": {"type": "noul", "noul": 0.95},
                    "confirm_box": {"type": "box", "box": {"top": 0.2, "bottom": 0.8, "left": 0.2, "right": 0.8}, "confidence": 0.95},
                }
            if "eval_parent" in questions:
                return {
                    "eval_parent": {"type": "choice", "choice": "unchecked" if len(exec_stub.actions) == 0 else "checked", "confidence": 0.96},
                    "box_parent": {"type": "box", "box": {"top": 0.1, "bottom": 0.3, "left": 0.1, "right": 0.3}, "confidence": 0.95},
                }
            if "eval_missing_child" in questions:
                return {
                    "eval_missing_child": {"type": "choice", "choice": "unchecked", "confidence": 0.90},
                    "box_missing_child": {"type": "box", "box": {"top": 0.1, "bottom": 0.2, "left": 0.1, "right": 0.2}, "confidence": 0.20},
                }
            return {}

        engine = SystemOneHostEngine(exec_stub, ScriptedSystemOneClient(responder), sleep_fn=clock.sleep, monotonic_fn=clock.monotonic)
        res = engine.execute_tool(
            "reconcile_ui_state",
            {
                "fields": [
                    {"id": "parent", "control_type": "toggle", "locator_prompt": "Parent toggle", "roi": [50, 50, 300, 350], "desired_value": True, "triggers_relayout": True},
                    {"id": "missing_child", "control_type": "checkbox", "locator_prompt": "Appearing child", "roi": [400, 50, 700, 400], "desired_value": True, "depends_on": ["parent"]},
                ],
                "allow_scroll": True,
                "settle_delay_ms": 0,
            },
        )
        self.assertGreaterEqual(len(wait_calls), 1)
        self.assertEqual(res["breaker_code"], CB_DEPENDENCY_BLOCKED)
        rep = {f["id"]: f for f in res["outcome"]["reconcile_summary"]["fields"]}
        self.assertEqual(rep["parent"]["status"], "reconciled")
        self.assertEqual(rep["missing_child"]["status"], "skipped_dependency")
        # Crucial: must NOT scroll blindly searching for missing downstream control
        self.assertEqual([a["type"] for a in exec_stub.actions], ["click"])

    # 6. Unannounced shift guard (outside SSIM < 0.95) invalidates remaining cached boxes and re-observes
    def test_unannounced_shift_invalidates_cached_boxes_and_reobserves(self) -> None:
        f0 = make_rgb_frame(800, 600, bg=(240, 240, 240), frame_seq=1)
        # Shifted region changes outside SSIM below 0.95 while keeping full-screen SSIM >= 0.50
        shift_stripes = [(x, 320, x + 20, 560, (120, 120, 120) if (x // 20) % 2 == 0 else (240, 240, 240)) for x in range(100, 700, 20)]
        f1_shifted = make_rgb_frame(
            800, 600, bg=(240, 240, 240), rects=[(80, 60, 160, 120, (20, 180, 40)), *shift_stripes], frame_seq=2
        )
        f2 = make_rgb_frame(
            800, 600, bg=(240, 240, 240), rects=[(80, 60, 160, 120, (20, 180, 40)), *shift_stripes, (100, 400, 220, 480, (20, 180, 40))], frame_seq=3
        )
        exec_stub = ReconcileFakeExecutor([f0, f1_shifted, f2])
        clock = FakeClock()
        second_box_queries = {"count": 0}

        def responder(call_idx: int, state: Dict[str, Any], questions: Dict[str, Any], assets: Any) -> Dict[str, Any]:
            if "confirm_noul" in questions:
                return {
                    "confirm_noul": {"type": "noul", "noul": 0.96},
                    "confirm_box": {"type": "box", "box": {"top": 0.2, "bottom": 0.8, "left": 0.2, "right": 0.8}, "confidence": 0.95},
                }
            if "loc" in questions:
                second_box_queries["count"] += 1
                # Shifted position inside field2 roi [500, 50, 900, 400]
                return {"loc": {"type": "box", "box": {"top": 0.42, "bottom": 0.70, "left": 0.20, "right": 0.55}, "confidence": 0.95}}
            out: Dict[str, Any] = {}
            if "eval_cb_a" in questions:
                cb_a_state = "unchecked" if len(exec_stub.actions) == 0 else "checked"
                out["eval_cb_a"] = {"type": "choice", "choice": cb_a_state, "confidence": 0.96}
            if "box_cb_a" in questions:
                out["box_cb_a"] = {"type": "box", "box": {"top": 0.2, "bottom": 0.5, "left": 0.2, "right": 0.5}, "confidence": 0.95}
            if "eval_cb_b" in questions:
                cb_b_state = "checked" if len(exec_stub.actions) >= 2 else "unchecked"
                out["eval_cb_b"] = {"type": "choice", "choice": cb_b_state, "confidence": 0.96}
            if "box_cb_b" in questions:
                out["box_cb_b"] = {"type": "box", "box": {"top": 0.1, "bottom": 0.3, "left": 0.2, "right": 0.5}, "confidence": 0.95}
            return out

        engine = SystemOneHostEngine(exec_stub, ScriptedSystemOneClient(responder), sleep_fn=clock.sleep, monotonic_fn=clock.monotonic)
        res = engine.execute_tool(
            "reconcile_ui_state",
            {
                "fields": [
                    {"id": "cb_a", "control_type": "checkbox", "locator_prompt": "CB A", "roi": [50, 50, 250, 350], "desired_value": True},
                    {"id": "cb_b", "control_type": "checkbox", "locator_prompt": "CB B", "roi": [500, 50, 900, 400], "desired_value": True},
                ],
                "settle_delay_ms": 0,
            },
        )
        self.assertEqual(res["status"], "completed")
        self.assertIsNone(res["breaker_code"])
        # After cb_a caused outside shift, cb_b's cached box was invalidated and re-located!
        self.assertGreaterEqual(second_box_queries["count"], 1)
        rep = {f["id"]: f for f in res["outcome"]["reconcile_summary"]["fields"]}
        self.assertEqual(rep["cb_b"]["status"], "reconciled")
        self.assertGreater(rep["cb_b"]["box"][0], 640)  # updated box at shifted coordinate (668 vs pre-shift 540)

    def test_executor_clipboard_and_text_reconciliation_helpers(self) -> None:
        calls: List[Tuple[str, Dict[str, Any]]] = []
        seq = {"val": 10}
        clip_text = {"val": "existing"}

        def request(cmd: str, **kwargs: Any) -> Tuple[Dict[str, Any], bytes]:
            calls.append((cmd, kwargs))
            if cmd == "CLIPBOARD_GET":
                return {"ok": True, "available": True, "sequence": seq["val"], "text": clip_text["val"]}, b""
            if cmd == "CLIPBOARD_SET":
                seq["val"] += 1
                clip_text["val"] = kwargs.get("text", "")
                return {"ok": True}, b""
            if cmd == "KEY_EVENT":
                # Simulate remote Ctrl+C updating clipboard on release of 'c' while Ctrl is held
                if not kwargs.get("down") and kwargs.get("keysym") == ord("c"):
                    seq["val"] += 1
                    clip_text["val"] = "copied-field-value"
                return {"ok": True}, b""
            if cmd == "TEXT_TYPE":
                return {"ok": True}, b""
            return {"ok": True}, b""

        ex = VncDesktopExecutor(mock.Mock(request=request))
        ok, copied = ex.copy_focused_text(timeout=0.3)
        self.assertTrue(ok)
        self.assertEqual(copied, "copied-field-value")

        self.assertTrue(ex.overwrite_focused_text_clipboard("new-value"))
        self.assertEqual(clip_text["val"], "new-value")

        ex.type_focused_text("ab")
        typed = [kw["text"] for c, kw in calls if c == "TEXT_TYPE"]
        self.assertEqual(typed, ["a", "b"])

        ex.clear_clipboard_secret()
        self.assertEqual(clip_text["val"], "")

    # 7. Scroll invalidation (never reuse cached BBox across scroll offsets)
    def test_scroll_invalidates_cached_boxes(self) -> None:
        f0 = make_rgb_frame(800, 600, bg=(240, 240, 240), frame_seq=1)
        f1_scrolled = make_rgb_frame(800, 600, bg=(240, 240, 240), rects=[(100, 200, 250, 260, (30, 140, 220))], frame_seq=2)
        f2_clicked = make_rgb_frame(800, 600, bg=(240, 240, 240), rects=[(100, 200, 250, 260, (20, 200, 40))], frame_seq=3)
        exec_stub = ReconcileFakeExecutor([f0, f1_scrolled, f2_clicked])
        clock = FakeClock()

        def responder(call_idx: int, state: Dict[str, Any], questions: Dict[str, Any], assets: Any) -> Dict[str, Any]:
            if "stop" in questions:
                return {"stop": {"type": "noul", "noul": 0.95 if len(exec_stub.actions) >= 1 else 0.05}}
            if "confirm_noul" in questions:
                return {
                    "confirm_noul": {"type": "noul", "noul": 0.96},
                    "confirm_box": {"type": "box", "box": {"top": 0.2, "bottom": 0.8, "left": 0.2, "right": 0.8}, "confidence": 0.95},
                }
            if "loc" in questions:
                if len(exec_stub.actions) == 0:
                    return {"loc": {"type": "box", "box": {"top": 0.1, "bottom": 0.2, "left": 0.1, "right": 0.2}, "confidence": 0.20}}
                return {"loc": {"type": "box", "box": {"top": 0.3, "bottom": 0.45, "left": 0.12, "right": 0.32}, "confidence": 0.95}}
            if "eval_cb" in questions:
                if len(exec_stub.actions) == 0:
                    return {
                        "eval_cb": {"type": "choice", "choice": "unchecked", "confidence": 0.92},
                        "box_cb": {"type": "box", "box": {"top": 0.1, "bottom": 0.2, "left": 0.1, "right": 0.2}, "confidence": 0.20},
                    }
                return {"eval_cb": {"type": "choice", "choice": "checked" if len(exec_stub.actions) >= 2 else "unchecked", "confidence": 0.95}}
            return {}

        engine = SystemOneHostEngine(exec_stub, ScriptedSystemOneClient(responder), sleep_fn=clock.sleep, monotonic_fn=clock.monotonic)
        res = engine.execute_tool(
            "reconcile_ui_state",
            {
                "fields": [{"id": "cb", "control_type": "checkbox", "locator_prompt": "Scrolled CB", "desired_value": True}],
                "allow_scroll": True,
                "settle_delay_ms": 0,
            },
        )
        self.assertEqual(res["status"], "completed")
        self.assertEqual([a["type"] for a in exec_stub.actions], ["wheel_scroll", "click"])
        # Click landed on post-scroll located box (~center y=375, x=220), NOT stale pre-scroll coordinates
        self.assertGreater(exec_stub.actions[1]["y"], 300)

    # 8. Dropdown (OCR + loop_until + collapsed verify) & Slider (loop_until directional)
    def test_dropdown_and_slider_delegation_to_loop_until(self) -> None:
        f0 = make_rgb_frame(800, 600, bg=(240, 240, 240), frame_seq=1)
        f1_open = make_rgb_frame(800, 600, bg=(240, 240, 240), rects=[(100, 100, 300, 350, (250, 250, 250))], frame_seq=2)
        f2_sel = make_rgb_frame(800, 600, bg=(240, 240, 240), rects=[(100, 100, 300, 140, (210, 230, 255))], frame_seq=3)
        f3_slider = make_rgb_frame(800, 600, bg=(240, 240, 240), rects=[(100, 100, 300, 140, (210, 230, 255)), (100, 420, 300, 460, (30, 120, 210))], frame_seq=4)
        exec_stub = ReconcileFakeExecutor([f0, f1_open, f2_sel, f2_sel, f3_slider])
        clock = FakeClock()

        def ocr_stub(frame: FrameSnapshot, roi: Sequence[int]) -> Optional[str]:
            return "Tokyo" if frame.frame_seq >= 3 else "Seoul"

        def responder(call_idx: int, state: Dict[str, Any], questions: Dict[str, Any], assets: Any) -> Dict[str, Any]:
            if "confirm_noul" in questions:
                return {
                    "confirm_noul": {"type": "noul", "noul": 0.96},
                    "confirm_box": {"type": "box", "box": {"top": 0.2, "bottom": 0.8, "left": 0.2, "right": 0.8}, "confidence": 0.95},
                }
            if "q" in questions:
                return {"q": {"type": "noul", "noul": 0.95 if exec_stub.current_frame().frame_seq >= 3 else 0.05}}
            if "eval_vol" in questions:
                vol_state = "matched" if any(a.get("key") == "Right" for a in exec_stub.actions) else "too_low"
                return {
                    "eval_vol": {"type": "choice", "choice": vol_state, "confidence": 0.96},
                    "box_vol": {"type": "box", "box": {"top": 0.70, "bottom": 0.80, "left": 0.12, "right": 0.38}, "confidence": 0.95},
                }
            if "loc" in questions:
                return {"loc": {"type": "box", "box": {"top": 0.16, "bottom": 0.24, "left": 0.12, "right": 0.38}, "confidence": 0.95}}
            if "stop" in questions:
                instr = questions["stop"]["instructions"]
                if "dropdown option" in instr:
                    return {
                        "stop": {"type": "noul", "noul": 0.95},
                        "target_box": {"type": "box", "box": {"top": 0.35, "bottom": 0.45, "left": 0.20, "right": 0.50}, "confidence": 0.95},
                    }
                # Slider stop predicate
                right_presses = sum(1 for a in exec_stub.actions if a["type"] == "key_press" and a["key"] == "Right")
                return {"stop": {"type": "choice", "choice": "matched" if right_presses >= 1 else "too_low", "confidence": 0.96}}
            return {}

        engine = SystemOneHostEngine(
            exec_stub,
            ScriptedSystemOneClient(responder),
            sleep_fn=clock.sleep,
            monotonic_fn=clock.monotonic,
            ocr_fn=ocr_stub,
        )
        res = engine.execute_tool(
            "reconcile_ui_state",
            {
                "fields": [
                    {"id": "city", "control_type": "dropdown", "locator_prompt": "City dropdown", "desired_value": "Tokyo"},
                    {"id": "vol", "control_type": "slider", "locator_prompt": "Volume slider", "desired_value": "80%"},
                ],
                "settle_delay_ms": 0,
            },
        )
        self.assertEqual(res["status"], "completed")
        rep = {f["id"]: f for f in res["outcome"]["reconcile_summary"]["fields"]}
        self.assertEqual(rep["city"]["status"], "reconciled")
        self.assertEqual(rep["city"]["observed_value"], "Tokyo")
        self.assertEqual(rep["vol"]["status"], "reconciled")
        self.assertIn("Right", [a["key"] for a in exec_stub.actions if a["type"] == "key_press"])

    # 9. text & number: clipboard preferred, OCR+Jev fallback, keystroke fallback only on paste failure, semantic & exact
    def test_text_and_number_clipboard_fallback_and_semantic_matching(self) -> None:
        f0 = make_rgb_frame(800, 600, bg=(240, 240, 240), frame_seq=1)
        f1 = make_rgb_frame(800, 600, bg=(240, 240, 240), rects=[(100, 100, 300, 140, (255, 255, 255))], frame_seq=2)

        def ok_responder(call_idx: int, state: Dict[str, Any], questions: Dict[str, Any], assets: Any) -> Dict[str, Any]:
            if "confirm_noul" in questions:
                return {
                    "confirm_noul": {"type": "noul", "noul": 0.96},
                    "confirm_box": {"type": "box", "box": {"top": 0.2, "bottom": 0.8, "left": 0.2, "right": 0.8}, "confidence": 0.95},
                }
            if "loc" in questions:
                return {"loc": {"type": "box", "box": {"top": 0.16, "bottom": 0.24, "left": 0.12, "right": 0.40}, "confidence": 0.95}}
            return {"q": {"type": "noul", "noul": 0.95}}

        # Case A: clipboard reads "$1,250.00" -> semantic number match -> already_matched, no paste/type!
        exec_sem = ReconcileFakeExecutor([f0])
        exec_sem.clipboard_values = [(True, "$1,250.00")]
        engine_sem = SystemOneHostEngine(exec_sem, ScriptedSystemOneClient(ok_responder))
        res_sem = engine_sem.execute_tool(
            "reconcile_ui_state",
            {
                "fields": [
                    {"id": "amt", "control_type": "number", "locator_prompt": "Amount", "desired_value": "1250", "match_mode": "semantic"}
                ],
                "settle_delay_ms": 0,
            },
        )
        self.assertEqual(res_sem["status"], "completed")
        self.assertEqual(res_sem["outcome"]["reconcile_summary"]["fields"][0]["status"], "already_matched")
        self.assertEqual(exec_sem.paste_calls, 0)
        self.assertEqual(exec_sem.type_calls, 0)

        # Case B: clipboard read fails -> falls back to OCR + Jev; paste fails -> falls back to keystrokes
        exec_fb = ReconcileFakeExecutor([f0, f1])
        exec_fb.clipboard_read_enabled = False
        exec_fb.clipboard_paste_works = False
        ocr_reads = ["old value", "Tokyo 日本語"]

        def fb_responder(call_idx: int, state: Dict[str, Any], questions: Dict[str, Any], assets: Any) -> Dict[str, Any]:
            if "confirm_noul" in questions or "loc" in questions:
                return ok_responder(call_idx, state, questions, assets)
            return {"q": {"type": "noul", "noul": 0.95 if exec_fb.type_calls >= 1 else 0.05}}

        engine_fb = SystemOneHostEngine(
            exec_fb,
            ScriptedSystemOneClient(fb_responder),
            ocr_fn=lambda _f, _r: ocr_reads.pop(0) if ocr_reads else "Tokyo 日本語",
        )
        res_fb = engine_fb.execute_tool(
            "reconcile_ui_state",
            {
                "fields": [
                    {"id": "note", "control_type": "text", "locator_prompt": "Note", "desired_value": "Tokyo 日本語", "match_mode": "exact"}
                ],
                "settle_delay_ms": 0,
            },
        )
        self.assertEqual(res_fb["status"], "completed")
        self.assertEqual(res_fb["outcome"]["reconcile_summary"]["fields"][0]["status"], "reconciled")
        self.assertEqual(exec_fb.paste_calls, 1)
        self.assertEqual(exec_fb.type_calls, 1)
        self.assertEqual(exec_fb.typed_texts, ["Tokyo 日本語"])

        # Case C: exact mode with clipboard unavailable and noisy OCR ('i') after typing '250' -> Jev crop noul confirms '250'
        exec_noisy = ReconcileFakeExecutor([f0, f1])
        exec_noisy.clipboard_read_enabled = False
        noisy_ocr_reads = ["50", "i"]

        def noisy_responder(call_idx: int, state: Dict[str, Any], questions: Dict[str, Any], assets: Any) -> Dict[str, Any]:
            if "confirm_noul" in questions or "loc" in questions:
                return ok_responder(call_idx, state, questions, assets)
            return {"q": {"type": "noul", "noul": 0.95 if exec_noisy.paste_calls >= 1 else 0.04}}

        engine_noisy = SystemOneHostEngine(
            exec_noisy,
            ScriptedSystemOneClient(noisy_responder),
            ocr_fn=lambda _f, _r: noisy_ocr_reads.pop(0) if noisy_ocr_reads else "i",
        )
        res_noisy = engine_noisy.execute_tool(
            "reconcile_ui_state",
            {
                "fields": [
                    {"id": "quota", "control_type": "text", "locator_prompt": "Quota", "desired_value": "250", "match_mode": "exact"}
                ],
                "settle_delay_ms": 0,
            },
        )
        self.assertEqual(res_noisy["status"], "completed")
        rep_noisy = res_noisy["outcome"]["reconcile_summary"]["fields"][0]
        self.assertEqual(rep_noisy["status"], "reconciled")
        self.assertEqual(rep_noisy["observed_value"], "250")

    # 10. secret_text never observes, never reads clipboard, never leaks plaintext
    def test_secret_text_never_leaks_plaintext_and_verifies_masked(self) -> None:
        secret = "SuperSecret!9988"
        f0 = make_rgb_frame(800, 600, bg=(240, 240, 240), frame_seq=1)
        f1 = make_rgb_frame(800, 600, bg=(240, 240, 240), rects=[(100, 100, 280, 140, (255, 255, 255))], frame_seq=2)
        exec_stub = ReconcileFakeExecutor([f0, f1])
        clock = FakeClock()
        s1_client = ScriptedSystemOneClient(
            lambda call_idx, state, questions, assets: (
                {
                    "confirm_noul": {"type": "noul", "noul": 0.96},
                    "confirm_box": {"type": "box", "box": {"top": 0.2, "bottom": 0.8, "left": 0.2, "right": 0.8}, "confidence": 0.95},
                }
                if "confirm_noul" in questions
                else (
                    {"loc": {"type": "box", "box": {"top": 0.16, "bottom": 0.24, "left": 0.12, "right": 0.35}, "confidence": 0.95}}
                    if "loc" in questions
                    else {"mask": {"type": "noul", "noul": 0.96}, "no_err": {"type": "noul", "noul": 0.02}}
                )
            )
        )
        engine = SystemOneHostEngine(exec_stub, s1_client, sleep_fn=clock.sleep, monotonic_fn=clock.monotonic)
        res = engine.execute_tool(
            "reconcile_ui_state",
            {
                "fields": [{"id": "api_secret", "control_type": "secret_text", "locator_prompt": "API Token input", "desired_value": secret}],
                "settle_delay_ms": 0,
            },
        )
        self.assertEqual(res["status"], "completed")
        self.assertEqual(exec_stub.copy_calls, 0)  # NEVER reads clipboard
        self.assertEqual(exec_stub.paste_calls, 1)
        self.assertGreaterEqual(exec_stub.clipboard_cleared_count, 1)
        serialized = json.dumps({k: v for k, v in res.items() if k != "_visual_evidence_images"})
        self.assertNotIn(secret, serialized)
        for call in s1_client.calls:
            self.assertNotIn(secret, json.dumps(call["questions"]))
        rep = res["outcome"]["reconcile_summary"]["fields"][0]
        self.assertEqual(rep["status"], "reconciled")
        self.assertEqual(rep["desired_value"], "<redacted>")
        self.assertEqual(rep["observed_value"], "<redacted>")

    # 11. commit_on_blur validation error (CB_VALIDATION_ERROR), required vs non-required dependents
    def test_commit_on_blur_validation_error_and_required_dependent_halt(self) -> None:
        f0 = make_rgb_frame(800, 600, bg=(240, 240, 240), frame_seq=1)
        f1 = make_rgb_frame(800, 600, bg=(240, 240, 240), rects=[(100, 100, 300, 140, (255, 220, 220))], frame_seq=2)
        exec_stub = ReconcileFakeExecutor([f0, f1, f1, f1])
        exec_stub.clipboard_values = [(True, "")]  # initial empty -> mismatch
        clock = FakeClock()

        def responder(call_idx: int, state: Dict[str, Any], questions: Dict[str, Any], assets: Any) -> Dict[str, Any]:
            if "confirm_noul" in questions:
                return {
                    "confirm_noul": {"type": "noul", "noul": 0.96},
                    "confirm_box": {"type": "box", "box": {"top": 0.2, "bottom": 0.8, "left": 0.2, "right": 0.8}, "confidence": 0.95},
                }
            if "loc" in questions:
                return {"loc": {"type": "box", "box": {"top": 0.16, "bottom": 0.24, "left": 0.12, "right": 0.35}, "confidence": 0.95}}
            if "q" in questions and "inline validation error" in questions["q"]["instructions"].lower():
                return {"q": {"type": "noul", "noul": 0.94}}
            return {}

        engine = SystemOneHostEngine(exec_stub, ScriptedSystemOneClient(responder), sleep_fn=clock.sleep, monotonic_fn=clock.monotonic)
        res = engine.execute_tool(
            "reconcile_ui_state",
            {
                "fields": [
                    {
                        "id": "email",
                        "control_type": "text",
                        "locator_prompt": "Email input",
                        "desired_value": "bad-email",
                        "commit_on_blur": True,
                        "required": True,
                    },
                    {
                        "id": "confirm_email",
                        "control_type": "text",
                        "locator_prompt": "Confirm Email",
                        "desired_value": "bad-email",
                        "depends_on": ["email"],
                    },
                ],
                "settle_delay_ms": 0,
            },
        )
        self.assertEqual(res["breaker_code"], CB_VALIDATION_ERROR)
        self.assertEqual(res["status"], "validation_error")
        self.assertEqual(res["visual_evidence"], ["final_fullscreen", "annotated_roi_crop"])
        self.assertIn({"type": "key_press", "key": "Tab"}, exec_stub.actions)
        rep = {f["id"]: f for f in res["outcome"]["reconcile_summary"]["fields"]}
        self.assertEqual(rep["email"]["status"], "failed_validation")
        self.assertEqual(rep["confirm_email"]["status"], "skipped_dependency")

    # 12. Unexpected navigation guard (full-screen SSIM < 0.50 -> CB_INTERRUPT)
    def test_unexpected_navigation_freezes_with_cb_interrupt(self) -> None:
        f0 = make_rgb_frame(640, 480, bg=(240, 240, 240), frame_seq=1)
        stripe_rects = [(x, 0, x + 12, 480, (10, 10, 20) if (x // 12) % 2 == 0 else (245, 245, 245)) for x in range(0, 640, 12)]
        f_redirect = make_rgb_frame(640, 480, bg=(10, 10, 10), rects=stripe_rects, frame_seq=2)
        exec_stub = ReconcileFakeExecutor([f0, f_redirect])
        clock = FakeClock()

        def responder(call_idx: int, state: Dict[str, Any], questions: Dict[str, Any], assets: Any) -> Dict[str, Any]:
            if "confirm_noul" in questions:
                return {
                    "confirm_noul": {"type": "noul", "noul": 0.96},
                    "confirm_box": {"type": "box", "box": {"top": 0.2, "bottom": 0.8, "left": 0.2, "right": 0.8}, "confidence": 0.95},
                }
            return {
                "eval_cb": {"type": "choice", "choice": "unchecked", "confidence": 0.95},
                "box_cb": {"type": "box", "box": {"top": 0.2, "bottom": 0.4, "left": 0.2, "right": 0.4}, "confidence": 0.95},
            }

        engine = SystemOneHostEngine(exec_stub, ScriptedSystemOneClient(responder), sleep_fn=clock.sleep, monotonic_fn=clock.monotonic)
        res = engine.execute_tool(
            "reconcile_ui_state",
            {
                "fields": [
                    {"id": "cb", "control_type": "checkbox", "locator_prompt": "CB", "desired_value": True},
                    {"id": "cb2", "control_type": "checkbox", "locator_prompt": "CB2", "desired_value": True, "depends_on": ["cb"]},
                ],
                "settle_delay_ms": 0,
            },
        )
        self.assertEqual(res["breaker_code"], CB_INTERRUPT)
        self.assertEqual(res["status"], "interrupted")
        self.assertEqual(res["visual_evidence"], ["final_fullscreen"])
        self.assertEqual(exec_stub.released_count, 1)
        self.assertEqual(len(exec_stub.actions), 1)

    # 13. CB_OSCILLATION on A -> B -> A state hash cycle & conflicting fields report
    def test_oscillation_cycle_detected_and_reported(self) -> None:
        # Coupled pair: clicking A checks A and unchecks B; clicking B checks B and unchecks A!
        f0 = make_rgb_frame(800, 600, bg=(240, 240, 240), frame_seq=1)
        f1 = make_rgb_frame(800, 600, bg=(240, 240, 240), rects=[(80, 100, 140, 160, (10, 180, 10))], frame_seq=2)
        f2 = make_rgb_frame(800, 600, bg=(240, 240, 240), rects=[(80, 300, 140, 360, (10, 180, 10))], frame_seq=3)

        state_ab = {"a": "unchecked", "b": "unchecked"}

        class CoupledExecutor(ReconcileFakeExecutor):
            def current_frame(self) -> FrameSnapshot:
                if state_ab["a"] == "checked" and state_ab["b"] == "unchecked":
                    return f1
                if state_ab["b"] == "checked" and state_ab["a"] == "unchecked":
                    return f2
                return f0

        exec_stub = CoupledExecutor([f0])

        def on_act(action: Dict[str, Any], ex: FakeFsmExecutor) -> None:
            if action["y"] < 400:
                state_ab["a"] = "checked"
                state_ab["b"] = "unchecked"
            else:
                state_ab["b"] = "checked"
                state_ab["a"] = "unchecked"

        exec_stub.on_action_cb = on_act
        clock = FakeClock()

        def responder(call_idx: int, state: Dict[str, Any], questions: Dict[str, Any], assets: Any) -> Dict[str, Any]:
            if "confirm_noul" in questions:
                return {
                    "confirm_noul": {"type": "noul", "noul": 0.96},
                    "confirm_box": {"type": "box", "box": {"top": 0.2, "bottom": 0.8, "left": 0.2, "right": 0.8}, "confidence": 0.95},
                }
            out: Dict[str, Any] = {}
            if "eval_a" in questions:
                out["eval_a"] = {"type": "choice", "choice": state_ab["a"], "confidence": 0.96}
            if "box_a" in questions:
                out["box_a"] = {"type": "box", "box": {"top": 0.2, "bottom": 0.7, "left": 0.2, "right": 0.7}, "confidence": 0.95}
            if "eval_b" in questions:
                out["eval_b"] = {"type": "choice", "choice": state_ab["b"], "confidence": 0.96}
            if "box_b" in questions:
                out["box_b"] = {"type": "box", "box": {"top": 0.2, "bottom": 0.7, "left": 0.2, "right": 0.7}, "confidence": 0.95}
            return out

        engine = SystemOneHostEngine(exec_stub, ScriptedSystemOneClient(responder), sleep_fn=clock.sleep, monotonic_fn=clock.monotonic)
        res = engine.execute_tool(
            "reconcile_ui_state",
            {
                "fields": [
                    {"id": "a", "control_type": "radio", "locator_prompt": "Radio A", "roi": [100, 50, 350, 350], "desired_value": True},
                    {"id": "b", "control_type": "radio", "locator_prompt": "Radio B", "roi": [450, 50, 750, 350], "desired_value": True},
                ],
                "max_reconcile_passes": 5,  # clamped to hard cap 3
                "settle_delay_ms": 0,
            },
        )
        self.assertEqual(res["breaker_code"], CB_OSCILLATION)
        self.assertEqual(res["status"], "oscillating")
        summary = res["outcome"]["reconcile_summary"]
        self.assertLessEqual(summary["passes_executed"], 3)
        self.assertEqual(sorted(summary["conflicting_fields"]), ["a", "b"])
        self.assertEqual(res["visual_evidence"], ["final_fullscreen", "annotated_roi_crop"])

    # 14. Gray-zone retry -> uncertain (2 images) + handoff 5-state invariants
    def test_gray_zone_uncertain_attaches_annotated_crop(self) -> None:
        f0 = make_rgb_frame(640, 480, bg=(240, 240, 240), frame_seq=1)
        exec_stub = ReconcileFakeExecutor([f0])
        clock = FakeClock()
        engine = SystemOneHostEngine(
            exec_stub,
            ScriptedSystemOneClient(
                lambda _i, _s, q, _a: {
                    k: (
                        {"type": "choice", "choice": "unreadable", "confidence": 0.99}
                        if k.startswith("eval_") or k == "retry_pred"
                        else {"type": "box", "box": {"top": 0.2, "bottom": 0.6, "left": 0.2, "right": 0.6}, "confidence": 0.95}
                    )
                    for k in q
                }
            ),
            sleep_fn=clock.sleep,
            monotonic_fn=clock.monotonic,
        )
        res = engine.execute_tool(
            "reconcile_ui_state",
            {
                "fields": [{"id": "cb", "control_type": "checkbox", "locator_prompt": "Blurry CB", "desired_value": True}],
                "settle_delay_ms": 0,
            },
        )
        self.assertEqual(res["breaker_code"], CB_AMBIGUITY)
        self.assertEqual(res["visual_evidence"], ["final_fullscreen", "annotated_roi_crop"])
        self.assertEqual(len(res["_visual_evidence_images"]), 2)
        rep = res["outcome"]["reconcile_summary"]["fields"][0]
        self.assertEqual(rep["status"], "uncertain")
        self.assertIn(rep["status"], VALID_FIELD_STATES)

    # 15. End-to-end run_agent integration with reconcile_ui_state
    def test_end_to_end_run_agent_with_reconcile_ui_state(self) -> None:
        sock_path = os.path.join(self.tmp, "agent.sock")
        frame = make_rgb_frame(640, 480, bg=(235, 235, 235), rects=[(100, 100, 300, 180, (220, 40, 40))], frame_seq=1)
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
                        "eval_opt_a": {"type": "choice", "choice": "unchecked", "confidence": 0.96},
                        "box_opt_a": {"type": "box", "box": {"top": 0.2, "bottom": 0.5, "left": 0.2, "right": 0.5}, "confidence": 0.96},
                        "confirm_noul": {"type": "noul", "noul": 0.96},
                        "confirm_box": {"type": "box", "box": {"top": 0.2, "bottom": 0.8, "left": 0.2, "right": 0.8}, "confidence": 0.95},
                    },
                },
            )
        )

        def scripted_gemini(req_body: Dict[str, Any]) -> Tuple[int, Dict[str, Any]]:
            if len(gemini_backend.interaction_requests) == 1:
                return 200, {
                    "id": "int_1",
                    "status": "requires_action",
                    "usage": {
                        "total_tokens": 1200,
                        "total_input_tokens": 900,
                        "total_cached_tokens": 0,
                        "total_output_tokens": 120,
                        "total_thought_tokens": 180,
                        "total_tool_use_tokens": 0,
                    },
                    "outputs": [
                        {
                            "type": "function_call",
                            "id": "call_rec_1",
                            "name": "reconcile_ui_state",
                            "arguments": {
                                "fields": [
                                    {
                                        "id": "opt_a",
                                        "control_type": "checkbox",
                                        "locator_prompt": "Option A",
                                        "desired_value": True,
                                    }
                                ],
                                "max_retries_per_field": 0,
                                "settle_delay_ms": 0,
                                "intent": "Reconcile Option A",
                            },
                        }
                    ],
                }
            return 200, {
                "id": "int_2",
                "status": "completed",
                "usage": {
                    "total_tokens": 800,
                    "total_input_tokens": 600,
                    "total_cached_tokens": 400,
                    "total_output_tokens": 40,
                    "total_thought_tokens": 160,
                    "total_tool_use_tokens": 0,
                },
                "outputs": [{"type": "text", "text": "Handled reconcile validation error."}],
            }

        gemini_backend.scripted_handler = scripted_gemini
        gemini_key = os.path.join(self.tmp, "gemini.key")
        ts_key = os.path.join(self.tmp, "typesafe.key")
        with open(gemini_key, "w", encoding="utf-8") as f:
            f.write("gem_test_key\n")
        with open(ts_key, "w", encoding="utf-8") as f:
            f.write("ts_test_key\n")

        args = argparse.Namespace(
            prompt="Reconcile form",
            socket=sock_path,
            vnc_host="localhost:5900",
            vncviewer_bin="vncviewer",
            legacy_clipboard_encoding=None,
            api_key_file=gemini_key,
            model="gemini-4-hydrogen",
            max_turns=5,
            log_dir=os.path.join(self.tmp, "logs"),
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
            with mock.patch(
                "vnc_agent.interactions.INTERACTIONS_URL",
                f"http://127.0.0.1:{gemini_srv.server_port}/v1beta/interactions",
            ), mock.patch(
                "vnc_agent.runner.InteractionsClient",
                side_effect=lambda **kw: agent_mod.InteractionsClient(
                    base_url=f"http://127.0.0.1:{gemini_srv.server_port}/v1beta/interactions",
                    **kw,
                ),
            ):
                rc = agent_mod.run_agent(args)
            self.assertEqual(rc, 0)
            req2 = gemini_backend.interaction_requests[1]
            self.assertEqual(req2.get("generation_config", {}).get("thinking_level"), "high")
            fres = req2["input"][0]
            self.assertEqual(fres["name"], "reconcile_ui_state")
            img_blocks = [b for b in fres["result"] if b.get("type") == "image"]
            self.assertEqual(len(img_blocks), 2)
            payload = json.loads(fres["result"][0]["text"])
            self.assertEqual(payload["breaker_code"], CB_VALIDATION_ERROR)
            self.assertNotIn("_visual_evidence_images", payload)
            self.assertEqual(payload["outcome"]["reconcile_summary"]["counts"]["failed_validation"], 1)

            trace_path = os.path.join(args.log_dir, "trace.jsonl")
            with open(trace_path, "r", encoding="utf-8") as tf:
                entries = [json.loads(line) for line in tf if line.strip()]
            self.assertEqual(len(entries), 2)
            self.assertEqual(entries[0]["usage"]["total_tokens"], 1200)
            self.assertEqual(entries[1]["usage"]["total_tokens"], 800)
            self.assertIsInstance(entries[0]["wall_elapsed_s"], float)
            self.assertIsInstance(entries[1]["wall_elapsed_s"], float)
            from vnc_agent.interactions import extract_usage

            self.assertEqual(
                extract_usage({"usageMetadata": {"total_tokens": 42}}),
                {"total_tokens": 42},
            )
            self.assertIsNone(extract_usage({}))
        finally:
            stop_event.set()
            t.join(timeout=2.0)
            gemini_srv.shutdown()
            gemini_srv.server_close()
            ts_srv.stop()


if __name__ == "__main__":
    unittest.main()
