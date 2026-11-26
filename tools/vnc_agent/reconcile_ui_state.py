"""L2 composite UI state reconciliation controller (``reconcile_ui_state``)."""

from collections import deque
from dataclasses import dataclass, field
import hashlib
import json
import math
import re
from typing import Any, Deque, Dict, List, Optional, Sequence, Set, Tuple

from .system_one_client import (
    FrameSnapshot,
    compute_pixel_diff_ratio,
    compute_ssim,
    compute_ssim_excluding_roi,
    extract_roi_text_ocr,
    pad_bbox_norm,
    prepare_image_for_system_one,
    validate_bbox,
)
from .system_one_fsm import (
    CB_AMBIGUITY,
    CB_BUDGET_EXCEEDED,
    CB_DEPENDENCY_BLOCKED,
    CB_INTERRUPT,
    CB_OSCILLATION,
    CB_SAFETY_GUARD,
    CB_VALIDATION_ERROR,
    DEFAULT_SETTLE_DELAY_MS,
    FULL_SCREEN_INTERRUPT_SSIM,
    GRAY_RETRY_ROI_SCALE,
    MAX_RECENT_CONFIDENCES,
    RISK_TIER_THRESHOLDS,
    WAIT_MAX_POLL_MS,
    WAIT_MIN_POLL_MS,
    WAIT_TIMEOUT_CAP_MS,
    NormalizedPredicate,
    _box_center_in_roi,
    _clamp_float,
    _clamp_int,
    _contains_irreversible_keyword,
    _extract_box_norm_and_conf,
    _normalize_risk_tier,
    _probability,
    adjudicate_predicate,
    normalize_predicate,
    validate_host_action,
)

VALID_CONTROL_TYPES = frozenset(
    {"text", "number", "secret_text", "checkbox", "radio", "toggle", "dropdown", "slider"}
)
BINARY_CONTROL_TYPES = frozenset({"checkbox", "radio", "toggle"})
TEXT_CONTROL_TYPES = frozenset({"text", "number"})
VALID_MATCH_MODES = frozenset({"exact", "semantic", "overwrite_always"})
VALID_FIELD_STATES = frozenset(
    {"already_matched", "reconciled", "failed_validation", "skipped_dependency", "uncertain"}
)

RECONCILE_DEFAULT_MAX_PASSES = 2
RECONCILE_MAX_PASSES_CAP = 3
RECONCILE_DEFAULT_MAX_RETRIES_PER_FIELD = 2
RECONCILE_MAX_RETRIES_PER_FIELD_CAP = 5
UNANNOUNCED_SHIFT_SSIM_THRESHOLD = 0.95
RECONCILE_DEFAULT_RELAYOUT_TIMEOUT_MS = 5000
RECONCILE_DEFAULT_RELAYOUT_POLL_MS = 250
RECONCILE_DEFAULT_MAX_SCROLL_STEPS = 5
RECONCILE_MAX_SCROLL_STEPS_CAP = 20
RECONCILE_DEFAULT_MAX_TOTAL_ACTIONS = 80
RECONCILE_MAX_TOTAL_ACTIONS_CAP = 200
SECRET_REDACTED_VALUE = "<redacted>"

_CHECKED_ALIASES = frozenset({"checked", "true", "on", "yes", "1", "selected", "enabled"})
_UNCHECKED_ALIASES = frozenset({"unchecked", "false", "off", "no", "0", "unselected"})

_HIGH_RISK_FIELD_PATTERNS: Tuple[Tuple[str, re.Pattern[str]], ...] = (
    (
        "final_submission",
        re.compile(
            r"\b(?:final\s+submi(?:t|ssion)|submit\s+(?:form|order|application|request|payment|button|now|final)"
            r"|confirm\s+(?:and\s+)?(?:submit|order|payment|purchase)|complete\s+(?:order|checkout|payment)"
            r"|place\s+order|checkout|check\s+out|finalize\s+(?:order|submission|payment))\b"
            r"|^\s*submit(?:\s+button)?\s*$",
            re.IGNORECASE,
        ),
    ),
    (
        "payment",
        re.compile(
            r"\b(?:pay\s+(?:now|order|invoice|bill|button)|make\s+payment|process\s+payment|submit\s+payment"
            r"|charge\s+card|complete\s+purchase|buy\s+now|wire\s+transfer|transfer\s+funds|send\s+money)\b"
            r"|^\s*pay(?:\s+button)?\s*$",
            re.IGNORECASE,
        ),
    ),
    (
        "destructive_action",
        re.compile(
            r"\b(?:delete\s+(?:account|all|permanently|forever|record|records|item|items|file|files|user|database|table)"
            r"|destroy|purge|erase\s+disk|format\s+drive|factory\s+reset|wipe|close\s+account|terminate\s+account"
            r"|remove\s+permanently|drop\s+(?:table|database))\b"
            r"|^\s*delete(?:\s+button)?\s*$",
            re.IGNORECASE,
        ),
    ),
    (
        "irreversible_autosave",
        re.compile(
            r"\b(?:irreversible|auto[- ]?save.{0,24}(?:irreversible|permanent|final))\b",
            re.IGNORECASE,
        ),
    ),
)

_NUMBER_DECORATION_RE = re.compile(r"[\s,$€£¥₩%]+")


@dataclass
class ReconcileFieldSpec:
    """Validated L2 target field specification."""

    field_id: str
    control_type: str
    locator_prompt: str
    roi: List[int]
    desired_value: str
    match_mode: str
    depends_on: List[str] = field(default_factory=list)
    triggers_relayout: bool = False
    commit_on_blur: bool = False
    risk_level: str = "reversible"
    required: bool = True
    auto_save: bool = False
    error_roi: Optional[List[int]] = None
    validation_error_prompt: Optional[str] = None
    regex_pattern: Optional[str] = None
    options: Optional[List[str]] = None
    loop_config: Optional[Dict[str, Any]] = None
    relayout_wait: Optional[Dict[str, Any]] = None
    min_confidence: Optional[float] = None


@dataclass
class _FieldRuntimeState:
    spec: ReconcileFieldSpec
    status: str = "uncertain"
    observed_value: Optional[str] = None
    attempts: int = 0
    breaker_code: Optional[str] = None
    reason: str = ""
    box_norm: Optional[List[int]] = None
    error_roi: Optional[List[int]] = None
    ever_matched: bool = False
    ever_reconciled: bool = False
    regressed: bool = False
    terminal_failure: bool = False
    state_history: List[str] = field(default_factory=list)

    def to_report(self) -> Dict[str, Any]:
        is_secret = self.spec.control_type == "secret_text"
        desired_out = SECRET_REDACTED_VALUE if is_secret else self.spec.desired_value
        observed_out = (
            SECRET_REDACTED_VALUE
            if is_secret and self.observed_value is not None
            else self.observed_value
        )
        return {
            "id": self.spec.field_id,
            "control_type": self.spec.control_type,
            "status": self.status,
            "desired_value": desired_out,
            "observed_value": observed_out,
            "attempts": self.attempts,
            "breaker_code": self.breaker_code,
            "reason": self.reason,
            "box": list(self.box_norm) if self.box_norm is not None else None,
        }


def _normalize_desired_value(raw_val: Any, control_type: str, field_id: str) -> str:
    if raw_val is None:
        raise ValueError(f"Field {field_id!r} requires a non-null 'desired_value'")
    if control_type in BINARY_CONTROL_TYPES:
        if isinstance(raw_val, bool):
            return "checked" if raw_val else "unchecked"
        if isinstance(raw_val, (int, float)) and not isinstance(raw_val, bool):
            if raw_val == 1:
                return "checked"
            if raw_val == 0:
                return "unchecked"
        norm = str(raw_val).strip().lower()
        if norm in _CHECKED_ALIASES:
            return "checked"
        if norm in _UNCHECKED_ALIASES:
            return "unchecked"
        raise ValueError(
            f"Field {field_id!r} ({control_type}) desired_value={raw_val!r} must be checked/unchecked (or boolean)"
        )
    if isinstance(raw_val, (bool, dict, list, tuple)):
        raise ValueError(
            f"Field {field_id!r} ({control_type}) desired_value must be a string or number, got {type(raw_val).__name__}"
        )
    return str(raw_val)


def _validate_loop_config(raw_cfg: Any, field_id: str, risk_tier: str) -> Dict[str, Any]:
    if not isinstance(raw_cfg, dict):
        raise ValueError(f"Field {field_id!r} loop_config must be an object")
    out = dict(raw_cfg)
    for act_key in ("step_action", "fallback_action"):
        if out.get(act_key) is not None:
            out[act_key] = validate_host_action(out[act_key], name=f"fields[{field_id!r}].loop_config.{act_key}")
    if out.get("directional_actions") is not None:
        if not isinstance(out["directional_actions"], dict):
            raise ValueError(f"fields[{field_id!r}].loop_config.directional_actions must be an object")
        out["directional_actions"] = {
            str(k).strip(): validate_host_action(v, name=f"fields[{field_id!r}].loop_config.directional_actions[{k!r}]")
            for k, v in out["directional_actions"].items()
        }
    if isinstance(out.get("on_match"), dict) and out["on_match"].get("action") is not None:
        om = dict(out["on_match"])
        om["action"] = validate_host_action(om["action"], name=f"fields[{field_id!r}].loop_config.on_match.action")
        out["on_match"] = om
    for roi_key in ("roi", "progress_roi"):
        if out.get(roi_key) is not None:
            out[roi_key] = validate_bbox(out[roi_key], name=f"fields[{field_id!r}].loop_config.{roi_key}")
    if out.get("stop_predicate") is not None:
        normalize_predicate(
            out["stop_predicate"],
            risk_tier=risk_tier,
            name=f"fields[{field_id!r}].loop_config.stop_predicate",
        )
    return out


def _validate_relayout_wait(raw_wait: Any, field_id: str) -> Dict[str, Any]:
    if not isinstance(raw_wait, dict):
        raise ValueError(f"Field {field_id!r} relayout_wait must be an object")
    out = dict(raw_wait)
    if out.get("condition") is None and out.get("type") is not None and (out.get("instructions") or out.get("question")):
        cond_payload = {
            k: v
            for k, v in out.items()
            if k in ("type", "instructions", "question", "criteria", "expected_value", "target_choices", "target_choice", "min_confidence", "roi", "label")
        }
        out["condition"] = cond_payload
    if out.get("watch_roi") is not None:
        out["watch_roi"] = validate_bbox(out["watch_roi"], name=f"fields[{field_id!r}].relayout_wait.watch_roi")
    if out.get("condition") is not None:
        normalize_predicate(
            out["condition"],
            risk_tier="read_only",
            default_roi=out.get("watch_roi"),
            name=f"fields[{field_id!r}].relayout_wait.condition",
        )
    return out


def normalize_reconcile_fields(
    raw_fields: Any,
    container_roi: Sequence[int] = (0, 0, 1000, 1000),
) -> Tuple[List[ReconcileFieldSpec], List[List[str]]]:
    """Validate target fields and group them into topological DAG waves via ``depends_on``."""
    if not isinstance(raw_fields, list) or not raw_fields:
        raise ValueError("reconcile_ui_state requires a non-empty 'fields' list")

    default_roi = validate_bbox(list(container_roi), name="container_roi")
    specs: List[ReconcileFieldSpec] = []
    seen_ids: Set[str] = set()

    for idx, raw in enumerate(raw_fields):
        if not isinstance(raw, dict):
            raise ValueError(f"fields[{idx}] must be an object, got {type(raw).__name__}")
        fid = str(raw.get("id") or "").strip()
        if not fid:
            raise ValueError(f"fields[{idx}].id must be a non-empty string")
        if fid in seen_ids:
            raise ValueError(f"Duplicate field id {fid!r} in fields")
        seen_ids.add(fid)

        ctype = str(raw.get("control_type") or "").strip().lower()
        if ctype not in VALID_CONTROL_TYPES:
            raise ValueError(
                f"fields[{fid!r}].control_type={ctype!r} must be one of {sorted(VALID_CONTROL_TYPES)}"
            )

        prompt = str(raw.get("locator_prompt") or "").strip()
        if not prompt:
            raise ValueError(f"fields[{fid!r}].locator_prompt must be a non-empty string")

        roi = validate_bbox(raw["roi"], name=f"fields[{fid!r}].roi") if raw.get("roi") is not None else list(default_roi)
        desired_value = _normalize_desired_value(raw.get("desired_value"), ctype, fid)

        raw_mm = raw.get("match_mode")
        if ctype == "secret_text":
            if raw_mm is not None and str(raw_mm).strip().lower() != "overwrite_always":
                raise ValueError(f"fields[{fid!r}] (secret_text) only supports match_mode='overwrite_always'")
            match_mode = "overwrite_always"
        else:
            match_mode = str(raw_mm or "exact").strip().lower()
            if match_mode not in VALID_MATCH_MODES:
                raise ValueError(
                    f"fields[{fid!r}].match_mode={match_mode!r} must be one of {sorted(VALID_MATCH_MODES)}"
                )

        raw_deps = raw.get("depends_on")
        deps: List[str] = []
        if raw_deps is not None:
            if not isinstance(raw_deps, list):
                raise ValueError(f"fields[{fid!r}].depends_on must be a list of field IDs")
            for dep in raw_deps:
                dep_id = str(dep or "").strip()
                if not dep_id:
                    continue
                if dep_id == fid:
                    raise ValueError(f"fields[{fid!r}].depends_on cannot reference itself")
                if dep_id not in deps:
                    deps.append(dep_id)

        raw_risk = raw.get("risk_level") if raw.get("risk_level") is not None else raw.get("risk_tier")
        risk_level = _normalize_risk_tier(raw_risk, default="reversible")
        if risk_level not in ("read_only", "reversible", "irreversible"):
            raise ValueError(
                f"fields[{fid!r}].risk_level={risk_level!r} must be read_only, reversible, or irreversible"
            )

        error_roi = (
            validate_bbox(raw["error_roi"], name=f"fields[{fid!r}].error_roi")
            if raw.get("error_roi") is not None
            else None
        )
        validation_error_prompt = (
            str(raw["validation_error_prompt"]).strip()
            if raw.get("validation_error_prompt") is not None and str(raw["validation_error_prompt"]).strip()
            else None
        )

        regex_pattern: Optional[str] = None
        if raw.get("regex_pattern") is not None:
            regex_pattern = str(raw["regex_pattern"])
            try:
                re.compile(regex_pattern)
            except re.error as exc:
                raise ValueError(f"fields[{fid!r}].regex_pattern is invalid: {exc}") from exc

        options: Optional[List[str]] = None
        if raw.get("options") is not None:
            if not isinstance(raw["options"], list):
                raise ValueError(f"fields[{fid!r}].options must be a list of strings")
            options = [str(o).strip() for o in raw["options"] if str(o).strip()]
            if not options:
                raise ValueError(f"fields[{fid!r}].options must not be empty when provided")

        min_conf: Optional[float] = None
        if raw.get("min_confidence") is not None:
            min_conf = _clamp_float(raw["min_confidence"], 0.0, 1.0, f"fields[{fid!r}].min_confidence")

        safe_cfg_tier = "read_only" if risk_level == "read_only" else "reversible"
        loop_config = (
            _validate_loop_config(raw["loop_config"], fid, safe_cfg_tier)
            if raw.get("loop_config") is not None
            else None
        )
        relayout_wait = (
            _validate_relayout_wait(raw["relayout_wait"], fid)
            if raw.get("relayout_wait") is not None
            else None
        )

        specs.append(
            ReconcileFieldSpec(
                field_id=fid,
                control_type=ctype,
                locator_prompt=prompt,
                roi=roi,
                desired_value=desired_value,
                match_mode=match_mode,
                depends_on=deps,
                triggers_relayout=bool(raw.get("triggers_relayout", False)),
                commit_on_blur=bool(raw.get("commit_on_blur", False)),
                risk_level=risk_level,
                required=bool(raw.get("required", True)),
                auto_save=bool(raw.get("auto_save", False)),
                error_roi=error_roi,
                validation_error_prompt=validation_error_prompt,
                regex_pattern=regex_pattern,
                options=options,
                loop_config=loop_config,
                relayout_wait=relayout_wait,
                min_confidence=min_conf,
            )
        )

    for spec in specs:
        for dep_id in spec.depends_on:
            if dep_id not in seen_ids:
                raise ValueError(f"fields[{spec.field_id!r}].depends_on references unknown field {dep_id!r}")

    # Topological wave grouping via Kahn's algorithm preserving original declaration order.
    order_index = {spec.field_id: i for i, spec in enumerate(specs)}
    in_degree: Dict[str, int] = {spec.field_id: len(spec.depends_on) for spec in specs}
    dependents: Dict[str, List[str]] = {spec.field_id: [] for spec in specs}
    for spec in specs:
        for dep_id in spec.depends_on:
            dependents[dep_id].append(spec.field_id)

    waves: List[List[str]] = []
    ready = sorted([fid for fid, deg in in_degree.items() if deg == 0], key=lambda x: order_index[x])
    emitted = 0
    while ready:
        waves.append(ready)
        emitted += len(ready)
        next_ready: List[str] = []
        for fid in ready:
            for child in dependents[fid]:
                in_degree[child] -= 1
                if in_degree[child] == 0:
                    next_ready.append(child)
        ready = sorted(next_ready, key=lambda x: order_index[x])

    if emitted != len(specs):
        cycled = [spec.field_id for spec in specs if in_degree[spec.field_id] > 0]
        raise ValueError(f"depends_on contains a dependency cycle involving: {cycled}")

    return specs, waves


def check_reconcile_safety_guard(
    specs: Sequence[ReconcileFieldSpec],
    top_risk_level: str = "reversible",
) -> Optional[Tuple[ReconcileFieldSpec, str]]:
    """Return ``(offending_field, reason)`` if any target field violates safety boundaries."""
    for spec in specs:
        if top_risk_level == "irreversible" or spec.risk_level == "irreversible":
            kind = "irreversible auto-save control" if spec.auto_save else "irreversible control"
            return spec, f"Hard-blocked {kind} on field {spec.field_id!r}"

        haystack = f"{spec.field_id.replace('_', ' ').replace('-', ' ')} | {spec.locator_prompt}"
        kw = _contains_irreversible_keyword(haystack)
        if kw is not None:
            return spec, f"Hard-blocked unsafe keyword {kw!r} on field {spec.field_id!r}"

        for label, pat in _HIGH_RISK_FIELD_PATTERNS:
            if pat.search(spec.locator_prompt) or pat.search(spec.field_id.replace("_", " ").replace("-", " ")):
                return spec, f"Hard-blocked {label} control on field {spec.field_id!r}"
    return None


def _compare_text_values(spec: ReconcileFieldSpec, observed: str) -> bool:
    """Compare observed text/number/dropdown/slider string against ``spec.desired_value``."""
    expected = spec.desired_value
    norm_obs = observed.replace("\r\n", "\n").replace("\r", "\n")
    norm_exp = expected.replace("\r\n", "\n").replace("\r", "\n")

    if spec.match_mode == "exact":
        return norm_obs == norm_exp

    if spec.regex_pattern is not None:
        return re.search(spec.regex_pattern, norm_obs) is not None

    if spec.control_type == "number":
        clean_obs = _NUMBER_DECORATION_RE.sub("", norm_obs).strip()
        clean_exp = _NUMBER_DECORATION_RE.sub("", norm_exp).strip()
        try:
            return abs(float(clean_obs) - float(clean_exp)) <= 1e-9
        except ValueError:
            pass

    collapsed_obs = " ".join(norm_obs.split()).casefold()
    collapsed_exp = " ".join(norm_exp.split()).casefold()
    return collapsed_obs == collapsed_exp


def _union_bboxes(b1: Sequence[int], b2: Sequence[int]) -> List[int]:
    return [
        min(int(b1[0]), int(b2[0])),
        min(int(b1[1]), int(b2[1])),
        max(int(b1[2]), int(b2[2])),
        max(int(b1[3]), int(b2[3])),
    ]


def _expand_context_roi(frame: FrameSnapshot, roi: Sequence[int], min_px: int = 280) -> List[int]:
    """Expand ``roi`` symmetrically with real pixels when smaller than ``min_px`` per side."""
    y0, x0, y1, x1 = [int(v) for v in roi]
    min_nh = min(1000, max(1, int(math.ceil((float(min_px) / max(1, frame.height)) * 1000.0))))
    min_nw = min(1000, max(1, int(math.ceil((float(min_px) / max(1, frame.width)) * 1000.0))))
    cur_h = y1 - y0
    cur_w = x1 - x0
    if cur_h < min_nh:
        cy = (y0 + y1) / 2.0
        y0 = max(0, int(round(cy - min_nh / 2.0)))
        y1 = min(1000, y0 + min_nh)
        if y1 - y0 < min_nh:
            y0 = max(0, y1 - min_nh)
    if cur_w < min_nw:
        cx = (x0 + x1) / 2.0
        x0 = max(0, int(round(cx - min_nw / 2.0)))
        x1 = min(1000, x0 + min_nw)
        if x1 - x0 < min_nw:
            x0 = max(0, x1 - min_nw)
    return [y0, x0, y1, x1]


def _compute_state_vector_hash(
    specs: Sequence[ReconcileFieldSpec],
    states: Dict[str, _FieldRuntimeState],
) -> str:
    vector: List[Tuple[str, str, Optional[str]]] = []
    for spec in specs:
        st = states[spec.field_id]
        if spec.control_type == "secret_text":
            obs_token: Optional[str] = "__secret_ok__" if st.status == "reconciled" else f"__secret_{st.status}__"
        else:
            obs_token = st.observed_value
        vector.append((spec.field_id, st.status, obs_token))
    encoded = json.dumps(vector, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()[:16]


def run_reconcile_ui_state(
    engine: Any,
    args: Dict[str, Any],
    collaborate_mode: bool = False,
) -> Dict[str, Any]:
    """Execute the L2 ``reconcile_ui_state`` composite controller on ``engine``."""
    from .system_one_client import SystemOneApiError

    start_time = engine.monotonic()
    metrics = {"steps_executed": 0, "s1_calls": 0}
    recent_confs: Deque[Dict[str, Any]] = deque(maxlen=MAX_RECENT_CONFIDENCES)
    fsm_trace: List[str] = ["S0_INIT_VALIDATE"]

    final_frame: Optional[FrameSnapshot] = None
    matched_box: Optional[List[int]] = None
    matched_label: Optional[str] = None
    annotated_frame: Optional[FrameSnapshot] = None
    annotated_roi: Optional[List[int]] = None
    annotated_label: Optional[str] = None
    attach_crop = False
    status = "error"
    breaker_code: Optional[str] = None
    detail = ""
    reconcile_summary: Optional[Dict[str, Any]] = None

    try:
        # -------------------------------------------------------------------
        # S0_INIT_VALIDATE
        # -------------------------------------------------------------------
        container_roi = (
            validate_bbox(args["container_roi"], name="container_roi")
            if args.get("container_roi") is not None
            else [0, 0, 1000, 1000]
        )
        top_risk = _normalize_risk_tier(
            args.get("risk_level") if args.get("risk_level") is not None else args.get("risk_tier"),
            default="reversible",
        )
        if top_risk not in ("read_only", "reversible", "irreversible"):
            raise ValueError(f"risk_level={top_risk!r} must be read_only, reversible, or irreversible")

        specs, waves = normalize_reconcile_fields(args.get("fields"), container_roi=container_roi)
        spec_by_id: Dict[str, ReconcileFieldSpec] = {s.field_id: s for s in specs}

        max_passes = _clamp_int(
            args.get("max_reconcile_passes", RECONCILE_DEFAULT_MAX_PASSES),
            1,
            RECONCILE_MAX_PASSES_CAP,
            "max_reconcile_passes",
        )
        max_retries_per_field = _clamp_int(
            args.get("max_retries_per_field", RECONCILE_DEFAULT_MAX_RETRIES_PER_FIELD),
            0,
            RECONCILE_MAX_RETRIES_PER_FIELD_CAP,
            "max_retries_per_field",
        )
        settle_ms = _clamp_int(args.get("settle_delay_ms", DEFAULT_SETTLE_DELAY_MS), 0, 5000, "settle_delay_ms")
        relayout_timeout_ms = _clamp_int(
            args.get("relayout_timeout_ms", RECONCILE_DEFAULT_RELAYOUT_TIMEOUT_MS),
            200,
            WAIT_TIMEOUT_CAP_MS,
            "relayout_timeout_ms",
        )
        relayout_poll_ms = _clamp_int(
            args.get("relayout_poll_interval_ms", RECONCILE_DEFAULT_RELAYOUT_POLL_MS),
            WAIT_MIN_POLL_MS,
            WAIT_MAX_POLL_MS,
            "relayout_poll_interval_ms",
        )
        allow_scroll = bool(args.get("allow_scroll", False))
        max_scroll_steps = _clamp_int(
            args.get("max_scroll_steps", RECONCILE_DEFAULT_MAX_SCROLL_STEPS),
            1,
            RECONCILE_MAX_SCROLL_STEPS_CAP,
            "max_scroll_steps",
        )
        max_total_actions = _clamp_int(
            args.get("max_total_actions", RECONCILE_DEFAULT_MAX_TOTAL_ACTIONS),
            1,
            RECONCILE_MAX_TOTAL_ACTIONS_CAP,
            "max_total_actions",
        )
        ignore_scene_change = bool(args.get("ignore_scene_change", False))

        scroll_action: Dict[str, Any]
        if args.get("scroll_action") is not None:
            scroll_action = validate_host_action(args["scroll_action"], name="scroll_action")
        else:
            cx = int(round((container_roi[1] + container_roi[3]) / 2.0))
            cy = int(round((container_roi[0] + container_roi[2]) / 2.0))
            scroll_action = {"type": "wheel_scroll", "x": cx, "y": cy, "direction": "down", "magnitude": 300}

        blur_action: Dict[str, Any]
        if args.get("blur_action") is not None:
            blur_action = validate_host_action(args["blur_action"], name="blur_action")
        elif args.get("neutral_blur_point") is not None:
            pt = args["neutral_blur_point"]
            if not isinstance(pt, (list, tuple)) or len(pt) != 2:
                raise ValueError("neutral_blur_point must be a 2-element [y, x] list in 0..1000")
            blur_action = validate_host_action(
                {"type": "click", "x": pt[1], "y": pt[0], "button": "left", "count": 1},
                name="neutral_blur_point",
            )
        else:
            blur_action = validate_host_action({"type": "key_press", "key": "Tab"}, name="blur_action")

        raw_interrupts = list(args.get("interrupt_predicates") or [])
        interrupt_preds: List[NormalizedPredicate] = [
            normalize_predicate(ip, risk_tier="interrupt", name=f"interrupt_predicates[{i}]")
            for i, ip in enumerate(raw_interrupts)
        ]

        field_states: Dict[str, _FieldRuntimeState] = {
            s.field_id: _FieldRuntimeState(spec=s) for s in specs
        }

        # Hard-block final submissions, payments, destructive actions, and irreversible auto-save controls.
        safety_hit = check_reconcile_safety_guard(specs, top_risk_level=top_risk)
        if safety_hit is not None:
            offending_spec, safety_reason = safety_hit
            final_frame = engine._capture_frame()
            blocked_ids: Set[str] = {offending_spec.field_id}
            # Mark downstream dependents of blocked field as skipped_dependency.
            for wave in waves:
                for fid in wave:
                    st = field_states[fid]
                    if fid == offending_spec.field_id:
                        st.status = "uncertain"
                        st.breaker_code = CB_SAFETY_GUARD
                        st.reason = safety_reason
                    elif any(dep in blocked_ids for dep in st.spec.depends_on):
                        st.status = "skipped_dependency"
                        st.breaker_code = CB_DEPENDENCY_BLOCKED
                        st.reason = f"Blocked because upstream field {offending_spec.field_id!r} hit {CB_SAFETY_GUARD}"
                        blocked_ids.add(fid)
                    else:
                        st.status = "uncertain"
                        st.reason = f"Aborted before execution due to {CB_SAFETY_GUARD} on {offending_spec.field_id!r}"

            counts = {k: 0 for k in VALID_FIELD_STATES}
            for s in specs:
                counts[field_states[s.field_id].status] += 1
            reconcile_summary = {
                "passes_executed": 0,
                "waves_count": len(waves),
                "waves": waves,
                "counts": counts,
                "breaker_history": [CB_SAFETY_GUARD],
                "conflicting_fields": [],
                "state_hashes": [],
                "fields": [field_states[s.field_id].to_report() for s in specs],
            }
            fsm_trace.append("S5_HANDOFF_TO_S2")
            return engine._build_handoff_result(
                tool_name="reconcile_ui_state",
                status="blocked",
                breaker_code=CB_SAFETY_GUARD,
                detail=safety_reason,
                metrics=metrics,
                start_time=start_time,
                final_frame=final_frame,
                matched_box=offending_spec.roi,
                matched_label=offending_spec.field_id,
                recent_confs=recent_confs,
                reconcile_summary=reconcile_summary,
                annotated_crop_frame=final_frame,
                annotated_crop_roi=offending_spec.roi,
                attach_annotated_crop=True,
                annotated_crop_label=CB_SAFETY_GUARD,
                fsm_trace=fsm_trace,
            )

        # -------------------------------------------------------------------
        # Runtime helpers & state
        # -------------------------------------------------------------------
        baseline_frame = engine._capture_frame()
        final_frame = baseline_frame
        cur_frame = baseline_frame

        cached_boxes: Dict[str, Tuple[List[int], float, int, int]] = {}
        scroll_epoch = 0
        layout_epoch = 0
        scroll_steps_used = 0
        breaker_history: List[str] = []
        state_hashes: List[str] = []
        seen_state_hashes: Set[str] = set()
        conflicting_fields: List[str] = []
        immediate_halt_breaker: Optional[str] = None
        immediate_halt_detail = ""
        error_crop_frame: Optional[FrameSnapshot] = None
        error_crop_roi: Optional[List[int]] = None
        error_crop_box: Optional[List[int]] = None
        error_crop_label: Optional[str] = None
        passes_executed = 0

        def _record_breaker(code: str) -> None:
            if code not in breaker_history:
                breaker_history.append(code)

        def _invalidate_for_scroll() -> None:
            nonlocal scroll_epoch
            scroll_epoch += 1
            cached_boxes.clear()

        def _invalidate_for_relayout_or_shift() -> None:
            nonlocal layout_epoch
            layout_epoch += 1
            cached_boxes.clear()

        def _get_cached_box(fid: str) -> Optional[Tuple[List[int], float]]:
            entry = cached_boxes.get(fid)
            if entry is None:
                return None
            b, c, s_ep, l_ep = entry
            if s_ep != scroll_epoch or l_ep != layout_epoch:
                return None
            return list(b), c

        def _set_cached_box(fid: str, box_norm: List[int], conf: float) -> None:
            cached_boxes[fid] = (list(box_norm), float(conf), scroll_epoch, layout_epoch)
            field_states[fid].box_norm = list(box_norm)

        def _merge_sub_metrics(sub_res: Dict[str, Any]) -> None:
            sub_m = sub_res.get("metrics") or {}
            metrics["s1_calls"] += int(sub_m.get("s1_calls", 0))
            metrics["steps_executed"] += int(sub_m.get("steps_executed", 0))

        def _effective_tier(spec: ReconcileFieldSpec) -> str:
            return "read_only" if "read_only" in (top_risk, spec.risk_level) else "reversible"

        def _run_ocr(frame: FrameSnapshot, roi: Sequence[int]) -> Optional[str]:
            ocr_fn = getattr(engine, "ocr_fn", None)
            if callable(ocr_fn):
                return ocr_fn(frame, roi)
            return extract_roi_text_ocr(frame, roi)

        def _sleep_settle() -> None:
            if settle_ms > 0:
                engine.sleep(settle_ms / 1000.0)

        def _check_budget() -> bool:
            nonlocal immediate_halt_breaker, immediate_halt_detail
            if metrics["steps_executed"] >= max_total_actions:
                immediate_halt_breaker = CB_BUDGET_EXCEEDED
                immediate_halt_detail = f"Exceeded max_total_actions={max_total_actions}"
                _record_breaker(CB_BUDGET_EXCEEDED)
                return False
            return True

        def _exec_action(action: Dict[str, Any]) -> bool:
            if not _check_budget():
                return False
            engine._check_collaborate_pause(collaborate_mode)
            fsm_trace.append("S4_ACT_OR_RECOVER")
            engine.executor.execute_host_action(action)
            metrics["steps_executed"] += 1
            return True

        def _check_unexpected_navigation(pre_f: FrameSnapshot, post_f: FrameSnapshot, context: str) -> bool:
            nonlocal immediate_halt_breaker, immediate_halt_detail, final_frame
            final_frame = post_f
            step_ssim = compute_ssim(pre_f, post_f)
            if step_ssim < FULL_SCREEN_INTERRUPT_SSIM:
                immediate_halt_breaker = CB_INTERRUPT
                immediate_halt_detail = (
                    f"Unexpected navigation after {context}: full-screen SSIM dropped to "
                    f"{step_ssim:.3f} (< {FULL_SCREEN_INTERRUPT_SSIM:.2f})"
                )
                _record_breaker(CB_INTERRUPT)
                return False
            if not ignore_scene_change:
                base_ssim = compute_ssim(baseline_frame, post_f)
                if base_ssim < FULL_SCREEN_INTERRUPT_SSIM:
                    immediate_halt_breaker = CB_INTERRUPT
                    immediate_halt_detail = (
                        f"Unexpected navigation relative to baseline after {context}: full-screen SSIM "
                        f"dropped to {base_ssim:.3f} (< {FULL_SCREEN_INTERRUPT_SSIM:.2f})"
                    )
                    _record_breaker(CB_INTERRUPT)
                    return False
            return True

        def _run_s1_with_interrupts(
            frame: FrameSnapshot,
            crop_roi: Sequence[int],
            questions: Dict[str, Dict[str, Any]],
            image_ref_name: str = "crop",
        ) -> Tuple[Optional[Dict[str, Any]], Any, bool]:
            """Query System One on ``crop_roi`` plus interrupt predicates. Returns ``(answers, prep, interrupted)``."""
            nonlocal immediate_halt_breaker, immediate_halt_detail
            fsm_trace.append("S2_S1_INFERENCE")
            prep = prepare_image_for_system_one(frame, crop_roi)
            asset_key = "crop_img"
            state_obj: Dict[str, Any] = {image_ref_name: asset_key}
            assets: Dict[str, Tuple[bytes, str]] = {asset_key: (prep.image_bytes, prep.image_format)}
            q_copy = {k: dict(v) for k, v in questions.items()}
            engine._add_interrupt_questions(frame, interrupt_preds, state_obj, q_copy, assets)
            answers = engine._query_s1(state_obj, q_copy, assets, metrics)
            fsm_trace.append("S3_ADJUDICATE")
            intr = engine._check_interrupts(interrupt_preds, answers)
            if intr is not None:
                immediate_halt_breaker = CB_INTERRUPT
                immediate_halt_detail = (
                    f"Interrupt check: {intr['label']} ({intr['answer_label']}, score={intr['score']})"
                )
                _record_breaker(CB_INTERRUPT)
                return answers, prep, True
            return answers, prep, False

        def _query_predicate_with_retry(
            frame: FrameSnapshot,
            pred: NormalizedPredicate,
            risk_tier: str,
            roi: Sequence[int],
            step_idx: int,
        ) -> Tuple[Any, FrameSnapshot, bool]:
            """Evaluate a single ``NormalizedPredicate`` with 1x gray-zone retry and interrupt checks."""
            answers, _, interrupted = _run_s1_with_interrupts(
                frame,
                roi,
                {"q": pred.to_system_one_question(image_ref_name="crop")},
                image_ref_name="crop",
            )
            if interrupted or answers is None:
                return None, frame, True
            adj = adjudicate_predicate(pred, answers.get("q"), risk_tier)
            engine._record_confidence(recent_confs, step_idx, pred.pred_type, adj.label, adj.score, adj.zone)
            if adj.zone == "gray":
                fsm_trace.append("S4_ACT_OR_RECOVER")
                retry_adj, retry_frame, intr = engine._run_gray_zone_retry(
                    pred,
                    risk_tier,
                    roi,
                    settle_ms,
                    metrics,
                    recent_confs,
                    step_idx,
                    interrupt_preds=interrupt_preds,
                )
                if intr is not None:
                    nonlocal immediate_halt_breaker, immediate_halt_detail, final_frame
                    final_frame = retry_frame
                    immediate_halt_breaker = CB_INTERRUPT
                    immediate_halt_detail = (
                        f"Interrupt check during retry: {intr['label']} "
                        f"({intr['answer_label']}, score={intr['score']})"
                    )
                    _record_breaker(CB_INTERRUPT)
                    return retry_adj, retry_frame, True
                return retry_adj, retry_frame, False
            return adj, frame, False

        def _post_mutation_guards(
            spec: ReconcileFieldSpec,
            pre_f: FrameSnapshot,
            post_f: FrameSnapshot,
            edited_box: Optional[Sequence[int]],
            step_idx: int,
        ) -> Tuple[bool, bool, FrameSnapshot]:
            """Run Navigation Guard, Relayout wait/invalidation, and Unannounced Shift Guard.

            Returns ``(ok_to_continue, needs_wave_reobserve, latest_frame)``.
            """
            nonlocal cur_frame, final_frame, immediate_halt_breaker, immediate_halt_detail
            if not _check_unexpected_navigation(pre_f, post_f, f"editing {spec.field_id}"):
                return False, False, post_f

            latest = post_f
            if spec.triggers_relayout:
                wait_cfg = dict(spec.relayout_wait or {})
                wait_roi = wait_cfg.get("watch_roi") or container_roi
                wait_cond = wait_cfg.get("condition") or {
                    "type": "noul",
                    "instructions": (
                        f"Has the UI layout finished settling and updating after changing {spec.locator_prompt}, "
                        "with no spinner or loading indicator visible?"
                    ),
                }
                wait_args: Dict[str, Any] = {
                    "condition": wait_cond,
                    "watch_roi": wait_roi,
                    "poll_interval_ms": wait_cfg.get("poll_interval_ms", relayout_poll_ms),
                    "timeout_ms": wait_cfg.get("timeout_ms", relayout_timeout_ms),
                    "require_stable_frames": wait_cfg.get("require_stable_frames", 2),
                    "diff_gate_threshold": wait_cfg.get("diff_gate_threshold", 0.005),
                    "settle_delay_ms": settle_ms,
                    "risk_tier": "read_only",
                    "interrupt_predicates": raw_interrupts,
                    "ignore_scene_change": ignore_scene_change,
                }
                fsm_trace.append("S4_ACT_OR_RECOVER")
                wait_res = engine.run_wait_until(wait_args, collaborate_mode=collaborate_mode, _skip_handoff_png=True)
                _merge_sub_metrics(wait_res)
                latest = engine._capture_frame()
                cur_frame = latest
                final_frame = latest
                if wait_res.get("status") != "matched":
                    immediate_halt_breaker = wait_res.get("breaker_code") or CB_AMBIGUITY
                    immediate_halt_detail = f"Relayout after {spec.field_id!r} did not settle: {wait_res.get('detail')}"
                    _record_breaker(immediate_halt_breaker)
                    return False, False, latest
                if not _check_unexpected_navigation(pre_f, latest, f"relayout after {spec.field_id}"):
                    return False, False, latest
                _invalidate_for_relayout_or_shift()
                return True, True, latest

            # Unannounced Shift Guard: container SSIM outside edited field's ROI < 0.95
            if edited_box is not None and spec.roi != container_roi:
                excl_roi: Sequence[int] = _union_bboxes(spec.roi, edited_box)
            elif edited_box is not None:
                excl_roi = pad_bbox_norm(edited_box, 0.20, mode="pad")
            else:
                excl_roi = spec.roi

            outside_ssim = compute_ssim_excluding_roi(
                pre_f,
                latest,
                container_roi=container_roi,
                exclude_roi=excl_roi,
            )
            if outside_ssim < UNANNOUNCED_SHIFT_SSIM_THRESHOLD:
                _invalidate_for_relayout_or_shift()
                return True, True, latest
            return True, False, latest

        def _trigger_blur_and_check_validation(
            spec: ReconcileFieldSpec,
            pre_edit_frame: FrameSnapshot,
            current_f: FrameSnapshot,
            edited_box: Optional[Sequence[int]],
            step_idx: int,
        ) -> Tuple[str, FrameSnapshot, bool]:
            """Run blur + inline validation check for ``commit_on_blur`` fields.

            Returns ``('ok'|'validation_error'|'halt', latest_frame, needs_wave_reobserve)``.
            """
            nonlocal cur_frame, final_frame, error_crop_frame, error_crop_roi, error_crop_box, error_crop_label
            latest = current_f
            needs_reobserve = False

            if not spec.commit_on_blur:
                return "ok", latest, needs_reobserve

            if not _exec_action(blur_action):
                return "halt", latest, needs_reobserve
            _sleep_settle()
            post_blur = engine._capture_frame()
            cur_frame = post_blur
            final_frame = post_blur

            ok_nav, relayout_shifted, latest = _post_mutation_guards(
                spec, current_f, post_blur, edited_box, step_idx
            )
            needs_reobserve = needs_reobserve or relayout_shifted
            if not ok_nav:
                return "halt", latest, needs_reobserve

            err_roi = spec.error_roi or (
                pad_bbox_norm(edited_box, 0.50, mode="pad") if edited_box is not None else spec.roi
            )
            err_desc = spec.validation_error_prompt or (
                f"Is there an inline validation error message, red warning border, or error cue for {spec.locator_prompt}?"
            )
            err_pred = normalize_predicate(
                {
                    "type": "noul",
                    "instructions": err_desc,
                    "expected_value": True,
                    "min_confidence": spec.min_confidence,
                },
                risk_tier="read_only",
                default_roi=err_roi,
                name=f"validation[{spec.field_id}]",
            )
            err_adj, check_frame, halted = _query_predicate_with_retry(
                latest, err_pred, "read_only", err_roi, step_idx
            )
            latest = check_frame
            cur_frame = latest
            final_frame = latest
            if halted or err_adj is None:
                return "halt", latest, needs_reobserve
            if err_adj.zone == "pass":
                error_crop_frame = latest
                error_crop_roi = list(err_roi)
                error_crop_box = list(edited_box) if edited_box is not None else list(err_roi)
                error_crop_label = CB_VALIDATION_ERROR
                return "validation_error", latest, needs_reobserve

            return "ok", latest, needs_reobserve

        def _locate_box_in_frame(
            frame: FrameSnapshot,
            spec: ReconcileFieldSpec,
            step_idx: int,
        ) -> Tuple[Optional[List[int]], float, bool, bool]:
            """Run single-frame ``box`` localization inside ``spec.roi`` with 1x gray/context retry."""
            nonlocal cur_frame, final_frame
            tier = _effective_tier(spec)
            tier_cfg = RISK_TIER_THRESHOLDS[tier]
            global_floor = max(
                float(tier_cfg["box_global"] if tier_cfg["box_global"] is not None else 0.85),
                spec.min_confidence or 0.0,
            )
            tau_low = float(tier_cfg["tau_low"] if tier_cfg["tau_low"] is not None else 0.65)

            prompt_clean = re.sub(r"^\s*the\s+", "", spec.locator_prompt, flags=re.IGNORECASE)
            answers, prep, interrupted = _run_s1_with_interrupts(
                frame,
                spec.roi,
                {"loc": {"type": "box", "instructions": f"the {prompt_clean} in `crop`"}},
                image_ref_name="crop",
            )
            if interrupted or answers is None:
                return None, 0.0, True, False
            cand_box, cand_conf = _extract_box_norm_and_conf(answers.get("loc"), prep)
            engine._record_confidence(
                recent_confs,
                step_idx,
                "box",
                spec.field_id,
                cand_conf,
                "pass" if cand_conf >= global_floor else ("gray" if cand_conf >= tau_low else "reject"),
            )

            used_container_retry = False
            if tau_low <= cand_conf < global_floor or (cand_box is not None and not _box_center_in_roi(cand_box, spec.roi)):
                _sleep_settle()
                fresh = engine._capture_frame()
                cur_frame = final_frame = fresh
                use_cont = spec.roi != container_roi and _box_center_in_roi(spec.roi, container_roi)
                retry_roi = container_roi if use_cont else _expand_context_roi(fresh, spec.roi, min_px=280)
                retry_ans, retry_prep, retry_intr = _run_s1_with_interrupts(
                    fresh,
                    retry_roi,
                    {"loc": {"type": "box", "instructions": f"the {prompt_clean} in `crop`"}},
                    image_ref_name="crop",
                )
                if retry_intr or retry_ans is None:
                    return None, 0.0, True, False
                retry_box, retry_conf = _extract_box_norm_and_conf(retry_ans.get("loc"), retry_prep)
                cand_ok = (
                    cand_box is not None
                    and _box_center_in_roi(cand_box, spec.roi)
                    and compute_pixel_diff_ratio(frame, fresh, roi=spec.roi) == 0.0
                )
                retry_ok = retry_box is not None and _box_center_in_roi(retry_box, spec.roi)
                if retry_ok and (not cand_ok or retry_conf >= cand_conf):
                    cand_box, cand_conf = retry_box, retry_conf
                    used_container_retry = use_cont
                elif not cand_ok:
                    cand_box, cand_conf = None, 0.0
                engine._record_confidence(
                    recent_confs,
                    step_idx,
                    "box:retry",
                    spec.field_id,
                    cand_conf,
                    "pass" if cand_conf >= global_floor else "fail",
                )
            return cand_box, cand_conf, False, used_container_retry

        def _locate_and_verify_box(
            spec: ReconcileFieldSpec,
            step_idx: int,
            *,
            verify_geometry: bool = True,
        ) -> Tuple[str, Optional[List[int]], str, FrameSnapshot]:
            """Locate and geometry-verify ``spec`` in the current viewport (with optional scroll).

            Returns ``('ok'|'dependency_blocked'|'uncertain'|'halt', box_norm, reason, frame)``.
            """
            nonlocal cur_frame, final_frame, scroll_steps_used
            nonlocal immediate_halt_breaker, immediate_halt_detail
            nonlocal error_crop_frame, error_crop_roi, error_crop_box, error_crop_label

            tier = _effective_tier(spec)
            tier_cfg = RISK_TIER_THRESHOLDS[tier]
            tau_low = float(tier_cfg["tau_low"] if tier_cfg["tau_low"] is not None else 0.65)
            global_floor = max(
                float(tier_cfg["box_global"] if tier_cfg["box_global"] is not None else 0.85),
                spec.min_confidence or 0.0,
            )

            used_container_retry = False
            cached = _get_cached_box(spec.field_id)
            if cached is not None:
                cand_box, cand_conf = cached
            else:
                cand_box, cand_conf, halted, used_container_retry = _locate_box_in_frame(cur_frame, spec, step_idx)
                if halted:
                    return "halt", None, immediate_halt_detail, cur_frame

                if (cand_box is None or cand_conf < tau_low):
                    if spec.depends_on:
                        return (
                            "dependency_blocked",
                            None,
                            f"Expected downstream control {spec.field_id!r} did not appear",
                            cur_frame,
                        )
                    if allow_scroll and scroll_steps_used < max_scroll_steps:
                        if not _check_budget():
                            return "halt", None, immediate_halt_detail, cur_frame
                        rem_scroll = max_scroll_steps - scroll_steps_used
                        loop_res = engine.run_loop_until(
                            {
                                "stop_predicate": {
                                    "type": "noul",
                                    "instructions": f"Is {spec.locator_prompt} clearly visible in the viewport?",
                                },
                                "step_action": scroll_action,
                                "roi": container_roi,
                                "max_steps": rem_scroll,
                                "settle_delay_ms": settle_ms,
                                "risk_tier": "read_only",
                                "interrupt_predicates": raw_interrupts,
                                "ignore_scene_change": ignore_scene_change,
                            },
                            collaborate_mode=collaborate_mode,
                            _max_actions=max_total_actions - metrics["steps_executed"],
                            _skip_handoff_png=True,
                        )
                        _merge_sub_metrics(loop_res)
                        steps_taken = int((loop_res.get("metrics") or {}).get("steps_executed", 0))
                        scroll_steps_used += steps_taken
                        if steps_taken > 0:
                            # Never reuse cached BBox coordinates across scroll offsets.
                            _invalidate_for_scroll()
                        cur_frame = engine._capture_frame()
                        final_frame = cur_frame
                        if loop_res.get("breaker_code") == CB_BUDGET_EXCEEDED and not _check_budget():
                            return "halt", None, immediate_halt_detail, cur_frame
                        if loop_res.get("breaker_code") == CB_INTERRUPT:
                            immediate_halt_breaker = CB_INTERRUPT
                            immediate_halt_detail = str(loop_res.get("detail") or "Interrupted while scrolling")
                            _record_breaker(CB_INTERRUPT)
                            return "halt", None, immediate_halt_detail, cur_frame
                        if loop_res.get("status") != "matched":
                            error_crop_frame = cur_frame
                            error_crop_roi = list(spec.roi)
                            error_crop_box = None
                            error_crop_label = CB_AMBIGUITY
                            return (
                                "uncertain",
                                None,
                                f"Control {spec.field_id!r} not found after scrolling ({loop_res.get('breaker_code') or loop_res.get('status')})",
                                cur_frame,
                            )
                        cand_box, cand_conf, halted, used_container_retry = _locate_box_in_frame(cur_frame, spec, step_idx)
                        if halted:
                            return "halt", None, immediate_halt_detail, cur_frame

                if cand_box is None or cand_conf < tau_low:
                    error_crop_frame = cur_frame
                    error_crop_roi = list(spec.roi)
                    error_crop_box = cand_box
                    error_crop_label = CB_AMBIGUITY
                    return (
                        "uncertain",
                        cand_box,
                        f"Failed to locate {spec.field_id!r} in viewport (conf={cand_conf:.3f})",
                        cur_frame,
                    )

            if verify_geometry:
                roi_area = (spec.roi[2] - spec.roi[0]) * (spec.roi[3] - spec.roi[1])
                confirm_ctx_roi: Optional[List[int]] = None
                if used_container_retry:
                    confirm_ctx_roi = list(container_roi)
                elif spec.roi != container_roi and roi_area < 250000:
                    confirm_ctx_roi = _union_bboxes(pad_bbox_norm(cand_box, 0.20, mode="pad"), spec.roi)
                geom_ok, geom_msg = engine._verify_box_geometry_and_crop(
                    frame=cur_frame,
                    box_norm=cand_box,
                    box_conf=cand_conf,
                    roi_norm=spec.roi,
                    target_desc=spec.locator_prompt,
                    risk_tier=tier,
                    metrics=metrics,
                    recent_confs=recent_confs,
                    step=step_idx,
                    caller_box_min_conf=spec.min_confidence,
                    include_crop_box_check=True,
                    confirm_crop_roi=confirm_ctx_roi,
                )
                if not geom_ok:
                    if spec.depends_on and cand_conf < global_floor:
                        return (
                            "dependency_blocked",
                            cand_box,
                            f"Expected downstream control {spec.field_id!r} failed appearance check: {geom_msg}",
                            cur_frame,
                        )
                    error_crop_frame = cur_frame
                    error_crop_roi = list(spec.roi)
                    error_crop_box = cand_box
                    error_crop_label = CB_AMBIGUITY
                    return "uncertain", cand_box, f"Box geometry pre-click check failed: {geom_msg}", cur_frame

            _set_cached_box(spec.field_id, cand_box, cand_conf)
            return "ok", cand_box, "ok", cur_frame

        def _binary_predicate(spec: ReconcileFieldSpec) -> NormalizedPredicate:
            return normalize_predicate(
                {
                    "type": "choice",
                    "instructions": f"What is the current state of {spec.locator_prompt}?",
                    "criteria": {
                        "checked": "The control is checked, selected, or switched ON.",
                        "unchecked": "The control is unchecked, unselected, or switched OFF.",
                        "disabled": "The control is disabled, grayed out, or non-interactive.",
                    },
                    "min_confidence": spec.min_confidence,
                },
                risk_tier=_effective_tier(spec),
                default_roi=spec.roi,
                name=f"binary_obs[{spec.field_id}]",
            )

        def _dropdown_choice_predicate(spec: ReconcileFieldSpec) -> NormalizedPredicate:
            assert spec.options is not None
            criteria: Dict[str, Any] = {opt: f"Selected dropdown value is {opt!r}." for opt in spec.options}
            if spec.desired_value not in criteria:
                criteria[spec.desired_value] = f"Selected dropdown value is {spec.desired_value!r}."
            criteria["other"] = "A different option not listed above is selected."
            criteria["disabled"] = "The dropdown control is disabled or non-interactive."
            return normalize_predicate(
                {
                    "type": "choice",
                    "instructions": f"What option is currently displayed by the collapsed dropdown {spec.locator_prompt}?",
                    "criteria": criteria,
                    "min_confidence": spec.min_confidence,
                },
                risk_tier=_effective_tier(spec),
                default_roi=spec.roi,
                name=f"dropdown_obs[{spec.field_id}]",
            )

        def _slider_choice_predicate(spec: ReconcileFieldSpec) -> NormalizedPredicate:
            return normalize_predicate(
                {
                    "type": "choice",
                    "instructions": (
                        f"Compare the slider {spec.locator_prompt} to target {spec.desired_value!r}."
                    ),
                    "criteria": {
                        "matched": f"The slider is at target value {spec.desired_value!r}.",
                        "too_low": f"The slider value is below target {spec.desired_value!r} and needs to increase.",
                        "too_high": f"The slider value is above target {spec.desired_value!r} and needs to decrease.",
                        "disabled": "The slider is disabled or non-interactive.",
                    },
                    "min_confidence": spec.min_confidence,
                },
                risk_tier=_effective_tier(spec),
                default_roi=spec.roi,
                name=f"slider_obs[{spec.field_id}]",
            )

        def _batch_observe_fields(
            wave_specs: Sequence[ReconcileFieldSpec],
            frame: FrameSnapshot,
            *,
            request_boxes: bool = True,
        ) -> Tuple[Dict[str, Dict[str, Any]], bool]:
            """Batch-observe multiple controls in a single ``batch_apply`` Parallel Questions call."""
            nonlocal immediate_halt_breaker, immediate_halt_detail, cur_frame, final_frame
            slots_arg: List[Dict[str, Any]] = []
            slot_preds: Dict[str, NormalizedPredicate] = {}
            locate_prompts: Dict[str, str] = {}

            for sp in wave_specs:
                if sp.control_type in BINARY_CONTROL_TYPES:
                    slot_preds[sp.field_id] = _binary_predicate(sp)
                elif sp.control_type == "dropdown" and sp.options:
                    slot_preds[sp.field_id] = _dropdown_choice_predicate(sp)
                elif sp.control_type == "slider":
                    slot_preds[sp.field_id] = _slider_choice_predicate(sp)
                else:
                    continue
                slots_arg.append({"id": sp.field_id, "roi": sp.roi})
                if request_boxes and _get_cached_box(sp.field_id) is None:
                    locate_prompts[sp.field_id] = sp.locator_prompt

            if not slots_arg:
                return {}, False

            batch_res = engine.run_batch_apply(
                {
                    "item_action": "noop_collect",
                    "locate_mode": "fixed_slots",
                    "slots": slots_arg,
                    "page_roi": container_roi,
                    "risk_tier": "read_only",
                    "settle_delay_ms": settle_ms,
                    "interrupt_predicates": raw_interrupts,
                    "ignore_scene_change": ignore_scene_change,
                },
                collaborate_mode=collaborate_mode,
                _slot_predicates=slot_preds,
                _locate_box_prompts=locate_prompts or None,
                _allow_partial_errors=True,
                _precaptured_frame=frame,
                _skip_handoff_png=True,
                _release_inputs=False,
            )
            _merge_sub_metrics(batch_res)
            if batch_res.get("breaker_code") == CB_INTERRUPT:
                immediate_halt_breaker = CB_INTERRUPT
                immediate_halt_detail = str(batch_res.get("detail") or "Interrupted during batched observation")
                _record_breaker(CB_INTERRUPT)
                return {}, True

            by_id: Dict[str, Dict[str, Any]] = {}
            for item in (batch_res.get("outcome") or {}).get("batch_summary", {}).get("items", []):
                sid = str(item.get("slot_id") or "")
                if sid:
                    by_id[sid] = item
                    loc_b = item.get("located_box")
                    loc_c = float(item.get("box_conf", 0.0) or 0.0)
                    sp = spec_by_id[sid]
                    tier_cfg = RISK_TIER_THRESHOLDS[_effective_tier(sp)]
                    global_floor = max(
                        float(tier_cfg["box_global"] if tier_cfg["box_global"] is not None else 0.85),
                        sp.min_confidence or 0.0,
                    )
                    if isinstance(loc_b, list) and loc_c >= global_floor and _box_center_in_roi(loc_b, sp.roi):
                        _set_cached_box(sid, loc_b, loc_c)
            return by_id, False

        def _observe_text_or_number(
            spec: ReconcileFieldSpec,
            box_norm: List[int],
            step_idx: int,
            *,
            focus_for_clipboard: bool = True,
            read_clipboard: bool = True,
        ) -> Tuple[str, Optional[str], FrameSnapshot, bool]:
            """Observe a ``text``/``number`` control: prefer VNC clipboard, fallback to OCR + Jev crop score.

            Returns ``('matched'|'mismatch'|'uncertain'|'halt', observed_text, frame, focused)``.
            """
            nonlocal cur_frame, final_frame, error_crop_frame, error_crop_roi, error_crop_box, error_crop_label
            focused = False
            clipboard_fn = getattr(engine.executor, "copy_focused_text", None)
            if read_clipboard and callable(clipboard_fn):
                if focus_for_clipboard:
                    cy = int(round((box_norm[0] + box_norm[2]) / 2.0))
                    cx = int(round((box_norm[1] + box_norm[3]) / 2.0))
                    if not _exec_action({"type": "click", "x": cx, "y": cy, "button": "left", "count": 1}):
                        return "halt", None, cur_frame, False
                    focused = True
                ok_copy, clip_text = clipboard_fn()
                if ok_copy:
                    matched = _compare_text_values(spec, clip_text)
                    return ("matched" if matched else "mismatch"), clip_text, cur_frame, focused

            # Fallback: local OCR + Jev crop score
            ocr_roi = box_norm or spec.roi
            ocr_text = _run_ocr(cur_frame, ocr_roi)
            if ocr_text is not None and ocr_text != "":
                if _compare_text_values(spec, ocr_text):
                    return "matched", ocr_text, cur_frame, focused

            crop_roi = pad_bbox_norm(box_norm or spec.roi, 0.25, mode="pad")
            text_instr = (
                f"Does the centered text field in `crop` currently display the exact value {spec.desired_value!r}?"
                if spec.match_mode == "exact"
                else f"Does the centered text field in `crop` currently display a value matching {spec.desired_value!r}?"
            )
            pred = normalize_predicate(
                {
                    "type": "noul",
                    "instructions": text_instr,
                    "expected_value": True,
                    "min_confidence": spec.min_confidence,
                },
                risk_tier=_effective_tier(spec),
                default_roi=crop_roi,
                name=f"text_jev[{spec.field_id}]",
            )
            adj, check_f, halted = _query_predicate_with_retry(
                cur_frame, pred, _effective_tier(spec), crop_roi, step_idx
            )
            cur_frame = check_f
            final_frame = check_f
            if halted or adj is None:
                return "halt", ocr_text, cur_frame, focused
            if adj.zone == "pass":
                matched_val = (
                    ocr_text
                    if (ocr_text is not None and _compare_text_values(spec, ocr_text))
                    else spec.desired_value
                )
                return "matched", matched_val, cur_frame, focused
            if adj.zone == "reject":
                return "mismatch", ocr_text, cur_frame, focused

            error_crop_frame = cur_frame
            error_crop_roi = list(crop_roi)
            error_crop_box = list(box_norm)
            error_crop_label = CB_AMBIGUITY
            return "uncertain", ocr_text, cur_frame, focused

        def _observe_dropdown_value(
            spec: ReconcileFieldSpec,
            box_norm: List[int],
            step_idx: int,
            *,
            pre_obs_item: Optional[Dict[str, Any]] = None,
        ) -> Tuple[str, Optional[str], FrameSnapshot]:
            """Observe collapsed dropdown state via closed-set choice, OCR, or Jev crop score."""
            nonlocal cur_frame, final_frame, error_crop_frame, error_crop_roi, error_crop_box, error_crop_label
            if pre_obs_item is not None:
                lbl = str(pre_obs_item.get("label") or "")
                st_str = str(pre_obs_item.get("status") or "")
                if lbl == "disabled":
                    return "disabled", "disabled", cur_frame
                if st_str == "gray_fail" or lbl == "unreadable":
                    error_crop_frame = cur_frame
                    error_crop_roi = list(spec.roi)
                    error_crop_box = list(box_norm)
                    error_crop_label = CB_AMBIGUITY
                    return "uncertain", lbl, cur_frame
                if _compare_text_values(spec, lbl):
                    return "matched", lbl, cur_frame
                return "mismatch", lbl, cur_frame

            if spec.options:
                obs_map, halted = _batch_observe_fields([spec], cur_frame, request_boxes=False)
                if halted:
                    return "halt", None, cur_frame
                item = obs_map.get(spec.field_id)
                if item is not None:
                    return _observe_dropdown_value(spec, box_norm, step_idx, pre_obs_item=item)

            ocr_text = _run_ocr(cur_frame, box_norm or spec.roi)
            if ocr_text is not None and ocr_text != "":
                if _compare_text_values(spec, ocr_text):
                    return "matched", ocr_text, cur_frame

            crop_roi = pad_bbox_norm(box_norm or spec.roi, 0.25, mode="pad")
            pred = normalize_predicate(
                {
                    "type": "noul",
                    "instructions": (
                        f"Is the centered dropdown control in `crop` collapsed (menu closed) "
                        f"and currently displaying the value {spec.desired_value!r}?"
                    ),
                    "expected_value": True,
                    "min_confidence": spec.min_confidence,
                },
                risk_tier=_effective_tier(spec),
                default_roi=crop_roi,
                name=f"dropdown_jev[{spec.field_id}]",
            )
            adj, check_f, halted = _query_predicate_with_retry(
                cur_frame, pred, _effective_tier(spec), crop_roi, step_idx
            )
            cur_frame = check_f
            final_frame = check_f
            if halted or adj is None:
                return "halt", ocr_text, cur_frame
            if adj.zone == "pass":
                matched_val = (
                    ocr_text
                    if (ocr_text is not None and _compare_text_values(spec, ocr_text))
                    else spec.desired_value
                )
                return "matched", matched_val, cur_frame
            if adj.zone == "reject":
                return "mismatch", ocr_text, cur_frame

            error_crop_frame = cur_frame
            error_crop_roi = list(crop_roi)
            error_crop_box = list(box_norm)
            error_crop_label = CB_AMBIGUITY
            return "uncertain", ocr_text, cur_frame

        def _act_and_verify_binary(
            spec: ReconcileFieldSpec,
            st: _FieldRuntimeState,
            step_idx: int,
            *,
            skip_initial_observe: bool = False,
            initial_obs_item: Optional[Dict[str, Any]] = None,
        ) -> Tuple[bool, bool]:
            """Execute Observe/Act/Verify for ``checkbox`` / ``radio`` / ``toggle``.

            Returns ``(halt_now, wave_needs_reobserve)``.
            """
            nonlocal cur_frame, final_frame, error_crop_frame, error_crop_roi, error_crop_box, error_crop_label
            needs_reobserve = False

            if not skip_initial_observe and initial_obs_item is not None:
                lbl = str(initial_obs_item.get("label") or "")
                st_str = str(initial_obs_item.get("status") or "")
                st.observed_value = lbl
                if lbl == "disabled":
                    st.status = "skipped_dependency"
                    st.breaker_code = CB_DEPENDENCY_BLOCKED
                    st.reason = f"Control {spec.field_id!r} is disabled"
                    st.terminal_failure = True
                    _record_breaker(CB_DEPENDENCY_BLOCKED)
                    return False, needs_reobserve
                if st_str == "gray_fail" or lbl == "unreadable":
                    st.status = "uncertain"
                    st.breaker_code = CB_AMBIGUITY
                    st.reason = f"Gray-zone ambiguity observing {spec.field_id!r} ({lbl})"
                    st.terminal_failure = True
                    _record_breaker(CB_AMBIGUITY)
                    error_crop_frame = cur_frame
                    error_crop_roi = list(spec.roi)
                    error_crop_box = st.box_norm
                    error_crop_label = CB_AMBIGUITY
                    return False, needs_reobserve
                if spec.match_mode != "overwrite_always" and lbl == spec.desired_value:
                    st.status = "reconciled" if st.ever_reconciled else "already_matched"
                    st.breaker_code = None
                    st.reason = "Observed state already matches desired_value"
                    st.ever_matched = True
                    return False, needs_reobserve

            # Locate & geometry check before clicking
            loc_status, box_norm, loc_reason, cur_frame = _locate_and_verify_box(spec, step_idx)
            if loc_status == "halt":
                return True, needs_reobserve
            if loc_status == "dependency_blocked":
                st.status = "skipped_dependency"
                st.breaker_code = CB_DEPENDENCY_BLOCKED
                st.reason = loc_reason
                st.terminal_failure = True
                _record_breaker(CB_DEPENDENCY_BLOCKED)
                return False, needs_reobserve
            if loc_status == "uncertain" or box_norm is None:
                st.status = "uncertain"
                st.breaker_code = CB_AMBIGUITY
                st.reason = loc_reason
                st.terminal_failure = True
                _record_breaker(CB_AMBIGUITY)
                return False, needs_reobserve

            if skip_initial_observe and spec.match_mode != "overwrite_always":
                obs_map, halted = _batch_observe_fields([spec], cur_frame, request_boxes=False)
                if halted:
                    return True, needs_reobserve
                item = obs_map.get(spec.field_id)
                if item is not None:
                    lbl = str(item.get("label") or "")
                    st_str = str(item.get("status") or "")
                    st.observed_value = lbl
                    if lbl == "disabled":
                        st.status = "skipped_dependency"
                        st.breaker_code = CB_DEPENDENCY_BLOCKED
                        st.reason = f"Control {spec.field_id!r} is disabled"
                        _record_breaker(CB_DEPENDENCY_BLOCKED)
                        return False, needs_reobserve
                    if st_str == "gray_fail" or lbl == "unreadable":
                        st.status = "uncertain"
                        st.breaker_code = CB_AMBIGUITY
                        st.reason = f"Gray-zone ambiguity observing {spec.field_id!r} ({lbl})"
                        _record_breaker(CB_AMBIGUITY)
                        error_crop_frame = cur_frame
                        error_crop_roi = list(spec.roi)
                        error_crop_box = list(box_norm)
                        error_crop_label = CB_AMBIGUITY
                        return False, needs_reobserve
                    if lbl == spec.desired_value:
                        st.status = "reconciled" if st.ever_reconciled else "already_matched"
                        st.breaker_code = None
                        st.reason = "Observed state already matches desired_value"
                        st.ever_matched = True
                        return False, needs_reobserve

            max_tries = 1 + max_retries_per_field
            for attempt in range(max_tries):
                st.attempts += 1
                pre_click = engine._capture_frame()
                if compute_pixel_diff_ratio(cur_frame, pre_click, roi=box_norm) > 0.0:
                    _invalidate_for_relayout_or_shift()
                    loc_status, box_norm, loc_reason, cur_frame = _locate_and_verify_box(spec, step_idx)
                    if loc_status == "halt":
                        return True, needs_reobserve
                    if loc_status != "ok" or box_norm is None:
                        st.status = "skipped_dependency" if loc_status == "dependency_blocked" else "uncertain"
                        st.breaker_code = CB_DEPENDENCY_BLOCKED if loc_status == "dependency_blocked" else CB_AMBIGUITY
                        st.reason = loc_reason
                        _record_breaker(st.breaker_code)
                        return False, needs_reobserve
                    pre_click = cur_frame

                cy = int(round((box_norm[0] + box_norm[2]) / 2.0))
                cx = int(round((box_norm[1] + box_norm[3]) / 2.0))
                if not _exec_action({"type": "click", "x": cx, "y": cy, "button": "left", "count": 1}):
                    return True, needs_reobserve
                _sleep_settle()
                post_click = engine._capture_frame()
                cur_frame = post_click
                final_frame = post_click

                ok_nav, shifted, cur_frame = _post_mutation_guards(spec, pre_click, post_click, box_norm, step_idx)
                needs_reobserve = needs_reobserve or shifted
                if not ok_nav:
                    return True, needs_reobserve

                blur_res, cur_frame, blur_shifted = _trigger_blur_and_check_validation(
                    spec, pre_click, cur_frame, box_norm, step_idx
                )
                needs_reobserve = needs_reobserve or blur_shifted
                if blur_res == "halt":
                    return True, needs_reobserve
                if blur_res == "validation_error":
                    st.status = "failed_validation"
                    st.breaker_code = CB_VALIDATION_ERROR
                    st.reason = f"Inline validation error after changing {spec.field_id!r}"
                    st.terminal_failure = True
                    _record_breaker(CB_VALIDATION_ERROR)
                    return False, needs_reobserve

                local_diff = compute_pixel_diff_ratio(pre_click, cur_frame, roi=box_norm, channel_threshold=2)
                verify_map, halted = _batch_observe_fields([spec], cur_frame, request_boxes=False)
                if halted:
                    return True, needs_reobserve
                v_item = verify_map.get(spec.field_id) or {}
                v_lbl = str(v_item.get("label") or "")
                v_status = str(v_item.get("status") or "")
                st.observed_value = v_lbl

                if v_lbl == "disabled":
                    st.status = "skipped_dependency"
                    st.breaker_code = CB_DEPENDENCY_BLOCKED
                    st.reason = f"Control {spec.field_id!r} became disabled"
                    st.terminal_failure = True
                    _record_breaker(CB_DEPENDENCY_BLOCKED)
                    return False, needs_reobserve
                if v_status == "gray_fail" or v_lbl == "unreadable":
                    st.status = "uncertain"
                    st.breaker_code = CB_AMBIGUITY
                    st.reason = f"Verification gray-zone ambiguity on {spec.field_id!r} ({v_lbl})"
                    st.terminal_failure = True
                    _record_breaker(CB_AMBIGUITY)
                    error_crop_frame = cur_frame
                    error_crop_roi = list(spec.roi)
                    error_crop_box = list(box_norm)
                    error_crop_label = CB_AMBIGUITY
                    return False, needs_reobserve

                if v_lbl == spec.desired_value and local_diff > 0.0:
                    st.status = "reconciled"
                    st.breaker_code = None
                    st.reason = f"Clicked and verified {spec.desired_value!r}"
                    st.ever_reconciled = True
                    st.ever_matched = True
                    return False, needs_reobserve

                if attempt + 1 >= max_tries:
                    st.status = "failed_validation"
                    st.breaker_code = CB_VALIDATION_ERROR
                    st.terminal_failure = True
                    if local_diff <= 0.0:
                        st.reason = f"Click on {spec.field_id!r} produced zero local pixel change / reverted"
                    else:
                        st.reason = f"Control {spec.field_id!r} reverted or remained {v_lbl!r} (expected {spec.desired_value!r})"
                    _record_breaker(CB_VALIDATION_ERROR)
                    error_crop_frame = cur_frame
                    error_crop_roi = list(spec.error_roi or pad_bbox_norm(box_norm, 0.35, mode="pad"))
                    error_crop_box = list(box_norm)
                    error_crop_label = CB_VALIDATION_ERROR
                    return False, needs_reobserve

            return False, needs_reobserve

        def _act_and_verify_text(
            spec: ReconcileFieldSpec,
            st: _FieldRuntimeState,
            step_idx: int,
        ) -> Tuple[bool, bool]:
            """Execute Observe/Act/Verify for ``text``, ``number``, or ``secret_text``."""
            nonlocal cur_frame, final_frame, error_crop_frame, error_crop_roi, error_crop_box, error_crop_label
            needs_reobserve = False

            loc_status, box_norm, loc_reason, cur_frame = _locate_and_verify_box(spec, step_idx)
            if loc_status == "halt":
                return True, needs_reobserve
            if loc_status == "dependency_blocked":
                st.status = "skipped_dependency"
                st.breaker_code = CB_DEPENDENCY_BLOCKED
                st.reason = loc_reason
                st.terminal_failure = True
                _record_breaker(CB_DEPENDENCY_BLOCKED)
                return False, needs_reobserve
            if loc_status == "uncertain" or box_norm is None:
                st.status = "uncertain"
                st.breaker_code = CB_AMBIGUITY
                st.reason = loc_reason
                st.terminal_failure = True
                _record_breaker(CB_AMBIGUITY)
                return False, needs_reobserve

            is_secret = spec.control_type == "secret_text"
            already_focused = False

            # Step 1: Observe & Compare (SKIPPED for secret_text and overwrite_always)
            if not is_secret and spec.match_mode != "overwrite_always":
                obs_state, obs_val, cur_frame, already_focused = _observe_text_or_number(
                    spec, box_norm, step_idx, focus_for_clipboard=True
                )
                st.observed_value = obs_val
                if obs_state == "halt":
                    return True, needs_reobserve
                if obs_state == "uncertain":
                    st.status = "uncertain"
                    st.breaker_code = CB_AMBIGUITY
                    st.reason = f"Ambiguous text observation on {spec.field_id!r}"
                    st.terminal_failure = True
                    _record_breaker(CB_AMBIGUITY)
                    return False, needs_reobserve
                if obs_state == "matched":
                    if spec.commit_on_blur and already_focused:
                        if not _exec_action(blur_action):
                            return True, needs_reobserve
                        _sleep_settle()
                        cur_frame = engine._capture_frame()
                        final_frame = cur_frame
                    st.status = "reconciled" if st.ever_reconciled else "already_matched"
                    st.breaker_code = None
                    st.reason = "Observed value already matches desired_value"
                    st.ever_matched = True
                    return False, needs_reobserve

            # Step 2: Act (Full overwrite Ctrl+A, BackSpace, Ctrl+V; fallback to keystrokes only if paste fails)
            max_tries = 1 + max_retries_per_field
            for attempt in range(max_tries):
                st.attempts += 1
                pre_act = engine._capture_frame()
                if not already_focused or attempt > 0:
                    cy = int(round((box_norm[0] + box_norm[2]) / 2.0))
                    cx = int(round((box_norm[1] + box_norm[3]) / 2.0))
                    if not _exec_action({"type": "click", "x": cx, "y": cy, "button": "left", "count": 1}):
                        return True, needs_reobserve
                    _sleep_settle()

                overwrite_clip_fn = getattr(engine.executor, "overwrite_focused_text_clipboard", None)
                type_fn = getattr(engine.executor, "type_focused_text", None)
                clip_copy_fn = getattr(engine.executor, "copy_focused_text", None)
                clear_secret_fn = getattr(engine.executor, "clear_clipboard_secret", None)

                pasted_ok = False
                try:
                    if callable(overwrite_clip_fn):
                        if not _check_budget():
                            return True, needs_reobserve
                        pasted_ok = bool(overwrite_clip_fn(spec.desired_value))
                        metrics["steps_executed"] += 1
                    _sleep_settle()
                finally:
                    if is_secret and callable(clear_secret_fn):
                        clear_secret_fn()
                post_paste = engine._capture_frame()
                cur_frame = post_paste
                final_frame = post_paste

                need_keystroke_fallback = not pasted_ok
                if not need_keystroke_fallback and spec.desired_value != "":
                    if is_secret:
                        delta = compute_pixel_diff_ratio(pre_act, post_paste, roi=box_norm, channel_threshold=2)
                        if delta <= 0.0 and callable(type_fn):
                            need_keystroke_fallback = True
                    else:
                        if callable(clip_copy_fn):
                            ok_rb, rb_val = clip_copy_fn()
                            if ok_rb and not _compare_text_values(spec, rb_val):
                                need_keystroke_fallback = True
                            elif not ok_rb:
                                delta = compute_pixel_diff_ratio(pre_act, post_paste, roi=box_norm, channel_threshold=2)
                                if delta <= 0.0 and st.observed_value != spec.desired_value:
                                    need_keystroke_fallback = True

                if need_keystroke_fallback:
                    if callable(type_fn):
                        if not _check_budget():
                            return True, needs_reobserve
                        type_fn(spec.desired_value)
                        metrics["steps_executed"] += 1
                        _sleep_settle()
                        post_paste = engine._capture_frame()
                        cur_frame = post_paste
                        final_frame = post_paste

                ok_nav, shifted, cur_frame = _post_mutation_guards(spec, pre_act, cur_frame, box_norm, step_idx)
                needs_reobserve = needs_reobserve or shifted
                if not ok_nav:
                    return True, needs_reobserve

                blur_res, cur_frame, blur_shifted = _trigger_blur_and_check_validation(
                    spec, pre_act, cur_frame, box_norm, step_idx
                )
                needs_reobserve = needs_reobserve or blur_shifted
                if blur_res == "halt":
                    return True, needs_reobserve
                if blur_res == "validation_error":
                    st.status = "failed_validation"
                    st.breaker_code = CB_VALIDATION_ERROR
                    st.reason = f"Inline validation error after editing {spec.field_id!r}"
                    st.terminal_failure = True
                    _record_breaker(CB_VALIDATION_ERROR)
                    return False, needs_reobserve

                # Step 3: Verify
                if is_secret:
                    verify_roi = spec.error_roi or pad_bbox_norm(box_norm, 0.35, mode="pad")
                    mask_instr = (
                        f"Is the secret field {spec.locator_prompt} displaying masked characters "
                        "(dots, bullets, or asterisks) in a non-empty state?"
                        if spec.desired_value != ""
                        else f"Is the secret field {spec.locator_prompt} empty?"
                    )
                    mask_pred = normalize_predicate(
                        {"type": "noul", "instructions": mask_instr, "expected_value": True, "min_confidence": spec.min_confidence},
                        risk_tier=_effective_tier(spec),
                        default_roi=verify_roi,
                        name=f"secret_mask[{spec.field_id}]",
                    )
                    err_pred = normalize_predicate(
                        {
                            "type": "noul",
                            "instructions": spec.validation_error_prompt
                            or f"Is there any inline validation error or warning cue on {spec.locator_prompt}?",
                            "expected_value": False,
                            "min_confidence": spec.min_confidence,
                        },
                        risk_tier=_effective_tier(spec),
                        default_roi=verify_roi,
                        name=f"secret_err[{spec.field_id}]",
                    )
                    ans, _, intr = _run_s1_with_interrupts(
                        cur_frame,
                        verify_roi,
                        {
                            "mask": mask_pred.to_system_one_question("crop"),
                            "no_err": err_pred.to_system_one_question("crop"),
                        },
                        image_ref_name="crop",
                    )
                    if intr or ans is None:
                        return True, needs_reobserve
                    mask_adj = adjudicate_predicate(mask_pred, ans.get("mask"), _effective_tier(spec))
                    err_adj = adjudicate_predicate(err_pred, ans.get("no_err"), _effective_tier(spec))
                    engine._record_confidence(recent_confs, step_idx, "noul", f"{spec.field_id}:mask", mask_adj.score, mask_adj.zone)

                    if mask_adj.zone == "gray":
                        mask_adj, cur_frame, intr_m = engine._run_gray_zone_retry(
                            mask_pred, _effective_tier(spec), verify_roi, settle_ms, metrics, recent_confs, step_idx,
                            interrupt_preds=interrupt_preds,
                        )
                        final_frame = cur_frame
                        if intr_m is not None:
                            immediate_halt_breaker = CB_INTERRUPT
                            immediate_halt_detail = f"Interrupt check during retry: {intr_m['label']}"
                            _record_breaker(CB_INTERRUPT)
                            return True, needs_reobserve
                    if err_adj.zone == "gray":
                        err_adj, cur_frame, intr_e = engine._run_gray_zone_retry(
                            err_pred, _effective_tier(spec), verify_roi, settle_ms, metrics, recent_confs, step_idx,
                            interrupt_preds=interrupt_preds,
                        )
                        final_frame = cur_frame
                        if intr_e is not None:
                            immediate_halt_breaker = CB_INTERRUPT
                            immediate_halt_detail = f"Interrupt check during retry: {intr_e['label']}"
                            _record_breaker(CB_INTERRUPT)
                            return True, needs_reobserve

                    if err_adj.zone == "reject":
                        if attempt + 1 >= max_tries:
                            st.status = "failed_validation"
                            st.breaker_code = CB_VALIDATION_ERROR
                            st.reason = f"Validation error cue detected on secret field {spec.field_id!r}"
                            st.terminal_failure = True
                            _record_breaker(CB_VALIDATION_ERROR)
                            error_crop_frame = cur_frame
                            error_crop_roi = list(verify_roi)
                            error_crop_box = list(box_norm)
                            error_crop_label = CB_VALIDATION_ERROR
                            return False, needs_reobserve
                        already_focused = False
                        continue

                    if mask_adj.zone == "pass" and err_adj.zone == "pass":
                        st.observed_value = SECRET_REDACTED_VALUE
                        st.status = "reconciled"
                        st.breaker_code = None
                        st.reason = "Verified masked non-empty secret state with zero error cues"
                        st.ever_reconciled = True
                        st.ever_matched = True
                        return False, needs_reobserve

                    if mask_adj.zone == "gray" or err_adj.zone == "gray":
                        st.status = "uncertain"
                        st.breaker_code = CB_AMBIGUITY
                        st.reason = f"Ambiguous verification on secret field {spec.field_id!r}"
                        st.terminal_failure = True
                        _record_breaker(CB_AMBIGUITY)
                        error_crop_frame = cur_frame
                        error_crop_roi = list(verify_roi)
                        error_crop_box = list(box_norm)
                        error_crop_label = CB_AMBIGUITY
                        return False, needs_reobserve

                    # mask_adj.zone == "reject" -> field empty or rejected
                    if attempt + 1 >= max_tries:
                        st.status = "failed_validation"
                        st.breaker_code = CB_VALIDATION_ERROR
                        st.reason = f"Secret field {spec.field_id!r} failed masked non-empty verification"
                        st.terminal_failure = True
                        _record_breaker(CB_VALIDATION_ERROR)
                        error_crop_frame = cur_frame
                        error_crop_roi = list(verify_roi)
                        error_crop_box = list(box_norm)
                        error_crop_label = CB_VALIDATION_ERROR
                        return False, needs_reobserve
                    already_focused = False
                    continue

                # Non-secret text/number verification
                # Validate the committed state without refocusing a field that may have moved.
                if needs_reobserve:
                    loc_state, new_box, reason, cur_frame = _locate_and_verify_box(spec, step_idx, verify_geometry=False)
                    if loc_state == "halt":
                        return True, needs_reobserve
                    if loc_state != "ok" or new_box is None:
                        st.status = "uncertain"
                        st.breaker_code = CB_AMBIGUITY
                        st.reason = reason
                        st.terminal_failure = True
                        _record_breaker(CB_AMBIGUITY)
                        return False, needs_reobserve
                    box_norm = new_box
                v_state, v_val, cur_frame, _ = _observe_text_or_number(
                    spec, box_norm, step_idx, focus_for_clipboard=False,
                    read_clipboard=not (spec.commit_on_blur or needs_reobserve),
                )
                st.observed_value = v_val
                if v_state == "halt":
                    return True, needs_reobserve
                if v_state == "matched":
                    st.status = "reconciled"
                    st.breaker_code = None
                    st.reason = f"Overwrote and verified ({spec.match_mode})"
                    st.ever_reconciled = True
                    st.ever_matched = True
                    return False, needs_reobserve
                if v_state == "uncertain":
                    st.status = "uncertain"
                    st.breaker_code = CB_AMBIGUITY
                    st.reason = f"Verification ambiguity on {spec.field_id!r}"
                    st.terminal_failure = True
                    _record_breaker(CB_AMBIGUITY)
                    return False, needs_reobserve

                if attempt + 1 >= max_tries:
                    st.status = "failed_validation"
                    st.breaker_code = CB_VALIDATION_ERROR
                    st.reason = (
                        f"Text field {spec.field_id!r} reverted or mismatched after overwrite "
                        f"(observed={v_val!r}, expected={spec.desired_value!r})"
                    )
                    st.terminal_failure = True
                    _record_breaker(CB_VALIDATION_ERROR)
                    error_crop_frame = cur_frame
                    error_crop_roi = list(spec.error_roi or pad_bbox_norm(box_norm, 0.35, mode="pad"))
                    error_crop_box = list(box_norm)
                    error_crop_label = CB_VALIDATION_ERROR
                    return False, needs_reobserve
                already_focused = False

            return False, needs_reobserve

        def _act_and_verify_dropdown(
            spec: ReconcileFieldSpec,
            st: _FieldRuntimeState,
            step_idx: int,
            *,
            initial_obs_item: Optional[Dict[str, Any]] = None,
        ) -> Tuple[bool, bool]:
            """Execute Observe/Act/Verify for ``dropdown`` (delegating selection to ``loop_until``)."""
            nonlocal cur_frame, final_frame, error_crop_frame, error_crop_roi, error_crop_box, error_crop_label
            nonlocal immediate_halt_breaker, immediate_halt_detail
            needs_reobserve = False

            loc_status, box_norm, loc_reason, cur_frame = _locate_and_verify_box(spec, step_idx)
            if loc_status == "halt":
                return True, needs_reobserve
            if loc_status == "dependency_blocked":
                st.status = "skipped_dependency"
                st.breaker_code = CB_DEPENDENCY_BLOCKED
                st.reason = loc_reason
                st.terminal_failure = True
                _record_breaker(CB_DEPENDENCY_BLOCKED)
                return False, needs_reobserve
            if loc_status == "uncertain" or box_norm is None:
                st.status = "uncertain"
                st.breaker_code = CB_AMBIGUITY
                st.reason = loc_reason
                st.terminal_failure = True
                _record_breaker(CB_AMBIGUITY)
                return False, needs_reobserve

            if spec.match_mode != "overwrite_always":
                obs_state, obs_val, cur_frame = _observe_dropdown_value(
                    spec, box_norm, step_idx, pre_obs_item=initial_obs_item
                )
                st.observed_value = obs_val
                if obs_state == "halt":
                    return True, needs_reobserve
                if obs_state == "disabled":
                    st.status = "skipped_dependency"
                    st.breaker_code = CB_DEPENDENCY_BLOCKED
                    st.reason = f"Dropdown {spec.field_id!r} is disabled"
                    st.terminal_failure = True
                    _record_breaker(CB_DEPENDENCY_BLOCKED)
                    return False, needs_reobserve
                if obs_state == "uncertain":
                    st.status = "uncertain"
                    st.breaker_code = CB_AMBIGUITY
                    st.reason = f"Ambiguous dropdown state on {spec.field_id!r}"
                    st.terminal_failure = True
                    _record_breaker(CB_AMBIGUITY)
                    return False, needs_reobserve
                if obs_state == "matched":
                    st.status = "reconciled" if st.ever_reconciled else "already_matched"
                    st.breaker_code = None
                    st.reason = "Observed dropdown value already matches desired_value"
                    st.ever_matched = True
                    return False, needs_reobserve

            max_tries = 1 + max_retries_per_field
            for attempt in range(max_tries):
                st.attempts += 1
                pre_open = engine._capture_frame()
                cy = int(round((box_norm[0] + box_norm[2]) / 2.0))
                cx = int(round((box_norm[1] + box_norm[3]) / 2.0))
                if not _exec_action({"type": "click", "x": cx, "y": cy, "button": "left", "count": 1}):
                    return True, needs_reobserve
                _sleep_settle()
                post_open = engine._capture_frame()
                cur_frame = post_open
                final_frame = post_open
                if not _check_unexpected_navigation(pre_open, post_open, f"opening dropdown {spec.field_id}"):
                    return True, needs_reobserve

                lcfg = dict(spec.loop_config or {})
                default_menu_roi = [
                    max(0, box_norm[0] - 40),
                    max(0, box_norm[1] - 20),
                    min(1000, max(box_norm[2] + 350, container_roi[2])),
                    min(1000, max(box_norm[3] + 100, box_norm[3])),
                ]
                menu_roi = lcfg.get("roi") or default_menu_roi
                mx = int(round((menu_roi[1] + menu_roi[3]) / 2.0))
                my = int(round((menu_roi[0] + menu_roi[2]) / 2.0))
                loop_args: Dict[str, Any] = {
                    "stop_predicate": lcfg.get("stop_predicate")
                    or {
                        "type": "noul",
                        "instructions": f"Is the dropdown option {spec.desired_value!r} visible in the open menu?",
                    },
                    "step_action": lcfg.get("step_action")
                    or {"type": "wheel_scroll", "x": mx, "y": my, "direction": "down", "magnitude": 250},
                    "fallback_action": lcfg.get("fallback_action")
                    or {"type": "key_press", "key": "Page_Down"},
                    "roi": menu_roi,
                    "progress_roi": lcfg.get("progress_roi") or menu_roi,
                    "max_steps": lcfg.get("max_steps", 12),
                    "settle_delay_ms": lcfg.get("settle_delay_ms", settle_ms),
                    "on_match": lcfg.get("on_match")
                    or {
                        "mode": "click_target_box",
                        "target_description": f"dropdown option {spec.desired_value!r}",
                    },
                    "risk_tier": _effective_tier(spec),
                    "interrupt_predicates": raw_interrupts,
                    "ignore_scene_change": ignore_scene_change,
                }
                if lcfg.get("directional_actions") is not None:
                    loop_args["directional_actions"] = lcfg["directional_actions"]

                loop_res = engine.run_loop_until(
                    loop_args,
                    collaborate_mode=collaborate_mode,
                    _include_crop_box_check=False,
                    _confirm_crop_roi=menu_roi,
                    _max_actions=max_total_actions - metrics["steps_executed"],
                    _skip_handoff_png=True,
                )
                _merge_sub_metrics(loop_res)
                cur_frame = engine._capture_frame()
                final_frame = cur_frame

                l_breaker = loop_res.get("breaker_code")
                if l_breaker == CB_BUDGET_EXCEEDED and not _check_budget():
                    return True, needs_reobserve
                if l_breaker == CB_INTERRUPT:
                    immediate_halt_breaker = CB_INTERRUPT
                    immediate_halt_detail = str(loop_res.get("detail") or f"Interrupted in dropdown {spec.field_id!r}")
                    _record_breaker(CB_INTERRUPT)
                    return True, needs_reobserve
                if l_breaker == CB_SAFETY_GUARD:
                    immediate_halt_breaker = CB_SAFETY_GUARD
                    immediate_halt_detail = str(loop_res.get("detail") or f"Safety guard in dropdown {spec.field_id!r}")
                    _record_breaker(CB_SAFETY_GUARD)
                    return True, needs_reobserve

                if loop_res.get("status") != "matched":
                    # Close open dropdown menu safely with Escape before retry/handoff.
                    if _check_budget():
                        _exec_action({"type": "key_press", "key": "Escape"})
                        _sleep_settle()
                        cur_frame = engine._capture_frame()
                        final_frame = cur_frame
                    if attempt + 1 >= max_tries:
                        if l_breaker == CB_AMBIGUITY:
                            st.status = "uncertain"
                            st.breaker_code = CB_AMBIGUITY
                            st.reason = str(loop_res.get("detail") or f"Dropdown {spec.field_id!r} ambiguous")
                            st.terminal_failure = True
                            _record_breaker(CB_AMBIGUITY)
                            error_crop_frame = cur_frame
                            error_crop_roi = list(menu_roi)
                            error_crop_box = list(box_norm)
                            error_crop_label = CB_AMBIGUITY
                        else:
                            st.status = "failed_validation"
                            st.breaker_code = CB_VALIDATION_ERROR
                            st.reason = (
                                f"Dropdown option {spec.desired_value!r} not selectable for {spec.field_id!r}: "
                                f"{loop_res.get('detail')}"
                            )
                            st.terminal_failure = True
                            _record_breaker(CB_VALIDATION_ERROR)
                            error_crop_frame = cur_frame
                            error_crop_roi = list(menu_roi)
                            error_crop_box = list(box_norm)
                            error_crop_label = CB_VALIDATION_ERROR
                        return False, needs_reobserve
                    continue

                ok_nav, shifted, cur_frame = _post_mutation_guards(spec, pre_open, cur_frame, box_norm, step_idx)
                needs_reobserve = needs_reobserve or shifted
                if not ok_nav:
                    return True, needs_reobserve

                blur_res, cur_frame, blur_shifted = _trigger_blur_and_check_validation(
                    spec, pre_open, cur_frame, box_norm, step_idx
                )
                needs_reobserve = needs_reobserve or blur_shifted
                if blur_res == "halt":
                    return True, needs_reobserve
                if blur_res == "validation_error":
                    st.status = "failed_validation"
                    st.breaker_code = CB_VALIDATION_ERROR
                    st.reason = f"Inline validation error after selecting dropdown {spec.field_id!r}"
                    st.terminal_failure = True
                    _record_breaker(CB_VALIDATION_ERROR)
                    return False, needs_reobserve

                # Verify collapsed display value matches target
                v_state, v_val, cur_frame = _observe_dropdown_value(spec, box_norm, step_idx)
                st.observed_value = v_val
                if v_state == "halt":
                    return True, needs_reobserve
                if v_state == "matched":
                    st.status = "reconciled"
                    st.breaker_code = None
                    st.reason = f"Selected and verified collapsed dropdown value {spec.desired_value!r}"
                    st.ever_reconciled = True
                    st.ever_matched = True
                    return False, needs_reobserve
                if v_state == "uncertain":
                    st.status = "uncertain"
                    st.breaker_code = CB_AMBIGUITY
                    st.reason = f"Ambiguous collapsed dropdown verification on {spec.field_id!r}"
                    st.terminal_failure = True
                    _record_breaker(CB_AMBIGUITY)
                    return False, needs_reobserve

                if attempt + 1 >= max_tries:
                    st.status = "failed_validation"
                    st.breaker_code = CB_VALIDATION_ERROR
                    st.reason = (
                        f"Dropdown {spec.field_id!r} collapsed value mismatched/reverted "
                        f"(observed={v_val!r}, expected={spec.desired_value!r})"
                    )
                    st.terminal_failure = True
                    _record_breaker(CB_VALIDATION_ERROR)
                    error_crop_frame = cur_frame
                    error_crop_roi = list(spec.error_roi or pad_bbox_norm(box_norm, 0.35, mode="pad"))
                    error_crop_box = list(box_norm)
                    error_crop_label = CB_VALIDATION_ERROR
                    return False, needs_reobserve

            return False, needs_reobserve

        def _act_and_verify_slider(
            spec: ReconcileFieldSpec,
            st: _FieldRuntimeState,
            step_idx: int,
            *,
            initial_obs_item: Optional[Dict[str, Any]] = None,
        ) -> Tuple[bool, bool]:
            """Execute Observe/Act/Verify for ``slider`` (delegating convergence to ``loop_until``)."""
            nonlocal cur_frame, final_frame, error_crop_frame, error_crop_roi, error_crop_box, error_crop_label
            nonlocal immediate_halt_breaker, immediate_halt_detail
            needs_reobserve = False

            loc_status, box_norm, loc_reason, cur_frame = _locate_and_verify_box(spec, step_idx)
            if loc_status == "halt":
                return True, needs_reobserve
            if loc_status == "dependency_blocked":
                st.status = "skipped_dependency"
                st.breaker_code = CB_DEPENDENCY_BLOCKED
                st.reason = loc_reason
                st.terminal_failure = True
                _record_breaker(CB_DEPENDENCY_BLOCKED)
                return False, needs_reobserve
            if loc_status == "uncertain" or box_norm is None:
                st.status = "uncertain"
                st.breaker_code = CB_AMBIGUITY
                st.reason = loc_reason
                st.terminal_failure = True
                _record_breaker(CB_AMBIGUITY)
                return False, needs_reobserve

            if spec.match_mode != "overwrite_always" and initial_obs_item is not None:
                lbl = str(initial_obs_item.get("label") or "")
                st_str = str(initial_obs_item.get("status") or "")
                st.observed_value = lbl
                if lbl == "disabled":
                    st.status = "skipped_dependency"
                    st.breaker_code = CB_DEPENDENCY_BLOCKED
                    st.reason = f"Slider {spec.field_id!r} is disabled"
                    st.terminal_failure = True
                    _record_breaker(CB_DEPENDENCY_BLOCKED)
                    return False, needs_reobserve
                if st_str == "gray_fail" or lbl == "unreadable":
                    st.status = "uncertain"
                    st.breaker_code = CB_AMBIGUITY
                    st.reason = f"Ambiguous slider observation on {spec.field_id!r}"
                    st.terminal_failure = True
                    _record_breaker(CB_AMBIGUITY)
                    error_crop_frame = cur_frame
                    error_crop_roi = list(spec.roi)
                    error_crop_box = list(box_norm)
                    error_crop_label = CB_AMBIGUITY
                    return False, needs_reobserve
                if lbl == "matched":
                    st.status = "reconciled" if st.ever_reconciled else "already_matched"
                    st.breaker_code = None
                    st.reason = "Slider already at target value"
                    st.ever_matched = True
                    return False, needs_reobserve

            max_tries = 1 + max_retries_per_field
            for attempt in range(max_tries):
                st.attempts += 1
                pre_act = engine._capture_frame()
                lcfg = dict(spec.loop_config or {})
                if not lcfg.get("skip_focus_click", False):
                    cy = int(round((box_norm[0] + box_norm[2]) / 2.0))
                    cx = int(round((box_norm[1] + box_norm[3]) / 2.0))
                    if not _exec_action({"type": "click", "x": cx, "y": cy, "button": "left", "count": 1}):
                        return True, needs_reobserve
                    _sleep_settle()

                slider_roi = lcfg.get("roi") or pad_bbox_norm(box_norm, 0.35, mode="pad")
                loop_args: Dict[str, Any] = {
                    "stop_predicate": lcfg.get("stop_predicate")
                    or {
                        "type": "choice",
                        "instructions": f"Compare the slider {spec.locator_prompt} to target {spec.desired_value!r}.",
                        "criteria": {
                            "matched": f"The slider is at target {spec.desired_value!r}.",
                            "too_low": f"The slider is below {spec.desired_value!r}.",
                            "too_high": f"The slider is above {spec.desired_value!r}.",
                        },
                        "target_choices": ["matched"],
                    },
                    "roi": slider_roi,
                    "progress_roi": lcfg.get("progress_roi") or slider_roi,
                    "max_steps": lcfg.get("max_steps", 20),
                    "settle_delay_ms": lcfg.get("settle_delay_ms", settle_ms),
                    "risk_tier": _effective_tier(spec),
                    "interrupt_predicates": raw_interrupts,
                    "ignore_scene_change": ignore_scene_change,
                }
                if lcfg.get("step_action") is not None:
                    loop_args["step_action"] = lcfg["step_action"]
                if lcfg.get("directional_actions") is not None:
                    loop_args["directional_actions"] = lcfg["directional_actions"]
                elif lcfg.get("step_action") is None:
                    loop_args["directional_actions"] = {
                        "too_low": {"type": "key_press", "key": "Right"},
                        "too_high": {"type": "key_press", "key": "Left"},
                    }
                if lcfg.get("fallback_action") is not None:
                    loop_args["fallback_action"] = lcfg["fallback_action"]

                loop_res = engine.run_loop_until(
                    loop_args, collaborate_mode=collaborate_mode,
                    _max_actions=max_total_actions - metrics["steps_executed"],
                    _skip_handoff_png=True,
                )
                _merge_sub_metrics(loop_res)
                cur_frame = engine._capture_frame()
                final_frame = cur_frame

                l_breaker = loop_res.get("breaker_code")
                if l_breaker == CB_BUDGET_EXCEEDED and not _check_budget():
                    return True, needs_reobserve
                if l_breaker == CB_INTERRUPT:
                    immediate_halt_breaker = CB_INTERRUPT
                    immediate_halt_detail = str(loop_res.get("detail") or f"Interrupted on slider {spec.field_id!r}")
                    _record_breaker(CB_INTERRUPT)
                    return True, needs_reobserve
                if l_breaker == CB_SAFETY_GUARD:
                    immediate_halt_breaker = CB_SAFETY_GUARD
                    immediate_halt_detail = str(loop_res.get("detail") or f"Safety guard on slider {spec.field_id!r}")
                    _record_breaker(CB_SAFETY_GUARD)
                    return True, needs_reobserve

                if loop_res.get("status") != "matched":
                    if attempt + 1 >= max_tries:
                        st.status = "uncertain" if l_breaker == CB_AMBIGUITY else "failed_validation"
                        st.breaker_code = CB_AMBIGUITY if l_breaker == CB_AMBIGUITY else CB_VALIDATION_ERROR
                        st.reason = f"Slider {spec.field_id!r} failed to converge: {loop_res.get('detail')}"
                        st.terminal_failure = True
                        _record_breaker(st.breaker_code)
                        error_crop_frame = cur_frame
                        error_crop_roi = list(slider_roi)
                        error_crop_box = list(box_norm)
                        error_crop_label = st.breaker_code
                        return False, needs_reobserve
                    continue

                ok_nav, shifted, cur_frame = _post_mutation_guards(spec, pre_act, cur_frame, box_norm, step_idx)
                needs_reobserve = needs_reobserve or shifted
                if not ok_nav:
                    return True, needs_reobserve

                blur_res, cur_frame, blur_shifted = _trigger_blur_and_check_validation(
                    spec, pre_act, cur_frame, box_norm, step_idx
                )
                needs_reobserve = needs_reobserve or blur_shifted
                if blur_res == "halt":
                    return True, needs_reobserve
                if blur_res == "validation_error":
                    st.status = "failed_validation"
                    st.breaker_code = CB_VALIDATION_ERROR
                    st.reason = f"Inline validation error after adjusting slider {spec.field_id!r}"
                    st.terminal_failure = True
                    _record_breaker(CB_VALIDATION_ERROR)
                    return False, needs_reobserve

                st.observed_value = spec.desired_value
                st.status = "reconciled"
                st.breaker_code = None
                st.reason = f"Converged slider {spec.field_id!r} to {spec.desired_value!r}"
                st.ever_reconciled = True
                st.ever_matched = True
                return False, needs_reobserve

            return False, needs_reobserve

        # -------------------------------------------------------------------
        # Multi-pass Topological DAG Wave Loop
        # -------------------------------------------------------------------
        prev_pass_mismatches: Optional[int] = None

        for pass_idx in range(1, max_passes + 1):
            passes_executed = pass_idx
            engine._check_collaborate_pause(collaborate_mode)
            fsm_trace.append("S1_CAPTURE_AND_GATE")
            cur_frame = engine._capture_frame()
            final_frame = cur_frame
            any_mutation_this_pass = False

            for wave_idx, wave_fids in enumerate(waves):
                # 1. Dependency pre-check
                active_wave_specs: List[ReconcileFieldSpec] = []
                for fid in wave_fids:
                    sp = spec_by_id[fid]
                    st = field_states[fid]
                    if pass_idx > 1 and st.terminal_failure:
                        continue
                    blocked_by: Optional[Tuple[str, str]] = None
                    for dep_id in sp.depends_on:
                        dep_st = field_states[dep_id]
                        dep_sp = spec_by_id[dep_id]
                        if dep_st.status == "skipped_dependency" or (
                            dep_sp.required and dep_st.status in ("failed_validation", "uncertain")
                        ):
                            blocked_by = (dep_id, dep_st.status)
                            break
                    if blocked_by is not None:
                        dep_id, dep_status = blocked_by
                        st.status = "skipped_dependency"
                        st.breaker_code = CB_DEPENDENCY_BLOCKED
                        st.reason = f"Blocked by upstream dependency {dep_id!r} ({dep_status})"
                        st.terminal_failure = True
                        _record_breaker(CB_DEPENDENCY_BLOCKED)
                        continue
                    active_wave_specs.append(sp)

                if not active_wave_specs:
                    continue

                # 2. Batched observation for binary / dropdown-with-options / slider controls in wave
                writable_specs = [sp for sp in active_wave_specs if _effective_tier(sp) != "read_only"]
                batch_obs_map, halted = _batch_observe_fields(writable_specs, cur_frame, request_boxes=True)
                if halted:
                    break

                # Pre-classify already-matched vs mismatched vs blocked on initial wave read
                # Also record initial state hash before any Pass 1 mutations if all are observable here.
                wave_needs_reobserve = False

                for sp in active_wave_specs:
                    fid = sp.field_id
                    st = field_states[fid]
                    obs_item = batch_obs_map.get(fid)

                    if _effective_tier(sp) == "read_only":
                        if sp.control_type == "secret_text":
                            st.reason = "A masked value cannot be compared in read-only mode"
                        else:
                            predicate = normalize_predicate({
                                "type": "noul",
                                "instructions": f"Is the visible {sp.locator_prompt} ({sp.control_type}) set to {sp.desired_value!r}?",
                                "min_confidence": sp.min_confidence,
                            }, risk_tier="read_only", default_roi=sp.roi)
                            adj, cur_frame, halted = _query_predicate_with_retry(
                                cur_frame, predicate, "read_only", sp.roi, pass_idx * 100 + wave_idx,
                            )
                            final_frame = cur_frame
                            if halted:
                                break
                            if adj.zone == "pass":
                                st.status = "already_matched"
                                st.observed_value = sp.desired_value
                                st.breaker_code = None
                                st.reason = "Observed state already matches desired_value"
                                st.ever_matched = True
                                continue
                            st.reason = "The target value could not be confirmed in read-only mode"
                        st.status = "uncertain"
                        st.breaker_code = CB_AMBIGUITY
                        _record_breaker(CB_AMBIGUITY)
                        continue

                    # On Pass 2+, only re-act on fields that are currently mismatched (or failed/uncertain? no, those were skipped).
                    if sp.control_type in BINARY_CONTROL_TYPES:
                        if pass_idx > 1 and st.status in ("already_matched", "reconciled") and obs_item is not None:
                            lbl = str(obs_item.get("label") or "")
                            st.observed_value = lbl
                            if lbl == sp.desired_value:
                                continue

                        # If downstream control had no valid box localized in the batch call, check dependency block
                        tier_cfg = RISK_TIER_THRESHOLDS[_effective_tier(sp)]
                        tau_low = float(tier_cfg["tau_low"] if tier_cfg["tau_low"] is not None else 0.65)
                        if (
                            sp.depends_on
                            and obs_item is not None
                            and "located_box" in obs_item
                            and (
                                obs_item.get("located_box") is None
                                or float(obs_item.get("box_conf", 0.0) or 0.0) < tau_low
                            )
                            and not wave_needs_reobserve
                        ):
                            st.status = "skipped_dependency"
                            st.breaker_code = CB_DEPENDENCY_BLOCKED
                            st.reason = f"Expected downstream control {fid!r} did not appear"
                            st.terminal_failure = True
                            _record_breaker(CB_DEPENDENCY_BLOCKED)
                            continue

                        if obs_item is not None and not wave_needs_reobserve:
                            lbl = str(obs_item.get("label") or "")
                            st_str = str(obs_item.get("status") or "")
                            if lbl == sp.desired_value and sp.match_mode != "overwrite_always" and st_str == "collected":
                                st.observed_value = lbl
                                st.status = "reconciled" if st.ever_reconciled else "already_matched"
                                st.breaker_code = None
                                st.reason = "Observed state already matches desired_value"
                                st.ever_matched = True
                                continue

                        prev_reconciled = st.ever_reconciled
                        halt_now, shifted = _act_and_verify_binary(
                            sp,
                            st,
                            pass_idx * 100 + wave_idx,
                            skip_initial_observe=wave_needs_reobserve,
                            initial_obs_item=(None if wave_needs_reobserve else obs_item),
                        )
                        if st.ever_reconciled and not prev_reconciled or st.attempts > 0:
                            any_mutation_this_pass = True
                        if halt_now:
                            break
                        if shifted:
                            wave_needs_reobserve = True
                    elif sp.control_type in TEXT_CONTROL_TYPES or sp.control_type == "secret_text":
                        if pass_idx > 1 and st.status in ("already_matched", "reconciled") and sp.control_type != "secret_text":
                            # On pass 2+, check only if regressed; if still matched, skip
                            loc_b = _get_cached_box(fid)
                            if loc_b is not None:
                                o_st, o_val, cur_frame, foc = _observe_text_or_number(
                                    sp, loc_b[0], pass_idx * 100 + wave_idx,
                                    focus_for_clipboard=False, read_clipboard=False,
                                )
                                if o_st == "halt":
                                    break
                                if o_st == "matched":
                                    st.observed_value = o_val
                                    continue
                        halt_now, shifted = _act_and_verify_text(sp, st, pass_idx * 100 + wave_idx)
                        if st.attempts > 0:
                            any_mutation_this_pass = True
                        if halt_now:
                            break
                        if shifted:
                            wave_needs_reobserve = True
                    elif sp.control_type == "dropdown":
                        if pass_idx > 1 and st.status in ("already_matched", "reconciled") and obs_item is not None:
                            if _compare_text_values(sp, str(obs_item.get("label") or "")):
                                st.observed_value = str(obs_item.get("label") or "")
                                continue
                        halt_now, shifted = _act_and_verify_dropdown(
                            sp,
                            st,
                            pass_idx * 100 + wave_idx,
                            initial_obs_item=(None if wave_needs_reobserve else obs_item),
                        )
                        if st.attempts > 0:
                            any_mutation_this_pass = True
                        if halt_now:
                            break
                        if shifted:
                            wave_needs_reobserve = True
                    elif sp.control_type == "slider":
                        if pass_idx > 1 and st.status in ("already_matched", "reconciled") and obs_item is not None:
                            if str(obs_item.get("label") or "") == "matched":
                                st.observed_value = "matched"
                                continue
                        halt_now, shifted = _act_and_verify_slider(
                            sp,
                            st,
                            pass_idx * 100 + wave_idx,
                            initial_obs_item=(None if wave_needs_reobserve else obs_item),
                        )
                        if st.attempts > 0:
                            any_mutation_this_pass = True
                        if halt_now:
                            break
                        if shifted:
                            wave_needs_reobserve = True

                    if wave_needs_reobserve:
                        # Re-capture fresh frame for remaining controls in this wave
                        cur_frame = engine._capture_frame()
                        final_frame = cur_frame

                if immediate_halt_breaker is not None:
                    break

            if immediate_halt_breaker is not None:
                break

            # End-of-pass batched cross-field verification sweep (catches cross-field coupling/oscillation!)
            if any_mutation_this_pass and len(specs) > 1:
                observable_settled = [
                    sp
                    for sp in specs
                    if sp.control_type in BINARY_CONTROL_TYPES
                    or (sp.control_type == "dropdown" and sp.options)
                    or sp.control_type == "slider"
                ]
                observable_settled = [
                    sp for sp in observable_settled if field_states[sp.field_id].status in ("already_matched", "reconciled")
                ]
                if len(observable_settled) > 1:
                    sweep_map, halted = _batch_observe_fields(observable_settled, cur_frame, request_boxes=False)
                    if halted:
                        break
                    for sp in observable_settled:
                        item = sweep_map.get(sp.field_id)
                        if item is None:
                            continue
                        st = field_states[sp.field_id]
                        lbl = str(item.get("label") or "")
                        st.observed_value = lbl
                        if sp.control_type in BINARY_CONTROL_TYPES and lbl != sp.desired_value:
                            st.regressed = True
                            st.status = "uncertain"
                            st.reason = f"Regressed to {lbl!r} after subsequent field edits"
                        elif sp.control_type == "dropdown" and not _compare_text_values(sp, lbl):
                            st.regressed = True
                            st.status = "uncertain"
                            st.reason = f"Regressed to {lbl!r} after subsequent field edits"
                        elif sp.control_type == "slider" and lbl != "matched":
                            st.regressed = True
                            st.status = "uncertain"
                            st.reason = f"Regressed to {lbl!r} after subsequent field edits"

            # Check non-terminal mismatches and track state-vector hash
            pass_hash = _compute_state_vector_hash(specs, field_states)
            state_hashes.append(pass_hash)
            for sp in specs:
                st = field_states[sp.field_id]
                tok = f"{st.status}:{SECRET_REDACTED_VALUE if sp.control_type == 'secret_text' else st.observed_value}"
                st.state_history.append(tok)

            mismatched_non_terminal = [
                sp.field_id
                for sp in specs
                if field_states[sp.field_id].regressed
                or (
                    field_states[sp.field_id].status not in ("already_matched", "reconciled", "failed_validation", "skipped_dependency", "uncertain")
                )
            ]
            # Also count fields that had regressed or were not converged if any non-terminal remain
            # Note: failed_validation / skipped_dependency / un-regressed uncertain from S1 ambiguity are terminal.
            mismatch_count = len(mismatched_non_terminal)
            if mismatch_count == 0:
                break

            # Oscillation / non-convergence checks:
            # 1) State hash cycle (A -> B -> A)
            # 2) Mismatch count fails to shrink across passes
            # 3) Exceeds max_reconcile_passes with remaining mismatches
            if (
                pass_hash in seen_state_hashes
                or (prev_pass_mismatches is not None and mismatch_count >= prev_pass_mismatches)
                or pass_idx >= max_passes
            ):
                conflicting_fields = sorted(
                    {
                        fid
                        for fid in mismatched_non_terminal
                    }
                    | {
                        sp.field_id
                        for sp in specs
                        if field_states[sp.field_id].regressed
                        or len(set(field_states[sp.field_id].state_history)) > 1
                    }
                )
                immediate_halt_breaker = CB_OSCILLATION
                if pass_hash in seen_state_hashes:
                    immediate_halt_detail = (
                        f"State-vector hash cycled ({pass_hash}) across passes; conflicting fields: {conflicting_fields}"
                    )
                elif prev_pass_mismatches is not None and mismatch_count >= prev_pass_mismatches:
                    immediate_halt_detail = (
                        f"Mismatch count failed to shrink ({prev_pass_mismatches} -> {mismatch_count}); "
                        f"conflicting fields: {conflicting_fields}"
                    )
                else:
                    immediate_halt_detail = (
                        f"Exceeded max_reconcile_passes={max_passes} with {mismatch_count} mismatched field(s): "
                        f"{conflicting_fields}"
                    )
                _record_breaker(CB_OSCILLATION)
                break

            seen_state_hashes.add(pass_hash)
            prev_pass_mismatches = mismatch_count
            for fid in mismatched_non_terminal:
                field_states[fid].regressed = False

        # -------------------------------------------------------------------
        # Final Classification & S5_HANDOFF_TO_S2 Payload Construction
        # -------------------------------------------------------------------
        if immediate_halt_breaker is not None:
            # Ensure every field is cleanly classified into one of the 5 allowed states
            for wave in waves:
                for fid in wave:
                    sp = spec_by_id[fid]
                    st = field_states[fid]
                    if st.status in ("already_matched", "reconciled", "failed_validation", "skipped_dependency"):
                        continue
                    if any(
                        field_states[dep].status in ("skipped_dependency", "failed_validation", "uncertain")
                        and (field_states[dep].status == "skipped_dependency" or spec_by_id[dep].required)
                        for dep in sp.depends_on
                    ):
                        st.status = "skipped_dependency"
                        st.breaker_code = st.breaker_code or CB_DEPENDENCY_BLOCKED
                        if not st.reason:
                            st.reason = f"Skipped after halt ({immediate_halt_breaker})"
                    else:
                        st.status = "uncertain"
                        st.breaker_code = st.breaker_code or immediate_halt_breaker
                        if not st.reason:
                            st.reason = f"Halted by {immediate_halt_breaker}"

        counts = {k: 0 for k in VALID_FIELD_STATES}
        for sp in specs:
            st = field_states[sp.field_id]
            if st.status not in VALID_FIELD_STATES:
                st.status = "uncertain"
            counts[st.status] += 1

        if immediate_halt_breaker is not None:
            breaker_code = immediate_halt_breaker
            status = {
                CB_SAFETY_GUARD: "blocked",
                CB_INTERRUPT: "interrupted",
                CB_OSCILLATION: "oscillating",
                CB_BUDGET_EXCEEDED: "budget_exceeded",
            }.get(immediate_halt_breaker, "escalated")
            detail = immediate_halt_detail
        elif counts["failed_validation"] > 0:
            breaker_code = CB_VALIDATION_ERROR
            status = "validation_error"
            failed_ids = [s.field_id for s in specs if field_states[s.field_id].status == "failed_validation"]
            detail = f"Validation failed on field(s): {failed_ids}"
        elif counts["skipped_dependency"] > 0:
            breaker_code = CB_DEPENDENCY_BLOCKED
            status = "dependency_blocked"
            blocked_ids_list = [s.field_id for s in specs if field_states[s.field_id].status == "skipped_dependency"]
            detail = f"Dependency blocked on field(s): {blocked_ids_list}"
        elif counts["uncertain"] > 0:
            breaker_code = CB_AMBIGUITY
            status = "escalated"
            uncertain_ids = [s.field_id for s in specs if field_states[s.field_id].status == "uncertain"]
            detail = f"Uncertain state on field(s): {uncertain_ids}"
        else:
            breaker_code = None
            status = "completed"
            detail = (
                f"Reconciled {len(specs)} field(s) in {passes_executed} pass(es): "
                f"already_matched={counts['already_matched']}, reconciled={counts['reconciled']}."
            )

        # Attach optional annotated ROI crop ONLY on failed_validation, uncertain, or CB_SAFETY_GUARD
        attach_crop = (
            counts["failed_validation"] > 0
            or (counts["uncertain"] > 0 and breaker_code != CB_INTERRUPT)
            or breaker_code == CB_SAFETY_GUARD
        )
        if attach_crop:
            if error_crop_frame is None:
                error_crop_frame = final_frame
            if error_crop_roi is None:
                for sp in specs:
                    st = field_states[sp.field_id]
                    if st.status in ("failed_validation", "uncertain"):
                        error_crop_roi = list(sp.error_roi or st.box_norm or sp.roi)
                        error_crop_box = list(st.box_norm) if st.box_norm is not None else list(sp.roi)
                        error_crop_label = st.breaker_code or breaker_code or st.status
                        break
            annotated_frame = error_crop_frame
            annotated_roi = error_crop_roi or container_roi
            matched_box = error_crop_box or annotated_roi
            annotated_label = error_crop_label or breaker_code or "reconcile_issue"

        reconcile_summary = {
            "passes_executed": passes_executed,
            "waves_count": len(waves),
            "waves": waves,
            "counts": counts,
            "breaker_history": breaker_history,
            "conflicting_fields": conflicting_fields,
            "state_hashes": state_hashes,
            "fields": [field_states[s.field_id].to_report() for s in specs],
        }

    except SystemOneApiError as exc:
        status = "error"
        breaker_code = None
        detail = str(exc)
    finally:
        fsm_trace.append("S5_HANDOFF_TO_S2")
        engine._safe_release_inputs()

    return engine._build_handoff_result(
        tool_name="reconcile_ui_state",
        status=status,
        breaker_code=breaker_code,
        detail=detail,
        metrics=metrics,
        start_time=start_time,
        final_frame=final_frame,
        matched_box=matched_box,
        matched_label=matched_label,
        recent_confs=recent_confs,
        reconcile_summary=reconcile_summary,
        annotated_crop_frame=annotated_frame,
        annotated_crop_roi=annotated_roi,
        attach_annotated_crop=attach_crop,
        annotated_crop_label=annotated_label,
        fsm_trace=fsm_trace,
    )
