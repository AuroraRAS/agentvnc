#!/usr/bin/env python3
"""Gemini Computer Use agent over TigerVNC's -AgentSocket IPC bridge.

Thin CLI entrypoint and backwards-compatible re-export facade for the modular
``vnc_agent`` package.

Implements official Gemini Computer Use, Interactions, and File Search APIs:
  https://aistudio.google.com/docs/computer-use
  https://aistudio.google.com/docs/interactions-overview
  https://aistudio.google.com/docs/file-search
  https://aistudio.google.com/docs/tools
"""

import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

from test_agent_bridge import AgentClient  # noqa: E402, F401
from vnc_agent import *  # noqa: E402, F401, F403
from vnc_agent import main  # noqa: E402


if __name__ == "__main__":
    sys.exit(main())
