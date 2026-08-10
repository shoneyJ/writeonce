# mcp-think — a local-model MCP tool for Claude

A minimal, working **MCP server** that Claude (Claude Code or Claude Desktop) can call as a tool — and whose work runs entirely on **your machine**, on a local model served by [Ollama](https://ollama.com). One Python file, stdio transport, standard library only (plus the official `mcp` package).

Why offload from Claude to a local model:

| Reason | Example |
| --- | --- |
| **Privacy** | `summarize` a confidential document — the text never leaves the box |
| **Cost** | `brainstorm` 20 ideas or condense a 50k-line log at zero token cost |
| **Diversity** | `critique` from a different model family — an independent second opinion |
| **Bandwidth** | Claude stays on the main task while the local model grinds a side job |

## Tools

| Tool | What it does |
| --- | --- |
| `think(task, context?)` | Reason through a problem; conclusion first, reasoning after |
| `critique(work, focus?)` | Adversarial review — strongest objections, ranked |
| `brainstorm(topic, n?)` | `n` genuinely distinct ideas |
| `summarize(text, max_words?)` | Faithful local summary of large/sensitive material |

## Prerequisites

```bash
# 1. Ollama, serving a model
ollama serve                # if not already running as a service
ollama pull qwen3:4b        # the default model (or any other — see Configuration)

# 2. uv (provisions the `mcp` package on the fly) — or `pip install mcp`
```

For a quick low-disk trial, `ollama pull qwen3:0.6b` (~500 MB) works — set `THINK_MODEL=qwen3:0.6b`.

## Add to Claude Code

```bash
claude mcp add think -- uv run --with mcp python3 \
    /abs/path/to/docs/examples/mcp-think/server.py
```

(replace with your absolute path; add `-e THINK_MODEL=...` before `--` to override the model). Or declare it in a project's `.mcp.json`:

```json
{
  "mcpServers": {
    "think": {
      "command": "uv",
      "args": ["run", "--with", "mcp", "python3",
               "/abs/path/to/docs/examples/mcp-think/server.py"],
      "env": { "THINK_MODEL": "qwen3:4b" }
    }
  }
}
```

Claude Desktop: the same `command`/`args`/`env` block goes under `mcpServers` in `claude_desktop_config.json`.

Then just ask: *"use the think tool to weigh epoll vs io_uring for the WAL path"*, *"brainstorm 10 names for this feature"*, *"summarize this log with the local model"* — or let Claude reach for the tools on its own (the tool descriptions say when each applies).

## Configuration

| Variable | Default | Meaning |
| --- | --- | --- |
| `OLLAMA_URL` | `http://localhost:11434` | Ollama daemon base URL |
| `THINK_MODEL` | `qwen3:4b` | any pulled Ollama model |
| `THINK_TIMEOUT` | `300` | per-call timeout, seconds |

Errors (daemon down, model not pulled) come back as readable tool results, so Claude can tell you exactly what to run.

## Smoke test without Claude

```bash
python3 docs/examples/mcp-think/test_client.py       # uses uv + THINK_MODEL if set
```

Drives the server over stdio exactly like Claude does — `initialize` → `tools/list` → one `think` call — and prints the local model's answer.

## How this relates to writeonce

This example is the **consumer side** of MCP: Claude ← stdio → this server → local model. The **server side** is [plan 15](../../plan/15-mcp-streamable-http.md): the writeonce runtime itself becomes an MCP server over Streamable HTTP, generating tools/resources from `.wo` declarations — after 15d, a class method like [`Product.set_price`](../pricing/types/product.wo) is callable as an MCP tool with no Python in between. The two meet naturally: a writeonce app exposes its data as MCP tools, and a local model (through a server like this one) works over it.

## Ideas to build next

Same pattern (one file, stdio, local model), different capability:

- **mcp-redact** — strip PII/secrets from text locally *before* it is sent to any cloud model.
- **mcp-embed** — local embeddings (`ollama embed`) + a small vector index over a repo or notes; gives Claude semantic search without a cloud vector DB.
- **mcp-vision** — describe screenshots/diagrams via a local vision model (`llava`, `qwen2.5-vl`) for machines where images must stay local.
- **mcp-translate** — private translation of documents.
- **mcp-testgen** — bulk-generate test fixtures/edge cases on the local model, free of token cost.
- **mcp-judge** — a local LLM-as-judge for scoring outputs in eval loops, so the judge is independent of the model being judged.
