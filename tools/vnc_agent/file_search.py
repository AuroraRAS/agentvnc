"""Gemini File Search API REST client and workspace store registry."""

import json
import mimetypes
import os
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Dict, List, Optional

from .constants import GENAI_API_BASE, GENAI_UPLOAD_BASE, MAX_TRANSFER_BYTES
from .workspace import LocalWorkspace, atomic_write_json, utc_now_iso


_EXTRA_MIME_TYPES: Dict[str, str] = {
    ".md": "text/markdown",
    ".markdown": "text/markdown",
    ".rst": "text/plain",
    ".txt": "text/plain",
    ".log": "text/plain",
    ".json": "application/json",
    ".yaml": "text/yaml",
    ".yml": "text/yaml",
    ".toml": "text/plain",
    ".csv": "text/csv",
    ".tsv": "text/tab-separated-values",
    ".py": "text/x-python",
    ".js": "text/javascript",
    ".ts": "text/typescript",
    ".cpp": "text/plain",
    ".cc": "text/plain",
    ".c": "text/plain",
    ".h": "text/plain",
    ".hpp": "text/plain",
    ".html": "text/html",
    ".xml": "application/xml",
    ".pdf": "application/pdf",
}


class FileSearchApiError(RuntimeError):
    """Structured error from the Gemini File Search REST API."""

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
            summary = f"File Search API HTTP {status_code}: {self.error_message or self.error_code or 'Unknown error'}"
        else:
            summary = f"File Search API error: {self.error_message or 'Unknown error'}"
        super().__init__(summary)


def normalize_store_name(store_name: str) -> str:
    raw = (store_name or "").strip().strip("/")
    if not raw:
        raise ValueError("store_name must not be empty")
    if raw.startswith("fileSearchStores/"):
        return raw
    if "/" in raw:
        raise ValueError(f"Invalid File Search store resource name: {store_name!r}")
    return f"fileSearchStores/{raw}"


def guess_mime_type(file_path: str) -> str:
    ext = os.path.splitext(file_path)[1].lower()
    if ext in _EXTRA_MIME_TYPES:
        return _EXTRA_MIME_TYPES[ext]
    guessed, _ = mimetypes.guess_type(file_path)
    return guessed or "application/octet-stream"


class FileSearchClient:
    """Pure-stdlib REST client for Gemini File Search (`fileSearchStores`)."""

    def __init__(
        self,
        api_key: str,
        api_base: str = GENAI_API_BASE,
        upload_base: str = GENAI_UPLOAD_BASE,
        timeout: float = 60.0,
    ) -> None:
        self.api_key = api_key
        self.api_base = api_base.rstrip("/")
        self.upload_base = upload_base.rstrip("/")
        self.timeout = float(timeout)

    def _request_json(
        self,
        method: str,
        url: str,
        body: Optional[Dict[str, Any]] = None,
        extra_headers: Optional[Dict[str, str]] = None,
    ) -> Dict[str, Any]:
        headers: Dict[str, str] = {"x-goog-api-key": self.api_key}
        data: Optional[bytes] = None
        if body is not None:
            headers["Content-Type"] = "application/json"
            data = json.dumps(body).encode("utf-8")
        if extra_headers:
            headers.update(extra_headers)

        last_err: Optional[Exception] = None
        for attempt in range(3):
            req = urllib.request.Request(url, data=data, headers=headers, method=method)
            try:
                with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                    raw = resp.read().decode("utf-8")
                if not raw.strip():
                    return {}
                return json.loads(raw)
            except urllib.error.HTTPError as e:
                try:
                    err_text = e.read().decode("utf-8", errors="replace")
                    code = e.code
                finally:
                    e.close()
                api_err = FileSearchApiError(code, err_text)
                if code in (429, 500, 502, 503, 504) and attempt < 2:
                    last_err = api_err
                    time.sleep(1.0 * (2**attempt))
                    continue
                raise api_err from None
            except (urllib.error.URLError, TimeoutError, OSError) as e:
                if attempt < 2:
                    last_err = e
                    time.sleep(1.0 * (2**attempt))
                    continue
                raise FileSearchApiError(None, f"Network error: {e}") from None
            except json.JSONDecodeError as e:
                raise FileSearchApiError(None, f"Invalid JSON from File Search API: {e}") from None
        if isinstance(last_err, FileSearchApiError):
            raise last_err
        raise FileSearchApiError(None, f"File Search API failed after retries: {last_err}")

    def create_store(
        self,
        display_name: str,
        embedding_model: Optional[str] = None,
    ) -> Dict[str, Any]:
        if not isinstance(display_name, str) or not display_name.strip():
            raise ValueError("display_name must be a non-empty string")
        payload: Dict[str, Any] = {"displayName": display_name.strip()[:512]}
        if embedding_model and str(embedding_model).strip():
            payload["embeddingModel"] = str(embedding_model).strip()
        return self._request_json("POST", f"{self.api_base}/fileSearchStores", body=payload)

    def get_store(self, store_name: str) -> Dict[str, Any]:
        norm = normalize_store_name(store_name)
        return self._request_json("GET", f"{self.api_base}/{norm}")

    def list_stores(
        self,
        page_size: int = 20,
        page_token: Optional[str] = None,
    ) -> Dict[str, Any]:
        params: Dict[str, Any] = {"pageSize": max(1, min(int(page_size), 20))}
        if page_token:
            params["pageToken"] = page_token
        qs = urllib.parse.urlencode(params)
        return self._request_json("GET", f"{self.api_base}/fileSearchStores?{qs}")

    def delete_store(self, store_name: str, force: bool = True) -> Dict[str, Any]:
        norm = normalize_store_name(store_name)
        qs = urllib.parse.urlencode({"force": "true" if force else "false"})
        return self._request_json("DELETE", f"{self.api_base}/{norm}?{qs}")

    def get_operation(self, operation_name: str) -> Dict[str, Any]:
        norm = operation_name.strip().lstrip("/")
        if not norm:
            raise ValueError("operation_name must not be empty")
        return self._request_json("GET", f"{self.api_base}/{norm}")

    def list_documents(
        self,
        store_name: str,
        page_size: int = 20,
        page_token: Optional[str] = None,
    ) -> Dict[str, Any]:
        norm = normalize_store_name(store_name)
        params: Dict[str, Any] = {"pageSize": max(1, min(int(page_size), 20))}
        if page_token:
            params["pageToken"] = page_token
        qs = urllib.parse.urlencode(params)
        return self._request_json("GET", f"{self.api_base}/{norm}/documents?{qs}")

    def get_document(self, document_name: str) -> Dict[str, Any]:
        norm = document_name.strip().lstrip("/")
        if not norm.startswith("fileSearchStores/") or "/documents/" not in norm:
            raise ValueError(f"Invalid File Search document name: {document_name!r}")
        return self._request_json("GET", f"{self.api_base}/{norm}")

    def delete_document(self, document_name: str, force: bool = True) -> Dict[str, Any]:
        norm = document_name.strip().lstrip("/")
        if not norm.startswith("fileSearchStores/") or "/documents/" not in norm:
            raise ValueError(f"Invalid File Search document name: {document_name!r}")
        qs = urllib.parse.urlencode({"force": "true" if force else "false"})
        return self._request_json("DELETE", f"{self.api_base}/{norm}?{qs}")

    def upload_file(
        self,
        store_name: str,
        local_abs_path: str,
        display_name: Optional[str] = None,
        mime_type: Optional[str] = None,
        custom_metadata: Optional[List[Dict[str, Any]]] = None,
        chunking_config: Optional[Dict[str, Any]] = None,
        wait: bool = True,
        poll_interval: float = 1.0,
        timeout: float = 180.0,
    ) -> Dict[str, Any]:
        norm_store = normalize_store_name(store_name)
        if not os.path.exists(local_abs_path):
            raise FileNotFoundError(f"Local file not found: {local_abs_path}")
        if not os.path.isfile(local_abs_path):
            raise IsADirectoryError(f"Local path is not a regular file: {local_abs_path}")

        num_bytes = os.path.getsize(local_abs_path)
        if num_bytes > MAX_TRANSFER_BYTES:
            raise ValueError(
                f"File size ({num_bytes} bytes) exceeds File Search 100 MiB limit ({MAX_TRANSFER_BYTES})"
            )

        resolved_mime = (mime_type or "").strip() or guess_mime_type(local_abs_path)
        resolved_display = (display_name or os.path.basename(local_abs_path)).strip()[:512]

        start_meta: Dict[str, Any] = {"displayName": resolved_display}
        if resolved_mime:
            start_meta["mimeType"] = resolved_mime
        if custom_metadata:
            start_meta["customMetadata"] = custom_metadata
        if chunking_config:
            start_meta["chunkingConfig"] = chunking_config

        start_url = f"{self.upload_base}/{norm_store}:uploadToFileSearchStore"
        start_headers = {
            "x-goog-api-key": self.api_key,
            "X-Goog-Upload-Protocol": "resumable",
            "X-Goog-Upload-Command": "start",
            "X-Goog-Upload-Header-Content-Length": str(num_bytes),
            "X-Goog-Upload-Header-Content-Type": resolved_mime,
            "Content-Type": "application/json",
        }
        start_req = urllib.request.Request(
            start_url,
            data=json.dumps(start_meta).encode("utf-8"),
            headers=start_headers,
            method="POST",
        )
        try:
            with urllib.request.urlopen(start_req, timeout=self.timeout) as resp:
                upload_url = resp.headers.get("X-Goog-Upload-URL") or resp.headers.get("x-goog-upload-url")
        except urllib.error.HTTPError as e:
            try:
                err_text = e.read().decode("utf-8", errors="replace")
                code = e.code
            finally:
                e.close()
            raise FileSearchApiError(code, err_text) from None
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            raise FileSearchApiError(None, f"Resumable upload start failed: {e}") from None

        if not upload_url:
            raise FileSearchApiError(None, "Missing X-Goog-Upload-URL header in resumable upload response")

        with open(local_abs_path, "rb") as f:
            file_bytes = f.read()

        put_headers = {
            "Content-Length": str(len(file_bytes)),
            "X-Goog-Upload-Offset": "0",
            "X-Goog-Upload-Command": "upload, finalize",
            "Content-Type": resolved_mime,
        }
        put_req = urllib.request.Request(upload_url, data=file_bytes, headers=put_headers, method="POST")
        try:
            with urllib.request.urlopen(put_req, timeout=max(self.timeout, 120.0)) as resp:
                raw_op = resp.read().decode("utf-8")
                op = json.loads(raw_op) if raw_op.strip() else {"done": True}
        except urllib.error.HTTPError as e:
            try:
                err_text = e.read().decode("utf-8", errors="replace")
                code = e.code
            finally:
                e.close()
            raise FileSearchApiError(code, err_text) from None
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            raise FileSearchApiError(None, f"Resumable upload finalize failed: {e}") from None
        except json.JSONDecodeError as e:
            raise FileSearchApiError(None, f"Invalid JSON from upload finalize: {e}") from None

        if wait and isinstance(op, dict) and not op.get("done", False):
            op_name = op.get("name")
            if not op_name:
                return op
            deadline = time.monotonic() + float(timeout)
            while not op.get("done", False):
                if time.monotonic() >= deadline:
                    raise TimeoutError(f"Timed out waiting for File Search indexing operation {op_name}")
                time.sleep(max(0.05, float(poll_interval)))
                op = self.get_operation(str(op_name))

        if isinstance(op, dict) and op.get("error"):
            err = op["error"]
            raise FileSearchApiError(
                err.get("code") if isinstance(err, dict) else None,
                json.dumps(err),
            )
        return op


class FileSearchStoreRegistry:
    """Persists attached Gemini File Search Stores in `<workspace>/.agent/file_search_stores.json`."""

    def __init__(self, workspace: LocalWorkspace) -> None:
        self.workspace = workspace
        self.path = os.path.join(workspace.agent_dir, "file_search_stores.json")
        self._lock = threading.RLock()
        if not os.path.exists(self.path):
            atomic_write_json(self.path, {"default_store": None, "stores": {}})

    def _load_unlocked(self) -> Dict[str, Any]:
        try:
            with open(self.path, "r", encoding="utf-8") as f:
                data = json.load(f)
            if isinstance(data, dict) and isinstance(data.get("stores"), dict):
                return data
        except (OSError, json.JSONDecodeError):
            pass
        return {"default_store": None, "stores": {}}

    def _save_unlocked(self, data: Dict[str, Any]) -> None:
        atomic_write_json(self.path, data)

    def list_attached(self) -> List[str]:
        with self._lock:
            data = self._load_unlocked()
            return sorted(data["stores"].keys())

    def get_snapshot(self) -> Dict[str, Any]:
        with self._lock:
            return self._load_unlocked()

    def get_default_store(self) -> Optional[str]:
        with self._lock:
            data = self._load_unlocked()
            default = data.get("default_store")
            if isinstance(default, str) and default in data["stores"]:
                return default
            stores = sorted(data["stores"].keys())
            return stores[0] if stores else None

    def register_store(
        self,
        store_name: str,
        display_name: str = "",
        set_default: bool = False,
    ) -> Dict[str, Any]:
        norm = normalize_store_name(store_name)
        with self._lock:
            data = self._load_unlocked()
            prev = data["stores"].get(norm, {})
            data["stores"][norm] = {
                "name": norm,
                "display_name": display_name or prev.get("display_name", ""),
                "attached_at": prev.get("attached_at") or utc_now_iso(),
            }
            if set_default or not data.get("default_store"):
                data["default_store"] = norm
            self._save_unlocked(data)
            return dict(data)

    def remove_store(self, store_name: str) -> Dict[str, Any]:
        norm = normalize_store_name(store_name)
        with self._lock:
            data = self._load_unlocked()
            data["stores"].pop(norm, None)
            if data.get("default_store") == norm:
                remaining = sorted(data["stores"].keys())
                data["default_store"] = remaining[0] if remaining else None
            self._save_unlocked(data)
            return dict(data)
