"""Constants, keymaps, tool schemas, and system instructions for vnc_agent."""

import os
from typing import Any, Dict, List, Tuple

PACKAGE_DIR = os.path.dirname(os.path.abspath(__file__))
TOOLS_DIR = os.path.dirname(PACKAGE_DIR)
REPO_ROOT = os.path.dirname(TOOLS_DIR)

INTERACTIONS_URL = "https://generativelanguage.googleapis.com/v1beta/interactions"
INTERACTIONS_API_REVISION = "2026-05-20"
GENAI_API_BASE = "https://generativelanguage.googleapis.com/v1beta"
GENAI_UPLOAD_BASE = "https://generativelanguage.googleapis.com/upload/v1beta"

DEFAULT_MODEL = "gemini-4-hydrogen"
DEFAULT_ASSISTANT_MODEL = "gemini-3.8-flash"
DEFAULT_SYSTEM_ONE_MODEL = "jev-latest"
TYPESAFE_SYSTEM_ONE_URL = "https://api.typesafe.ai/v1/systemone"
DEFAULT_VNCVIEWER = os.path.join(REPO_ROOT, "build", "vncviewer", "vncviewer")

THINKING_LEVELS: Tuple[str, ...] = ("low", "medium", "high", "xhigh", "default")
RECOVERY_THINKING_LEVELS: Tuple[str, ...] = ("low", "medium", "high", "xhigh", "off")
FTP_PROTOCOLS: Tuple[str, ...] = ("Auto", "Tight2", "Tight13", "Ultra")

DEFAULT_FILE_READ_MAX_BYTES = 131072
MAX_FILE_READ_BYTES = 1048576
MAX_FILE_WRITE_BYTES = 16 * 1024 * 1024
MAX_TRANSFER_BYTES = 100 * 1024 * 1024  # 100 MiB (matches Gemini File Search file limit)
DEFAULT_FILE_LIST_MAX_ENTRIES = 200
MAX_FILE_LIST_ENTRIES = 1000

AGENT_COMPUTER_USE = "computer_use"
AGENT_ASSISTANT = "assistant"
VALID_AGENTS: Tuple[str, ...] = (AGENT_COMPUTER_USE, AGENT_ASSISTANT)
VALID_STATUS_STATES: Tuple[str, ...] = (
    "idle",
    "working",
    "waiting_peer",
    "waiting_user",
    "sleeping",
    "paused",
    "done",
    "error",
)
VALID_MESSAGE_KINDS: Tuple[str, ...] = ("request", "response", "update", "handoff", "alert")
VALID_PRIORITIES: Tuple[str, ...] = ("normal", "urgent")

PREDEFINED_DESKTOP_FUNCTIONS: Tuple[str, ...] = (
    "click",
    "double_click",
    "triple_click",
    "middle_click",
    "right_click",
    "mouse_down",
    "mouse_up",
    "move",
    "drag_and_drop",
    "scroll",
    "type",
    "press_key",
    "key_down",
    "key_up",
    "hotkey",
    "wait",
    "take_screenshot",
)

CUSTOM_FUNCTION_DECLARATIONS: List[Dict[str, Any]] = [
    {
        "type": "function",
        "name": "clipboard_get",
        "description": (
            "Read the current remote VNC clipboard text via ClientCutText without clicking or taking a screenshot. "
            "Returns text-only JSON."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "intent": {"type": "string", "description": "Short reason for reading the clipboard."},
            },
        },
    },
    {
        "type": "function",
        "name": "clipboard_verify_selection",
        "description": (
            "Copy the current desktop text selection with Ctrl+C and compare a fresh clipboard update "
            "to the expected text. Select the relevant text first. Use this to verify exact text in "
            "copyable controls before saving or submitting."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "expected_text": {"type": "string", "description": "The full expected text of the selection."},
                "intent": {"type": "string", "description": "Short reason for verifying the text."},
            },
            "required": ["expected_text"],
        },
    },
    {
        "type": "function",
        "name": "clipboard_set",
        "description": (
            "Send text to the remote VNC clipboard using the configured transport encoding. "
            "Optionally sends Ctrl+V if paste=true. A non-ASCII paste returns a screenshot for verification."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "text": {"type": "string", "description": "UTF-8 text to copy into the remote clipboard."},
                "paste": {"type": "boolean", "description": "If true, also press Ctrl+V after setting clipboard."},
                "intent": {"type": "string", "description": "Short reason for setting clipboard contents."},
            },
            "required": ["text"],
        },
    },
    {
        "type": "function",
        "name": "paste_text_at",
        "description": (
            "Click at normalized (x, y), optionally clear the existing field, "
            "copy text into the VNC clipboard, paste with Ctrl+V, and optionally press Enter. "
            "A non-ASCII paste returns a screenshot for verification."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "x": {"type": "integer", "description": "Normalized X coordinate (0-1000)."},
                "y": {"type": "integer", "description": "Normalized Y coordinate (0-1000)."},
                "text": {"type": "string", "description": "UTF-8 text to paste into the target input."},
                "clear_existing": {"type": "boolean", "description": "If true, select and delete existing field contents before pasting."},
                "press_enter": {"type": "boolean", "description": "If true, press Enter after an ASCII paste. Verify non-ASCII text in the screenshot first."},
                "intent": {"type": "string", "description": "Short description of why you are pasting here."},
            },
            "required": ["x", "y", "text"],
        },
    },
    {
        "type": "function",
        "name": "click_and_type",
        "description": (
            "Click at normalized (x, y), optionally clear the existing field, "
            "type plain ASCII text directly via VNC KeyEvents, and optionally press Enter. Returns text-only JSON on success."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "x": {"type": "integer", "description": "Normalized X coordinate (0-1000)."},
                "y": {"type": "integer", "description": "Normalized Y coordinate (0-1000)."},
                "text": {"type": "string", "description": "ASCII text to type into the focused widget."},
                "clear_existing": {"type": "boolean", "description": "If true, select and delete existing field contents before typing."},
                "press_enter": {"type": "boolean", "description": "If true, press Enter after typing."},
                "intent": {"type": "string", "description": "Short description of why you are typing here."},
            },
            "required": ["x", "y", "text"],
        },
    },
    {
        "type": "function",
        "name": "get_desktop_status",
        "description": (
            "Check lightweight VNC desktop metadata (resolution, desktop name, frame_seq, damage_count, resize_count, "
            "updating, has_clipboard) without capturing or transferring a screenshot image. Returns text-only JSON."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "intent": {"type": "string", "description": "Short reason for checking desktop status."},
            },
        },
    },
    {
        "type": "function",
        "name": "yield_to_user",
        "description": (
            "Pause automation and hand control over to the human collaborator in vncviewer for login, CAPTCHA, "
            "MFA, or manual intervention, resuming with an updated screenshot when the operator resumes."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "reason": {"type": "string", "description": "Clear explanation of what the human operator needs to do."},
                "intent": {"type": "string", "description": "Short summary for the collaborative overlay banner."},
            },
            "required": ["reason"],
        },
    },
]

FILE_FUNCTION_DECLARATIONS: List[Dict[str, Any]] = [
    {
        "type": "function",
        "name": "file_list",
        "description": (
            "List files and subdirectories in a remote directory over TigerVNC's RFB file transfer FTP bridge "
            "without opening a GUI file manager. Returns JSON entries with name, path, type ('file' or 'dir'), "
            "and optional size/modified timestamp."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "Remote directory path (default '/'). Forward slashes or Windows paths like 'C:\\Users' are accepted.",
                },
                "max_entries": {
                    "type": "integer",
                    "description": "Maximum number of entries to return (1-1000, default 200).",
                },
                "intent": {"type": "string", "description": "Short reason for listing this directory."},
            },
        },
    },
    {
        "type": "function",
        "name": "file_stat",
        "description": (
            "Check whether a remote file or directory exists and retrieve its type ('file' or 'dir'), size in bytes, "
            "and modified timestamp via the FTP bridge."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Remote file or directory path."},
                "intent": {"type": "string", "description": "Short reason for checking file metadata."},
            },
            "required": ["path"],
        },
    },
    {
        "type": "function",
        "name": "file_read",
        "description": (
            "Read bytes or decoded text directly from a remote file over the FTP bridge without taking a screenshot. "
            "Supports byte offset and max_bytes chunking for large files, or as_base64=true for binary files."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Remote file path to read."},
                "encoding": {"type": "string", "description": "Text encoding when as_base64 is false (default 'utf-8')."},
                "as_base64": {"type": "boolean", "description": "If true, return base64-encoded bytes in content_base64."},
                "offset": {"type": "integer", "description": "Byte offset to start reading from (default 0)."},
                "max_bytes": {"type": "integer", "description": "Maximum bytes to read (default 131072, max 1048576)."},
                "intent": {"type": "string", "description": "Short reason for reading this file."},
            },
            "required": ["path"],
        },
    },
    {
        "type": "function",
        "name": "file_write",
        "description": (
            "Write or append text or binary data directly to a remote file over the FTP bridge without GUI editors. "
            "Provide either 'content' (text) or 'content_base64' (binary)."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Remote destination file path."},
                "content": {"type": "string", "description": "Text content to write (mutually exclusive with content_base64)."},
                "content_base64": {
                    "type": "string",
                    "description": "Base64-encoded binary content to write (mutually exclusive with content).",
                },
                "encoding": {"type": "string", "description": "Text encoding when using content (default 'utf-8')."},
                "append": {"type": "boolean", "description": "If true, append to existing file instead of overwriting."},
                "create_parents": {"type": "boolean", "description": "If true, create missing parent directories first."},
                "intent": {"type": "string", "description": "Short reason for writing this file."},
            },
            "required": ["path"],
        },
    },
    {
        "type": "function",
        "name": "file_mkdir",
        "description": "Create a remote directory (and optionally missing parent directories) via the FTP bridge.",
        "parameters": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Remote directory path to create."},
                "parents": {"type": "boolean", "description": "If true (default), create missing parent directories."},
                "intent": {"type": "string", "description": "Short reason for creating this directory."},
            },
            "required": ["path"],
        },
    },
    {
        "type": "function",
        "name": "file_delete",
        "description": "Delete a remote file or empty directory via the FTP bridge.",
        "parameters": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Remote file or empty directory path to delete."},
                "is_dir": {"type": "boolean", "description": "Set true to delete an empty directory (default false for files)."},
                "intent": {"type": "string", "description": "Short reason for deleting this path."},
            },
            "required": ["path"],
        },
    },
    {
        "type": "function",
        "name": "file_rename",
        "description": "Rename or move a remote file or directory via the FTP bridge.",
        "parameters": {
            "type": "object",
            "properties": {
                "from_path": {"type": "string", "description": "Existing remote source path."},
                "to_path": {"type": "string", "description": "Target remote destination path."},
                "intent": {"type": "string", "description": "Short reason for renaming/moving."},
            },
            "required": ["from_path", "to_path"],
        },
    },
]

WORKSPACE_FUNCTION_DECLARATIONS: List[Dict[str, Any]] = [
    {
        "type": "function",
        "name": "workspace_list",
        "description": (
            "List files and subdirectories in the shared local workspace. "
            "Both the Computer Use agent and the Research Assistant share this directory."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Workspace-relative directory path (default '.')."},
                "recursive": {"type": "boolean", "description": "If true, list entries recursively."},
                "max_entries": {"type": "integer", "description": "Maximum entries to return (1-1000, default 200)."},
                "intent": {"type": "string", "description": "Short reason for listing the local workspace."},
            },
        },
    },
    {
        "type": "function",
        "name": "workspace_stat",
        "description": "Inspect metadata (existence, type, size, modified timestamp) of a local workspace path.",
        "parameters": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Workspace-relative file or directory path."},
                "intent": {"type": "string", "description": "Short reason for inspecting this path."},
            },
            "required": ["path"],
        },
    },
    {
        "type": "function",
        "name": "workspace_read",
        "description": (
            "Read text or base64 bytes from a file in the shared local workspace. "
            "Supports byte offset and max_bytes chunking."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Workspace-relative file path to read."},
                "encoding": {"type": "string", "description": "Text encoding when as_base64 is false (default 'utf-8')."},
                "as_base64": {"type": "boolean", "description": "If true, return base64-encoded bytes."},
                "offset": {"type": "integer", "description": "Byte offset to start reading from (default 0)."},
                "max_bytes": {"type": "integer", "description": "Maximum bytes to read (default 131072, max 1048576)."},
                "intent": {"type": "string", "description": "Short reason for reading this file."},
            },
            "required": ["path"],
        },
    },
    {
        "type": "function",
        "name": "workspace_write",
        "description": (
            "Write or append text or binary data to a file in the shared local workspace. "
            "Provide either 'content' or 'content_base64'."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Workspace-relative file path to write."},
                "content": {"type": "string", "description": "Text content to write (mutually exclusive with content_base64)."},
                "content_base64": {
                    "type": "string",
                    "description": "Base64-encoded binary content to write (mutually exclusive with content).",
                },
                "encoding": {"type": "string", "description": "Text encoding when using content (default 'utf-8')."},
                "append": {"type": "boolean", "description": "If true, append to existing file instead of overwriting."},
                "create_parents": {"type": "boolean", "description": "If true (default), create missing parent directories."},
                "intent": {"type": "string", "description": "Short reason for writing this file."},
            },
            "required": ["path"],
        },
    },
    {
        "type": "function",
        "name": "workspace_mkdir",
        "description": "Create a directory in the shared local workspace.",
        "parameters": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Workspace-relative directory path to create."},
                "parents": {"type": "boolean", "description": "If true (default), create missing parent directories."},
                "intent": {"type": "string", "description": "Short reason for creating this directory."},
            },
            "required": ["path"],
        },
    },
    {
        "type": "function",
        "name": "workspace_delete",
        "description": "Delete a file or directory from the shared local workspace.",
        "parameters": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Workspace-relative path to delete."},
                "recursive": {"type": "boolean", "description": "If true, recursively delete a non-empty directory."},
                "intent": {"type": "string", "description": "Short reason for deleting this path."},
            },
            "required": ["path"],
        },
    },
    {
        "type": "function",
        "name": "workspace_rename",
        "description": "Rename or move a file or directory inside the shared local workspace.",
        "parameters": {
            "type": "object",
            "properties": {
                "from_path": {"type": "string", "description": "Existing workspace-relative source path."},
                "to_path": {"type": "string", "description": "Target workspace-relative destination path."},
                "intent": {"type": "string", "description": "Short reason for renaming/moving."},
            },
            "required": ["from_path", "to_path"],
        },
    },
    {
        "type": "function",
        "name": "workspace_search",
        "description": "Search UTF-8 text files and filenames inside the shared local workspace for a query string.",
        "parameters": {
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "Case-insensitive substring to search for."},
                "path": {"type": "string", "description": "Subdirectory to scope the search (default '.')."},
                "glob_pattern": {"type": "string", "description": "Optional filename glob filter such as '*.md' or '*.txt'."},
                "max_matches": {"type": "integer", "description": "Maximum matches to return (default 50)."},
                "intent": {"type": "string", "description": "Short reason for searching the workspace."},
            },
            "required": ["query"],
        },
    },
    {
        "type": "function",
        "name": "memory_list",
        "description": "List persistent memory keys stored in the shared local workspace memory store.",
        "parameters": {
            "type": "object",
            "properties": {
                "tag": {"type": "string", "description": "Optional tag filter."},
                "limit": {"type": "integer", "description": "Maximum entries to list (default 100)."},
                "intent": {"type": "string", "description": "Short reason for listing memory entries."},
            },
        },
    },
    {
        "type": "function",
        "name": "memory_read",
        "description": (
            "Read persistent shared memory. Omit 'key' to read MEMORY.md and an index of structured keys, "
            "or pass a specific key (or 'MEMORY.md') to read its full value."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "key": {"type": "string", "description": "Optional memory key name or 'MEMORY.md'."},
                "intent": {"type": "string", "description": "Short reason for reading memory."},
            },
        },
    },
    {
        "type": "function",
        "name": "memory_write",
        "description": (
            "Persist a durable memory entry (fact, preference, finding, or note) in the shared local workspace. "
            "Use key='MEMORY.md' to update the main markdown memory document, or any named key for structured storage."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "key": {"type": "string", "description": "Memory key identifier or 'MEMORY.md'."},
                "value": {"type": "string", "description": "Text value to store."},
                "tags": {"type": "string", "description": "Optional comma-separated tags (e.g. 'research,credentials')."},
                "append": {"type": "boolean", "description": "If true, append to existing value instead of replacing."},
                "intent": {"type": "string", "description": "Short reason for saving to memory."},
            },
            "required": ["key", "value"],
        },
    },
    {
        "type": "function",
        "name": "memory_delete",
        "description": "Delete a structured persistent memory key from the shared workspace memory store.",
        "parameters": {
            "type": "object",
            "properties": {
                "key": {"type": "string", "description": "Memory key to delete."},
                "intent": {"type": "string", "description": "Short reason for deleting memory."},
            },
            "required": ["key"],
        },
    },
    {
        "type": "function",
        "name": "memory_search",
        "description": "Search persistent memory (MEMORY.md and structured memory entries) for a query string.",
        "parameters": {
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "Case-insensitive search query."},
                "tag": {"type": "string", "description": "Optional tag filter."},
                "limit": {"type": "integer", "description": "Maximum matching entries to return (default 20)."},
                "intent": {"type": "string", "description": "Short reason for searching memory."},
            },
            "required": ["query"],
        },
    },
]

REMOTE_TRANSFER_FUNCTION_DECLARATIONS: List[Dict[str, Any]] = [
    {
        "type": "function",
        "name": "workspace_download_from_remote",
        "description": (
            "Stream-download a file directly from the remote VNC host (via FTP bridge) into the shared local workspace. "
            "Supports large and binary files cleanly so the Research Assistant or File Search API can inspect/index them."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "remote_path": {"type": "string", "description": "Source path on the remote VNC host."},
                "local_path": {"type": "string", "description": "Destination path inside the local workspace."},
                "overwrite": {"type": "boolean", "description": "If true (default), overwrite an existing local file."},
                "intent": {"type": "string", "description": "Short reason for downloading this file."},
            },
            "required": ["remote_path", "local_path"],
        },
    },
    {
        "type": "function",
        "name": "workspace_upload_to_remote",
        "description": (
            "Stream-upload a file from the shared local workspace directly to the remote VNC host (via FTP bridge). "
            "Supports large and binary files."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "local_path": {"type": "string", "description": "Source path inside the local workspace."},
                "remote_path": {"type": "string", "description": "Destination path on the remote VNC host."},
                "create_parents": {"type": "boolean", "description": "If true (default), create missing remote parent directories."},
                "intent": {"type": "string", "description": "Short reason for uploading this file."},
            },
            "required": ["local_path", "remote_path"],
        },
    },
]

COORDINATION_FUNCTION_DECLARATIONS: List[Dict[str, Any]] = [
    {
        "type": "function",
        "name": "peer_get_status",
        "description": (
            "Read the `.coord/STATUS.*.marker` and `.coord/WAKE.*.marker` files for both agents, "
            "plus unread inbox message counts."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "intent": {"type": "string", "description": "Short reason for checking peer status."},
            },
        },
    },
    {
        "type": "function",
        "name": "peer_set_status",
        "description": (
            "Update your own `.coord/STATUS.<agent>.marker` file so your peer knows what you are currently doing."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "state": {
                    "type": "string",
                    "description": "Status state: idle, working, waiting_peer, waiting_user, sleeping, paused, done, or error.",
                },
                "task": {"type": "string", "description": "Short summary of your current task."},
                "detail": {"type": "string", "description": "Optional detailed progress note."},
                "intent": {"type": "string", "description": "Short reason for updating your status marker."},
            },
            "required": ["state"],
        },
    },
    {
        "type": "function",
        "name": "peer_send_message",
        "description": (
            "Write a JSON message marker into the peer agent's `.coord/inbox/` directory and optionally write a "
            "`.coord/WAKE.<peer>.marker` to wake them up asynchronously."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "subject": {"type": "string", "description": "Short subject line for the message."},
                "body": {"type": "string", "description": "Full message instructions, findings, or question."},
                "kind": {
                    "type": "string",
                    "description": "Message kind: request, response, update, handoff, or alert (default 'request').",
                },
                "attachments": {
                    "type": "string",
                    "description": "Optional comma-separated workspace-relative file paths referenced by this message.",
                },
                "in_reply_to": {"type": "string", "description": "Optional message ID this message replies to."},
                "wake": {"type": "boolean", "description": "If true (default), create a WAKE marker to wake the peer."},
                "priority": {"type": "string", "description": "Priority: normal or urgent (default 'normal')."},
                "intent": {"type": "string", "description": "Short reason for messaging the peer."},
            },
            "required": ["subject", "body"],
        },
    },
    {
        "type": "function",
        "name": "peer_read_messages",
        "description": (
            "Read message markers from your `.coord/inbox/` (and optionally archived messages). "
            "Marking messages read moves them to `.coord/archive/` and consumes your WAKE marker."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "include_read": {"type": "boolean", "description": "If true, also include recently archived messages."},
                "mark_read": {"type": "boolean", "description": "If true (default), archive unread messages and consume WAKE."},
                "limit": {"type": "integer", "description": "Maximum messages to return (default 20)."},
                "intent": {"type": "string", "description": "Short reason for reading peer messages."},
            },
        },
    },
    {
        "type": "function",
        "name": "peer_wake",
        "description": "Write a `.coord/WAKE.<peer>.marker` to wake the peer agent up immediately.",
        "parameters": {
            "type": "object",
            "properties": {
                "reason": {"type": "string", "description": "Short reason for waking the peer."},
                "priority": {"type": "string", "description": "Priority: normal or urgent (default 'normal')."},
                "intent": {"type": "string", "description": "Short reason for sending wakeup signal."},
            },
            "required": ["reason"],
        },
    },
    {
        "type": "function",
        "name": "peer_wait_for_message",
        "description": (
            "Sleep until a `.coord/WAKE` or inbox message marker arrives from the peer agent, or until timeout. "
            "Use this only when you cannot proceed until the peer replies."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "timeout_seconds": {
                    "type": "integer",
                    "description": "Maximum seconds to wait (1-120, default 30).",
                },
                "intent": {"type": "string", "description": "Short description of what response you are waiting for."},
            },
        },
    },
]

ASSISTANT_FILE_SEARCH_DECLARATIONS: List[Dict[str, Any]] = [
    {
        "type": "function",
        "name": "file_search_create_store",
        "description": (
            "Create a Gemini File Search Store (`fileSearchStores`) and attach it as the active store "
            "for document indexing and RAG queries."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "display_name": {"type": "string", "description": "Human-readable name for the File Search store."},
                "embedding_model": {
                    "type": "string",
                    "description": "Optional embedding model (e.g. 'models/gemini-embedding-001' or 'models/gemini-embedding-2').",
                },
                "intent": {"type": "string", "description": "Short reason for creating this store."},
            },
            "required": ["display_name"],
        },
    },
    {
        "type": "function",
        "name": "file_search_list_stores",
        "description": "List Gemini File Search Stores accessible with the configured API key and show locally attached stores.",
        "parameters": {
            "type": "object",
            "properties": {
                "page_size": {"type": "integer", "description": "Page size (1-20, default 20)."},
                "page_token": {"type": "string", "description": "Use next_page_token from the previous page."},
                "intent": {"type": "string", "description": "Short reason for listing File Search stores."},
            },
        },
    },
    {
        "type": "function",
        "name": "file_search_attach_store",
        "description": "Attach an existing Gemini File Search Store resource name (e.g. 'fileSearchStores/abc') to this workspace.",
        "parameters": {
            "type": "object",
            "properties": {
                "store_name": {"type": "string", "description": "Store ID or 'fileSearchStores/<id>' resource name."},
                "intent": {"type": "string", "description": "Short reason for attaching this store."},
            },
            "required": ["store_name"],
        },
    },
    {
        "type": "function",
        "name": "file_search_delete_store",
        "description": "Delete a Gemini File Search Store and detach it from the workspace registry.",
        "parameters": {
            "type": "object",
            "properties": {
                "store_name": {"type": "string", "description": "Store resource name ('fileSearchStores/<id>')."},
                "force": {"type": "boolean", "description": "If true (default), delete contained documents/chunks too."},
                "intent": {"type": "string", "description": "Short reason for deleting this store."},
            },
            "required": ["store_name"],
        },
    },
    {
        "type": "function",
        "name": "file_search_upload_file",
        "description": (
            "Upload and index a file from the shared local workspace into a Gemini File Search Store using the "
            "resumable `uploadToFileSearchStore` API and wait for indexing completion. Auto-creates a store if none is attached."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "local_path": {"type": "string", "description": "Workspace-relative path of the local file to upload."},
                "store_name": {
                    "type": "string",
                    "description": "Optional target File Search store resource name (defaults to workspace active store).",
                },
                "display_name": {"type": "string", "description": "Optional document display name for citations."},
                "mime_type": {"type": "string", "description": "Optional explicit MIME type (auto-detected if omitted)."},
                "custom_metadata_json": {
                    "type": "string",
                    "description": "Optional JSON array string of customMetadata objects, e.g. '[{\"key\":\"topic\",\"stringValue\":\"vnc\"}]'.",
                },
                "max_tokens_per_chunk": {"type": "integer", "description": "Optional white_space_config maxTokensPerChunk."},
                "max_overlap_tokens": {"type": "integer", "description": "Optional white_space_config maxOverlapTokens."},
                "intent": {"type": "string", "description": "Short reason for uploading this document."},
            },
            "required": ["local_path"],
        },
    },
    {
        "type": "function",
        "name": "file_search_list_documents",
        "description": "List indexed documents inside a Gemini File Search Store.",
        "parameters": {
            "type": "object",
            "properties": {
                "store_name": {"type": "string", "description": "Optional store name (defaults to workspace active store)."},
                "page_size": {"type": "integer", "description": "Maximum documents per page (1-20, default 20)."},
                "page_token": {"type": "string", "description": "Use next_page_token from the previous page."},
                "intent": {"type": "string", "description": "Short reason for listing documents."},
            },
        },
    },
    {
        "type": "function",
        "name": "file_search_delete_document",
        "description": "Delete a document from a Gemini File Search Store.",
        "parameters": {
            "type": "object",
            "properties": {
                "document_name": {
                    "type": "string",
                    "description": "Full document resource name ('fileSearchStores/<store>/documents/<doc>').",
                },
                "force": {"type": "boolean", "description": "If true (default), also delete chunks."},
                "intent": {"type": "string", "description": "Short reason for deleting this document."},
            },
            "required": ["document_name"],
        },
    },
    {
        "type": "function",
        "name": "file_search_query",
        "description": (
            "Run a grounded Gemini `file_search` query against one or more attached File Search Stores "
            "and return the answer along with extracted file citations."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "Natural-language question to answer from indexed documents."},
                "store_names": {
                    "type": "string",
                    "description": "Optional comma-separated File Search store names (defaults to all attached workspace stores).",
                },
                "metadata_filter": {
                    "type": "string",
                    "description": "Optional File Search metadata filter expression, e.g. 'topic=\"vnc\"'.",
                },
                "intent": {"type": "string", "description": "Short reason for querying File Search."},
            },
            "required": ["query"],
        },
    },
    {
        "type": "function",
        "name": "web_search_query",
        "description": (
            "Run a live grounded Google Web Search query (`google_search` tool) and return the synthesized "
            "answer and web citations."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "Web search query or research question."},
                "intent": {"type": "string", "description": "Short reason for searching the web."},
            },
            "required": ["query"],
        },
    },
]

SYSTEM_ONE_FUNCTION_DECLARATIONS: List[Dict[str, Any]] = [
    {
        "type": "function",
        "name": "wait_until",
        "description": (
            "Host-side deterministic wait loop backed by TypeSafe System One (stateless single-frame visual oracle). "
            "Polls watch_roi with pixel-diff gating (skipping System One when delta < diff_gate_threshold with periodic "
            "heartbeat calibration) until condition passes or a circuit breaker trips. Supports require_stable_frames "
            "and ring-buffered capture_transient for fleeting toasts/spinners."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "condition": {
                    "type": "object",
                    "description": (
                        "Atomic single-frame predicate: {type: 'noul'|'choice', instructions: string, "
                        "criteria?: object (for choice; 'unreadable' is auto-appended), expected_value?: boolean, "
                        "target_choices?: string[], min_confidence?: number}."
                    ),
                },
                "watch_roi": {
                    "type": "array",
                    "items": {"type": "integer"},
                    "description": "Normalized BBox [ymin, xmin, ymax, xmax] in 0..1000 (default [0, 0, 1000, 1000]).",
                },
                "poll_interval_ms": {
                    "type": "integer",
                    "description": "Polling interval in ms (default 500; use 150-250 for transient toasts).",
                },
                "timeout_ms": {
                    "type": "integer",
                    "description": "Maximum wait duration in ms (default 30000, hard cap 180000).",
                },
                "diff_gate_threshold": {
                    "type": "number",
                    "description": "Minimum pixel-diff ratio in watch_roi to trigger System One (default 0.005).",
                },
                "heartbeat_every_n": {
                    "type": "integer",
                    "description": "Force a calibration System One check after this many gated skips (default 6).",
                },
                "require_stable_frames": {
                    "type": "integer",
                    "description": "Consecutive passing evaluations required before returning matched (default 1).",
                },
                "capture_transient": {
                    "type": "boolean",
                    "description": "If true, buffer recent frames and attach the transient match crop in visual_evidence.",
                },
                "locate_box": {
                    "type": "string",
                    "description": "Optional UI element description to locate with a System One box question on match.",
                },
                "risk_tier": {
                    "type": "string",
                    "description": "Risk tier: 'read_only' (default), 'reversible', or 'irreversible'.",
                },
                "settle_delay_ms": {
                    "type": "integer",
                    "description": "Settle delay before gray-zone 1x retry in ms (default 250).",
                },
                "interrupt_predicates": {
                    "type": "array",
                    "items": {"type": "object"},
                    "description": "Optional list of atomic interrupt/abort predicates checked on every inference.",
                },
                "ignore_scene_change": {
                    "type": "boolean",
                    "description": "If true, do not trip CB_INTERRUPT when full-screen SSIM drops below 0.50.",
                },
                "intent": {"type": "string", "description": "Short reason for waiting on this UI state."},
            },
            "required": ["condition"],
        },
    },
    {
        "type": "function",
        "name": "loop_until",
        "description": (
            "Host-side bounded search/convergence loop (e.g. long dropdowns, calendars, sliders) backed by "
            "TypeSafe System One. Runs closed-set HostAction step_action (or directional_actions) until "
            "stop_predicate matches, switching to fallback_action on the first progress_roi SSIM stall. "
            "Supports on_match closed-loop click_target_box/action unless risk_tier is irreversible."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "stop_predicate": {
                    "type": "object",
                    "description": (
                        "Atomic single-frame stop predicate: {type: 'noul'|'choice', instructions: string, "
                        "criteria?: object, expected_value?: boolean, target_choices?: string[], min_confidence?: number}."
                    ),
                },
                "step_action": {
                    "type": "object",
                    "description": (
                        "Closed-set HostAction primitive: {type: 'hover'|'click'|'wheel_scroll'|'key_press'|'drag_step', ...} "
                        "with normalized 0..1000 coordinates."
                    ),
                },
                "fallback_action": {
                    "type": "object",
                    "description": "Optional closed-set HostAction (e.g. key_press PageDown) switched to on first stall.",
                },
                "directional_actions": {
                    "type": "object",
                    "description": "Optional map of stop_predicate choice label -> closed-set HostAction.",
                },
                "roi": {
                    "type": "array",
                    "items": {"type": "integer"},
                    "description": "Target search BBox [ymin, xmin, ymax, xmax] in 0..1000 (default [0, 0, 1000, 1000]).",
                },
                "progress_roi": {
                    "type": "array",
                    "items": {"type": "integer"},
                    "description": "BBox monitored via SSIM for scroll/step progress (defaults to roi).",
                },
                "ssim_stall_threshold": {
                    "type": "number",
                    "description": "SSIM threshold at or above which consecutive frames count as stalled (default 0.992).",
                },
                "max_stalls": {
                    "type": "integer",
                    "description": "Max consecutive post-fallback stalls before tripping CB_STALL (default 2).",
                },
                "max_steps": {
                    "type": "integer",
                    "description": "Maximum step iterations before CB_BUDGET_EXCEEDED (default 15, hard cap 50).",
                },
                "settle_delay_ms": {
                    "type": "integer",
                    "description": "Delay in ms after each action before re-capturing (default 250).",
                },
                "on_match": {
                    "type": "object",
                    "description": (
                        "Optional post-match closed-loop execution: {mode: 'none'|'click_target_box'|'action', "
                        "target_description?: string, action?: HostAction, button?: 'left'|'middle'|'right', count?: integer}."
                    ),
                },
                "risk_tier": {
                    "type": "string",
                    "description": "Risk tier: 'read_only', 'reversible' (default when on_match acts), or 'irreversible'.",
                },
                "interrupt_predicates": {
                    "type": "array",
                    "items": {"type": "object"},
                    "description": "Optional list of atomic interrupt/abort predicates checked on every step.",
                },
                "ignore_scene_change": {
                    "type": "boolean",
                    "description": "If true, do not trip CB_INTERRUPT when full-screen SSIM drops below 0.50.",
                },
                "intent": {"type": "string", "description": "Short reason for running this search loop."},
            },
            "required": ["stop_predicate"],
        },
    },
    {
        "type": "function",
        "name": "batch_apply",
        "description": (
            "Single- or multi-page bulk visual evaluation and action tool backed by TypeSafe System One Parallel Questions. "
            "Locates items via fixed_slots or dynamic_box, evaluates all visible slots in one batched System One call, "
            "and runs noop_collect, click_target_box, or click_relative with max_pages (20), max_mutations, "
            "max_error_ratio (0.25), and post-click pixel-change drift guards."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "item_predicate": {
                    "type": "object",
                    "description": "Atomic single-frame predicate evaluated per visible slot.",
                },
                "item_action": {
                    "type": "string",
                    "description": "Action on matched slots: 'noop_collect', 'click_target_box', or 'click_relative'.",
                },
                "locate_mode": {
                    "type": "string",
                    "description": "Slot location mode: 'fixed_slots' or 'dynamic_box'.",
                },
                "slots": {
                    "type": "array",
                    "items": {"type": "object"},
                    "description": "For fixed_slots: list of {id?: string, roi: [ymin, xmin, ymax, xmax]} slot objects.",
                },
                "slot_grid": {
                    "type": "object",
                    "description": "For fixed_slots: {rows: integer, cols: integer} grid subdividing page_roi.",
                },
                "dynamic_box_prompts": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "For dynamic_box: descriptions of target item slots to locate via System One box.",
                },
                "page_roi": {
                    "type": "array",
                    "items": {"type": "integer"},
                    "description": "Normalized BBox [ymin, xmin, ymax, xmax] containing the batch items.",
                },
                "progress_roi": {
                    "type": "array",
                    "items": {"type": "integer"},
                    "description": "BBox monitored via SSIM when paginating between pages.",
                },
                "relative_offset": {
                    "type": "array",
                    "items": {"type": "number"},
                    "description": "For click_relative: [rel_y, rel_x] in 0..1 within each matched slot ROI (default [0.5, 0.5]).",
                },
                "target_description": {
                    "type": "string",
                    "description": "Description of the element to locate/confirm inside each matched slot.",
                },
                "next_page_action": {
                    "type": "object",
                    "description": "Optional closed-set HostAction to advance to the next page.",
                },
                "fallback_next_page_action": {
                    "type": "object",
                    "description": "Optional fallback closed-set HostAction on pagination stall.",
                },
                "max_pages": {
                    "type": "integer",
                    "description": "Maximum pages to process (default 1, hard cap 20).",
                },
                "max_mutations": {
                    "type": "integer",
                    "description": "Maximum mutating clicks across the batch before CB_BUDGET_EXCEEDED.",
                },
                "max_error_ratio": {
                    "type": "number",
                    "description": "Maximum per-page failure/gray ratio before CB_BATCH_DRIFT (default/cap 0.25).",
                },
                "verify_after_action": {
                    "type": "boolean",
                    "description": "If true (default), verify pixel change after each click and trip CB_BATCH_DRIFT on 2 consecutive zero-delta clicks.",
                },
                "risk_tier": {
                    "type": "string",
                    "description": "Risk tier: 'read_only', 'reversible', or 'irreversible'.",
                },
                "settle_delay_ms": {
                    "type": "integer",
                    "description": "Delay in ms after clicks/pagination (default 250).",
                },
                "interrupt_predicates": {
                    "type": "array",
                    "items": {"type": "object"},
                    "description": "Optional list of atomic interrupt/abort predicates checked per page.",
                },
                "ignore_scene_change": {
                    "type": "boolean",
                    "description": "If true, do not trip CB_INTERRUPT when full-screen SSIM drops below 0.50.",
                },
                "intent": {"type": "string", "description": "Short reason for running this batch evaluation/action."},
            },
            "required": ["item_predicate", "item_action", "locate_mode"],
        },
    },
]

CUSTOM_FUNCTION_NAMES = frozenset(d["name"] for d in CUSTOM_FUNCTION_DECLARATIONS)
FILE_READ_ONLY_FUNCTIONS = frozenset({"file_list", "file_stat", "file_read"})
FILE_MUTATING_FUNCTIONS = frozenset({"file_write", "file_mkdir", "file_delete", "file_rename"})
FILE_FUNCTION_NAMES = FILE_READ_ONLY_FUNCTIONS | FILE_MUTATING_FUNCTIONS

WORKSPACE_READ_ONLY_FUNCTIONS = frozenset(
    {"workspace_list", "workspace_stat", "workspace_read", "workspace_search", "memory_list", "memory_read", "memory_search"}
)
WORKSPACE_MUTATING_FUNCTIONS = frozenset(
    {"workspace_write", "workspace_mkdir", "workspace_delete", "workspace_rename", "memory_write", "memory_delete"}
)
WORKSPACE_FUNCTION_NAMES = WORKSPACE_READ_ONLY_FUNCTIONS | WORKSPACE_MUTATING_FUNCTIONS

REMOTE_TRANSFER_READ_ONLY_FUNCTIONS = frozenset({"workspace_download_from_remote"})
REMOTE_TRANSFER_MUTATING_FUNCTIONS = frozenset({"workspace_upload_to_remote"})
REMOTE_TRANSFER_FUNCTION_NAMES = REMOTE_TRANSFER_READ_ONLY_FUNCTIONS | REMOTE_TRANSFER_MUTATING_FUNCTIONS

COORDINATION_FUNCTION_NAMES = frozenset(d["name"] for d in COORDINATION_FUNCTION_DECLARATIONS)
ASSISTANT_FILE_SEARCH_FUNCTION_NAMES = frozenset(d["name"] for d in ASSISTANT_FILE_SEARCH_DECLARATIONS)
SYSTEM_ONE_FUNCTION_NAMES = frozenset(d["name"] for d in SYSTEM_ONE_FUNCTION_DECLARATIONS)

REMOTE_MUTATING_FUNCTIONS = FILE_MUTATING_FUNCTIONS | REMOTE_TRANSFER_MUTATING_FUNCTIONS
ALL_CUSTOM_FUNCTION_NAMES = (
    CUSTOM_FUNCTION_NAMES
    | FILE_FUNCTION_NAMES
    | WORKSPACE_FUNCTION_NAMES
    | REMOTE_TRANSFER_FUNCTION_NAMES
    | COORDINATION_FUNCTION_NAMES
    | SYSTEM_ONE_FUNCTION_NAMES
)
READ_ONLY_FUNCTIONS = (
    frozenset(
        {
            "take_screenshot",
            "clipboard_get",
            "clipboard_verify_selection",
            "get_desktop_status",
            "wait",
            "wait_until",
            "yield_to_user",
        }
    )
    | FILE_READ_ONLY_FUNCTIONS
    | WORKSPACE_FUNCTION_NAMES
    | REMOTE_TRANSFER_READ_ONLY_FUNCTIONS
    | COORDINATION_FUNCTION_NAMES
)

SYSTEM_INSTRUCTION = """## RULE 1: Seek User Confirmation (USER_CONFIRMATION)

This is your first and most important check. If the next required action falls
into any of the following categories, you MUST stop immediately, and seek the
user's explicit permission.

**Procedure for Seeking Confirmation:**
* **For Consequential Actions:** Perform all preparatory steps (e.g., navigating,
  filling out forms, typing a message). You will ask for confirmation **AFTER**
  all necessary information is entered on the screen, but **BEFORE** you perform
  the final, irreversible action (e.g., before clicking "Send", "Submit",
  "Confirm Purchase", "Share").
* **For Prohibited Actions:** If the action is strictly forbidden (e.g., accepting
  legal terms, solving a CAPTCHA), you must first inform the user about the
  required action and ask for their confirmation to proceed.

**USER_CONFIRMATION Categories:**
* **Consent and Agreements:** Terms of Service, Privacy Policies, EULAs, or legally binding agreements.
* **Robot Detection:** CAPTCHAs or anti-robot checks.
* **Financial Transactions:** Purchases, money transfers, or payments.
* **Sending Communications:** Sending emails, messages, or social media posts.
* **Accessing or Modifying Sensitive Information:** Health, financial, or government records.
* **Security and Identity:** Logging into user accounts or impersonation.
* **Insurmountable Obstacles:** If you are technically unable to interact with a UI element or stuck in a loop you cannot resolve, use `yield_to_user` (in collaborative mode) or ask the user to take over.

---

## RULE 2: Default Behavior (ACTUATE)

If an action does **NOT** fall under the conditions for `USER_CONFIRMATION`,
your default behavior is to **Actuate**.

**Actuation Means:** You MUST proactively perform all necessary steps to move
the user's request forward. Continue to actuate until you either complete the
non-consequential task or encounter a condition defined in Rule 1.

## VNC Desktop Execution & Optimization Guidelines:
* **Prefer Custom Deterministic Macros:** When available, prefer `paste_text_at`, `click_and_type`, `clipboard_get`, `clipboard_set`, and `get_desktop_status` over multi-step click+type sequences; they execute deterministically and return compact text-only status results without extra round-trips.
* **Parallel Batching & Intent:** Always provide a clear `intent` argument on actions. Only batch independent actions in a single turn when their order is deterministic and does not depend on intermediate visual layout changes.
* **Visual Verification Before Form Commit/Close:** Do NOT batch form text/number entry (`click_and_type`, `paste_text_at`, `type`) with form-closing or commit buttons (`OK`, `Save`, `保存`, `作成完了`, `Confirm`, `Submit`) in the same turn. Enter the values first, visually verify exact cell/column placement on the resulting screenshot, and commit/close on the following turn.
* **Verify exact pasted text before saving:** Use `paste_text_at` or `clipboard_set` for non-ASCII or multiline text. A successful paste action means Ctrl+V was sent. Inspect the screenshot and compare the pasted characters and spaces with the requested text. For copyable text that must match exactly, select it and use `clipboard_verify_selection` before saving. If verification shows a mismatch, correct the text and verify again. If you cannot obtain the requested text, stop before saving or submitting and report the mismatch.
* **Verification:** When creating or saving a file on the Desktop, make sure the file is saved and verify on the screenshot that it exists on the Desktop before finishing.

# Final Response Guidelines:
Write final response to the user in the following cases:
- User confirmation
- When the task is complete or you have enough information to respond to the user
"""

FTP_SYSTEM_INSTRUCTION = """## Direct Remote File I/O Tools (RFB-to-FTP Bridge):
* **Prefer Direct File Tools:** When reading, writing, creating, inspecting, renaming, or deleting remote files/directories, prefer `file_list`, `file_stat`, `file_read`, `file_write`, `file_mkdir`, `file_delete`, and `file_rename` directly instead of opening GUI editors, file managers, or terminals.
* **Path Format:** Use `/` separators (e.g. `/tmp/notes.txt` or `/C:/Users/Alice/Desktop/hello.txt`). Windows paths such as `C:\\Users\\Alice\\Desktop\\hello.txt` are also normalized automatically.
* **Chunked & Binary I/O:** For large files, pass `offset` and `max_bytes` to `file_read`; for binary files, use `as_base64=true` on `file_read` and `content_base64` on `file_write`. Pass `append=true` or `create_parents=true` to `file_write` when appropriate.
* **Visual Verification When Requested:** Direct file tools return compact JSON without capturing a screenshot; if the task requires confirming a file is visually displayed on the desktop/UI, call `take_screenshot` afterward.
"""

WORKSPACE_SYSTEM_INSTRUCTION = """## Shared Local Workspace, Persistent Memory & Remote Transfer:
* **Local Workspace Files:** Use `workspace_list`, `workspace_stat`, `workspace_read`, `workspace_write`, `workspace_mkdir`, `workspace_delete`, `workspace_rename`, and `workspace_search` to stage artifacts, reports, scripts, or downloaded documents inside the local workspace.
* **Persistent Memory:** Use `memory_read`, `memory_write`, `memory_list`, `memory_search`, and `memory_delete` to store and retrieve durable facts across turns and tasks (saved in `memory/MEMORY.md` and `memory/entries.json`).
* **Remote Host <-> Local Workspace Transfer:** When the FTP bridge is active, use `workspace_download_from_remote` to stream files from the remote VNC host into the local workspace (e.g., so the Research Assistant can index/search them) and `workspace_upload_to_remote` to push local workspace artifacts onto the remote VNC host.
"""

COORDINATION_SYSTEM_INSTRUCTION = """## Asynchronous Research Assistant & Marker Coordination (`.coord/`):
* **Specialist Peer (`assistant`):** You have an asynchronous Research Assistant running in the background with access to live Web Search (`google_search`) and the Gemini File Search API (`fileSearchStores` / `file_search`), sharing your local workspace and persistent memory.
* **Marker Protocol:** Coordination uses `.coord/STATUS.<agent>.marker`, `.coord/WAKE.<agent>.marker`, and `.coord/inbox/<agent>/MSG_*.marker.json`.
* **Delegating Asynchronously:** Use `peer_send_message(..., wake=true)` to ask the assistant to search the web, create/manage File Search stores, index files you downloaded into the workspace, or synthesize research. You can continue working on the remote desktop while the assistant works in parallel.
* **Iterative Consultation:** Treat `assistant` as an ongoing specialist partner across the entire task, not a one-shot query. Whenever you encounter ambiguous domain rules, multi-document reconciliations (e.g., Balance Sheet vs. Profit & Loss discrepancies), uncertain column/row placements, or unindexed PDFs/binary documents, send a follow-up `peer_send_message(..., wake=true)` request for verification before committing.
* **Checking & Waiting:** Use `peer_get_status` to inspect the assistant's status marker (`sleeping`, `working`, `done`, `error`), `peer_read_messages` when notified of unread messages, or `peer_wait_for_message` when you need to sleep until the assistant wakes you up with a reply.
"""

SYSTEM_ONE_SYSTEM_INSTRUCTION = """## Host-Side System One Tools (`wait_until`, `loop_until`, `batch_apply`):
* **Strict Decoupling:** Emit declarative tool parameters only; the host executes a deterministic 6-state FSM (`S0_INIT_VALIDATE`..`S5_HANDOFF_TO_S2`) and calls TypeSafe System One (`noul`, `choice`, `box`) as a stateless single-frame visual oracle.
* **When to Use:**
  - `wait_until`: Wait for spinners, progress bars, page loads, or fleeting toasts (`poll_interval_ms=150..250`, `capture_transient=true`) without burning System 2 turns.
  - `loop_until`: Bounded scrolling/stepping through long dropdowns, calendars, lists, or sliders (`step_action`, `fallback_action` on first stall, `on_match` closed-loop click unless `irreversible`).
  - `batch_apply`: Evaluate or act on multiple visible items/checkboxes/rows on one or more pages in one batched Parallel Questions call per page.
* **Normalized Coordinates & Closed-Set `HostAction`:** All `BBox` values are `[ymin, xmin, ymax, xmax]` in `0..1000`. Actions in `step_action`, `fallback_action`, `directional_actions`, `next_page_action`, and `on_match.action` MUST use closed-set primitives: `hover`, `click`, `wheel_scroll`, `key_press`, or `drag_step`.
* **Risk Tiers & Circuit Breakers:** Choose `read_only`, `reversible`, or `irreversible` accurately. Never expect host auto-clicks on `irreversible` targets (`CB_SAFETY_GUARD` blocks input and returns the annotated candidate box for your review). On `CB_AMBIGUITY`, `CB_STALL`, `CB_INTERRUPT`, `CB_SAFETY_GUARD`, `CB_BUDGET_EXCEEDED`, or `CB_BATCH_DRIFT`, inspect the returned `HostToolResult` and `visual_evidence` (max 2 images) before deciding the next step.
"""

ASSISTANT_SYSTEM_INSTRUCTION = """You are the asynchronous **Research Assistant** collaborating with the **Computer Use Agent** (`computer_use`) via a shared local workspace and `.coord/` coordination markers.

## Capabilities & Responsibilities:
1. **Live Web Search:** Actively verify factual, regulatory, and locale-specific details via live Google Web Search (`google_search` when enabled, or `web_search_query`) and include explicit source citations rather than relying solely on unverified assumptions.
2. **Gemini File Search API:** Proactively use `file_search_create_store`, `file_search_list_stores`, `file_search_attach_store`, `file_search_delete_store`, `file_search_upload_file`, `file_search_list_documents`, `file_search_delete_document`, and `file_search_query` (plus built-in `file_search` when stores are attached) to index raw PDFs or multi-file documents in the local workspace and answer grounded RAG queries with citations.
3. **Shared Local Workspace & Memory:** Read files staged in the local workspace (including files downloaded from the remote host by `computer_use`), write structured research notes or prepared outputs into the workspace, and update shared persistent memory (`memory_write` / `memory_read`).
4. **Clear Accounting & Field Guidance:** When advising on forms or reconciliations, explicitly highlight exact column/row placements (e.g., retained vs. outflow columns) and any discrepancies across source statements so `computer_use` can verify them visually.
5. **Asynchronous Marker Coordination:**
   - Update your status with `peer_set_status(state='working', ...)` when tackling multi-step research if helpful.
   - Always send your final findings back to `computer_use` via `peer_send_message(kind='response', wake=true, attachments=...)`, citing sources/documents and naming any workspace files you created.
   - Do NOT attempt to interact with the VNC GUI directly; ask `computer_use` if remote desktop actions or remote host file transfers are needed.
"""

KEY_MAP: Dict[str, int] = {
    "backspace": 0xFF08,
    "tab": 0xFF09,
    "return": 0xFF0D,
    "enter": 0xFF0D,
    "pause": 0xFF13,
    "scroll_lock": 0xFF14,
    "scrolllock": 0xFF14,
    "sys_req": 0xFF15,
    "escape": 0xFF1B,
    "esc": 0xFF1B,
    "delete": 0xFFFF,
    "del": 0xFFFF,
    "home": 0xFF50,
    "left": 0xFF51,
    "arrowleft": 0xFF51,
    "up": 0xFF52,
    "arrowup": 0xFF52,
    "right": 0xFF53,
    "arrowright": 0xFF53,
    "down": 0xFF54,
    "arrowdown": 0xFF54,
    "page_up": 0xFF55,
    "pageup": 0xFF55,
    "pgup": 0xFF55,
    "prior": 0xFF55,
    "page_down": 0xFF56,
    "pagedown": 0xFF56,
    "pgdn": 0xFF56,
    "next": 0xFF56,
    "end": 0xFF57,
    "print": 0xFF61,
    "printscreen": 0xFF61,
    "insert": 0xFF63,
    "ins": 0xFF63,
    "menu": 0xFF67,
    "apps": 0xFF67,
    "num_lock": 0xFF7F,
    "numlock": 0xFF7F,
    "f1": 0xFFBE,
    "f2": 0xFFBF,
    "f3": 0xFFC0,
    "f4": 0xFFC1,
    "f5": 0xFFC2,
    "f6": 0xFFC3,
    "f7": 0xFFC4,
    "f8": 0xFFC5,
    "f9": 0xFFC6,
    "f10": 0xFFC7,
    "f11": 0xFFC8,
    "f12": 0xFFC9,
    "shift": 0xFFE1,
    "shift_l": 0xFFE1,
    "lshift": 0xFFE1,
    "shift_r": 0xFFE2,
    "rshift": 0xFFE2,
    "control": 0xFFE3,
    "control_l": 0xFFE3,
    "ctrl": 0xFFE3,
    "lctrl": 0xFFE3,
    "control_r": 0xFFE4,
    "rctrl": 0xFFE4,
    "caps_lock": 0xFFE5,
    "capslock": 0xFFE5,
    "meta": 0xFFE7,
    "meta_l": 0xFFE7,
    "alt": 0xFFE9,
    "alt_l": 0xFFE9,
    "lalt": 0xFFE9,
    "alt_r": 0xFFEA,
    "ralt": 0xFFEA,
    "altgr": 0xFE03,
    "super": 0xFFEB,
    "super_l": 0xFFEB,
    "win": 0xFFEB,
    "windows": 0xFFEB,
    "cmd": 0xFFEB,
    "command": 0xFFEB,
    "super_r": 0xFFEC,
    "space": 0x0020,
    "spacebar": 0x0020,
    "plus": 0x002B,
    "minus": 0x002D,
    "equal": 0x003D,
    "comma": 0x002C,
    "period": 0x002E,
    "slash": 0x002F,
    "backslash": 0x005C,
    "semicolon": 0x003B,
    "apostrophe": 0x0027,
    "grave": 0x0060,
    "bracketleft": 0x005B,
    "bracketright": 0x005D,
}

SHIFT_KEYSYMS = {0xFFE1, 0xFFE2}
MODIFIER_KEYSYMS = {0xFFE1, 0xFFE2, 0xFFE3, 0xFFE4, 0xFFE7, 0xFFE9, 0xFFEA, 0xFFEB, 0xFFEC}
SHIFTED_ASCII = set('ABCDEFGHIJKLMNOPQRSTUVWXYZ~!@#$%^&*()_+{}|:"<>?')
