"""Gemini Interactions API REST client and response extractors."""

import json
import os
import time
import urllib.error
import urllib.request
from typing import Any, Dict, List, Optional

from .constants import INTERACTIONS_API_REVISION, INTERACTIONS_URL


class InteractionsApiError(RuntimeError):
    """Structured error from the Gemini Interactions REST API."""

    def __init__(self, status_code: Optional[int], raw_body: str) -> None:
        self.status_code = status_code
        self.raw_body = raw_body or ""
        self.error_message = self.raw_body.strip()
        self.error_code = ""

        try:
            parsed = json.loads(self.raw_body)
        except (json.JSONDecodeError, TypeError):
            parsed = None

        if isinstance(parsed, dict):
            err_obj = parsed.get("error") if isinstance(parsed.get("error"), dict) else parsed
            msg = err_obj.get("message")
            code = err_obj.get("code") or err_obj.get("status")
            if isinstance(msg, str) and msg.strip():
                self.error_message = msg.strip()
            if isinstance(code, (str, int)):
                self.error_code = str(code).strip()

        if status_code is not None:
            summary = f"Interactions API HTTP {status_code}: {self.error_message or self.error_code or 'Unknown error'}"
        else:
            summary = f"Interactions API error: {self.error_message or 'Unknown error'}"
        super().__init__(summary)

    @property
    def is_safety_blocked(self) -> bool:
        hay = f"{self.error_message} {self.error_code}".lower()
        if any(tok in hay for tok in ("safety", "harmful", "policy violation", "prohibited_content", "content_filter")):
            return True
        if "blocked" in hay and any(tok in hay for tok in ("safety", "harmful", "policy", "content")):
            return True
        return False

    @property
    def is_auth_error(self) -> bool:
        return self.status_code in (401, 403)

    @property
    def is_previous_interaction_error(self) -> bool:
        if self.is_safety_blocked or self.is_auth_error:
            return False
        if self.status_code in (429, 500, 502, 503, 504) or self.status_code is None:
            return False
        if self.status_code == 404:
            return True
        if self.status_code == 400:
            msg = self.error_message.lower()
            if "previous_interaction" in msg:
                return True
            if "interaction" in msg and any(
                tok in msg for tok in ("not found", "expired", "invalid", "unknown", "requires_action", "pending")
            ):
                return True
        return False

    @property
    def is_tool_combination_error(self) -> bool:
        if self.status_code != 400:
            return False
        msg = self.error_message.lower()
        has_tool_kw = any(tok in msg for tok in ("tool", "google_search", "file_search", "function"))
        has_combo_kw = any(
            tok in msg
            for tok in (
                "combine",
                "combination",
                "alongside",
                "cannot be used with",
                "not supported with",
                "built-in",
                "builtin",
                "unsupported tool",
            )
        )
        return has_tool_kw and has_combo_kw


class InteractionsClient:
    """Thin REST client for Gemini v1beta Interactions API."""

    def __init__(
        self,
        api_key: str,
        model: str,
        tools: List[Dict[str, Any]],
        system_instruction: str,
        generation_config: Optional[Dict[str, Any]] = None,
        base_url: str = INTERACTIONS_URL,
        timeout: float = 90.0,
    ) -> None:
        self.api_key = api_key
        self.model = model
        self.tools = tools
        self.system_instruction = system_instruction
        self.default_generation_config: Dict[str, Any] = dict(generation_config or {})
        self.base_url = base_url
        self.timeout = float(timeout)

    def create(
        self,
        input_payload: Any,
        previous_interaction_id: Optional[str] = None,
        thinking_level: Optional[str] = None,
        tools_override: Optional[List[Dict[str, Any]]] = None,
    ) -> Dict[str, Any]:
        active_tools = self.tools if tools_override is None else tools_override
        body: Dict[str, Any] = {
            "model": self.model,
            "input": input_payload,
            "system_instruction": self.system_instruction,
        }
        if active_tools:
            body["tools"] = active_tools
        if previous_interaction_id:
            body["previous_interaction_id"] = previous_interaction_id

        gen_cfg = dict(self.default_generation_config)
        if thinking_level is not None and thinking_level != "default":
            gen_cfg["thinking_level"] = thinking_level
        if gen_cfg:
            body["generation_config"] = gen_cfg

        encoded = json.dumps(body).encode("utf-8")
        last_err: Optional[Exception] = None
        for attempt in range(3):
            req = urllib.request.Request(
                self.base_url,
                data=encoded,
                headers={
                    "Content-Type": "application/json",
                    "Api-Revision": INTERACTIONS_API_REVISION,
                    "x-goog-api-key": self.api_key,
                },
                method="POST",
            )
            try:
                with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                    raw_resp = resp.read().decode("utf-8")
                try:
                    interaction = json.loads(raw_resp)
                except json.JSONDecodeError as e:
                    raise InteractionsApiError(None, f"Invalid JSON response from Interactions API: {e}") from None
                if not isinstance(interaction, dict):
                    raise InteractionsApiError(None, "Expected a JSON object from the Interactions API")
                status = interaction.get("status")
                if status in ("failed", "cancelled", "incomplete"):
                    err = interaction.get("error")
                    raise InteractionsApiError(None, json.dumps(err) if err else f"Interaction {status}")
                return interaction
            except urllib.error.HTTPError as e:
                try:
                    err_text = e.read().decode("utf-8", errors="replace")
                    code = e.code
                finally:
                    e.close()
                api_err = InteractionsApiError(code, err_text)
                if code in (429, 500, 502, 503, 504) and attempt < 2:
                    last_err = api_err
                    time.sleep(1.5 * (2**attempt))
                    continue
                raise api_err from None
            except (urllib.error.URLError, TimeoutError, OSError) as e:
                if attempt < 2:
                    last_err = e
                    time.sleep(1.5 * (2**attempt))
                    continue
                raise InteractionsApiError(None, f"Network error: {e}") from None
        if isinstance(last_err, InteractionsApiError):
            raise last_err
        raise InteractionsApiError(None, f"Interactions API failed after retries: {last_err}")


def load_api_key(api_key_file: str) -> str:
    env_key = os.environ.get("GEMINI_API_KEY", "").strip()
    if env_key and not api_key_file:
        return env_key
    if api_key_file and os.path.exists(api_key_file):
        with open(api_key_file, "r", encoding="utf-8") as f:
            key = f.read().strip()
        if key:
            return key
    if env_key:
        return env_key
    raise FileNotFoundError(f"Gemini API key not found at {api_key_file!r} or GEMINI_API_KEY")


def extract_function_calls(interaction: Dict[str, Any]) -> List[Dict[str, Any]]:
    steps = interaction.get("steps")
    if isinstance(steps, list) and steps:
        calls = [s for s in steps if isinstance(s, dict) and s.get("type") == "function_call"]
        if calls:
            return calls
    outputs = interaction.get("outputs")
    if isinstance(outputs, list):
        return [o for o in outputs if isinstance(o, dict) and o.get("type") == "function_call"]
    return []


def extract_model_text(interaction: Dict[str, Any]) -> str:
    texts: List[str] = []
    for step in interaction.get("steps", []) or []:
        if isinstance(step, dict) and step.get("type") == "model_output":
            for block in step.get("content", []) or []:
                if isinstance(block, dict) and block.get("type") == "text" and block.get("text"):
                    texts.append(str(block["text"]))
    if not texts:
        for out in interaction.get("outputs", []) or []:
            if isinstance(out, dict) and out.get("type") == "text" and out.get("text"):
                texts.append(str(out["text"]))
    return "\n".join(texts).strip()


def extract_usage(interaction: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    """Return a copy of the Gemini interaction's token usage dict, if present."""
    raw = interaction.get("usage") or interaction.get("usage_metadata") or interaction.get("usageMetadata")
    if isinstance(raw, dict) and raw:
        return dict(raw)
    return None


def _is_server_tool_type(raw_type: Any) -> bool:
    t = str(raw_type or "").lower()
    if not t or t in ("function_call", "function_result", "text", "model_output", "thought", "user_input"):
        return False
    if t in (
        "google_search_call",
        "google_search_result",
        "file_search_call",
        "file_search_result",
        "url_context_call",
        "url_context_result",
        "grounding_metadata",
    ):
        return True
    return ("search" in t) or ("grounding" in t)


def extract_server_tool_events(interaction: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Extract normalized server-side search/grounding tool calls and results."""
    events: List[Dict[str, Any]] = []
    seen = set()

    def _add(item: Dict[str, Any]) -> None:
        compact: Dict[str, Any] = {}
        for k in ("type", "id", "status", "queries", "query", "webSearchQueries", "search_queries"):
            if k in item and item[k] is not None:
                compact[k] = item[k]
        for k in ("arguments", "result", "results", "groundingChunks", "grounding_chunks"):
            if k in item and item[k] is not None:
                compact[k] = item[k]
        if not compact:
            compact = dict(item)
        key = json.dumps(compact, sort_keys=True, ensure_ascii=False, default=str)
        if key not in seen:
            seen.add(key)
            events.append(compact)

    def _inspect_item(item: Any) -> None:
        if not isinstance(item, dict):
            return
        if _is_server_tool_type(item.get("type")):
            _add(item)
        for gm_key in ("grounding_metadata", "groundingMetadata"):
            gm = item.get(gm_key)
            if isinstance(gm, dict) and gm:
                ev = {"type": "grounding_metadata"}
                ev.update(gm)
                _add(ev)
        content = item.get("content")
        if isinstance(content, list):
            for block in content:
                _inspect_item(block)

    for step in interaction.get("steps", []) or []:
        _inspect_item(step)
    for out in interaction.get("outputs", []) or []:
        _inspect_item(out)
    for gm_key in ("grounding_metadata", "groundingMetadata"):
        gm = interaction.get(gm_key)
        if isinstance(gm, dict) and gm:
            ev = {"type": "grounding_metadata"}
            ev.update(gm)
            _add(ev)
    return events


def extract_citations(interaction: Dict[str, Any]) -> List[Dict[str, Any]]:
    citations: List[Dict[str, Any]] = []
    seen = set()

    def _add(item: Dict[str, Any]) -> None:
        key = json.dumps(item, sort_keys=True, ensure_ascii=False, default=str)
        if key not in seen:
            seen.add(key)
            citations.append(item)

    def _collect_grounding_meta(gm: Any) -> None:
        if not isinstance(gm, dict) or not gm:
            return
        chunks = gm.get("groundingChunks") or gm.get("grounding_chunks")
        if isinstance(chunks, list) and chunks:
            for ch in chunks:
                if isinstance(ch, dict):
                    _add(ch)
        else:
            _add({"type": "grounding_metadata", **gm})

    def _collect_blocks(blocks: Any) -> None:
        if not isinstance(blocks, list):
            return
        for block in blocks:
            if not isinstance(block, dict):
                continue
            anns = block.get("annotations")
            if isinstance(anns, list):
                for ann in anns:
                    if isinstance(ann, dict):
                        _add(ann)
            btype = str(block.get("type", "")).lower()
            if btype in ("google_search_result", "file_search_result", "url_context_result", "grounding_metadata"):
                _add(block)
            for gm_key in ("grounding_metadata", "groundingMetadata"):
                _collect_grounding_meta(block.get(gm_key))

    for step in interaction.get("steps", []) or []:
        if not isinstance(step, dict):
            continue
        _collect_blocks(step.get("content"))
        stype = str(step.get("type", "")).lower()
        if stype in ("google_search_result", "file_search_result", "url_context_result", "grounding_metadata"):
            _add(step)
        for gm_key in ("grounding_metadata", "groundingMetadata"):
            _collect_grounding_meta(step.get(gm_key))

    _collect_blocks(interaction.get("outputs"))
    for gm_key in ("grounding_metadata", "groundingMetadata"):
        _collect_grounding_meta(interaction.get(gm_key))
    return citations
