# Dependency graphs — iterations and framework features

> Companion to [00-status.md](stories/00-status.md) (states live THERE; this page
> carries the edges). An arrow `A --> B` means **A must exist before B**;
> a dashed arrow is a scope DIRECTIVE, not a technical dependency. Use it
> to pick the next implementation: anything whose incoming arrows are all
> green is startable today. Rebuilt 2026-08-20 from a sweep of every
> story/spec/plan markdown (the "misses" pass: iteration 17's outgoing
> edges, the concurrency chain, the parked drain, 9b→25, 28's gap
> fan-out, 20's fiber caveat), and **refreshed 2026-08-26** against the
> code and the story frontmatter: graph 1 had drifted a generation
> behind — it still showed 17 parked and 18 as next, and it used the
> pre-renumber ids 10/12/13/14 for what are now stories 25/26/29/28. All
> iterations through 38 are now nodes.

## 1. Story iterations

```mermaid
flowchart TD
    classDef done fill:#1a7f37,color:#fff,stroke:none
    classDef parked fill:#6e7781,color:#fff,stroke:none
    classDef specd fill:#0969da,color:#fff,stroke:none
    classDef open fill:#eac54f,color:#000,stroke:none

    FOUND["1–6 foundation: doctrine, VM, compiler, binary, surface, stdlib"]:::done
    I7["7 log-watcher proof"]:::done
    I7b["7b inferred GC + per-shard mark-sweep"]:::done
    I9["9 database engine"]:::done
    I9b["9b @table + query"]:::done
    I15["15 deps package manager"]:::done
    I16["16 web framework v1 core"]:::done

    I17["17 library kind + internal/ ✅ 2026-08-20"]:::done
    FWREORG["framework internal/ reorg + check mode ✅ landed with 17 (WO-E108/E109 shipped)"]:::done
    I19["19 Float + Bytes ✅ 2026-08-20"]:::done
    I37["37 wo-html components + raw text literal ✅ 2026-08-25"]:::done
    I35["35 net runtime seams ✅ 2026-08-23"]:::done
    I36["36 operator parity: not/bitwise/hex literals — code landed 2026-08-22, awaiting the manual pass"]:::specd
    RELEASE["packaging + release pipeline ✅ 2026-08-25 (no story: VERSION, just dist, install-accept, release.yml)"]:::done

    I18["18 framework v2: transaction{} + cache/flags/jobs (⏸ hold 2026-08-21; spec+plan approved, held intact)"]:::parked

    I9c["20 cross-program tables (⏸ hold 2026-08-21; channel half-built)"]:::parked
    I9d["21 keypair attach auth (⏸ hold 2026-08-21; crypto floor now exists via 34)"]:::parked
    I9e["22 durability + throughput baseline ✅ 2026-08-21"]:::done
    I8["8 shard-actor runtime ✅ 2026-08-21"]:::done
    I24["24 chat + actor lifecycle 🔄 THE LIVE SLICE (absorbing 31 + 34)"]:::specd
    I34["34 crypto builtins ✅ code landed as 24's T1 (ids 85-87)"]:::done
    I31["31 actor lifecycle — call/mailbox-cap/death landed in 24; monitor + time.after (ids 89/90) open"]:::specd
    I9f["23 io_uring group-commit"]:::open
    I32["32 WAL checkpoint (append-only today; bounds replay)"]:::open
    I33["33 single-file store WO_DATA=<path>.db (driver-only, off-chain)"]:::open
    I25["25 HTTP service layer — `service` blocks (⏸ hold 2026-08-21; story file removed, plan remains)"]:::parked
    I11["11 fibers ✅ 2026-08-21"]:::done
    I26["26 blue-green deploy (⏸ hold)"]:::parked
    I29["29 metaprogramming @derive (⏸ hold)"]:::parked
    I28["28 skillhost workload (⏸ hold; demoted)"]:::parked
    I38["38 content platform capabilities: fs mutation verbs + net.connect"]:::open
    I9g["27 query grammar corpus (⏸ hold; likely collapses)"]:::parked
    I30["30 observability, CI, fuzz — release-only CI exists; per-change gates + fuzz open (no story file)"]:::open
    GAPS["28's gap fan-out, what is LEFT of it: fs metadata, FFI-vs-out-of-process (bounded subprocess + stdio transport moved to 42)"]:::open
    I42["42 bounded subprocess ✅ 2026-09-01: proc.run bounded + parked (pidfd), proc.run_dl; streaming form deferred by name"]:::done
    DRAIN["parked drain, what is LEFT of it: WO-E225 roster, ADT roster, group-by aggregates"]:::parked

    FOUND --> I7
    FOUND --> I9
    I7 --> I7b
    I9 --> I9b
    I9b --> I15
    I15 --> I16
    I15 --> I17
    I16 --> I17
    I17 --> FWREORG
    I16 --> I37
    I19 --> I37
    I17 --> RELEASE
    I9 --> I18
    I16 --> I18
    I9 --> I9c
    I9c --> I9d
    I9c --> I25
    I9b --> I25
    I16 --> I25
    I9b --> I9e
    I7b --> I8
    I9e --> I9f
    I8 --> I9f
    I8 --> I11
    I19 --> I34
    I34 --> I24
    I8 --> I24
    I11 --> I24
    I35 --> I24
    I31 --- I24
    I9f --> I32
    I32 --- I33
    I9 --> I26
    I25 --> I26
    I9g --> I28
    I7 --> I28
    I28 --> GAPS
    I11 --> I42
    I24 --> I42
    I16 --> I38
    I32 --> I38
    I36 -.reopens the pure-wo HMAC question.-> I34
    I26 -.scope directive.-> I29
    I26 -.scope directive.-> DRAIN
```

Reading it: **the live slice is 24** (chat + actor lifecycle, absorbing 31
and 34), and the chain behind it is 23 → 32. Everything else with all-green
incoming arrows is startable: **33** (driver-only, off-chain), **38** (the
fs-mutation and outbound-socket gaps), and **30**'s remaining half
(per-change CI and fuzzing — the release pipeline covered only publishing).
**36** needs no work, only the developer's manual pass over
`docs/examples/operators/`. The held tail — 18, 20/21, 25, 26, 27, 28, 29 —
resumes on its own precedence notes; 29 and what is left of the drain still
sit behind 26 by the 2026-08-08 scope directive (dashed), not by any
technical edge. Note what left the drain: `pub(read)`, `using` and `#if` all
shipped, so only the WO-E225/ADT rosters and group-by aggregates remain in
it.

## 2. The concurrency chain (iterations 8 / 23 / 11 and everything they gate)

The runtime's concurrency work is the single biggest unlocker — every
⏸ row in the framework ledger and two v2 follow-ons hang off it.

```mermaid
flowchart TD
    classDef rt fill:#8250df,color:#fff,stroke:none
    classDef gated fill:#eac54f,color:#000,stroke:none
    classDef v2 fill:#0969da,color:#fff,stroke:none
    classDef done fill:#1a7f37,color:#fff,stroke:none

    I7b2["7b per-shard collector (done — the precondition 8 waited on)"]:::rt
    I8x["8 shard-actor runtime: thread-per-core, ownership-move messages"]:::rt
    I9fx["23 io_uring group-commit (batch = the shard tick)"]:::rt
    I11x["11 fibers: reduction-budget preemption, blocking builtins park"]:::rt
    I9ex["22 baseline (numbers 8/23 sign against)"]:::rt

    KEEPAL["keep-alive parking retired ✅ iteration 35 (app-owned fiber-per-connection + idle deadline)"]:::rt
    H2C2["h2c HTTP/2 cleartext (spec §C: also needs 23)"]:::gated
    STREAM2["request body streaming + backpressure"]:::gated
    SRESP2["streaming responses + explicit commit point"]:::gated
    CANCEL2["per-request cancellation propagation"]:::gated
    PUBSUB2["DONE 2026-08-27 — pub/sub + WebSockets (iteration 24: ws_accept + wsframe + room actors)"]:::done
    ASYNC9C["20 async attach statements (rejected-for-now alternative)"]:::gated
    TIMEOUTS2["idle timeouts become schedulable (net seam still needed)"]:::gated

    FIBJOBS2["fiber-scheduled jobs (replaces drain-on-request; queue table stays)"]:::v2
    CANCELRB["cancellation → transaction rollback"]:::v2
    I18x["18 transaction{} + jobs"]:::v2

    I7b2 --> I8x
    I9ex --> I9fx
    I8x --> I9fx
    I8x --> I11x
    I8x --> KEEPAL
    I11x --> KEEPAL
    I8x --> H2C2
    I9fx --> H2C2
    I11x --> H2C2
    I11x --> STREAM2
    I11x --> SRESP2
    I11x --> CANCEL2
    I8x --> PUBSUB2
    I11x --> PUBSUB2
    I11x --> ASYNC9C
    I11x --> TIMEOUTS2
    I11x --> FIBJOBS2
    I18x --> FIBJOBS2
    I11x --> CANCELRB
    I18x --> CANCELRB
    CANCEL2 --> CANCELRB
```

## 3. Framework v1 — remaining ledger items

Three gates recur: **net seams** (runtime `net` builtins), the **crypto
fork** (bitwise operators + hex literals landed with iteration 36, so
digests are now expressible in pure `.wo` — pure-`.wo` vs C-builtin is
story 34's brainstorm before the slice), and the
**concurrency chain above** (its gated nodes are not repeated here).

```mermaid
flowchart TD
    classDef gate fill:#8250df,color:#fff,stroke:none
    classDef ready fill:#1a7f37,color:#fff,stroke:none
    classDef blocked fill:#eac54f,color:#000,stroke:none
    classDef done fill:#6e7781,color:#fff,stroke:none

    CORS["CORS middleware ✅ slice 2"]:::done
    SECH["security-headers middleware ✅ slice 2"]:::done
    HOSTV["host validation (421) ✅ slice 2"]:::done
    STRICT["strict-parsing audit (dup Content-Length = 400) ✅ slice 2"]:::done
    WILD["wildcard segments *rest ✅ slice 2"]:::done
    PREC["precedence: registration order stands, wildcards last by construction ✅"]:::done
    GROUPS["route groups + group middleware ✅ slice 2"]:::done
    CTX["req.ctx bag ✅ slice 2"]:::done
    XFF["client_ip: X-Forwarded-For parsing ✅ slice 2 (peer VERIFY stays gated)"]:::done
    ACCEPT["accepts(): response-side negotiation ✅ slice 2"]:::done

    NETSEAM["GATE CLEARED: iteration 35 landed the net seams — _dl deadlines, listen_unix, peer (ids 91-95)"]:::done
    TMOUT["read/write/idle timeouts ✅ iteration 35 (serve_conn read_ms/idle_ms)"]:::done
    UNIX["unix socket binding ✅ iteration 35"]:::done
    PEERV["trusted-proxy PEER verification — net.peer landed; the verify middleware is a ready framework slice"]:::ready

    CRYPTO["GATE CLEARED: iteration 34 landed C builtins — sha1/sha256/hmac_sha256 (ids 85-87)"]:::done
    SHA["sha1/sha256/hmac_sha256 ✅ iteration 34; SHA-512/CRC32 wait for a consumer"]:::done
    ETAG["ETag + If-None-Match 304 ✅ slice 2"]:::done
    COOKIE["signed cookies"]:::ready
    CSRF["CSRF"]:::ready
    SESS["session integrity"]:::ready
    HOOKV["webhook verification"]:::ready
    JWT["JWT HS256 (HARD STOP after)"]:::ready

    RADIX["radix-tree routing"]:::blocked
    I9E3["GATE: router scan unmeasured — 22's harness landed but benched the DB, not the router; perf-targets entry first"]:::gate

    STORAGE["storage-integration rows: migrations (future story), eager loading + tenant roots (query-surface work, 9-series)"]:::blocked

    NETSEAM --> TMOUT
    NETSEAM --> UNIX
    NETSEAM --> PEERV
    CRYPTO --> SHA
    SHA --> ETAG
    SHA --> COOKIE
    SHA --> CSRF
    SHA --> SESS
    SHA --> HOOKV
    SHA --> JWT
    I9E3 --> RADIX
```

**Slice 2 landed (2026-08-23, branch `framework-v1b`):** every
formerly-green node plus the crypto chain's first consumers — CORS,
security headers, host validation (421), strict dup-Content-Length,
`*rest` wildcards, route groups + group middleware, `req.ctx`,
`client_ip`, `accepts()`, ETag/304 — all gated by `just web-app`
(38 checks). The after-middleware seam (`After`/`use_after`) carries
the response-header half. Now READY with the digest builtins landed:
signed cookies, CSRF, session integrity, webhook verification, JWT
HS256 (the hard stop). Still gated: timeouts/unix-socket/peer-verify
(story 35's net seams) and the radix tree (router scan unmeasured).
Note: 21's keypair crypto is its own C implementation (already on
branch `keypair-auth`) — it neither waits for nor feeds this chain.

## 4. Framework v2 (iteration 18) — internal order

```mermaid
flowchart TD
    classDef piece fill:#0969da,color:#fff,stroke:none
    classDef indep fill:#1a7f37,color:#fff,stroke:none
    classDef later fill:#eac54f,color:#000,stroke:none

    TXN["transaction{} (engine undo log + language block)"]:::piece
    JOBS["jobs.wo: wf_jobs + enqueue + JobRunner + idle() drain"]:::piece
    DEMO["web-app demo: transactional order+confirm, GET /jobs, flags route"]:::piece
    CACHE["cache.wo: TTL + FIFO"]:::indep
    FLAGS["flags.wo: wf_flags + read-through map"]:::indep
    TPRMW["txn-per-request middleware (v1 ledger's storage row; single-thread OK)"]:::later

    TXN --> JOBS
    JOBS --> DEMO
    FLAGS --> DEMO
    TXN --> TPRMW
```

Cache and flags are dependency-free warm-ups; `transaction { }` is the
critical path (the only engine + language work); jobs compose on it; the
demo and gate close it. Fiber-scheduled jobs and cancellation→rollback
appear in graph 2 — they need iteration 11 as well as 18.

## 5. porch — the web framework track

States live on [the board's porch section](stories/00-status.md). **The whole
track (2–8) is `readiness: ready`** as of the 2026-09-06 brainstorm; **1** is
done, **9** is held (blocked on the lang-41 arena hang, not an enhancement).
Three independent roots: **2** (the auth chain), **6** (the streaming chain),
**5** (anytime, no incoming edges at all — not even iteration 2).

This graph makes the **cross-track language edges** visible: the three builtins
the track needs, each drawn as a `lang` node feeding the story that owns it.

```mermaid
flowchart TD
    classDef done fill:#1a7f37,color:#fff,stroke:none
    classDef ready fill:#0969da,color:#fff,stroke:none
    classDef held fill:#6e7781,color:#fff,stroke:none
    classDef lang fill:#8250df,color:#fff,stroke:none

    RB["language work: random_bytes builtin (bare-name, id 84/90) — porch 2 Phase A"]:::lang
    DFL["language work: deflate + crc32 builtins (C) — porch 7 Phase C"]:::lang
    TU["language work: time.utc builtin (gmtime sibling of time.local) — porch 8 Phase A"]:::lang

    P1["porch 1 store-backed middleware ✅ 2026-08-30"]:::done
    P2["porch 2 randomness + cookies"]:::ready
    P3["porch 3 sessions"]:::ready
    P4["porch 4 CSRF"]:::ready
    P5["porch 5 routing + response ergonomics (zero upstream deps)"]:::ready
    P6["porch 6 streaming core"]:::ready
    P7["porch 7 SSE + compression"]:::ready
    P8["porch 8 static files + lifecycle"]:::ready
    P9["porch 9 idempotent replay (ready — unblocked 2026-09-09)"]:::ready
    L41["language 41 actor-arena double free ✅ fixed 63065ff (cross-shard marshal)"]:::done
    L44["language 44 poison-on-free ✅ (41's decision 3: a freed header can never pass for live; double free aborts)"]:::done
    L41 -.follow-up.-> L44

    RB --> P2
    P2 --> P3
    P2 --> P4
    P3 --> P4
    P6 --> P7
    P6 --> P8
    P5 --> P7
    P5 --> P8
    DFL --> P7
    TU --> P8
    L41 -.fixed 2026-09-09 — no longer blocks.-> P9
    P1 -.re-scope 79e6da4: replay-on-retry split out of 1.-> P9
```

Edges corrected by the 2026-09-06 brainstorm: `P5 --> P7` (gzip's
`Accept-Encoding` reuses iteration 5's q-value ranking) stays, but the old
`P2 --> P7` edge is **gone** — story 7 decided `Vary` accumulates by comma-join
(iteration 5's shape), not iteration 2's repeated-header work. `P5 --> P8` is the
`Download`/`Attachment` helper. The three `lang` nodes are the track's entire
language bill; each is a builtin with a named consumer, none shipped as
decoration.

## 5a. porch's language-driven gaps (out-of-scope features and the language stories that own them)

These are the features fiber ships that porch deliberately does **not** — each
excluded because a language primitive does not exist yet. Every edge points from
the owning language story to the porch feature it would unblock (see the
[porch↔fiber scope-gap analysis](plan/exploration/fiber/01-porch-vs-fiber-scope-gap.md)).

```mermaid
flowchart LR
    classDef done fill:#1a7f37,color:#fff,stroke:none
    classDef refine fill:#eac54f,color:#000,stroke:none
    classDef held fill:#6e7781,color:#fff,stroke:none
    classDef gap fill:#cf222e,color:#fff,stroke:none

    L29["language 29 @derive (⏸ hold)"]:::held
    L38["language 38 net.connect ✅ landed (id 110); proxy middleware now buildable"]:::done
    L43["runtime-v2 8 symmetric cipher (refine, NEW 2026-09-06)"]:::refine
    L30["runtime-v2 7 observability (refine, moved from language 30, 2026-09-06)"]:::refine
    L18["language 18 TTL cache + transaction{} (⏸ hold)"]:::held
    L31["language 31 cancellation ✅ (landed in 24)"]:::done

    BIND["typed request binding (fiber Bind)"]:::gap
    PROXY["reverse proxy + outbound HTTP client"]:::gap
    ENC["encrypted cookies (fiber encryptcookie)"]:::gap
    METRICS["metrics / pprof / expvar endpoints"]:::gap
    CACHE["cache middleware + recovery rollback"]:::gap
    TIMEOUT["per-handler timeout + streaming backpressure"]:::gap

    L29 --> BIND
    L38 --> PROXY
    L43 --> ENC
    L30 --> METRICS
    L18 --> CACHE
    L31 --> TIMEOUT
```

31 (cancellation) is already green — per-handler timeout and streaming
backpressure are unblocked at the language level and wait only on a porch slice
to consume them. The other five gaps are gated on an upstream story: two
brand-new runtime-v2 iterations (8 cipher, 7 observability — moved out of the
language track 2026-09-06), two language iterations on hold (29, 18), one pending
a spec (38). Landed enablers the
track already consumed — 34 (crypto digests), 36 (bit operators), 35 (net
seams) — are green in graphs 1–3 and not repeated here.

## 6. wmux — the multiplexer track (wmux 1) and its gap chain

The tmux study's gaps, remapped as buildable edges now that iteration 42
landed. Every yellow node is an iteration of the
[runtime-v2 track](stories/runtime-v2/00-story.md) ("the runtime beyond
sockets"), ALL `readiness: ready` since the track-wide brainstorm
([spec](superpowers/specs/2026-09-01-runtime-v2-design.md), 2026-09-01) —
[1 streaming subprocess](stories/runtime-v2/01-streaming-subprocess.md) ·
[2 PTY](stories/runtime-v2/02-pty.md) ·
[3 signals as events](stories/runtime-v2/03-signals-as-events.md) ·
[4 termios](stories/runtime-v2/04-termios.md) ·
[5 fd passing](stories/runtime-v2/05-fd-passing.md). wmux — its own
track, first of the softwares built with writeonce — is the driving
workload that consumes them all — [wmux 1](stories/wmux/01-wmux.md).

```mermaid
flowchart TD
    classDef done fill:#1a7f37,color:#fff,stroke:none
    classDef gap fill:#eac54f,color:#000,stroke:none
    classDef product fill:#0969da,color:#fff,stroke:none
    classDef later fill:#6e7781,color:#fff,stroke:none

    I42w["42 bounded subprocess ✅ 2026-09-01"]:::done
    GSTREAM["runtime-v2 1 ✅ 2026-09-02 streaming subprocess: Child fds driven by the net verbs, wait_dl, signal"]:::done
    GPTY["runtime-v2 2 ✅ 2026-09-02 PTY: spawn_pty + resize"]:::done
    GSIG["runtime-v2 3 ✅ 2026-09-02 signals as events: signal.on delivers Signal records"]:::done
    GTERMIOS["runtime-v2 4 ✅ 2026-09-02 termios: raw/restore, restore a runtime obligation"]:::done
    GFDPASS["runtime-v2 5 ✅ 2026-09-02 fd passing: send_fd/recv_fd/connect_unix"]:::done
    GVTE["VTE grid in pure .wo + unicode width tables (pinned against recorded sessions)"]:::gap
    WMUX["wmux 1 (was language 43): server owns sessions/PTYs in durable tables, thin client hands over its tty — reattach after server RESTART replays from the WAL"]:::product
    TINFO["terminfo fork: parse the db in .wo vs fixed xterm-256color + refusal by name (decide at 43's brainstorm)"]:::later
    TMONO["time.mono returns (status clock, repaint pacing) — v2"]:::later

    I42w --> GSTREAM
    GSTREAM --> GPTY
    GPTY --> WMUX
    GSIG --> WMUX
    GTERMIOS --> WMUX
    GFDPASS --> WMUX
    GVTE --> WMUX
    TINFO -.settled at wmux's brainstorm.-> WMUX
    TMONO -.v2.-> WMUX
```

**The track landed whole on 2026-09-02** — every runtime edge into wmux
is green; what remains for wmux 1 is its own `.wo` work (the VTE grid +
unicode width node) and its brainstorm's terminfo fork. Sibling reuse:
the alacritty Wayland stage reuses GFDPASS + GVTE; the zen CDP driver
now lacks only a WebSocket client; skillhost (28) has its stdin
transport.

## 7. jarvis — the AI-assistant track and everything it waits on

The sixth track ([jarvis](stories/jarvis/00-story.md)): an AI assistant built in
writeonce. **The runtime side is done** — the whole outbound HTTPS path landed
2026-09-09 (runtime-v2 9, both directions, live-gated) and language 41 is fixed.
What jarvis 1 waits on now is purely the **framework**: the developer set the
order *porch first, then jarvis*. So this graph is the porch→jarvis chain.

```mermaid
flowchart TD
    classDef done fill:#1a7f37,color:#fff,stroke:none
    classDef ready fill:#0969da,color:#fff,stroke:none
    classDef refine fill:#eac54f,color:#000,stroke:none

    NC["net.connect (id 110) ✅"]:::done
    TLS["rv2 9 in-process TLS 1.3 ✅ — net.connect_tls / read_tls / write_tls (115–117), net.accept_tls (118)"]:::done
    L41["language 41 cross-shard marshal ✅"]:::done
    WOHTML["wo-html / writeonce-view ✅"]:::done

    RB["random_bytes builtin (porch 2 phase A / lang 39) — surfaces the runtime's getrandom"]:::ready
    P2["porch 2 randomness + cookies (ready)"]:::ready
    P3["porch 3 sessions (ready)"]:::ready
    P4["porch 4 CSRF (ready)"]:::ready
    P5["porch 5 routing + response ergonomics (ready, zero deps)"]:::ready
    P6["porch 6 streaming core (ready)"]:::ready
    P7["porch 7 SSE + compression (ready)"]:::ready
    P8["porch 8 static + lifecycle (ready)"]:::ready
    P9["porch 9 idempotent replay (ready — unblocked by L41)"]:::ready

    J1["jarvis 1 — the chat loop (ready; forks auto-approved, review_pending)"]:::ready
    J2["jarvis 2 — tool use / agent loop (refine)"]:::refine
    J3["jarvis 3 — retrieval (RAG) (refine)"]:::refine

    NC --> TLS
    RB --> P2
    P2 --> P3
    P2 --> P4
    P3 --> P4
    P5 --> P7
    P5 --> P8
    P6 --> P7
    P6 --> P8
    L41 --> P9

    TLS --> J1
    P2 --> J1
    P3 --> J1
    P4 -. CSRF-protects the POST once built .-> J1
    P6 --> J1
    P7 --> J1
    WOHTML --> J1
    J1 --> J2
    J1 --> J3
```

**jarvis 1's dependency list, from the code and the stories (2026-09-09):**

| jarvis 1 needs | for | state |
| --- | --- | --- |
| `net.connect_tls` / `net.read_tls` / `net.write_tls` (runtime-v2 9) | dialing the LLM API over HTTPS, streaming its SSE reply | ✅ landed, live-gated |
| `net.connect` (id 110) | the TCP under it | ✅ landed |
| language 41 fix | actors carrying messages across shards without the double free | ✅ landed |
| `@table` | durable `Conversation` / `Message` history | ✅ exists |
| wo-html / writeonce-view | the chat page | ✅ exists |
| **porch 2** randomness + cookies (needs the `random_bytes` builtin first) | session id + signed cookie | ready, **unbuilt** |
| **porch 3** sessions | the session principal history is keyed to | ready, unbuilt (after 2) |
| **porch 6** streaming core | incremental response writes | ready, unbuilt |
| **porch 7** SSE + compression | token streaming to the browser | ready, unbuilt (after 5 + 6) |
| porch 4 CSRF | protecting `POST /message` (bearer-gated until then) | ready, unbuilt (after 2 + 3) |
| porch 5 routing + response ergonomics | the route surface | ready, unbuilt, zero deps |

**Build order that satisfies it** (the porch critical path to jarvis): the
`random_bytes` builtin → porch 2 → porch 3 → porch 5 → porch 6 → porch 7 (→ porch
4, 8, 9 to complete porch) → **jarvis 1**. Nothing on the runtime side is
outstanding; every remaining edge into jarvis 1 is a porch iteration.

## Maintenance rule

When an iteration or slice lands, update its node's class here in the
same change that moves the board row — the two documents answer
different questions (status vs. edges) and drift kills both.
