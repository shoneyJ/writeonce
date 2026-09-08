---
track: jarvis
iteration: "3"
status: pending
readiness: refine
---

# jarvis 3 — retrieval (RAG): grounded answers over a document set

> Part of [Story — jarvis, the writeonce AI assistant](00-story.md).
> Answers grounded in a corpus: embed documents, embed the query, retrieve the
> nearest chunks, and put them in the prompt. Needs jarvis [1](01-chat-loop.md)
> and the embeddings/vector decision below.

## Why this exists

A chat/agent loop knows only the model's training and the tools it can call.
Retrieval lets jarvis answer over the developer's own documents — the on-brand
"ask my codebase / my notes" use case — without fine-tuning.

## What it should deliver (to be refined)

- **Embeddings** for documents and queries — an embeddings API call over the
  same outbound TLS path jarvis 1 uses.
- **A vector store**: chunk text, store `{chunk, vector}` durably, and a
  similarity search (cosine / dot-product) over it.
- **Retrieval into the prompt**: top-k chunks prepended as context in the chat
  loop.

## Forks the brainstorm must settle

1. **The vector store: pure `.wo` or a new runtime primitive.** A brute-force
   cosine scan over a `@table` of `Bytes` vectors is pure `.wo` and fine for a
   modest corpus; an approximate-nearest-neighbour index or a SIMD dot-product
   builtin is a runtime iteration if scale demands it. Decide on a measured
   need, not up front.
2. **Chunking strategy** — fixed-size vs semantic; overlap; where metadata
   (source, offset) lives.
3. **Embedding storage** — vectors as `Bytes` (packed floats) in a `@table`, and
   whether Float arrays need a better carrier than iteration 19's `Bytes`.

## Out of scope

- Re-ranking models, hybrid keyword+vector search, and multi-corpus tenancy —
  each its own later slice.

## Info

Depends on jarvis 1's outbound path (for embeddings) and the vector-store fork.
The only possible new runtime work is fork 1's ANN/SIMD option, deferred until a
corpus size measures the need.
