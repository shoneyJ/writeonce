#!/usr/bin/env python3
"""Smoke-test the agent loop without any model.

Stands up a canned OpenAI-compatible /chat/completions endpoint on a local
port and drives agent.run_turn() through a scripted conversation:

  round 1: the "model" calls read_file on notes.txt   -> executed for real
  round 2: it sends a tool call with broken JSON args -> fed back as an error, not a crash
  round 3: it returns a final answer

This verifies the three load-bearing behaviours of the loop: tool execution
with result feedback, errors-as-results self-correction, and termination on
a plain (tool-call-free) answer.

Usage:  python3 test_loop.py     # standard library only, no model needed
"""

import json
import os
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
MARKER = "the WAL fsyncs before acknowledging the commit"

# What the fake model answers, round by round.
SCRIPT = [
    {"role": "assistant", "content": None, "tool_calls": [
        {"id": "call_1", "type": "function", "function": {
            "name": "read_file",
            "arguments": json.dumps({"path": "notes.txt"})}}]},
    {"role": "assistant", "content": None, "tool_calls": [
        {"id": "call_2", "type": "function", "function": {
            "name": "search",
            "arguments": '{"pattern": '}}]},          # deliberately broken JSON
    {"role": "assistant",
     "content": f"FINAL: per notes.txt, {MARKER}."},
]

REQUESTS: list[dict] = []


class MockModel(BaseHTTPRequestHandler):
    def do_POST(self):
        REQUESTS.append(json.loads(self.rfile.read(int(self.headers["Content-Length"]))))
        body = json.dumps({"choices": [{"message": SCRIPT[len(REQUESTS) - 1]}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        pass


def main() -> int:
    server = ThreadingHTTPServer(("127.0.0.1", 0), MockModel)
    threading.Thread(target=server.serve_forever, daemon=True).start()

    workdir = tempfile.mkdtemp(prefix="agent-loop-test-")
    with open(os.path.join(workdir, "notes.txt"), "w") as f:
        f.write(f"Durability rule: {MARKER}.\n")

    # agent.py reads its configuration at import time — set env first.
    os.environ["LLM_URL"] = f"http://127.0.0.1:{server.server_address[1]}/v1"
    os.environ["LLM_MODEL"] = "mock"
    os.environ["AGENT_ROOT"] = workdir
    sys.path.insert(0, HERE)
    import agent

    answer = agent.run_turn([
        {"role": "system", "content": "test"},
        {"role": "user", "content": "what is the durability rule?"},
    ])
    server.shutdown()

    def tool_results(request: dict) -> list[str]:
        return [m["content"] for m in request["messages"] if m.get("role") == "tool"]

    assert len(REQUESTS) == 3, f"expected 3 model rounds, got {len(REQUESTS)}"

    # Round 2's request must carry the real file content back as a tool result.
    round2 = tool_results(REQUESTS[1])
    assert any(MARKER in r for r in round2), f"file content not fed back: {round2}"
    print("ok — tool call executed, result appended to the transcript")

    # Round 3's request must carry the JSON error as a result, not a crash.
    round3 = tool_results(REQUESTS[2])
    assert any("[agent]" in r and "JSON" in r for r in round3), \
        f"broken arguments not reported back: {round3}"
    print("ok — malformed tool arguments came back as an error result")

    assert answer.startswith("FINAL:") and MARKER in answer, f"unexpected answer: {answer}"
    print("ok — loop terminated on the tool-call-free answer")

    print(f"\nOK — the agent loop round-trips tools, self-corrects, and stops.\n"
          f"Now run it against a real model: python3 {os.path.join(HERE, 'agent.py')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
