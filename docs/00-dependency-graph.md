# Dependency graphs — iterations and framework features

> Companion to [00-status.md](00-status.md) (states live THERE; this page
> carries the edges). An arrow `A --> B` means **A must exist before B**.
> Use it to pick the next implementation: anything whose incoming arrows
> are all green is startable today. Updated 2026-08-20.

## 1. Story iterations

Hard dependencies only — edges that force order. The CHOSEN order among
startable nodes (9e before 8 so the restructure has a baseline; 9c/9d
before 10 goes 9c-first because 9d folds into 9c's plan) lives in
00-status.md's "Implementation order".

```mermaid
flowchart TD
    classDef done fill:#1a7f37,color:#fff,stroke:none
    classDef parked fill:#6e7781,color:#fff,stroke:none
    classDef specd fill:#0969da,color:#fff,stroke:none
    classDef open fill:#eac54f,color:#000,stroke:none

    I7["7 log-watcher proof"]:::done
    I7b["7b inferred GC + mark-sweep"]:::done
    I9["9 database engine"]:::done
    I9b["9b @table + query"]:::done
    I15["15 deps package manager"]:::done
    I16["16 web framework v1 core"]:::done

    I17["17 library kind + internal/ (PARKED, spec+plan ready)"]:::parked
    I18["18 framework v2: transaction{} + cache/flags/jobs (spec APPROVED)"]:::specd

    I9c["9c cross-program tables (half-built)"]:::open
    I9d["9d keypair attach auth (half-built)"]:::open
    I9e["9e durability + throughput baseline"]:::open
    I8["8 shard-actor runtime"]:::open
    I9f["9f io_uring group-commit"]:::open
    I10["10 HTTP service layer"]:::open
    I11["11 fibers"]:::open
    I12["12 blue-green deploy"]:::open
    I13["13 metaprogramming @derive"]:::open
    I14["14 skillhost workload (demoted)"]:::open
    I9g["9g query grammar corpus (likely collapses)"]:::open
    H2C["h2c HTTP/2 cleartext (spec §C)"]:::open

    I15 --> I17
    I16 --> I17
    I9 --> I18
    I16 --> I18
    I9 --> I9c
    I9c --> I9d
    I9c --> I10
    I16 --> I10
    I9b --> I9e
    I7b --> I8
    I9e --> I9f
    I8 --> I9f
    I8 --> I11
    I9 --> I12
    I10 --> I12
    I8 --> H2C
    I9f --> H2C
    I11 --> H2C
    I9g --> I14
    I7 --> I14
```

Reading it: **18 is the only spec-approved open node with all
prerequisites green** — it is the next implementation. After 18: 9c/9d
and 9e are startable in parallel-in-principle (order chosen: 9c/9d
first, half-built branches rot). 13 has no incoming edges but is
scope-directive-parked behind 12. 17 unparks whenever directed — its
prerequisites landed and its spec+plan sit on branch `library-internal`.

## 2. Framework v1 — remaining ledger items

What each open ledger row waits on. Three gates recur: **net seams**
(runtime `net` builtins), the **crypto fork** (the language has no
bitwise operators — hashes become C runtime builtins or bit ops land
first; brainstorm before the slice), and **runtime iterations 8/11**.

```mermaid
flowchart TD
    classDef gate fill:#8250df,color:#fff,stroke:none
    classDef ready fill:#1a7f37,color:#fff,stroke:none
    classDef blocked fill:#eac54f,color:#000,stroke:none

    READY["startable today, pure .wo"]:::ready
    CORS["CORS middleware"]:::ready
    SECH["security-headers middleware"]:::ready
    HOSTV["host validation"]:::ready
    STRICT["strict-parsing audit (dup/conflicting Content-Length)"]:::ready
    WILD["wildcard segments *rest"]:::ready
    PREC["specificity precedence"]:::blocked
    GROUPS["route groups"]:::ready
    CTX["req.ctx bag"]:::ready
    XFF["X-Forwarded-For/-Proto parsing"]:::ready
    ACCEPT["Accept-driven negotiation"]:::ready

    NETSEAM["GATE: net runtime seams (timeouts, unix socket, peer address)"]:::gate
    TMOUT["read/write/idle timeouts"]:::blocked
    UNIX["unix socket binding"]:::blocked
    PEERV["trusted-proxy PEER verification"]:::blocked

    CRYPTO["GATE: crypto fork — C builtins vs language bit ops (brainstorm)"]:::gate
    SHA["SHA-256/512, HMAC, CRC32"]:::blocked
    ETAG["ETag + conditional requests"]:::blocked
    COOKIE["signed cookies"]:::blocked
    CSRF["CSRF"]:::blocked
    SESS["session integrity"]:::blocked
    HOOKV["webhook verification"]:::blocked
    JWT["JWT HS256 (HARD STOP after)"]:::blocked

    RT811["GATE: iterations 8/11 (shards, fibers)"]:::gate
    STREAM["body streaming + backpressure"]:::blocked
    SRESP["streaming responses + commit point"]:::blocked
    CANCEL["per-request cancellation"]:::blocked
    PUBSUB["pub/sub + WebSockets"]:::blocked

    RADIX["radix-tree routing"]:::blocked
    I9E2["9e measures the linear scan"]:::gate

    WILD --> PREC
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
    RT811 --> STREAM
    RT811 --> SRESP
    RT811 --> CANCEL
    RT811 --> PUBSUB
    I9E2 --> RADIX
```

The green column (CORS, security headers, host validation, strict-parsing
audit, wildcards, route groups, `req.ctx`, XFF parsing, Accept
negotiation) needs nothing — each is a framework-v1 slice startable in
any order, gated by `just web-app`.

## 3. Framework v2 (iteration 18) — internal order

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
    TPRMW["txn-per-request middleware (v1 ledger's storage row)"]:::later
    FIBJ["fiber-scheduled jobs"]:::later
    I11B["iteration 11"]:::later

    TXN --> JOBS
    JOBS --> DEMO
    FLAGS --> DEMO
    TXN --> TPRMW
    I11B --> FIBJ
    JOBS --> FIBJ
```

Cache and flags have no dependencies — they can land first as warm-up
slices; `transaction { }` is the critical path (the only engine +
language work), jobs compose on it, the demo and gate close it out.

## Maintenance rule

When an iteration or slice lands, update its node's class here in the
same change that moves the board row — the two documents answer
different questions (status vs. edges) and drift kills both.
