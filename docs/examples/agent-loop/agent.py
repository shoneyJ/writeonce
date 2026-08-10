#!/usr/bin/env python3
"""agent-loop — a complete agent in one file, on a local model.

The whole trick behind Claude Code, opencode, Cursor and every other
"agentic" tool is one loop:

    send the transcript + tool schemas to the model
    while the model answers with tool calls:
        run the tools, append the results to the transcript
        send again
    print the final text

Everything else those tools add (permissions, context management,
sub-agents) is elaboration on that loop. This file IS the loop, small
enough to read in one sitting: an OpenAI-compatible chat endpoint
(llama-server from prototypes/llama-moe-stream, or Ollama) + three
read-only repo tools + the guards that keep the loop alive when the
model misbehaves.

Run (one-shot):     python3 agent.py "what does crates/rt/src/shard.rs do?"
Run (interactive):  python3 agent.py

Configuration (environment):
  LLM_URL      OpenAI-compatible base URL     (default http://127.0.0.1:8080/v1)
  LLM_MODEL    model name / alias             (default qwen3-coder)
  LLM_TIMEOUT  per-request timeout in seconds (default 300)
  AGENT_ROOT   directory the tools may touch  (default: current directory)
  MAX_TURNS    tool rounds per user message   (default 20)

Dependencies: Python standard library only.
"""

import json
import os
import re
import sys
import urllib.error
import urllib.request

LLM_URL     = os.environ.get("LLM_URL", "http://127.0.0.1:8080/v1").rstrip("/")
LLM_MODEL   = os.environ.get("LLM_MODEL", "qwen3-coder")
LLM_TIMEOUT = int(os.environ.get("LLM_TIMEOUT", "300"))
ROOT        = os.path.realpath(os.environ.get("AGENT_ROOT", os.getcwd()))
MAX_TURNS   = int(os.environ.get("MAX_TURNS", "20"))
MAX_RESULT  = 8_000          # chars of tool output fed back per call
SKIP_DIRS   = {".git", "target", "node_modules", "build", "__pycache__", ".cache"}


# ---------------------------------------------------------------- the tools
# Schemas are the contract shown to the model; implementations are the
# security boundary. Read-only on purpose — an agent is exactly as dangerous
# as its tools, never more.

TOOLS = [
    {"type": "function", "function": {
        "name": "list_dir",
        "description": "List one directory: entries with a trailing / for "
                       "subdirectories and a byte size for files.",
        "parameters": {"type": "object", "properties": {
            "path": {"type": "string",
                     "description": "directory, relative to the project root"},
        }, "required": ["path"]}}},
    {"type": "function", "function": {
        "name": "read_file",
        "description": "Read a text file with line numbers. Large files are "
                       "windowed — pass offset (1-based first line) and limit "
                       "(max lines) to page through.",
        "parameters": {"type": "object", "properties": {
            "path":   {"type": "string", "description": "file, relative to the project root"},
            "offset": {"type": "integer", "description": "first line to show, 1-based (default 1)"},
            "limit":  {"type": "integer", "description": "max lines to show (default 200)"},
        }, "required": ["path"]}}},
    {"type": "function", "function": {
        "name": "search",
        "description": "Search file contents under a directory with a Python "
                       "regular expression. Returns path:line: text matches. "
                       "Use a specific pattern — results cap at 100 matches.",
        "parameters": {"type": "object", "properties": {
            "pattern": {"type": "string", "description": "Python regex to find"},
            "path":    {"type": "string", "description": "directory to search, relative to the project root (default: whole root)"},
        }, "required": ["pattern"]}}},
]


class ToolError(Exception):
    """A tool refusing to do something — reported to the model, never fatal."""


def _resolve(path: str) -> str:
    """Confine every path the model asks for to ROOT (symlinks resolved)."""
    full = os.path.realpath(os.path.join(ROOT, path))
    if full != ROOT and not full.startswith(ROOT + os.sep):
        raise ToolError(f"path escapes the project root: {path}")
    return full


def list_dir(path: str = ".") -> str:
    full = _resolve(path)
    if not os.path.isdir(full):
        raise ToolError(f"not a directory: {path}")
    rows = []
    for name in sorted(os.listdir(full)):
        p = os.path.join(full, name)
        rows.append(f"{name}/" if os.path.isdir(p)
                    else f"{name}  ({os.path.getsize(p)} bytes)")
    return "\n".join(rows) or "(empty directory)"


def read_file(path: str, offset: int = 1, limit: int = 200) -> str:
    full = _resolve(path)
    if os.path.isdir(full):
        raise ToolError(f"{path} is a directory — use list_dir")
    try:
        with open(full, errors="replace") as f:
            lines = f.readlines()
    except FileNotFoundError:
        raise ToolError(f"no such file: {path}")
    if not lines:
        return "(empty file)"
    offset = max(1, int(offset))
    limit = max(1, min(int(limit), 1000))
    window = lines[offset - 1: offset - 1 + limit]
    if not window:
        raise ToolError(f"{path} has only {len(lines)} lines; offset {offset} is past the end")
    out = "".join(f"{i}\t{line}" for i, line in enumerate(window, offset))
    last = offset + len(window) - 1
    if last < len(lines):
        out += (f"\n[agent] showing lines {offset}-{last} of {len(lines)} — "
                f"call again with offset={last + 1} for more")
    return out


def search(pattern: str, path: str = ".") -> str:
    try:
        rx = re.compile(pattern)
    except re.error as e:
        raise ToolError(f"bad regex {pattern!r}: {e}")
    start = _resolve(path)
    hits: list[str] = []
    for dirpath, dirnames, filenames in os.walk(start):   # never follows symlinks
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
        for fname in sorted(filenames):
            full = os.path.join(dirpath, fname)
            if os.path.islink(full) or os.path.getsize(full) > 2_000_000:
                continue
            try:
                with open(full, errors="replace") as f:
                    head = f.read(1024)
                    if "\0" in head:            # binary — skip
                        continue
                    f.seek(0)
                    for i, line in enumerate(f, 1):
                        if rx.search(line):
                            rel = os.path.relpath(full, ROOT)
                            hits.append(f"{rel}:{i}: {line.rstrip()[:200]}")
                            if len(hits) >= 100:
                                hits.append("[agent] 100-match cap hit — tighten the pattern or narrow the path")
                                return "\n".join(hits)
            except OSError:
                continue
    return "\n".join(hits) or f"no matches for {pattern!r} under {path}"


TOOL_IMPLS = {"list_dir": list_dir, "read_file": read_file, "search": search}


# ---------------------------------------------------------- executing calls
# The one rule that keeps the loop alive: NEVER raise at the model. Whatever
# goes wrong — unknown tool, broken JSON, missing file — comes back as the
# tool result, in words. A tool-calling model reads the error and corrects
# itself on the next round; an exception would just kill the conversation.

def execute(call: dict) -> str:
    fn_block = call.get("function") or {}
    name = fn_block.get("name", "")
    impl = TOOL_IMPLS.get(name)
    if impl is None:
        return f"[agent] unknown tool {name!r} — available: {', '.join(TOOL_IMPLS)}"
    try:
        args = json.loads(fn_block.get("arguments") or "{}")
    except json.JSONDecodeError as e:
        return f"[agent] arguments were not valid JSON ({e}) — resend the call with corrected JSON"
    if not isinstance(args, dict):
        return "[agent] arguments must be a JSON object"
    try:
        out = impl(**args)
    except ToolError as e:
        return f"[agent] {e}"
    except TypeError as e:
        return f"[agent] bad arguments for {name}: {e}"
    except OSError as e:
        return f"[agent] {name} failed: {e}"
    if len(out) > MAX_RESULT:
        out = (out[:MAX_RESULT] + f"\n[agent] truncated — {len(out)} chars total; "
               "narrow the request (offset/limit, tighter pattern)")
    return out


# ------------------------------------------------------------------ the loop

def chat(messages: list[dict]) -> dict:
    """One request to the model. Returns the assistant message verbatim."""
    body = json.dumps({
        "model": LLM_MODEL,
        "messages": messages,
        "tools": TOOLS,
        "tool_choice": "auto",
    }).encode()
    req = urllib.request.Request(
        f"{LLM_URL}/chat/completions",
        data=body,
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=LLM_TIMEOUT) as resp:
            data = json.load(resp)
    except urllib.error.HTTPError as e:
        detail = e.read().decode(errors="replace")[:400]
        raise SystemExit(f"the model endpoint rejected the request (HTTP {e.code}): {detail}")
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        raise SystemExit(
            f"cannot reach the model at {LLM_URL} ({e}).\n"
            "Start one first:\n"
            "  prototypes/llama-moe-stream/start-local-agents.sh        # llama-server on :8080\n"
            "  LLM_URL=http://localhost:11434/v1 LLM_MODEL=qwen3:4b …   # or a pulled Ollama model")
    return data["choices"][0]["message"]


def run_turn(messages: list[dict]) -> str:
    """THE AGENT LOOP. Everything above exists to serve these few lines."""
    for _ in range(MAX_TURNS):
        msg = chat(messages)
        messages.append(msg)                      # the transcript is the only state
        calls = msg.get("tool_calls") or []
        if not calls:                             # no tool call = the model is done
            return _strip_think(msg.get("content") or "")
        for call in calls:
            fn = call.get("function") or {}
            print(f"  ⚙ {fn.get('name', '?')}({(fn.get('arguments') or '')[:120]})",
                  file=sys.stderr)
            messages.append({
                "role": "tool",
                "tool_call_id": call.get("id", ""),
                "content": execute(call),
            })
    return (f"[agent] stopped after {MAX_TURNS} tool rounds without a final answer — "
            "ask a narrower question or raise MAX_TURNS")


def _strip_think(text: str) -> str:
    # Reasoning models may inline chain-of-thought as <think>…</think>;
    # the user wants the conclusion, not the scratchpad.
    return re.sub(r"<think>.*?</think>", "", text, flags=re.DOTALL).strip()


# ----------------------------------------------------------------- the shell

SYSTEM = (f"You are a code assistant working inside the project rooted at {ROOT}. "
          "Use the tools to look at real files before answering; never invent "
          "file contents or paths. When you have enough evidence, answer "
          "concisely and cite locations as path:line.")


def main() -> None:
    messages: list[dict] = [{"role": "system", "content": SYSTEM}]
    if len(sys.argv) > 1:                                   # one-shot
        messages.append({"role": "user", "content": " ".join(sys.argv[1:])})
        print(run_turn(messages))
        return
    print(f"agent-loop — model {LLM_MODEL} at {LLM_URL}\n"
          f"root {ROOT}  (Ctrl-D to exit; the conversation persists across questions)")
    while True:                                             # interactive
        try:
            line = input("\nyou> ").strip()
        except EOFError:
            print()
            return
        if not line:
            continue
        messages.append({"role": "user", "content": line})
        print(run_turn(messages))


if __name__ == "__main__":
    main()
