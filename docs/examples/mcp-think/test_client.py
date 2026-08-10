#!/usr/bin/env python3
"""Smoke-test mcp-think over stdio, exactly the way Claude drives it.

Launches server.py as a subprocess (via `uv run --with mcp`), performs the
MCP lifecycle (initialize → initialized), lists the tools, and calls
`think` once. Requires Ollama running with the configured model pulled.

Usage:  python3 test_client.py            # standard library only
        THINK_MODEL=qwen3:0.6b python3 test_client.py
"""

import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main() -> int:
    proc = subprocess.Popen(
        ["uv", "run", "--with", "mcp", "python3", os.path.join(HERE, "server.py")],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        env=os.environ.copy(), text=True,
    )
    stdin, stdout = proc.stdin, proc.stdout
    assert stdin is not None and stdout is not None

    msg_id = 0

    def send(method: str, params: dict | None = None, notify: bool = False) -> dict:
        nonlocal msg_id
        msg: dict = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        if not notify:
            msg_id += 1
            msg["id"] = msg_id
        stdin.write(json.dumps(msg) + "\n")
        stdin.flush()
        if notify:
            return {}
        # stdio transport: one JSON-RPC message per line
        line = stdout.readline()
        if not line:
            raise RuntimeError("server closed the pipe")
        return json.loads(line)

    try:
        r = send("initialize", {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "mcp-think-smoke", "version": "0"},
        })
        server = r["result"]["serverInfo"]
        print(f"initialize ok — server: {server['name']}")

        send("notifications/initialized", {}, notify=True)

        r = send("tools/list", {})
        tools = [t["name"] for t in r["result"]["tools"]]
        print(f"tools/list  ok — {tools}")
        assert {"think", "critique", "brainstorm", "summarize"} <= set(tools)

        print("tools/call  think(...) — waiting on the local model…")
        r = send("tools/call", {
            "name": "think",
            "arguments": {"task": "In one sentence: why do write-ahead logs "
                                  "fsync before acknowledging a commit?"},
        })
        content = r["result"]["content"][0]["text"]
        print(f"\n--- local model says ---\n{content}\n")
        if content.startswith("[mcp-think]"):
            print("NOT OK — the server answered, but the local model is unavailable.")
            return 1
        print("OK — MCP lifecycle, tool discovery, and local-model call all work.")
        return 0
    finally:
        stdin.close()
        proc.terminate()
        proc.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
