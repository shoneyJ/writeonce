---
track: jarvis
iteration: "2"
status: pending
readiness: refine
---

# jarvis 2 — tool use / the agent loop

> Part of [Story — jarvis, the writeonce AI assistant](00-story.md).
> Where the assistant becomes an agent: the model may call tools, and jarvis
> runs them and feeds results back until the model answers. Needs jarvis
> [1](01-chat-loop.md).

## Why this exists

A chat loop answers from the model alone. An agent can act — read a file, query
a `@table`, call a `.wo` function — by the model emitting a tool call, jarvis
executing it, and returning the result for another turn. The actor model fits
the multi-step orchestration naturally.

## What it should deliver (to be refined)

- A **tool registry**: named tools, each a `.wo` class with a typed input and a
  handler, satisfying a `Tool` interface structurally (the porch middleware
  precedent).
- The **agent loop**: relay the tool-use request, dispatch to the tool actor,
  append the tool result, continue the turn — bounded to a max step count.
- **Tool-call framing** in the backend adapter (the Messages API `tool_use` /
  `tool_result` blocks), isolated in the same adapter file jarvis 1 established.

## Forks the brainstorm must settle

1. **Tool declaration shape** — a `.wo` class + interface (no reflection, per
   principle 13) versus a declarative schema; and how the input JSON maps to a
   typed value without `@derive` (language 29).
2. **Sandboxing / permissions** — which tools are allowed, and whether a tool
   that touches the filesystem or spawns a process (runtime-v2) needs an
   explicit grant.
3. **Step bound + loop safety** — the max tool-call depth and how a runaway loop
   is stopped.

## Out of scope

- Retrieval (iteration [3](00-story.md)); multi-model routing (later).

## Info

Pure `.wo` on iteration 1. No new runtime primitive expected unless a tool needs
one (each such tool names its own dependency).
