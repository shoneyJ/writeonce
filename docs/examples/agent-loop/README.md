# agent-loop — build the loop that turns a model into an agent

A guided, working implementation of an **agent loop** — the mechanism at the
core of Claude Code, opencode, Cursor, and every other "agentic" tool — in one
Python file, standard library only, running entirely on the **local model**
served by [`prototypes/llama-moe-stream/start-local-agents.sh`](../../../prototypes/llama-moe-stream/start-local-agents.sh)
(or any OpenAI-compatible endpoint, e.g. Ollama).

You already run opencode against the local Qwen3-Coder server. This example is
what opencode *is*, with the product stripped away: read
[`agent.py`](agent.py) top to bottom and you know how every coding agent works.

## 1. The idea

A language model only ever produces text. What makes it an *agent* is a loop
around it that (a) tells it what tools exist, (b) executes the tool calls it
emits, and (c) feeds the results back — until it stops asking:

```
messages = [system, user question]
loop:
    reply = POST /v1/chat/completions   (messages + TOOL SCHEMAS)
    append reply to messages
    if reply has no tool_calls:         # the model is done
        print reply.content; stop
    for each tool_call in reply:
        result = run it locally         # YOUR code — the model never executes anything
        append {role: "tool", content: result} to messages
```

Two properties fall out of this shape, and they are the whole mental model:

- **The transcript is the only state.** `messages` grows append-only; the
  model re-reads the entire history every round. There is no other memory —
  which is why the assistant's own tool-call message must be appended too, not
  just the results (the model has to see *what it asked for* next round).
- **The model proposes, your process disposes.** Tool calls are requests in
  JSON. The executor decides what actually happens — it is the security
  boundary, so the agent is exactly as dangerous as its tools, never more.

## 2. The five pieces (each maps to a section of `agent.py`)

| # | Piece | In `agent.py` |
| --- | --- | --- |
| 1 | **A tool-calling endpoint** — `llama-server --jinja` applies Qwen's chat template so tool schemas go in and structured `tool_calls` come out | `chat()` |
| 2 | **Tool schemas** — the JSON contract shown to the model; descriptions are prompts, write them like documentation | `TOOLS` |
| 3 | **The executor** — dispatch, argument parsing, sandboxing; every failure returned as words, never raised | `execute()` |
| 4 | **The transcript** — one append-only `messages` list | `run_turn()` |
| 5 | **Stop conditions** — natural (no `tool_calls`) and budgeted (`MAX_TURNS`) | `run_turn()` |

The tools here are deliberately read-only (`list_dir`, `read_file`, `search`)
and confined to `AGENT_ROOT` — enough to make a useful repo-Q&A agent with
zero risk while you study the loop.

## 3. Run it

```bash
# 1. Start the local model (from prototypes/llama-moe-stream)
prototypes/llama-moe-stream/start-local-agents.sh     # Qwen3-Coder on :8080

# 2. One-shot, over this repo
cd /path/to/writeonce-all
python3 docs/examples/agent-loop/agent.py "which file implements the shard bus, and how do cross-shard reads work?"

# 3. Interactive — the conversation persists across questions
python3 docs/examples/agent-loop/agent.py
```

Tool calls print to stderr as they happen (`⚙ search({"pattern": ...})`), so
you watch the loop investigate before it answers.

Against Ollama instead: `LLM_URL=http://localhost:11434/v1 LLM_MODEL=qwen3:4b
python3 agent.py ...` (Ollama serves the same OpenAI-compatible `/v1`; small
models tool-call noticeably worse than Qwen3-Coder-30B — that difference is
itself instructive).

## 4. The guards that keep the loop alive

The naive loop works until the model misbehaves — and it will. The one rule:
**never raise at the model; return the failure as the tool result.** A
tool-calling model reads the error and corrects itself next round; an
exception just kills the conversation.

| Failure | Guard in `agent.py` |
| --- | --- |
| Arguments aren't valid JSON | error string back: "resend the call with corrected JSON" |
| Tool name doesn't exist | error string back, listing the real tools |
| Wrong/missing/extra parameters | `TypeError` caught → described back |
| Tool output too big for the context | truncated at 8k chars with an instruction to narrow |
| Path outside the project | `_resolve()` refuses (symlinks resolved first) |
| Model never stops calling tools | `MAX_TURNS` budget ends the turn with a readable note |

## 5. Smoke-test without a model

```bash
python3 docs/examples/agent-loop/test_loop.py
```

Stands up a canned `/chat/completions` on a local port and scripts three
rounds — a real `read_file`, a tool call with deliberately broken JSON
arguments, then a final answer — asserting that results are fed back, errors
self-correct, and the loop terminates. This is the loop's mechanics verified
in milliseconds, no GGUF required.

## Configuration

| Variable | Default | Meaning |
| --- | --- | --- |
| `LLM_URL` | `http://127.0.0.1:8080/v1` | OpenAI-compatible base URL |
| `LLM_MODEL` | `qwen3-coder` | model name (`--alias` of the llama-server) |
| `LLM_TIMEOUT` | `300` | per-request timeout, seconds |
| `AGENT_ROOT` | current directory | sandbox root for all three tools |
| `MAX_TURNS` | `20` | tool rounds per user message |

## How this relates to writeonce

This is the third corner of the local-agent triangle in this repo:

- [`mcp-think`](../mcp-think/) is the **tool side** — a local model offered
  *as tools to* an agent (Claude).
- [`prototypes/llama-moe-stream`](../../../prototypes/llama-moe-stream/) is
  the **model side** — serving the MoE this loop drives.
- **This example is the agent side** — the harness itself.

The natural next step joins them to [plan 15](../../plan/15-mcp-streamable-http.md):
once the writeonce runtime speaks MCP over Streamable HTTP, the hardcoded
`TOOLS` list here gets replaced by an **MCP client** — `tools/list` supplies
the schemas, `tools/call` becomes the executor (the JSON-RPC lifecycle is
already demonstrated in [`mcp-think/test_client.py`](../mcp-think/test_client.py)).
Then this same ~40-line loop lets a fully local model operate a `.wo`
application: `article_create`, `set_price`, live resources — one catalog, one
engine, one port, no cloud.

## Ideas to build next

Each is a small, self-contained extension of the loop — and each corresponds
to a feature you use daily in Claude Code/opencode:

- **MCP tool source** — fetch schemas from an MCP server at startup instead of
  hardcoding `TOOLS`; route `execute()` through `tools/call`. (= MCP support)
- **A `write_file` tool behind a y/n prompt** — the executor asks *you* before
  acting. (= permission modes)
- **A `spawn_agent` tool** that runs a fresh `run_turn()` with its own
  transcript and returns only the final answer. (= sub-agents)
- **Transcript compaction** — when `messages` outgrows the context window,
  summarize the older rounds into one message. (= auto-compact)
- **Parallel tool execution** — a reply may carry several `tool_calls`; run
  them concurrently, append results in order. (= parallel tool use)
