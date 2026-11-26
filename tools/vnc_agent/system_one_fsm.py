"""Host-side deterministic 6-state FSM, confidence gating, geometry guards, and System One tools."""

from collections import deque
from dataclasses import dataclass, field
import json
import math
import re
import time
from typing import Any, Callable, Deque, Dict, List, Optional, Sequence, Tuple

from .constants import KEY_MAP
from .system_one_client import (
    FrameSnapshot,
    PreparedImage,
    SystemOneApiError,
    SystemOneClient,
    bbox_to_pixels,
    compute_pixel_diff_ratio,
    compute_ssim,
    pad_bbox_norm,
    prepare_image_for_system_one,
    render_annotated_crop_png,
    validate_bbox,
)

# ---------------------------------------------------------------------------
# 6-State FSM & Circuit Breakers
# ---------------------------------------------------------------------------

FSM_STATES: Tuple[str, ...] = (
    "S0_INIT_VALIDATE",
    "S1_CAPTURE_AND_GATE",
    "S2_S1_INFERENCE",
    "S3_ADJUDICATE",
    "S4_ACT_OR_RECOVER",
    "S5_HANDOFF_TO_S2",
)

CB_AMBIGUITY = "CB_AMBIGUITY"
CB_STALL = "CB_STALL"
CB_INTERRUPT = "CB_INTERRUPT"
CB_SAFETY_GUARD = "CB_SAFETY_GUARD"
CB_BUDGET_EXCEEDED = "CB_BUDGET_EXCEEDED"
CB_BATCH_DRIFT = "CB_BATCH_DRIFT"
CB_VALIDATION_ERROR = "CB_VALIDATION_ERROR"
CB_OSCILLATION = "CB_OSCILLATION"
CB_DEPENDENCY_BLOCKED = "CB_DEPENDENCY_BLOCKED"

VALID_BREAKER_CODES = frozenset(
    {
        CB_AMBIGUITY,
        CB_STALL,
        CB_INTERRUPT,
        CB_SAFETY_GUARD,
        CB_BUDGET_EXCEEDED,
        CB_BATCH_DRIFT,
        CB_VALIDATION_ERROR,
        CB_OSCILLATION,
        CB_DEPENDENCY_BLOCKED,
    }
)

ALLOWED_HOST_ACTIONS = frozenset({"hover", "click", "wheel_scroll", "key_press", "drag_step"})

# Hard system limits & defaults
WAIT_DEFAULT_POLL_MS = 500
WAIT_MIN_POLL_MS = 50
WAIT_MAX_POLL_MS = 10000
WAIT_DEFAULT_TIMEOUT_MS = 30000
WAIT_TIMEOUT_CAP_MS = 180000
WAIT_DEFAULT_DIFF_GATE = 0.005
WAIT_DEFAULT_HEARTBEAT_N = 6

LOOP_DEFAULT_MAX_STEPS = 15
LOOP_MAX_STEPS_CAP = 50
DEFAULT_SSIM_STALL_THRESHOLD = 0.992
DEFAULT_MAX_STALLS = 2
FULL_SCREEN_INTERRUPT_SSIM = 0.50
DEFAULT_SETTLE_DELAY_MS = 250

BATCH_DEFAULT_MAX_PAGES = 1
BATCH_MAX_PAGES_CAP = 20
BATCH_DEFAULT_MAX_MUTATIONS = 50
BATCH_MAX_MUTATIONS_CAP = 200
BATCH_MAX_ERROR_RATIO_CAP = 0.25

BOX_MIN_PIXEL_SIDE = 6
BOX_MAX_VIEWPORT_AREA_RATIO = 0.80
BOX_CROP_PAD_RATIO = 0.20
GRAY_RETRY_ROI_SCALE = 1.5
TRANSIENT_RING_BUFFER_SIZE = 6
MAX_RECENT_CONFIDENCES = 3
MAX_VISUAL_EVIDENCE_IMAGES = 2

RISK_TIERS = frozenset({"read_only", "reversible", "interrupt", "abort", "irreversible"})

# System floor thresholds (callers may tighten min_confidence, never loosen below these floors)
RISK_TIER_THRESHOLDS: Dict[str, Dict[str, Optional[float]]] = {
    "read_only": {
        "tau_high": 0.85,
        "tau_low": 0.60,
        "two_frame_tau": 0.80,
        "box_global": 0.75,
        "box_crop": 0.75,
    },
    "reversible": {
        "tau_high": 0.90,
        "tau_low": 0.65,
        "two_frame_tau": None,
        "box_global": 0.85,
        "box_crop": 0.85,
    },
    "interrupt": {
        "tau_high": 0.78,
        "tau_low": 0.55,
        "two_frame_tau": None,
        "box_global": None,
        "box_crop": None,
    },
    "abort": {
        "tau_high": 0.78,
        "tau_low": 0.55,
        "two_frame_tau": None,
        "box_global": None,
        "box_crop": None,
    },
    "irreversible": {
        "tau_high": 1.01,  # Never auto-pass
        "tau_low": 0.65,
        "two_frame_tau": None,
        "box_global": None,
        "box_crop": None,
    },
}

IRREVERSIBLE_SAFETY_KEYWORDS: Tuple[str, ...] = (
    "permanently delete",
    "delete account",
    "delete forever",
    "delete all",
    "erase disk",
    "format drive",
    "factory reset",
    "wipe",
    "drop table",
    "drop database",
    "confirm purchase",
    "place order",
    "complete purchase",
    "buy now",
    "pay now",
    "submit payment",
    "wire transfer",
    "transfer funds",
    "send money",
    "send email",
    "send message",
    "post publicly",
    "publish now",
    "accept terms",
    "agree to terms",
    "i agree",
    "sign contract",
    "authorize",
    "revoke access",
    "uninstall",
    "shut down",
    "shutdown",
)

UNREADABLE_OPTION_KEY = "unreadable"
UNREADABLE_OPTION_DESC = "The region is blank, occluded, blurred, cut off, or too ambiguous to read."


def _clamp_int(val: Any, lo: int, hi: int, name: str) -> int:
    if isinstance(val, bool) or not isinstance(val, (int, float)) or (isinstance(val, float) and not math.isfinite(val)):
        raise ValueError(f"{name} must be a finite number, got {val!r}")
    return int(round(max(lo, min(hi, val))))


def _clamp_float(val: Any, lo: float, hi: float, name: str) -> float:
    if isinstance(val, bool) or not isinstance(val, (int, float)) or not math.isfinite(float(val)):
        raise ValueError(f"{name} must be a finite number, got {val!r}")
    return max(lo, min(hi, float(val)))


def _normalize_risk_tier(raw_tier: Optional[str], default: str = "read_only") -> str:
    tier = str(raw_tier or default).strip().lower()
    if tier == "abort":
        return "interrupt"
    if tier not in RISK_TIERS:
        raise ValueError(f"Unsupported risk_tier {raw_tier!r}; expected one of {sorted(RISK_TIERS)}")
    return tier


def _contains_irreversible_keyword(text: str) -> Optional[str]:
    low = " ".join((text or "").lower().split())
    for kw in IRREVERSIBLE_SAFETY_KEYWORDS:
        if re.search(r"\b" + re.escape(kw) + r"\b", low):
            return kw
    return None


def validate_host_action(action: Any, name: str = "action") -> Dict[str, Any]:
    """Validate and normalize a closed-set ``HostAction`` primitive."""
    if not isinstance(action, dict):
        raise ValueError(f"{name} must be an object, got {type(action).__name__}")
    for forbidden_key in ("script", "code", "eval", "shell", "command", "python", "js"):
        if forbidden_key in action:
            raise ValueError(f"{name} must not contain dynamic script field {forbidden_key!r}")

    atype = str(action.get("type", "")).strip().lower()
    if atype not in ALLOWED_HOST_ACTIONS:
        raise ValueError(
            f"{name}.type={atype!r} is not in allowed closed-set HostActions {sorted(ALLOWED_HOST_ACTIONS)}"
        )

    out: Dict[str, Any] = {"type": atype}
    if atype in ("hover", "click", "wheel_scroll"):
        if "x" not in action or "y" not in action:
            raise ValueError(f"{name} ({atype}) requires normalized 'x' and 'y' in 0..1000")
        out["x"] = _clamp_int(action["x"], 0, 1000, f"{name}.x")
        out["y"] = _clamp_int(action["y"], 0, 1000, f"{name}.y")

    if atype == "click":
        btn = str(action.get("button", "left")).strip().lower()
        if btn not in ("left", "middle", "right"):
            raise ValueError(f"{name}.button={btn!r} must be left, middle, or right")
        out["button"] = btn
        out["count"] = _clamp_int(action.get("count", 1), 1, 3, f"{name}.count")
    elif atype == "wheel_scroll":
        direction = str(action.get("direction", "down")).strip().lower()
        if direction not in ("up", "down", "left", "right"):
            raise ValueError(f"{name}.direction={direction!r} must be up, down, left, or right")
        out["direction"] = direction
        out["magnitude"] = _clamp_int(action.get("magnitude", 300), 50, 2000, f"{name}.magnitude")
    elif atype == "key_press":
        key = str(action.get("key", "")).strip()
        if not key:
            raise ValueError(f"{name} (key_press) requires non-empty 'key'")
        norm_key = key.lower().replace("-", "_").replace(" ", "_")
        if norm_key not in KEY_MAP and not (len(key) == 1 and 0x20 <= ord(key) <= 0x7E):
            raise ValueError(f"{name}.key={key!r} is not a supported VNC key name or printable ASCII character")
        out["key"] = key
    elif atype == "drag_step":
        for coord in ("start_x", "start_y", "end_x", "end_y"):
            if coord not in action:
                raise ValueError(f"{name} (drag_step) requires '{coord}' in 0..1000")
            out[coord] = _clamp_int(action[coord], 0, 1000, f"{name}.{coord}")

    return out


def _normalize_choice_criteria(raw_criteria: Any) -> Dict[str, Any]:
    """Normalize ``choice`` criteria and always auto-append ``unreadable``."""
    parsed = raw_criteria
    if isinstance(parsed, str):
        try:
            parsed = json.loads(parsed)
        except json.JSONDecodeError as exc:
            raise ValueError(f"choice criteria JSON string is invalid: {exc}") from exc

    criteria: Dict[str, Any] = {}
    if isinstance(parsed, dict):
        for k, v in parsed.items():
            key = str(k).strip()
            if key:
                criteria[key] = v if v is not None else key
    elif isinstance(parsed, list):
        for item in parsed:
            if isinstance(item, dict):
                lbl = str(item.get("label") or item.get("name") or item.get("key") or "").strip()
                desc = item.get("description", lbl)
                if lbl:
                    criteria[lbl] = desc
            elif isinstance(item, str) and item.strip():
                lbl = item.strip()
                criteria[lbl] = lbl
    else:
        raise ValueError(f"choice predicate requires criteria dict or list, got {type(raw_criteria).__name__}")

    if not criteria:
        raise ValueError("choice predicate criteria must contain at least one option")

    if UNREADABLE_OPTION_KEY not in criteria:
        criteria[UNREADABLE_OPTION_KEY] = UNREADABLE_OPTION_DESC
    return criteria


@dataclass
class NormalizedPredicate:
    """Validated single-frame System One predicate specification."""

    pred_type: str
    instructions: str
    criteria: Optional[Dict[str, Any]] = None
    expected_value: bool = True
    target_choices: Optional[List[str]] = None
    tau_high: float = 0.85
    tau_low: float = 0.60
    two_frame_tau: Optional[float] = None
    roi: List[int] = field(default_factory=lambda: [0, 0, 1000, 1000])
    raw_label: str = ""

    def to_system_one_question(self, image_ref_name: str = "frame") -> Dict[str, Any]:
        """Build the System One ``questions`` entry referencing ``image_ref_name``."""
        instr = self.instructions
        if "`" not in instr and image_ref_name:
            instr = f"In `{image_ref_name}`: {instr}"
        q: Dict[str, Any] = {"type": self.pred_type, "instructions": instr}
        if self.pred_type == "choice":
            assert self.criteria is not None
            q["criteria"] = dict(self.criteria)
        return q


def normalize_predicate(
    raw_pred: Any,
    risk_tier: str,
    default_roi: Optional[Sequence[int]] = None,
    name: str = "predicate",
) -> NormalizedPredicate:
    """Validate and normalize a ``noul``, ``choice``, or ``box`` predicate."""
    if not isinstance(raw_pred, dict):
        raise ValueError(f"{name} must be an object, got {type(raw_pred).__name__}")

    ptype = str(raw_pred.get("type", "")).strip().lower()
    if ptype not in ("noul", "choice", "box"):
        raise ValueError(f"{name}.type={ptype!r} must be 'noul', 'choice', or 'box'")

    instructions = str(raw_pred.get("instructions") or raw_pred.get("question") or "").strip()
    if not instructions:
        raise ValueError(f"{name}.instructions must be a non-empty string")

    roi = (
        validate_bbox(raw_pred["roi"], name=f"{name}.roi")
        if raw_pred.get("roi") is not None
        else (list(default_roi) if default_roi is not None else [0, 0, 1000, 1000])
    )

    tier_cfg = RISK_TIER_THRESHOLDS[risk_tier]
    floor_high = float(tier_cfg["tau_high"] if tier_cfg["tau_high"] is not None else 0.85)
    floor_low = float(tier_cfg["tau_low"] if tier_cfg["tau_low"] is not None else 0.60)
    two_frame = tier_cfg["two_frame_tau"]

    caller_min = raw_pred.get("min_confidence")
    if caller_min is not None:
        caller_val = _clamp_float(caller_min, 0.0, 1.0, f"{name}.min_confidence")
        # Callers may tighten thresholds, NEVER loosen below system floors.
        tau_high = max(floor_high, caller_val)
        if two_frame is not None:
            two_frame = max(two_frame, caller_val)
    else:
        tau_high = floor_high

    criteria: Optional[Dict[str, Any]] = None
    target_choices: Optional[List[str]] = None
    if ptype == "choice":
        criteria = _normalize_choice_criteria(raw_pred.get("criteria"))
        raw_tc = raw_pred.get("target_choices")
        if raw_tc is None and raw_pred.get("target_choice") is not None:
            raw_tc = [raw_pred.get("target_choice")]
        if raw_tc is not None:
            if not isinstance(raw_tc, (list, tuple)):
                raise ValueError(f"{name}.target_choices must be a list of choice labels")
            target_choices = list(dict.fromkeys(str(c).strip() for c in raw_tc if str(c).strip()))
            if not target_choices:
                raise ValueError(f"{name}.target_choices must contain at least one label")
            for c in target_choices:
                if c == UNREADABLE_OPTION_KEY:
                    raise ValueError(f"{name}.target_choices cannot target 'unreadable'")
                if c not in criteria:
                    raise ValueError(f"{name}.target_choices contains unknown option {c!r}")

    expected_val = raw_pred.get("expected_value", True)
    if not isinstance(expected_val, bool):
        raise ValueError(f"{name}.expected_value must be a boolean")
    return NormalizedPredicate(
        pred_type=ptype,
        instructions=instructions,
        criteria=criteria,
        expected_value=expected_val,
        target_choices=target_choices,
        tau_high=tau_high,
        tau_low=floor_low,
        two_frame_tau=two_frame,
        roi=roi,
        raw_label=str(raw_pred.get("label") or instructions[:64]),
    )


@dataclass
class AdjudicationResult:
    """Three-zone hysteresis judgment for a single System One answer."""

    zone: str  # "pass" | "gray" | "reject"
    score: float
    label: str
    unreadable: bool = False
    invalid: bool = False
    two_frame_eligible: bool = False
    reason: str = ""


def adjudicate_predicate(
    pred: NormalizedPredicate,
    answer: Optional[Dict[str, Any]],
    risk_tier: str,
    prev_two_frame_hits: int = 0,
) -> AdjudicationResult:
    """Apply three-zone hysteresis (``tau_low``, ``tau_high``) to a System One answer."""
    invalid = AdjudicationResult(
        zone="gray", score=0.0, label="invalid", invalid=True, reason="missing or malformed answer"
    )
    if not isinstance(answer, dict) or answer.get("type", pred.pred_type) != pred.pred_type:
        return invalid

    if pred.pred_type == "noul":
        raw_p = _probability(answer.get("noul"))
        if raw_p is None:
            return invalid
        score = raw_p if pred.expected_value else (1.0 - raw_p)
        label = "true" if raw_p >= 0.5 else "false"
    elif pred.pred_type == "choice":
        chosen = str(answer.get("choice", "") or "").strip()
        probs = answer.get("probabilities") if isinstance(answer.get("probabilities"), dict) else {}
        raw_conf = answer.get("confidence")
        if raw_conf is None:
            raw_conf = probs.get(chosen)
        conf = _probability(raw_conf)
        if conf is None or chosen not in (pred.criteria or {}):
            return invalid

        if chosen == UNREADABLE_OPTION_KEY:
            # Rule: Treat "unreadable" as gray-zone/fail regardless of score.
            return AdjudicationResult(
                zone="gray",
                score=conf,
                label=UNREADABLE_OPTION_KEY,
                unreadable=True,
                reason="choice returned unreadable",
            )

        if pred.target_choices is not None and chosen not in pred.target_choices:
            return AdjudicationResult(
                zone="reject" if conf >= pred.tau_high else "gray",
                score=conf,
                label=chosen,
                reason=f"choice {chosen!r} not in target_choices {pred.target_choices}",
            )
        score = conf
        label = chosen
    else:
        score = _probability(answer.get("confidence"))
        if score is None:
            return invalid
        if not _box_edges_valid(answer.get("box")):
            return invalid if score >= pred.tau_low else AdjudicationResult("reject", score, "box")
        label = "box"

    if risk_tier == "irreversible":
        # Rule: irreversible never auto-passes; treat high/gray scores as candidate/gray.
        if score >= pred.tau_low:
            return AdjudicationResult(
                zone="pass",
                score=score,
                label=label,
                reason="irreversible candidate matched (requires System 2 confirmation)",
            )
        return AdjudicationResult(zone="reject", score=score, label=label, reason="below tau_low")

    if score >= pred.tau_high:
        return AdjudicationResult(zone="pass", score=score, label=label, reason="score >= tau_high")

    if (
        pred.two_frame_tau is not None
        and score >= pred.two_frame_tau
        and prev_two_frame_hits >= 1
    ):
        return AdjudicationResult(
            zone="pass",
            score=score,
            label=label,
            two_frame_eligible=True,
            reason=f"2 consecutive frames >= {pred.two_frame_tau:.2f}",
        )

    if score >= pred.tau_low:
        return AdjudicationResult(
            zone="gray",
            score=score,
            label=label,
            two_frame_eligible=(pred.two_frame_tau is not None and score >= pred.two_frame_tau),
            reason=f"gray zone ({pred.tau_low:.2f} <= {score:.3f} < {pred.tau_high:.2f})",
        )

    return AdjudicationResult(zone="reject", score=score, label=label, reason="score < tau_low")


def _probability(value: Any) -> Optional[float]:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not 0 <= value <= 1:
        return None
    return float(value)


def _box_edges_valid(box: Any) -> bool:
    if not isinstance(box, dict):
        return False
    top, bottom, left, right = (_probability(box.get(edge)) for edge in ("top", "bottom", "left", "right"))
    return None not in (top, bottom, left, right) and top < bottom and left < right


def _extract_box_norm_and_conf(
    answer: Optional[Dict[str, Any]],
    prepared: PreparedImage,
) -> Tuple[Optional[List[int]], float]:
    if not isinstance(answer, dict) or answer.get("type", "box") != "box":
        return None, 0.0
    conf = _probability(answer.get("confidence"))
    if conf is None or not _box_edges_valid(answer.get("box")):
        return None, 0.0
    try:
        return prepared.map_box_to_screen_bbox(answer["box"]), conf
    except ValueError:
        return None, 0.0


def _box_center_in_roi(box_norm: Sequence[int], roi_norm: Sequence[int]) -> bool:
    cy = (box_norm[0] + box_norm[2]) / 2.0
    cx = (box_norm[1] + box_norm[3]) / 2.0
    return (
        roi_norm[0] <= cy <= roi_norm[2]
        and roi_norm[1] <= cx <= roi_norm[3]
    )


def _box_pixel_bounds_ok(box_norm: Sequence[int], frame_w: int, frame_h: int) -> Tuple[bool, str]:
    x0, y0, x1, y1 = bbox_to_pixels(box_norm, frame_w, frame_h)
    pw = x1 - x0
    ph = y1 - y0
    if pw < BOX_MIN_PIXEL_SIDE or ph < BOX_MIN_PIXEL_SIDE:
        return False, f"degenerate box size {pw}x{ph}px (< {BOX_MIN_PIXEL_SIDE}px)"
    viewport_area = max(1, frame_w * frame_h)
    area_ratio = (pw * ph) / float(viewport_area)
    if area_ratio > BOX_MAX_VIEWPORT_AREA_RATIO:
        return False, f"oversized box covers {area_ratio:.2%} of viewport (> {BOX_MAX_VIEWPORT_AREA_RATIO:.0%})"
    return True, "ok"


def _subdivide_slot_grid(page_roi: Sequence[int], rows: int, cols: int) -> List[List[int]]:
    y0, x0, y1, x1 = [int(v) for v in page_roi]
    slots: List[List[int]] = []
    for r in range(rows):
        sy0 = y0 + int(round((y1 - y0) * (r / rows)))
        sy1 = y0 + int(round((y1 - y0) * ((r + 1) / rows)))
        for c in range(cols):
            sx0 = x0 + int(round((x1 - x0) * (c / cols)))
            sx1 = x0 + int(round((x1 - x0) * ((c + 1) / cols)))
            if sy1 > sy0 and sx1 > sx0:
                slots.append([sy0, sx0, sy1, sx1])
    return slots


class SystemOneHostEngine:
    """Executes ``wait_until``, ``loop_until``, ``batch_apply``, and ``reconcile_ui_state`` on a shared FSM."""

    def __init__(
        self,
        executor: Any,
        s1_client: SystemOneClient,
        sleep_fn: Callable[[float], None] = time.sleep,
        monotonic_fn: Callable[[], float] = time.monotonic,
        ocr_fn: Optional[Callable[[FrameSnapshot, Sequence[int]], Optional[str]]] = None,
    ) -> None:
        self.executor = executor
        self.s1_client = s1_client
        self.sleep = sleep_fn
        self.monotonic = monotonic_fn
        self.ocr_fn = ocr_fn

    def _capture_frame(self) -> FrameSnapshot:
        frame = self.executor.capture_rgb_frame()
        if not frame.timestamp:
            frame.timestamp = self.monotonic()
        return frame

    def _record_confidence(
        self,
        ring: Deque[Dict[str, Any]],
        step: int,
        kind: str,
        label: str,
        score: float,
        zone: str,
    ) -> None:
        ring.append(
            {
                "step": int(step),
                "kind": str(kind),
                "label": str(label),
                "score": round(float(score), 4),
                "zone": str(zone),
            }
        )

    def _check_interrupts(
        self,
        interrupt_preds: Sequence[NormalizedPredicate],
        answers: Dict[str, Any],
    ) -> Optional[Dict[str, Any]]:
        for idx, ipred in enumerate(interrupt_preds):
            key = f"interrupt_{idx}"
            adj = adjudicate_predicate(ipred, answers.get(key), "interrupt")
            if adj.zone == "pass" or adj.invalid or adj.unreadable:
                return {
                    "index": idx,
                    "label": ipred.raw_label,
                    "answer_label": adj.label,
                    "score": round(adj.score, 4),
                }
        return None

    def _query_s1(
        self,
        state: Dict[str, Any],
        questions: Dict[str, Dict[str, Any]],
        image_assets: Dict[str, Tuple[bytes, str]],
        metrics: Dict[str, int],
        deadline: Optional[float] = None,
    ) -> Dict[str, Any]:
        timeout = None if deadline is None else deadline - self.monotonic()
        if timeout is not None and timeout <= 0:
            raise TimeoutError("Wait deadline reached before inference")
        metrics["s1_calls"] += 1
        resp = self.s1_client.query(state=state, questions=questions, image_assets=image_assets, timeout=timeout)
        if deadline is not None and self.monotonic() >= deadline:
            raise TimeoutError("Wait deadline reached during inference")
        return resp.get("answers", {})

    def _add_interrupt_questions(
        self,
        frame: FrameSnapshot,
        predicates: Sequence[NormalizedPredicate],
        state: Dict[str, Any],
        questions: Dict[str, Dict[str, Any]],
        assets: Dict[str, Tuple[bytes, str]],
    ) -> None:
        refs: Dict[Tuple[int, ...], str] = {}
        for i, pred in enumerate(predicates):
            roi = tuple(pred.roi)
            if roi not in refs:
                ref = f"interrupt_view_{i}"
                prep = prepare_image_for_system_one(frame, roi)
                refs[roi] = ref
                state[ref] = ref
                assets[ref] = (prep.image_bytes, prep.image_format)
            questions[f"interrupt_{i}"] = pred.to_system_one_question(image_ref_name=refs[roi])

    def _run_gray_zone_retry(
        self,
        pred: NormalizedPredicate,
        risk_tier: str,
        roi_or_box: Sequence[int],
        settle_delay_ms: int,
        metrics: Dict[str, int],
        recent_confs: Deque[Dict[str, Any]],
        step: int,
        prev_two_frame_hits: int = 0,
        interrupt_preds: Sequence[NormalizedPredicate] = (),
        deadline: Optional[float] = None,
    ) -> Tuple[AdjudicationResult, FrameSnapshot, Optional[Dict[str, Any]]]:
        """Run 1x local retry on a 1.5x padded ROI crop after ``settle_delay_ms``."""
        if settle_delay_ms > 0:
            delay = settle_delay_ms / 1000.0
            if deadline is not None and delay >= deadline - self.monotonic():
                raise TimeoutError("Gray-zone retry would exceed the wait deadline")
            self.sleep(delay)
        fresh = self._capture_frame()
        padded_roi = pad_bbox_norm(roi_or_box, GRAY_RETRY_ROI_SCALE, mode="scale")
        prep = prepare_image_for_system_one(fresh, padded_roi)
        ref = "retry_roi_img"
        q = pred.to_system_one_question(image_ref_name="roi_crop")
        state = {"roi_crop": ref}
        questions = {"retry_pred": q}
        assets = {ref: (prep.image_bytes, prep.image_format)}
        self._add_interrupt_questions(fresh, interrupt_preds, state, questions, assets)
        answers = self._query_s1(state, questions, assets, metrics, deadline=deadline)
        adj = adjudicate_predicate(
            pred, answers.get("retry_pred"), risk_tier, prev_two_frame_hits=prev_two_frame_hits
        )
        self._record_confidence(recent_confs, step, f"{pred.pred_type}:retry", adj.label, adj.score, adj.zone)
        return adj, fresh, self._check_interrupts(interrupt_preds, answers)

    def _verify_box_geometry_and_crop(
        self,
        frame: FrameSnapshot,
        box_norm: Optional[List[int]],
        box_conf: float,
        roi_norm: Sequence[int],
        target_desc: str,
        risk_tier: str,
        metrics: Dict[str, int],
        recent_confs: Deque[Dict[str, Any]],
        step: int,
        caller_box_min_conf: Optional[float] = None,
        include_crop_box_check: bool = True,
        confirm_crop_roi: Optional[Sequence[int]] = None,
    ) -> Tuple[bool, str]:
        """Run Box Geometry Pre-Click Checks: ROI containment, pixel bounds, and 20%-padded crop confirmation."""
        if box_norm is None:
            return False, "missing candidate box"

        tier_cfg = RISK_TIER_THRESHOLDS[risk_tier]
        global_floor = float(tier_cfg["box_global"] if tier_cfg["box_global"] is not None else 0.85)
        crop_floor = float(tier_cfg["box_crop"] if tier_cfg["box_crop"] is not None else 0.85)
        noul_floor = float(tier_cfg["tau_high"] if tier_cfg["tau_high"] is not None else 0.90)
        if caller_box_min_conf is not None:
            global_floor = max(global_floor, caller_box_min_conf)
            crop_floor = max(crop_floor, caller_box_min_conf)

        if include_crop_box_check and box_conf < global_floor:
            return False, f"global box confidence {box_conf:.3f} < {global_floor:.2f}"

        if not _box_center_in_roi(box_norm, roi_norm):
            return False, f"box center outside roi {list(roi_norm)}"

        bounds_ok, bounds_msg = _box_pixel_bounds_ok(box_norm, frame.width, frame.height)
        if not bounds_ok:
            return False, bounds_msg

        # Check (3): local crop re-passes a noul confirmation check (+ local box check)
        padded = list(confirm_crop_roi) if confirm_crop_roi is not None else pad_bbox_norm(box_norm, BOX_CROP_PAD_RATIO, mode="pad")
        prep = prepare_image_for_system_one(frame, padded)
        ref = "box_crop_img"
        noul_instr = (
            f"In `crop`: is the target UI control ({target_desc}) clearly visible?"
            if confirm_crop_roi is not None
            else f"In `crop`: does the centered UI element clearly show {target_desc}?"
        )
        questions: Dict[str, Dict[str, Any]] = {
            "confirm_noul": {
                "type": "noul",
                "instructions": noul_instr,
            },
        }
        if include_crop_box_check:
            questions["confirm_box"] = {
                "type": "box",
                "instructions": f"the {target_desc} in `crop`",
            }
        answers = self._query_s1(
            state={"crop": ref},
            questions=questions,
            image_assets={ref: (prep.image_bytes, prep.image_format)},
            metrics=metrics,
        )
        noul_answer = answers.get("confirm_noul")
        noul_score = _probability(noul_answer.get("noul")) if isinstance(noul_answer, dict) else None
        noul_score = noul_score or 0.0
        self._record_confidence(
            recent_confs,
            step,
            "box_crop_noul",
            target_desc[:40],
            noul_score,
            "pass" if noul_score >= noul_floor else "fail",
        )
        if noul_score < noul_floor:
            return False, f"20%-padded crop noul confirmation {noul_score:.3f} < {noul_floor:.2f}"

        if include_crop_box_check:
            crop_box, crop_box_conf = _extract_box_norm_and_conf(answers.get("confirm_box"), prep)
            tau_low = float(tier_cfg["tau_low"] if tier_cfg["tau_low"] is not None else 0.65)
            self._record_confidence(
                recent_confs,
                step,
                "box_crop_loc",
                target_desc[:40],
                crop_box_conf,
                "pass" if crop_box_conf >= crop_floor else ("gray" if crop_box_conf >= tau_low else "fail"),
            )
            if tau_low <= crop_box_conf < crop_floor:
                padded_retry = pad_bbox_norm(padded, GRAY_RETRY_ROI_SCALE, mode="scale")
                prep_retry = prepare_image_for_system_one(frame, padded_retry)
                retry_ans = self._query_s1(
                    state={"crop": "box_crop_retry_img"},
                    questions={
                        "confirm_box": {
                            "type": "box",
                            "instructions": f"the {target_desc} in `crop`",
                        }
                    },
                    image_assets={"box_crop_retry_img": (prep_retry.image_bytes, prep_retry.image_format)},
                    metrics=metrics,
                )
                crop_box, crop_box_conf = _extract_box_norm_and_conf(retry_ans.get("confirm_box"), prep_retry)
                self._record_confidence(
                    recent_confs,
                    step,
                    "box_crop_loc:retry",
                    target_desc[:40],
                    crop_box_conf,
                    "pass" if crop_box_conf >= crop_floor else "fail",
                )
            if crop_box_conf < crop_floor:
                return False, f"20%-padded crop box confidence {crop_box_conf:.3f} < {crop_floor:.2f}"
            if crop_box is None or not _box_center_in_roi(box_norm, crop_box):
                return False, "crop-localized target does not contain the proposed click"

        return True, "ok"

    def _build_handoff_result(
        self,
        *,
        tool_name: str,
        status: str,
        breaker_code: Optional[str],
        detail: str,
        metrics: Dict[str, int],
        start_time: float,
        final_frame: Optional[FrameSnapshot],
        matched_box: Optional[List[int]] = None,
        matched_label: Optional[str] = None,
        recent_confs: Optional[Deque[Dict[str, Any]]] = None,
        batch_summary: Optional[Dict[str, Any]] = None,
        reconcile_summary: Optional[Dict[str, Any]] = None,
        annotated_crop_frame: Optional[FrameSnapshot] = None,
        annotated_crop_roi: Optional[Sequence[int]] = None,
        attach_annotated_crop: Optional[bool] = None,
        annotated_crop_label: Optional[str] = None,
        skip_visual_encoding: bool = False,
        transient_frame: Optional[FrameSnapshot] = None,
        transient_roi: Optional[Sequence[int]] = None,
        fsm_trace: Optional[List[str]] = None,
    ) -> Dict[str, Any]:
        """Construct compact ``HostToolResult`` during ``S5_HANDOFF_TO_S2``."""
        elapsed_ms = max(0, int(round((self.monotonic() - start_time) * 1000.0)))
        if final_frame is None:
            try:
                final_frame = self._capture_frame()
            except Exception:
                final_frame = None

        evidence_images: List[Tuple[str, bytes]] = []
        evidence_labels: List[str] = []
        if not skip_visual_encoding:
            if final_frame is not None:
                evidence_images.append(("final_fullscreen", final_frame.to_png_bytes()))
                evidence_labels.append("final_fullscreen")

            should_attach_crop = (
                attach_annotated_crop
                if attach_annotated_crop is not None
                else (breaker_code in (CB_AMBIGUITY, CB_SAFETY_GUARD))
            )
            if should_attach_crop:
                crop_src = annotated_crop_frame or final_frame
                if crop_src is not None:
                    roi_to_crop = list(annotated_crop_roi or matched_box or [0, 0, 1000, 1000])
                    crop_png = render_annotated_crop_png(
                        crop_src,
                        roi_to_crop,
                        highlight_box_norm=matched_box,
                        label=str(annotated_crop_label or breaker_code or "annotated"),
                    )
                    evidence_images.append(("annotated_roi_crop", crop_png))
                    evidence_labels.append("annotated_roi_crop")
            elif transient_frame is not None:
                roi_to_crop = list(transient_roi or [0, 0, 1000, 1000])
                t_png = render_annotated_crop_png(
                    transient_frame,
                    roi_to_crop,
                    highlight_box_norm=matched_box,
                    label="transient",
                )
                evidence_images.append(("capture_transient", t_png))
                evidence_labels.append("capture_transient")

            # Enforce hard max 2 images invariant
            evidence_images = evidence_images[:MAX_VISUAL_EVIDENCE_IMAGES]
            evidence_labels = evidence_labels[:MAX_VISUAL_EVIDENCE_IMAGES]

        confs = list(recent_confs or ())[-MAX_RECENT_CONFIDENCES:]
        outcome: Dict[str, Any] = {
            "matched_box": matched_box,
            "matched_label": matched_label,
            "recent_confidences": confs,
            "batch_summary": batch_summary,
        }
        if reconcile_summary is not None:
            outcome["reconcile_summary"] = reconcile_summary
        result: Dict[str, Any] = {
            "tool": tool_name,
            "status": status,
            "breaker_code": breaker_code,
            "detail": detail,
            "metrics": {
                "steps_executed": int(metrics.get("steps_executed", 0)),
                "s1_calls": int(metrics.get("s1_calls", 0)),
                "elapsed_ms": elapsed_ms,
            },
            "outcome": outcome,
            "visual_evidence": evidence_labels,
            "fsm_states": list(fsm_trace or []),
            "_visual_evidence_images": evidence_images,
        }
        return result

    def _safe_release_inputs(self) -> None:
        release_fn = getattr(self.executor, "release_all_inputs", None)
        if callable(release_fn):
            try:
                release_fn()
            except Exception:
                pass

    def _check_collaborate_pause(self, collaborate_mode: bool) -> None:
        if not collaborate_mode:
            return
        st = self.executor.get_status()
        if st.get("agent_paused"):
            from .executor import AgentPausedError

            raise AgentPausedError("Agent input paused by operator")

    # -----------------------------------------------------------------------
    # Public Tool Dispatcher
    # -----------------------------------------------------------------------

    def execute_tool(
        self,
        tool_name: str,
        raw_args: Dict[str, Any],
        collaborate_mode: bool = False,
    ) -> Dict[str, Any]:
        if tool_name == "wait_until":
            return self.run_wait_until(raw_args, collaborate_mode=collaborate_mode)
        if tool_name == "loop_until":
            return self.run_loop_until(raw_args, collaborate_mode=collaborate_mode)
        if tool_name == "batch_apply":
            return self.run_batch_apply(raw_args, collaborate_mode=collaborate_mode)
        if tool_name == "reconcile_ui_state":
            return self.run_reconcile_ui_state(raw_args, collaborate_mode=collaborate_mode)
        raise ValueError(f"Unsupported System One host tool: {tool_name!r}")

    def run_reconcile_ui_state(self, args: Dict[str, Any], collaborate_mode: bool = False) -> Dict[str, Any]:
        """Run the L2 composite ``reconcile_ui_state`` controller."""
        from .reconcile_ui_state import run_reconcile_ui_state as _run_reconcile

        return _run_reconcile(self, args, collaborate_mode=collaborate_mode)

    # -----------------------------------------------------------------------
    # Tool 1: wait_until
    # -----------------------------------------------------------------------

    def run_wait_until(self, args: Dict[str, Any], collaborate_mode: bool = False) -> Dict[str, Any]:
        start_time = self.monotonic()
        metrics = {"steps_executed": 0, "s1_calls": 0}
        recent_confs: Deque[Dict[str, Any]] = deque(maxlen=MAX_RECENT_CONFIDENCES)
        fsm_trace: List[str] = ["S0_INIT_VALIDATE"]

        final_frame: Optional[FrameSnapshot] = None
        matched_box: Optional[List[int]] = None
        matched_label: Optional[str] = None
        transient_frame: Optional[FrameSnapshot] = None
        annotated_frame: Optional[FrameSnapshot] = None
        watch_roi: List[int] = [0, 0, 1000, 1000]
        status = "error"
        breaker_code: Optional[str] = None
        detail = ""

        try:
            # S0_INIT_VALIDATE
            risk_tier = _normalize_risk_tier(args.get("risk_tier"), default="read_only")
            if args.get("watch_roi") is not None:
                watch_roi = validate_bbox(args["watch_roi"], name="watch_roi")
            pred = normalize_predicate(args.get("condition"), risk_tier=risk_tier, default_roi=watch_roi, name="condition")

            poll_ms = _clamp_int(args.get("poll_interval_ms", WAIT_DEFAULT_POLL_MS), WAIT_MIN_POLL_MS, WAIT_MAX_POLL_MS, "poll_interval_ms")
            timeout_ms = _clamp_int(args.get("timeout_ms", WAIT_DEFAULT_TIMEOUT_MS), 100, WAIT_TIMEOUT_CAP_MS, "timeout_ms")
            deadline = start_time + timeout_ms / 1000.0
            diff_gate = _clamp_float(args.get("diff_gate_threshold", WAIT_DEFAULT_DIFF_GATE), 0.0, 1.0, "diff_gate_threshold")
            heartbeat_n = _clamp_int(args.get("heartbeat_every_n", WAIT_DEFAULT_HEARTBEAT_N), 1, 100, "heartbeat_every_n")
            require_stable = _clamp_int(args.get("require_stable_frames", 1), 1, 10, "require_stable_frames")
            settle_ms = _clamp_int(args.get("settle_delay_ms", DEFAULT_SETTLE_DELAY_MS), 0, 5000, "settle_delay_ms")
            capture_transient = bool(args.get("capture_transient", False))
            ignore_scene_change = bool(args.get("ignore_scene_change", False))

            interrupt_preds: List[NormalizedPredicate] = [
                normalize_predicate(ip, risk_tier="interrupt", name=f"interrupt_predicates[{i}]")
                for i, ip in enumerate(args.get("interrupt_predicates") or [])
            ]

            locate_desc: Optional[str] = None
            raw_locate = args.get("locate_box")
            if isinstance(raw_locate, str) and raw_locate.strip():
                locate_desc = raw_locate.strip()
            elif isinstance(raw_locate, dict) and str(raw_locate.get("instructions") or "").strip():
                locate_desc = str(raw_locate["instructions"]).strip()

            baseline = self._capture_frame()
            final_frame = baseline
            last_eval_frame: Optional[FrameSnapshot] = None
            transient_buf: Deque[FrameSnapshot] = deque(maxlen=TRANSIENT_RING_BUFFER_SIZE)
            if capture_transient:
                transient_buf.append(baseline)

            consecutive_skips = 0
            stable_hits = 0
            two_frame_hits = 0
            poll_idx = 0

            while True:
                if self.monotonic() >= deadline:
                    raise TimeoutError("Wait deadline reached before the next poll")
                self._check_collaborate_pause(collaborate_mode)
                fsm_trace.append("S1_CAPTURE_AND_GATE")

                cur_frame = baseline if poll_idx == 0 else self._capture_frame()
                final_frame = cur_frame
                if capture_transient:
                    transient_buf.append(cur_frame)

                if poll_idx > 0 and not ignore_scene_change:
                    scene_ssim = compute_ssim(baseline, cur_frame)
                    if scene_ssim < FULL_SCREEN_INTERRUPT_SSIM:
                        status = "interrupted"
                        breaker_code = CB_INTERRUPT
                        detail = f"Full-screen SSIM dropped to {scene_ssim:.3f} (< {FULL_SCREEN_INTERRUPT_SSIM:.2f})"
                        break

                should_infer = True
                if poll_idx > 0 and last_eval_frame is not None and not interrupt_preds:
                    delta = compute_pixel_diff_ratio(last_eval_frame, cur_frame, roi=watch_roi)
                    if delta < diff_gate:
                        consecutive_skips += 1
                        if consecutive_skips < heartbeat_n:
                            should_infer = False
                        else:
                            consecutive_skips = 0
                    else:
                        consecutive_skips = 0

                if should_infer:
                    # S2_S1_INFERENCE
                    fsm_trace.append("S2_S1_INFERENCE")
                    last_eval_frame = cur_frame
                    prep_roi = prepare_image_for_system_one(cur_frame, watch_roi)
                    assets: Dict[str, Tuple[bytes, str]] = {"roi_img": (prep_roi.image_bytes, prep_roi.image_format)}
                    state_obj: Dict[str, Any] = {"roi": "roi_img"}
                    questions: Dict[str, Dict[str, Any]] = {
                        "condition": pred.to_system_one_question(image_ref_name="roi")
                    }
                    if locate_desc:
                        questions["locate_box"] = {"type": "box", "instructions": f"the {locate_desc} in `roi`"}

                    self._add_interrupt_questions(cur_frame, interrupt_preds, state_obj, questions, assets)
                    answers = self._query_s1(state_obj, questions, assets, metrics, deadline=deadline)

                    # S3_ADJUDICATE
                    fsm_trace.append("S3_ADJUDICATE")
                    intr = self._check_interrupts(interrupt_preds, answers)
                    if intr is not None:
                        status = "interrupted"
                        breaker_code = CB_INTERRUPT
                        detail = f"Interrupt check: {intr['label']} ({intr['answer_label']}, score={intr['score']})"
                        break

                    matched_box = None
                    if "locate_box" in answers:
                        cand_box, cand_conf = _extract_box_norm_and_conf(answers.get("locate_box"), prep_roi)
                        if cand_box is not None and cand_conf >= 0.5:
                            matched_box = cand_box

                    adj = adjudicate_predicate(pred, answers.get("condition"), risk_tier, prev_two_frame_hits=two_frame_hits)
                    self._record_confidence(recent_confs, poll_idx, pred.pred_type, adj.label, adj.score, adj.zone)
                    matched_label = adj.label

                    if adj.zone == "gray":
                        # S4_ACT_OR_RECOVER: 1x gray-zone retry
                        fsm_trace.append("S4_ACT_OR_RECOVER")
                        retry_adj, retry_frame, intr = self._run_gray_zone_retry(
                            pred,
                            risk_tier,
                            watch_roi,
                            settle_ms,
                            metrics,
                            recent_confs,
                            poll_idx,
                            prev_two_frame_hits=(1 if adj.two_frame_eligible else 0),
                            interrupt_preds=interrupt_preds,
                            deadline=deadline,
                        )
                        final_frame = retry_frame
                        matched_box = None
                        if intr is not None:
                            status, breaker_code = "interrupted", CB_INTERRUPT
                            detail = f"Interrupt check during retry: {intr['label']} ({intr['answer_label']}, score={intr['score']})"
                            break
                        if capture_transient:
                            transient_buf.append(retry_frame)
                        matched_label = retry_adj.label
                        if retry_adj.zone != "pass":
                            status = "escalated"
                            breaker_code = CB_AMBIGUITY
                            annotated_frame = retry_frame
                            detail = (
                                f"Ambiguity persisted after 1x gray-zone retry: "
                                f"label={retry_adj.label}, score={retry_adj.score:.3f}"
                            )
                            break
                        adj = retry_adj
                        if locate_desc:
                            prep = prepare_image_for_system_one(retry_frame, watch_roi)
                            box_answers = self._query_s1(
                                {"roi": "roi_img"},
                                {"locate_box": {"type": "box", "instructions": f"the {locate_desc} in `roi`"}},
                                {"roi_img": (prep.image_bytes, prep.image_format)},
                                metrics,
                                deadline=deadline,
                            )
                            box, confidence = _extract_box_norm_and_conf(box_answers.get("locate_box"), prep)
                            if confidence >= 0.5:
                                matched_box = box

                    if adj.zone == "pass":
                        if risk_tier == "irreversible":
                            status = "blocked"
                            breaker_code = CB_SAFETY_GUARD
                            annotated_frame = cur_frame
                            detail = "Irreversible condition matched; escalating to System 2 without local action."
                            break
                        two_frame_hits = (two_frame_hits + 1) if adj.two_frame_eligible or adj.score >= pred.tau_high else 0
                        stable_hits += 1
                        if capture_transient:
                            transient_frame = transient_buf[-1] if transient_buf else cur_frame
                        if stable_hits >= require_stable:
                            status = "matched"
                            breaker_code = None
                            detail = f"Condition matched ({adj.label}, score={adj.score:.3f}) after {stable_hits} stable frame(s)."
                            break
                    else:
                        stable_hits = 0
                        two_frame_hits = 1 if adj.two_frame_eligible else 0

                # S4_ACT_OR_RECOVER: check budget & sleep poll_interval
                fsm_trace.append("S4_ACT_OR_RECOVER")
                metrics["steps_executed"] += 1
                poll_idx += 1
                remaining = deadline - self.monotonic()
                if remaining > 0:
                    self.sleep(min(poll_ms / 1000.0, remaining))

        except TimeoutError as exc:
            status, breaker_code = "timeout", CB_BUDGET_EXCEEDED
            detail = f"wait_until deadline ({timeout_ms}ms): {exc}"
        except SystemOneApiError as exc:
            status = "error"
            breaker_code = None
            detail = str(exc)
        finally:
            fsm_trace.append("S5_HANDOFF_TO_S2")
            self._safe_release_inputs()

        return self._build_handoff_result(
            tool_name="wait_until",
            status=status,
            breaker_code=breaker_code,
            detail=detail,
            metrics=metrics,
            start_time=start_time,
            final_frame=final_frame,
            matched_box=matched_box,
            matched_label=matched_label,
            recent_confs=recent_confs,
            annotated_crop_frame=annotated_frame,
            annotated_crop_roi=watch_roi,
            transient_frame=transient_frame,
            transient_roi=watch_roi,
            fsm_trace=fsm_trace,
        )

    # -----------------------------------------------------------------------
    # Tool 2: loop_until
    # -----------------------------------------------------------------------

    def run_loop_until(
        self,
        args: Dict[str, Any],
        collaborate_mode: bool = False,
        *,
        _include_crop_box_check: bool = True,
        _confirm_crop_roi: Optional[Sequence[int]] = None,
    ) -> Dict[str, Any]:
        start_time = self.monotonic()
        metrics = {"steps_executed": 0, "s1_calls": 0}
        recent_confs: Deque[Dict[str, Any]] = deque(maxlen=MAX_RECENT_CONFIDENCES)
        fsm_trace: List[str] = ["S0_INIT_VALIDATE"]

        final_frame: Optional[FrameSnapshot] = None
        matched_box: Optional[List[int]] = None
        matched_label: Optional[str] = None
        annotated_frame: Optional[FrameSnapshot] = None
        roi: List[int] = [0, 0, 1000, 1000]
        status = "error"
        breaker_code: Optional[str] = None
        detail = ""

        try:
            # S0_INIT_VALIDATE
            if args.get("roi") is not None:
                roi = validate_bbox(args["roi"], name="roi")
            progress_roi = (
                validate_bbox(args["progress_roi"], name="progress_roi")
                if args.get("progress_roi") is not None
                else list(roi)
            )

            on_match_raw = args.get("on_match")
            on_match_mode = "none"
            on_match_desc = ""
            on_match_action: Optional[Dict[str, Any]] = None
            on_match_button = "left"
            on_match_count = 1
            on_match_box_min_conf: Optional[float] = None
            if isinstance(on_match_raw, dict):
                on_match_mode = str(on_match_raw.get("mode", "none")).strip().lower()
                if on_match_mode not in ("none", "click_target_box", "action"):
                    raise ValueError(f"on_match.mode={on_match_mode!r} must be none, click_target_box, or action")
                on_match_desc = str(on_match_raw.get("target_description") or on_match_raw.get("instructions") or "").strip()
                on_match_button = str(on_match_raw.get("button", "left")).strip().lower()
                if on_match_button not in ("left", "middle", "right"):
                    raise ValueError(f"on_match.button={on_match_button!r} must be left, middle, or right")
                on_match_count = _clamp_int(on_match_raw.get("count", 1), 1, 3, "on_match.count")
                if on_match_raw.get("box_min_confidence") is not None:
                    on_match_box_min_conf = _clamp_float(on_match_raw["box_min_confidence"], 0.0, 1.0, "on_match.box_min_confidence")
                if on_match_mode == "click_target_box" and not on_match_desc:
                    raise ValueError("on_match with mode='click_target_box' requires 'target_description'")
                if on_match_mode == "action":
                    on_match_action = validate_host_action(on_match_raw.get("action"), name="on_match.action")

            default_tier = "reversible" if on_match_mode != "none" else "read_only"
            risk_tier = _normalize_risk_tier(args.get("risk_tier"), default=default_tier)
            stop_pred = normalize_predicate(args.get("stop_predicate"), risk_tier=risk_tier, default_roi=roi, name="stop_predicate")

            keyword = _contains_irreversible_keyword(on_match_desc) or _contains_irreversible_keyword(stop_pred.instructions)
            if risk_tier == "irreversible" or (on_match_mode != "none" and keyword is not None):
                final_frame = self._capture_frame()
                status = "blocked"
                breaker_code = CB_SAFETY_GUARD
                detail = f"Safety guard prohibits host-side loop input ({keyword or 'irreversible risk tier'})."
                return self._build_handoff_result(
                    tool_name="loop_until",
                    status=status,
                    breaker_code=breaker_code,
                    detail=detail,
                    metrics=metrics,
                    start_time=start_time,
                    final_frame=final_frame,
                    recent_confs=recent_confs,
                    annotated_crop_frame=final_frame,
                    annotated_crop_roi=roi,
                    fsm_trace=[*fsm_trace, "S5_HANDOFF_TO_S2"],
                )

            step_action: Optional[Dict[str, Any]] = None
            if args.get("step_action") is not None:
                step_action = validate_host_action(args["step_action"], name="step_action")

            directional_actions: Dict[str, Dict[str, Any]] = {}
            if args.get("directional_actions") is not None:
                raw_dir = args["directional_actions"]
                if not isinstance(raw_dir, dict):
                    raise ValueError("directional_actions must be an object mapping choice label -> HostAction")
                for lbl, act in raw_dir.items():
                    directional_actions[str(lbl).strip()] = validate_host_action(act, name=f"directional_actions[{lbl!r}]")

            if step_action is None and not directional_actions:
                raise ValueError("loop_until requires either 'step_action' or 'directional_actions'")

            fallback_action: Optional[Dict[str, Any]] = None
            if args.get("fallback_action") is not None:
                fallback_action = validate_host_action(args["fallback_action"], name="fallback_action")

            max_steps = _clamp_int(args.get("max_steps", LOOP_DEFAULT_MAX_STEPS), 1, LOOP_MAX_STEPS_CAP, "max_steps")
            ssim_stall = _clamp_float(args.get("ssim_stall_threshold", DEFAULT_SSIM_STALL_THRESHOLD), 0.5, 1.0, "ssim_stall_threshold")
            max_stalls = _clamp_int(args.get("max_stalls", DEFAULT_MAX_STALLS), 1, 10, "max_stalls")
            settle_ms = _clamp_int(args.get("settle_delay_ms", DEFAULT_SETTLE_DELAY_MS), 0, 5000, "settle_delay_ms")
            ignore_scene_change = bool(args.get("ignore_scene_change", False))

            interrupt_preds: List[NormalizedPredicate] = [
                normalize_predicate(ip, risk_tier="interrupt", name=f"interrupt_predicates[{i}]")
                for i, ip in enumerate(args.get("interrupt_predicates") or [])
            ]

            baseline = self._capture_frame()
            final_frame = baseline
            prev_frame: Optional[FrameSnapshot] = None
            using_fallback = False
            consecutive_stalls = 0
            post_fallback_stalls = 0
            two_frame_hits = 0

            for step_idx in range(max_steps + 1):
                self._check_collaborate_pause(collaborate_mode)
                fsm_trace.append("S1_CAPTURE_AND_GATE")
                stall_detail = None

                cur_frame = baseline if step_idx == 0 else self._capture_frame()
                final_frame = cur_frame

                if step_idx > 0 and not ignore_scene_change:
                    scene_ssim = compute_ssim(baseline, cur_frame)
                    if scene_ssim < FULL_SCREEN_INTERRUPT_SSIM:
                        status = "interrupted"
                        breaker_code = CB_INTERRUPT
                        detail = f"Full-screen SSIM dropped to {scene_ssim:.3f} (< {FULL_SCREEN_INTERRUPT_SSIM:.2f})"
                        break

                if step_idx > 0 and prev_frame is not None:
                    prog_ssim = compute_ssim(prev_frame, cur_frame, roi=progress_roi)
                    if prog_ssim >= ssim_stall:
                        consecutive_stalls += 1
                        if fallback_action is not None and not using_fallback:
                            using_fallback = True
                            post_fallback_stalls = 0
                        else:
                            if using_fallback:
                                post_fallback_stalls += 1
                            stalls_to_check = post_fallback_stalls if using_fallback else consecutive_stalls
                            if stalls_to_check >= max_stalls:
                                stall_detail = (
                                    f"progress_roi SSIM={prog_ssim:.4f} >= {ssim_stall:.3f} "
                                    f"for {stalls_to_check} consecutive frame(s)"
                                )
                    else:
                        consecutive_stalls = 0
                        post_fallback_stalls = 0

                # S2_S1_INFERENCE
                fsm_trace.append("S2_S1_INFERENCE")
                prep_roi = prepare_image_for_system_one(cur_frame, roi)
                assets: Dict[str, Tuple[bytes, str]] = {"roi_img": (prep_roi.image_bytes, prep_roi.image_format)}
                state_obj: Dict[str, Any] = {"roi": "roi_img"}
                questions: Dict[str, Dict[str, Any]] = {
                    "stop": stop_pred.to_system_one_question(image_ref_name="roi")
                }

                box_prep = prep_roi
                screen_ref_key = "roi"
                if on_match_mode == "click_target_box":
                    if roi != [0, 0, 1000, 1000]:
                        prep_full = prepare_image_for_system_one(cur_frame, [0, 0, 1000, 1000])
                        assets["full_img"] = (prep_full.image_bytes, prep_full.image_format)
                        state_obj["screen"] = "full_img"
                        screen_ref_key = "screen"
                        box_prep = prep_full

                if on_match_mode == "click_target_box":
                    questions["target_box"] = {
                        "type": "box",
                        "instructions": f"the {on_match_desc} in `{screen_ref_key}`",
                    }
                self._add_interrupt_questions(cur_frame, interrupt_preds, state_obj, questions, assets)

                answers = self._query_s1(state_obj, questions, assets, metrics)

                # S3_ADJUDICATE
                fsm_trace.append("S3_ADJUDICATE")
                intr = self._check_interrupts(interrupt_preds, answers)
                if intr is not None:
                    status = "interrupted"
                    breaker_code = CB_INTERRUPT
                    detail = f"Interrupt check: {intr['label']} ({intr['answer_label']}, score={intr['score']})"
                    break

                cand_box: Optional[List[int]] = None
                cand_box_conf = 0.0
                if "target_box" in answers:
                    cand_box, cand_box_conf = _extract_box_norm_and_conf(answers.get("target_box"), box_prep)
                    if cand_box is not None:
                        matched_box = cand_box

                adj = adjudicate_predicate(stop_pred, answers.get("stop"), risk_tier, prev_two_frame_hits=two_frame_hits)
                self._record_confidence(recent_confs, step_idx, stop_pred.pred_type, adj.label, adj.score, adj.zone)
                matched_label = adj.label

                used_stop_retry = False
                if adj.zone == "gray":
                    fsm_trace.append("S4_ACT_OR_RECOVER")
                    retry_adj, retry_frame, intr = self._run_gray_zone_retry(
                        stop_pred,
                        risk_tier,
                        roi,
                        settle_ms,
                        metrics,
                        recent_confs,
                        step_idx,
                        prev_two_frame_hits=(1 if adj.two_frame_eligible else 0),
                        interrupt_preds=interrupt_preds,
                    )
                    final_frame = retry_frame
                    if intr is not None:
                        status, breaker_code = "interrupted", CB_INTERRUPT
                        detail = f"Interrupt check during retry: {intr['label']} ({intr['answer_label']}, score={intr['score']})"
                        break
                    matched_label = retry_adj.label
                    if retry_adj.zone != "pass":
                        status = "escalated"
                        breaker_code = CB_AMBIGUITY
                        annotated_frame = retry_frame
                        detail = (
                            f"Gray-zone ambiguity persisted on stop_predicate after 1x retry: "
                            f"label={retry_adj.label}, score={retry_adj.score:.3f}"
                        )
                        break
                    adj = retry_adj
                    cur_frame = retry_frame
                    used_stop_retry = True

                if adj.zone == "pass":
                    if on_match_mode == "click_target_box":
                        tier_cfg = RISK_TIER_THRESHOLDS[risk_tier]
                        global_floor = max(
                            float(tier_cfg["box_global"] if tier_cfg["box_global"] is not None else 0.85),
                            on_match_box_min_conf or 0.0,
                        )
                        tau_low = float(tier_cfg["tau_low"] if tier_cfg["tau_low"] is not None else 0.65)
                        if used_stop_retry or (tau_low <= cand_box_conf < global_floor):
                            fsm_trace.append("S4_ACT_OR_RECOVER")
                            if not used_stop_retry and settle_ms > 0:
                                self.sleep(settle_ms / 1000.0)
                            prep_retry_full = prepare_image_for_system_one(cur_frame, [0, 0, 1000, 1000])
                            retry_box_ans = self._query_s1(
                                state={"screen": "full_img"},
                                questions={
                                    "target_box": {
                                        "type": "box",
                                        "instructions": f"the {on_match_desc} in `screen`",
                                    }
                                },
                                image_assets={"full_img": (prep_retry_full.image_bytes, prep_retry_full.image_format)},
                                metrics=metrics,
                            )
                            cand_box, cand_box_conf = _extract_box_norm_and_conf(
                                retry_box_ans.get("target_box"), prep_retry_full
                            )
                            if cand_box is not None:
                                matched_box = cand_box
                            self._record_confidence(
                                recent_confs,
                                step_idx,
                                "box:retry",
                                on_match_desc[:40],
                                cand_box_conf,
                                "pass" if cand_box_conf >= global_floor else "fail",
                            )

                        if not _include_crop_box_check and cand_box_conf < global_floor:
                            geom_ok, geom_msg = False, f"global box confidence {cand_box_conf:.3f} < {global_floor:.2f}"
                        else:
                            geom_ok, geom_msg = self._verify_box_geometry_and_crop(
                                frame=cur_frame,
                                box_norm=cand_box,
                                box_conf=cand_box_conf,
                                roi_norm=roi,
                                target_desc=on_match_desc,
                                risk_tier=risk_tier,
                                metrics=metrics,
                                recent_confs=recent_confs,
                                step=step_idx,
                                caller_box_min_conf=on_match_box_min_conf,
                                include_crop_box_check=_include_crop_box_check,
                                confirm_crop_roi=_confirm_crop_roi,
                            )
                        if not geom_ok:
                            status = "escalated"
                            breaker_code = CB_AMBIGUITY
                            annotated_frame = cur_frame
                            detail = f"Box geometry pre-click check failed: {geom_msg}"
                            break

                        assert cand_box is not None
                        final_frame = self._capture_frame()
                        if (
                            compute_pixel_diff_ratio(cur_frame, final_frame, roi=roi) > 0.0
                            or compute_pixel_diff_ratio(cur_frame, final_frame, roi=cand_box) > 0.0
                        ):
                            status, breaker_code = "interrupted", CB_INTERRUPT
                            detail = "Target changed after box confirmation and before the click."
                            break
                        cy = int(round((cand_box[0] + cand_box[2]) / 2.0))
                        cx = int(round((cand_box[1] + cand_box[3]) / 2.0))
                        fsm_trace.append("S4_ACT_OR_RECOVER")
                        self.executor.execute_host_action(
                            {"type": "click", "x": cx, "y": cy, "button": on_match_button, "count": on_match_count}
                        )
                        if settle_ms > 0:
                            self.sleep(settle_ms / 1000.0)
                        post_click = self._capture_frame()
                        final_frame = post_click
                        if risk_tier == "reversible":
                            delta = compute_pixel_diff_ratio(cur_frame, post_click, channel_threshold=2)
                            if delta <= 0.0:
                                status = "escalated"
                                breaker_code = CB_AMBIGUITY
                                annotated_frame = post_click
                                detail = "on_match click produced zero post-click pixel delta."
                                break
                    elif on_match_mode == "action" and on_match_action is not None:
                        fsm_trace.append("S4_ACT_OR_RECOVER")
                        final_frame = self._capture_frame()
                        if compute_pixel_diff_ratio(cur_frame, final_frame, roi=roi) > 0.0:
                            status, breaker_code = "interrupted", CB_INTERRUPT
                            detail = "Stop predicate region changed before the on-match action."
                            break
                        self.executor.execute_host_action(on_match_action)
                        if settle_ms > 0:
                            self.sleep(settle_ms / 1000.0)
                        final_frame = self._capture_frame()

                    status = "matched"
                    breaker_code = None
                    detail = f"stop_predicate matched ({adj.label}, score={adj.score:.3f}) in {metrics['steps_executed']} step(s)."
                    break

                two_frame_hits = 1 if adj.two_frame_eligible else 0

                # S4_ACT_OR_RECOVER: check step budget and inject next step action
                fsm_trace.append("S4_ACT_OR_RECOVER")
                if stall_detail is not None:
                    status, breaker_code = "stalled", CB_STALL
                    detail = stall_detail
                    break
                if metrics["steps_executed"] >= max_steps:
                    status = "budget_exceeded"
                    breaker_code = CB_BUDGET_EXCEEDED
                    detail = f"loop_until reached max_steps={max_steps} without matching."
                    break

                chosen_action: Optional[Dict[str, Any]] = None
                if using_fallback and fallback_action is not None:
                    chosen_action = fallback_action
                elif directional_actions and stop_pred.pred_type == "choice":
                    chosen_action = directional_actions.get(adj.label, step_action)
                else:
                    chosen_action = step_action

                if chosen_action is None:
                    status = "escalated"
                    breaker_code = CB_AMBIGUITY
                    annotated_frame = cur_frame
                    detail = f"No step_action or directional_action mapped for choice {adj.label!r}"
                    break

                prev_frame = cur_frame
                self.executor.execute_host_action(chosen_action)
                metrics["steps_executed"] += 1
                if settle_ms > 0:
                    self.sleep(settle_ms / 1000.0)

        except SystemOneApiError as exc:
            status = "error"
            breaker_code = None
            detail = str(exc)
        finally:
            fsm_trace.append("S5_HANDOFF_TO_S2")
            self._safe_release_inputs()

        return self._build_handoff_result(
            tool_name="loop_until",
            status=status,
            breaker_code=breaker_code,
            detail=detail,
            metrics=metrics,
            start_time=start_time,
            final_frame=final_frame,
            matched_box=matched_box,
            matched_label=matched_label,
            recent_confs=recent_confs,
            annotated_crop_frame=annotated_frame,
            annotated_crop_roi=roi,
            fsm_trace=fsm_trace,
        )

    # -----------------------------------------------------------------------
    # Tool 3: batch_apply
    # -----------------------------------------------------------------------

    def run_batch_apply(
        self,
        args: Dict[str, Any],
        collaborate_mode: bool = False,
        *,
        _slot_predicates: Optional[Dict[str, NormalizedPredicate]] = None,
        _locate_box_prompts: Optional[Dict[str, str]] = None,
        _allow_partial_errors: bool = False,
        _precaptured_frame: Optional[FrameSnapshot] = None,
        _skip_handoff_png: bool = False,
        _release_inputs: bool = True,
    ) -> Dict[str, Any]:
        start_time = self.monotonic()
        metrics = {"steps_executed": 0, "s1_calls": 0}
        recent_confs: Deque[Dict[str, Any]] = deque(maxlen=MAX_RECENT_CONFIDENCES)
        fsm_trace: List[str] = ["S0_INIT_VALIDATE"]

        final_frame: Optional[FrameSnapshot] = None
        matched_box: Optional[List[int]] = None
        matched_label: Optional[str] = None
        annotated_frame: Optional[FrameSnapshot] = None
        annotated_roi: List[int] = [0, 0, 1000, 1000]
        status = "error"
        breaker_code: Optional[str] = None
        detail = ""

        batch_summary: Dict[str, Any] = {
            "pages_processed": 0,
            "items_evaluated": 0,
            "items_matched": 0,
            "mutations_executed": 0,
            "errors": 0,
            "items": [],
        }

        try:
            # S0_INIT_VALIDATE
            item_action = str(args.get("item_action", "noop_collect")).strip().lower()
            if item_action not in ("noop_collect", "click_target_box", "click_relative"):
                raise ValueError(
                    f"item_action={item_action!r} must be 'noop_collect', 'click_target_box', or 'click_relative'"
                )
            default_tier = "read_only" if item_action == "noop_collect" else "reversible"
            risk_tier = _normalize_risk_tier(args.get("risk_tier"), default=default_tier)

            page_roi = (
                validate_bbox(args["page_roi"], name="page_roi")
                if args.get("page_roi") is not None
                else [0, 0, 1000, 1000]
            )
            annotated_roi = list(page_roi)
            progress_roi = (
                validate_bbox(args["progress_roi"], name="progress_roi")
                if args.get("progress_roi") is not None
                else list(page_roi)
            )

            if risk_tier == "irreversible" and (item_action != "noop_collect" or args.get("next_page_action") is not None):
                final_frame = self._capture_frame()
                status = "blocked"
                breaker_code = CB_SAFETY_GUARD
                detail = "Irreversible risk tier prohibits batch item actions and pagination input."
                return self._build_handoff_result(
                    tool_name="batch_apply",
                    status=status,
                    breaker_code=breaker_code,
                    detail=detail,
                    metrics=metrics,
                    start_time=start_time,
                    final_frame=final_frame,
                    recent_confs=recent_confs,
                    batch_summary=batch_summary,
                    annotated_crop_frame=final_frame,
                    annotated_crop_roi=page_roi,
                    fsm_trace=[*fsm_trace, "S5_HANDOFF_TO_S2"],
                )

            raw_item_pred = args.get("item_predicate")
            item_pred: Optional[NormalizedPredicate] = (
                normalize_predicate(raw_item_pred, risk_tier=risk_tier, default_roi=page_roi, name="item_predicate")
                if raw_item_pred is not None
                else None
            )
            locate_mode = str(args.get("locate_mode", "fixed_slots")).strip().lower()
            if locate_mode not in ("fixed_slots", "dynamic_box"):
                raise ValueError(f"locate_mode={locate_mode!r} must be 'fixed_slots' or 'dynamic_box'")

            fixed_slots: List[Dict[str, Any]] = []
            if locate_mode == "fixed_slots":
                raw_slots = args.get("slots")
                if raw_slots is not None:
                    if not isinstance(raw_slots, list) or not raw_slots:
                        raise ValueError("slots must be a non-empty list when provided")
                    for idx, s in enumerate(raw_slots):
                        if isinstance(s, dict) and "roi" in s:
                            s_roi = validate_bbox(s["roi"], name=f"slots[{idx}].roi")
                            s_id = str(s.get("id") or f"slot_{idx}")
                        else:
                            s_roi = validate_bbox(s, name=f"slots[{idx}]")
                            s_id = f"slot_{idx}"
                        fixed_slots.append({"id": s_id, "roi": s_roi})
                    if len({slot["id"] for slot in fixed_slots}) != len(fixed_slots):
                        raise ValueError("slots must have unique IDs")
                elif args.get("slot_grid") is not None:
                    grid = args["slot_grid"]
                    if not isinstance(grid, dict):
                        raise ValueError("slot_grid must be an object with 'rows' and 'cols'")
                    rows = _clamp_int(grid.get("rows", 1), 1, 20, "slot_grid.rows")
                    cols = _clamp_int(grid.get("cols", 1), 1, 20, "slot_grid.cols")
                    for idx, s_roi in enumerate(_subdivide_slot_grid(page_roi, rows, cols)):
                        fixed_slots.append({"id": f"slot_{idx}", "roi": s_roi})
                else:
                    raise ValueError("locate_mode='fixed_slots' requires 'slots' or 'slot_grid'")

            if item_pred is None:
                if _slot_predicates and fixed_slots and all(s["id"] in _slot_predicates for s in fixed_slots):
                    item_pred = _slot_predicates[fixed_slots[0]["id"]]
                else:
                    item_pred = normalize_predicate(raw_item_pred, risk_tier=risk_tier, default_roi=page_roi, name="item_predicate")

            dynamic_box_prompts: List[str] = []
            if locate_mode == "dynamic_box":
                raw_prompts = args.get("dynamic_box_prompts")
                if isinstance(raw_prompts, list) and raw_prompts:
                    dynamic_box_prompts = [str(p).strip() for p in raw_prompts if str(p).strip()]
                elif args.get("dynamic_box_prompt"):
                    dynamic_box_prompts = [str(args["dynamic_box_prompt"]).strip()]
                elif args.get("target_description"):
                    dynamic_box_prompts = [str(args["target_description"]).strip()]
                if not dynamic_box_prompts:
                    raise ValueError("locate_mode='dynamic_box' requires 'dynamic_box_prompts', 'dynamic_box_prompt', or 'target_description'")

            target_desc = str(args.get("target_description") or item_pred.instructions).strip()
            relative_offset = args.get("relative_offset") or [0.5, 0.5]
            if not isinstance(relative_offset, (list, tuple)) or len(relative_offset) != 2:
                raise ValueError("relative_offset must be a 2-element [rel_y, rel_x] list")
            rel_y = _clamp_float(relative_offset[0], 0.0, 1.0, "relative_offset[0]")
            rel_x = _clamp_float(relative_offset[1], 0.0, 1.0, "relative_offset[1]")

            next_page_action: Optional[Dict[str, Any]] = None
            if args.get("next_page_action") is not None:
                next_page_action = validate_host_action(args["next_page_action"], name="next_page_action")
            fallback_next_page: Optional[Dict[str, Any]] = None
            if args.get("fallback_next_page_action") is not None:
                fallback_next_page = validate_host_action(args["fallback_next_page_action"], name="fallback_next_page_action")

            max_pages = _clamp_int(args.get("max_pages", BATCH_DEFAULT_MAX_PAGES), 1, BATCH_MAX_PAGES_CAP, "max_pages")
            max_mutations = _clamp_int(args.get("max_mutations", BATCH_DEFAULT_MAX_MUTATIONS), 0, BATCH_MAX_MUTATIONS_CAP, "max_mutations")
            max_error_ratio = _clamp_float(args.get("max_error_ratio", BATCH_MAX_ERROR_RATIO_CAP), 0.0, BATCH_MAX_ERROR_RATIO_CAP, "max_error_ratio")
            verify_after_action = bool(args.get("verify_after_action", True))
            settle_ms = _clamp_int(args.get("settle_delay_ms", DEFAULT_SETTLE_DELAY_MS), 0, 5000, "settle_delay_ms")
            ssim_stall = _clamp_float(args.get("ssim_stall_threshold", DEFAULT_SSIM_STALL_THRESHOLD), 0.5, 1.0, "ssim_stall_threshold")
            max_stalls = _clamp_int(args.get("max_stalls", DEFAULT_MAX_STALLS), 1, 10, "max_stalls")
            ignore_scene_change = bool(args.get("ignore_scene_change", False))

            interrupt_preds: List[NormalizedPredicate] = [
                normalize_predicate(ip, risk_tier="interrupt", name=f"interrupt_predicates[{i}]")
                for i, ip in enumerate(args.get("interrupt_predicates") or [])
            ]

            baseline = _precaptured_frame if _precaptured_frame is not None else self._capture_frame()
            final_frame = baseline
            consecutive_zero_delta_clicks = 0
            using_fallback = False
            consecutive_stalls = 0
            post_fallback_stalls = 0
            prev_page_frame: Optional[FrameSnapshot] = None

            for page_idx in range(max_pages):
                self._check_collaborate_pause(collaborate_mode)
                fsm_trace.append("S1_CAPTURE_AND_GATE")

                cur_frame = baseline if page_idx == 0 else self._capture_frame()
                final_frame = cur_frame

                if page_idx > 0 and not ignore_scene_change:
                    scene_ssim = compute_ssim(baseline, cur_frame)
                    if scene_ssim < FULL_SCREEN_INTERRUPT_SSIM:
                        status = "interrupted"
                        breaker_code = CB_INTERRUPT
                        detail = f"Full-screen SSIM dropped to {scene_ssim:.3f} (< {FULL_SCREEN_INTERRUPT_SSIM:.2f})"
                        break

                if page_idx > 0 and prev_page_frame is not None:
                    prog_ssim = compute_ssim(prev_page_frame, cur_frame, roi=progress_roi)
                    if prog_ssim >= ssim_stall:
                        consecutive_stalls += 1
                        if fallback_next_page is not None and not using_fallback:
                            using_fallback = True
                            post_fallback_stalls = 0
                        else:
                            if using_fallback:
                                post_fallback_stalls += 1
                            stalls_to_check = post_fallback_stalls if using_fallback else consecutive_stalls
                            if stalls_to_check >= max_stalls:
                                status = "stalled"
                                breaker_code = CB_STALL
                                detail = (
                                    f"Pagination progress_roi SSIM={prog_ssim:.4f} >= {ssim_stall:.3f} "
                                    f"for {stalls_to_check} consecutive page(s)"
                                )
                                break
                    else:
                        consecutive_stalls = 0
                        post_fallback_stalls = 0

                # Locate slots for current page
                page_slots: List[Dict[str, Any]]
                if locate_mode == "fixed_slots":
                    page_slots = fixed_slots
                else:
                    # S2_S1_INFERENCE for dynamic_box item location
                    fsm_trace.append("S2_S1_INFERENCE")
                    prep_page = prepare_image_for_system_one(cur_frame, page_roi)
                    loc_questions = {
                        f"dyn_{i}": {"type": "box", "instructions": f"the {p} in `page`"}
                        for i, p in enumerate(dynamic_box_prompts)
                    }
                    loc_answers = self._query_s1(
                        state={"page": "page_img"},
                        questions=loc_questions,
                        image_assets={"page_img": (prep_page.image_bytes, prep_page.image_format)},
                        metrics=metrics,
                    )
                    page_slots = []
                    for i, p in enumerate(dynamic_box_prompts):
                        b_norm, b_conf = _extract_box_norm_and_conf(loc_answers.get(f"dyn_{i}"), prep_page)
                        if b_norm is not None:
                            page_slots.append({"id": f"dyn_{i}", "roi": b_norm, "box_conf": b_conf, "prompt": p})
                    if not page_slots:
                        status = "escalated"
                        breaker_code = CB_AMBIGUITY
                        annotated_frame = cur_frame
                        detail = "dynamic_box failed to locate any candidate slots on page."
                        break

                # S2_S1_INFERENCE: evaluate ALL visible slots in ONE batched Parallel Questions call
                fsm_trace.append("S2_S1_INFERENCE")
                batch_assets: Dict[str, Tuple[bytes, str]] = {}
                batch_state: Dict[str, Any] = {"slots": {}}
                batch_questions: Dict[str, Dict[str, Any]] = {}
                slot_preps: Dict[str, PreparedImage] = {}

                for slot in page_slots:
                    sid = slot["id"]
                    s_roi = slot["roi"]
                    prep_s = prepare_image_for_system_one(cur_frame, s_roi)
                    slot_preps[sid] = prep_s
                    ref = f"asset_{sid}"
                    batch_assets[ref] = (prep_s.image_bytes, prep_s.image_format)
                    batch_state["slots"][sid] = ref
                    slot_ref = f"slots.{sid}"

                    slot_pred = _slot_predicates.get(sid, item_pred) if _slot_predicates else item_pred
                    q_eval = slot_pred.to_system_one_question(image_ref_name=slot_ref)
                    batch_questions[f"eval_{sid}"] = q_eval
                    if item_action == "click_target_box":
                        batch_questions[f"box_{sid}"] = {
                            "type": "box",
                            "instructions": f"the {target_desc} in `{slot_ref}`",
                        }
                    elif _locate_box_prompts and sid in _locate_box_prompts:
                        batch_questions[f"box_{sid}"] = {
                            "type": "box",
                            "instructions": f"the {_locate_box_prompts[sid]} in `{slot_ref}`",
                        }

                self._add_interrupt_questions(cur_frame, interrupt_preds, batch_state, batch_questions, batch_assets)

                answers = self._query_s1(batch_state, batch_questions, batch_assets, metrics)

                # S3_ADJUDICATE
                fsm_trace.append("S3_ADJUDICATE")
                intr = self._check_interrupts(interrupt_preds, answers)
                if intr is not None:
                    status = "interrupted"
                    breaker_code = CB_INTERRUPT
                    detail = f"Interrupt check: {intr['label']} ({intr['answer_label']}, score={intr['score']})"
                    break

                page_errors = 0
                aborted_breaker: Optional[str] = None
                working_frame = cur_frame

                for slot in page_slots:
                    if not _allow_partial_errors and page_errors / len(page_slots) > max_error_ratio:
                        break
                    sid = slot["id"]
                    s_roi = slot["roi"]
                    batch_summary["items_evaluated"] += 1
                    slot_pred = _slot_predicates.get(sid, item_pred) if _slot_predicates else item_pred

                    located_box_info: Dict[str, Any] = {}
                    if f"box_{sid}" in answers:
                        loc_b, loc_c = _extract_box_norm_and_conf(answers.get(f"box_{sid}"), slot_preps[sid])
                        located_box_info = {
                            "located_box": loc_b,
                            "box_conf": round(loc_c, 4),
                        }

                    adj = adjudicate_predicate(slot_pred, answers.get(f"eval_{sid}"), risk_tier)
                    self._record_confidence(recent_confs, page_idx, slot_pred.pred_type, adj.label, adj.score, adj.zone)

                    if adj.zone == "gray":
                        fsm_trace.append("S4_ACT_OR_RECOVER")
                        retry_adj, retry_frame, intr = self._run_gray_zone_retry(
                            slot_pred, risk_tier, s_roi, settle_ms, metrics, recent_confs, page_idx,
                            interrupt_preds=interrupt_preds,
                        )
                        working_frame = retry_frame
                        final_frame = retry_frame
                        if intr is not None:
                            aborted_breaker = CB_INTERRUPT
                            detail = f"Interrupt check during retry: {intr['label']} ({intr['answer_label']}, score={intr['score']})"
                            break
                        if retry_adj.zone != "pass":
                            page_errors += 1
                            batch_summary["errors"] += 1
                            item_entry: Dict[str, Any] = {
                                "page": page_idx,
                                "slot_id": sid,
                                "roi": s_roi,
                                "status": "gray_fail",
                                "label": retry_adj.label,
                                "score": round(retry_adj.score, 4),
                            }
                            if located_box_info:
                                item_entry.update(located_box_info)
                            batch_summary["items"].append(item_entry)
                            continue
                        adj = retry_adj

                    if adj.zone == "reject":
                        item_entry = {
                            "page": page_idx,
                            "slot_id": sid,
                            "roi": s_roi,
                            "status": "skipped",
                            "label": adj.label,
                            "score": round(adj.score, 4),
                        }
                        if located_box_info:
                            item_entry.update(located_box_info)
                        batch_summary["items"].append(item_entry)
                        continue

                    # adj.zone == "pass"
                    batch_summary["items_matched"] += 1
                    matched_label = adj.label

                    if item_action == "noop_collect":
                        if risk_tier == "irreversible":
                            matched_box = s_roi
                            aborted_breaker = CB_SAFETY_GUARD
                            annotated_frame = working_frame
                            annotated_roi = s_roi
                            detail = "Irreversible target matched; returning annotated candidate only."
                            break
                        item_entry = {
                            "page": page_idx,
                            "slot_id": sid,
                            "roi": s_roi,
                            "status": "collected",
                            "label": adj.label,
                            "score": round(adj.score, 4),
                        }
                        if located_box_info:
                            item_entry.update(located_box_info)
                        batch_summary["items"].append(item_entry)
                        continue

                    # Mutating action on matched slot
                    if batch_summary["mutations_executed"] >= max_mutations:
                        aborted_breaker = CB_BUDGET_EXCEEDED
                        detail = f"Reached max_mutations={max_mutations}"
                        break

                    working_frame = self._capture_frame()
                    final_frame = working_frame
                    if compute_pixel_diff_ratio(cur_frame, working_frame, roi=s_roi) > 0.0:
                        aborted_breaker = CB_BATCH_DRIFT
                        detail = f"Slot {sid} changed after page evaluation. Re-evaluate the current layout."
                        break

                    kw = _contains_irreversible_keyword(target_desc) or _contains_irreversible_keyword(item_pred.instructions)
                    if kw is not None or risk_tier == "irreversible":
                        matched_box = s_roi
                        aborted_breaker = CB_SAFETY_GUARD
                        annotated_frame = working_frame
                        annotated_roi = s_roi
                        detail = f"Safety guardrail triggered ({kw or 'irreversible'}); zero input injection."
                        break

                    if item_action == "click_target_box":
                        cand_box, cand_conf = _extract_box_norm_and_conf(answers.get(f"box_{sid}"), slot_preps[sid])
                        if cand_box is not None:
                            matched_box = cand_box
                        geom_ok, geom_msg = self._verify_box_geometry_and_crop(
                            frame=working_frame,
                            box_norm=cand_box,
                            box_conf=cand_conf,
                            roi_norm=s_roi,
                            target_desc=target_desc,
                            risk_tier=risk_tier,
                            metrics=metrics,
                            recent_confs=recent_confs,
                            step=page_idx,
                        )
                        if not geom_ok:
                            page_errors += 1
                            batch_summary["errors"] += 1
                            batch_summary["items"].append(
                                {
                                    "page": page_idx,
                                    "slot_id": sid,
                                    "roi": s_roi,
                                    "status": "geometry_fail",
                                    "reason": geom_msg,
                                }
                            )
                            continue
                        assert cand_box is not None
                        click_box = cand_box
                    else:
                        # click_relative inside slot ROI, still guarded by geometry + 20% crop confirm
                        matched_box = s_roi
                        geom_ok, geom_msg = self._verify_box_geometry_and_crop(
                            frame=working_frame,
                            box_norm=s_roi,
                            box_conf=max(float(slot.get("box_conf", 1.0)), adj.score),
                            roi_norm=page_roi,
                            target_desc=target_desc,
                            risk_tier=risk_tier,
                            metrics=metrics,
                            recent_confs=recent_confs,
                            step=page_idx,
                            include_crop_box_check=False,
                        )
                        if not geom_ok:
                            page_errors += 1
                            batch_summary["errors"] += 1
                            batch_summary["items"].append(
                                {
                                    "page": page_idx,
                                    "slot_id": sid,
                                    "roi": s_roi,
                                    "status": "geometry_fail",
                                    "reason": geom_msg,
                                }
                            )
                            continue

                    if item_action == "click_target_box":
                        cy_click = int(round((click_box[0] + click_box[2]) / 2.0))
                        cx_click = int(round((click_box[1] + click_box[3]) / 2.0))
                    else:
                        y0, x0, y1, x1 = s_roi
                        cy_click = min(y1 - 1, int(round(y0 + (y1 - y0) * rel_y)))
                        cx_click = min(x1 - 1, int(round(x0 + (x1 - x0) * rel_x)))

                    fsm_trace.append("S4_ACT_OR_RECOVER")
                    pre_click_frame = self._capture_frame()
                    final_frame = pre_click_frame
                    if compute_pixel_diff_ratio(working_frame, pre_click_frame, roi=s_roi) > 0.0:
                        aborted_breaker = CB_BATCH_DRIFT
                        detail = f"Slot {sid} changed after crop confirmation and before the click."
                        break
                    self.executor.execute_host_action(
                        {"type": "click", "x": cx_click, "y": cy_click, "button": "left", "count": 1}
                    )
                    metrics["steps_executed"] += 1
                    batch_summary["mutations_executed"] += 1

                    if settle_ms > 0:
                        self.sleep(settle_ms / 1000.0)
                    post_click_frame = self._capture_frame()
                    working_frame = post_click_frame
                    final_frame = post_click_frame

                    if verify_after_action:
                        delta = compute_pixel_diff_ratio(pre_click_frame, post_click_frame, roi=s_roi, channel_threshold=2)
                        if delta <= 0.0:
                            consecutive_zero_delta_clicks += 1
                        else:
                            consecutive_zero_delta_clicks = 0
                        if consecutive_zero_delta_clicks >= 2:
                            aborted_breaker = CB_BATCH_DRIFT
                            detail = "2 consecutive batch clicks yielded zero pixel change."
                            break

                    batch_summary["items"].append(
                        {
                            "page": page_idx,
                            "slot_id": sid,
                            "roi": s_roi,
                            "status": "clicked",
                            "click_xy": [cx_click, cy_click],
                            "label": adj.label,
                            "score": round(adj.score, 4),
                        }
                    )

                batch_summary["pages_processed"] += 1

                if aborted_breaker is not None:
                    breaker_code = aborted_breaker
                    status = {
                        CB_SAFETY_GUARD: "blocked",
                        CB_BUDGET_EXCEEDED: "budget_exceeded",
                        CB_INTERRUPT: "interrupted",
                        CB_BATCH_DRIFT: "batch_drift",
                    }[aborted_breaker]
                    break

                err_ratio = page_errors / max(1, len(page_slots))
                if not _allow_partial_errors and err_ratio > max_error_ratio:
                    status = "batch_drift"
                    breaker_code = CB_BATCH_DRIFT
                    detail = f"Page failure/gray ratio {err_ratio:.2%} exceeded max_error_ratio={max_error_ratio:.2%}"
                    break

                if page_idx + 1 >= max_pages or next_page_action is None:
                    status = "completed"
                    breaker_code = None
                    detail = (
                        f"Completed batch_apply across {batch_summary['pages_processed']} page(s): "
                        f"evaluated={batch_summary['items_evaluated']}, matched={batch_summary['items_matched']}, "
                        f"mutations={batch_summary['mutations_executed']}."
                    )
                    break

                # S4_ACT_OR_RECOVER: paginate to next page
                fsm_trace.append("S4_ACT_OR_RECOVER")
                prev_page_frame = working_frame
                page_act = fallback_next_page if (using_fallback and fallback_next_page is not None) else next_page_action
                self.executor.execute_host_action(page_act)
                metrics["steps_executed"] += 1
                if settle_ms > 0:
                    self.sleep(settle_ms / 1000.0)

        except SystemOneApiError as exc:
            status = "error"
            breaker_code = None
            detail = str(exc)
        finally:
            fsm_trace.append("S5_HANDOFF_TO_S2")
            if _release_inputs:
                self._safe_release_inputs()

        return self._build_handoff_result(
            tool_name="batch_apply",
            status=status,
            breaker_code=breaker_code,
            detail=detail,
            metrics=metrics,
            start_time=start_time,
            final_frame=final_frame,
            matched_box=matched_box,
            matched_label=matched_label,
            recent_confs=recent_confs,
            batch_summary=batch_summary,
            annotated_crop_frame=annotated_frame,
            annotated_crop_roi=annotated_roi,
            skip_visual_encoding=_skip_handoff_png,
            fsm_trace=fsm_trace,
        )
