#!/usr/bin/env python3
"""mcp-think — offload thinking to a LOCAL model, exposed to Claude over MCP.

A minimal MCP server (stdio transport) whose tools run entirely on your
machine: each tool sends a prompt to a local model served by Ollama and
returns the answer. Nothing in the tool inputs ever leaves the box.

Why offload to a local model from Claude?
  * privacy   — summarize/critique content that must not leave the machine
  * cost      — bulk work (summaries, drafts, idea generation) at zero token cost
  * diversity — a second opinion from a different model family
  * bandwidth — Claude stays on the main task while the local model grinds

Run (Claude Code):
  claude mcp add think -- uv run --with mcp python3 /abs/path/to/server.py

Configuration (environment):
  OLLAMA_URL     base URL of the Ollama daemon   (default http://localhost:11434)
  THINK_MODEL    model to use                    (default qwen3:4b)
  THINK_TIMEOUT  per-call timeout in seconds     (default 300)

Dependencies: the `mcp` package only (`uv run --with mcp` provisions it).
The Ollama call uses the Python standard library — no SDK, no httpx.
"""

import json
import os
import re
import urllib.error
import urllib.request

# The SDK is renaming FastMCP → MCPServer; support both so the example works
# with the released `mcp` package today and with the renamed one later.
try:
    from mcp.server.mcpserver import MCPServer as Server  # unreleased SDK head
except ImportError:
    from mcp.server.fastmcp import FastMCP as Server      # mcp <= 1.x (PyPI)

OLLAMA_URL    = os.environ.get("OLLAMA_URL", "http://localhost:11434").rstrip("/")
THINK_MODEL   = os.environ.get("THINK_MODEL", "qwen3:4b")
THINK_TIMEOUT = int(os.environ.get("THINK_TIMEOUT", "300"))

mcp = Server("think")


def ask_local_model(system: str, prompt: str) -> str:
    """One chat round against the local model. Errors come back as text so
    the calling model can read them and tell the user what to fix."""
    body = json.dumps({
        "model": THINK_MODEL,
        "messages": [
            {"role": "system", "content": system},
            {"role": "user",   "content": prompt},
        ],
        "stream": False,
    }).encode()
    req = urllib.request.Request(
        f"{OLLAMA_URL}/api/chat",
        data=body,
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=THINK_TIMEOUT) as resp:
            data = json.load(resp)
    except urllib.error.HTTPError as e:
        detail = e.read().decode(errors="replace")[:300]
        return (f"[mcp-think] the local model rejected the request "
                f"(HTTP {e.code}): {detail}\n"
                f"Is the model pulled? Try: ollama pull {THINK_MODEL}")
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        return (f"[mcp-think] cannot reach the local model at {OLLAMA_URL} ({e}).\n"
                f"Start it with `ollama serve`, pull the model with "
                f"`ollama pull {THINK_MODEL}`, then retry.")

    text = (data.get("message") or {}).get("content", "")
    # Reasoning models may inline their chain of thought as <think>…</think>;
    # strip it — the caller wants the conclusion, not the scratchpad.
    text = re.sub(r"<think>.*?</think>", "", text, flags=re.DOTALL).strip()
    return text or "[mcp-think] the local model returned an empty response"


@mcp.tool()
def think(task: str, context: str = "") -> str:
    """Reason through a problem on the local model and return its conclusion.

    Call this to get an independent, private, zero-cost analysis of a
    question — weighing a design trade-off, sanity-checking a plan, working
    through logic — especially when a second opinion from a different model
    family is valuable. `context` carries any background the task needs
    (code, notes, constraints); it never leaves this machine.
    """
    system = ("You are a careful reasoning assistant. Think the problem "
              "through step by step, then answer with your conclusion first, "
              "followed by the key reasoning in a few short paragraphs.")
    prompt = f"{task}\n\n--- context ---\n{context}" if context else task
    return ask_local_model(system, prompt)


@mcp.tool()
def critique(work: str, focus: str = "") -> str:
    """Adversarially review a piece of work (code, prose, a plan, a design)
    on the local model and return the strongest objections.

    Call this before committing to a decision or shipping a draft, when an
    independent devil's advocate is useful, or when the material is private
    and must be reviewed without leaving the machine. `focus` optionally
    narrows the review (e.g. "error handling", "argument structure").
    """
    system = ("You are a rigorous, adversarial reviewer. Find the strongest "
              "objections: errors, gaps, risks, unstated assumptions. Rank "
              "them most-severe first. Be specific — point at the exact "
              "part you object to and say why. Do not pad with praise.")
    prompt = f"Review the following{f', focusing on {focus}' if focus else ''}:\n\n{work}"
    return ask_local_model(system, prompt)


@mcp.tool()
def brainstorm(topic: str, n: int = 5) -> str:
    """Generate `n` distinct ideas on a topic using the local model.

    Call this to widen the option space cheaply before converging — naming,
    approaches, test cases, failure modes, feature ideas. The local model's
    different training makes its ideas usefully different from yours.
    """
    system = ("You are a prolific idea generator. Produce genuinely distinct "
              "ideas — different mechanisms, not rephrasings. One line of "
              "pitch plus one line of how it would work, per idea.")
    return ask_local_model(system, f"Generate {n} distinct ideas for: {topic}")


@mcp.tool()
def summarize(text: str, max_words: int = 200) -> str:
    """Summarize text on the local model — the content never leaves this
    machine and costs no API tokens.

    Call this to condense large material (logs, documents, transcripts,
    diffs) before reasoning about it, or when the content is sensitive and
    must stay local. The summary comes back; the original stays here.
    """
    system = (f"Summarize faithfully in at most {max_words} words. Keep "
              "concrete facts, numbers, names, and conclusions; drop filler. "
              "Note anything surprising or anomalous explicitly.")
    return ask_local_model(system, text)


if __name__ == "__main__":
    mcp.run()   # stdio — Claude launches this as a subprocess
