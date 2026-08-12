# HTTP Service Layer Implementation Plan

> **Status: ⬜ pending** (story iteration 10) — `service` blocks route to VM methods; REST parity with the shipped Rust Stage 2 runtime. Board: [00-status.md](../../00-status.md)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Style rule (user convention):** concept, reason, and required behavior in words only; the executor writes the code.

**Goal:** Sub-project 4 of the OOP spec — `service rest` blocks become live routes on the C runtime: generated CRUD over the DB binding, method-RPC endpoints, one trap-to-HTTP mapping, hand-rolled JSON — the blog sample's REST surface served by the new stack.

**Architecture:** Plan 6 of 7. Depends on plans 1–5. The HTTP substrate is shipped C: `runtime/wo-rt.c` phase C's io_uring loop with keep-alive and a per-connection write queue (strace-verified: zero epoll/recv/send syscalls on the request path). This plan ports that pattern into `runtime/src/` and mounts a router on it, per shard (routers are shard-local state, mirroring the Rust runtime's thread-local routers — `HandlerFn` not shared). The compiler stops skipping `service` blocks and compiles them into a route section the runtime loads. JSON is hand-rolled per the existing dependency-removal doc (`docs/plan/05-hand-rolled-json.md`) — the C sibling of what the Rust track already specified.

**Tech Stack:** C11 + libc (io_uring per the shipped phase-C pattern, SO_REUSEPORT per phase A). OCaml (service-block parsing + route emission). No TLS in this plan — stated non-scope, terminate elsewhere.

## Global Constraints

- All plan-1/4/5 constraints carry over (libc only, no commits — drafts to `.dev/commit.md`, ASan/TSan gates, docs under `docs/`).
- **Routers are shard-local** — built per worker at boot from the module's route section; no shared routing state.
- **Reads and acks never leave the shard's serial stream** — handlers run as jobs on the owning shard; cross-shard point ops use the plan-4/5 hop-once machinery.
- **The `.wob` format change (route section) bumps the version to 2** — one coordinated change to the format doc, loader, builder, and emitter, in one task, with the loader still rejecting v1-invalid images identically ("format changes go through the format doc" rule honored by doing it once, visibly).
- **Stage-3 endpoints stay honest:** `/api/<type>/live` returns 501 exactly as `.dev/reference/rest/*.rest` documents — LIVE lands in plan 7; don't "fix" the 501s.
- **JSON is hand-rolled** — no library, bounded depth, UTF-8 safe, byte-length (not char-length) content framing.

---

## File Structure

```
runtime/src/
  http.c http.h          request parse, keep-alive state machine, response write (Task 1)
  json.c json.h          hand-rolled encoder/decoder (Task 2)
  router.c router.h      per-shard route table, path/method match (Task 3)
  handlers.c handlers.h  CRUD + method-RPC handlers over db.h (Tasks 4–5)
compiler/src/            service-block parsing + route-section emission (Task 3)
tests/corpus/http/       request/response fixtures driven over real sockets (Task 6)
docs/plan/oop-vm/05-http-service.md   route section format, trap→HTTP table, JSON subset (Task 1)
```

---

### Task 1: HTTP module port

**Concept & reason:** lift phase C's proven connection machine out of the single file into a module the scheduler mounts per shard: multishot accept, keep-alive request parsing (method, path, headers subset, content-length bodies), per-connection write queue, pipelined-tail carry-over — behaviors phase C already measured; the port's test evidence must match (single connection serving many requests, syscall profile clean). The doc opens with the layer's contract: how handlers receive a parsed request and return status/headers/body, and the trap→HTTP table Task 5 implements.

- [ ] Failing tests: socket-level harness — keep-alive sequence on one connection, bad-request handling (malformed start line → 400 and close), body framing by content-length, oversized request → 413 and close.
- [ ] Implement the port; green; syscall profile spot-checked against the phase-C evidence.
- [ ] Record commit draft: `feat(runtime): http module — phase-C io_uring keep-alive machine as a per-shard module (multishot accept, write queues, pipelined tails); socket-harness tests; docs/plan/oop-vm/05-http-service.md contract.`

### Task 2: Hand-rolled JSON

**Concept & reason:** the codec both directions of every endpoint use, built to the existing plan-05 doc's discipline: encoder streams rows/objects using the six field kinds (scalars, texts with escaping, containers as arrays/objects, ids as numbers); decoder parses request bodies into field initializers with bounded nesting depth, exact UTF-8 validation, and duplicate-key rejection; numbers are i64-safe (no doubles in milestone types). Errors carry positions for 400 responses that name the byte offset. Content-Length is computed from encoded byte length — the log-watcher-documented non-ASCII truncation bug class, prevented by rule here.

- [ ] Failing tests: round-trip across all kinds; escaping/UTF-8 edges (embedded quotes, multibyte, invalid sequences rejected); depth bomb rejected; duplicate keys rejected; byte-length framing with multibyte content.
- [ ] Implement; green.
- [ ] Record commit draft: `feat(runtime): hand-rolled JSON — kind-driven encode, strict decode (UTF-8, depth, dup keys) with byte-offset errors, byte-length framing; edge-case battery.`

### Task 3: Service blocks compile to a route section

**Concept & reason:** the compiler's skip-on-block for `service rest "..." expose list, get, create, update, delete` ends: the parser builds a service AST (path prefix, exposed verbs, owning class), the typechecker validates verbs against the class (subscribe stays legal to declare — it routes to the 501 stub), and the emitter writes a route section into the `.wob` — the coordinated **format v2** change: header gains the section, builder and loader gain support with full validation (paths well-formed, class ids in range, verbs known), version bumps, format doc updated, and every plan-1 loader test re-run to prove v1-shaped rejects still reject. The runtime's router loads the section per shard into a match table (exact-prefix plus `:id` segment).

- [ ] Failing tests: compiler goldens (service AST, route section in the disassembler); loader validation of malformed route sections; router unit tests (match/miss/verb table incl. 405-shaped responses per the `.rest` conventions).
- [ ] Implement across compiler, builder, loader, router; all prior gates re-run green.
- [ ] Record commit draft: `feat: service rest compiles — service AST + .wob v2 route section (coordinated builder/loader/emitter bump, format doc updated), per-shard router with :id matching and 405 semantics.`

### Task 4: Generated CRUD over the DB binding

**Concept & reason:** the six-endpoint contract the README promises, on the new stack: list (shard-local per the plan-5 scope, documented), get by id (hop-once cross-shard), create (JSON body → insert path with defaults, 201 with the row), update (PATCH partial-set → point update), delete (row remove through the choke-point API), each encoding responses through Task 2 and running as a job on the owning shard. Behavior parity target is the Rust runtime's Stage-2 semantics as documented by `.dev/reference/rest/blog.rest` — including auto-id, default seeding, and partial-update PATCH.

- [ ] Failing tests: socket-level CRUD round-trips against a compiled blog-shaped fixture; PATCH partial semantics; 404 on missing ids; create-on-foreign-shard impossible by construction (create is local — test proves ids from the accepting shard).
- [ ] Implement; green.
- [ ] Record commit draft: `feat(runtime): generated CRUD — list/get/create/update/delete over the row API with hop-once foreign reads, Stage-2 parity semantics (auto-id, defaults, PATCH).`

### Task 5: Method RPC + trap→HTTP mapping

**Concept & reason:** the 13b story on the new stack: exposed methods get POST routes (path per the Rust runtime's method-RPC convention), the handler decodes arguments, runs the method on the row's owning shard as a row-scoped job, and encodes the return value. The trap table becomes the error contract, implemented once in the handler layer and documented in the Task-1 doc: BOUNDS/KEY on missing rows or fields → 404/400 as appropriate; BORROW (residual aliasing) → 409; DIV0 and EXPLICIT → 500 with the structured `{code, method, line, message}` body the spec promised in section 6; DB → 501 (anything still unbound); STACK/OOM → 500 with no body detail. The `subscribe` verb's `/live` route returns the honest 501.

- [ ] Failing tests: method round-trip on the pricing fixture (set_price over HTTP mutates, current_price reads back); each trap class mapped (fixtures rig each trap) with the structured error body asserted; /live 501.
- [ ] Implement; green.
- [ ] Record commit draft: `feat(runtime): method RPC + trap contract — exposed methods as shard-local POST jobs, one trap→HTTP table (409 borrow, 404/400 bounds, structured 500 bodies), honest /live 501.`

### Task 6: Blog-sample smoke + acceptance

**Concept & reason:** the end-to-end proof the stack means something: the blog sample's milestone-compatible subset (types + service blocks; policies/triggers still parse-and-discard) compiles with `woc`, serves with the sharded runtime, and a scripted run of the `.dev/reference/rest/blog.rest` request sequence (curl-driven, per that directory's README) passes — expected statuses including the documented 501s and policy-shaped 405/404s where applicable. A light bench recipe (reusing the phase-C bench harness shape) records requests/sec for the record, not as a gate. `just oop-accept` gains the HTTP corpus and the blog smoke; CLAUDE.md/kanban sync.

- [ ] Wire smoke + bench + gate; green; docs synced.
- [ ] Record commit draft: `test: blog-sample smoke on the C stack — scripted blog.rest sequence green (incl. honest 501/405 semantics), bench recipe for the record; oop-accept gains the HTTP gate; docs sync.`

---

## Plan self-review notes

- **Spec coverage (sub-project 4):** service blocks routed, trap surface → HTTP exactly as spec section 6 promised ("one trap surface forever"), CRUD parity with Stage 2, method RPC, shard-local routers — all tasked. Non-scope, stated: TLS, LIVE/WebSocket (plan 7), policies/triggers (still parse-and-discard), scatter-gather list.
- **Order rationale:** transport before codec before routing before handlers; the format-v2 bump isolated in one task with full regression re-run; end-to-end smoke last.
- **Consistency check:** the trap→HTTP table lives in one doc section and one handler-layer implementation; CRUD and RPC both route through it. Route section validated with the same loader rigor as every other section.
