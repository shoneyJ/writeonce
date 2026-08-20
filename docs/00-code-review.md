# Code Review: log-watcher Compilation Requirements

This document was a gap analysis of what the `woc` front end needs before the
log-watcher sample compiles. Its findings were extracted on 2026-08-10 into
[`superpowers/specs/2026-08-10-logwatcher-gap-closure-design.md`](superpowers/specs/2026-08-10-logwatcher-gap-closure-design.md)
and the plans it amends; its Phase 1–4 roadmap is retired in favour of the
approved story iterations. See [`00-status.md`](00-status.md) for current
status.

Native speed — the big one. Everything is interpreted: ~40× behind Go on raw compute, no JIT, no AOT-to-native. The scheduling primitives win benchmarks; a compute-bound handler loses them all back. There is also no Float type at all (the storefront prices in cents for a reason), no SIMD story, and fixed interpreter ceilings (4096 register slots, 256 frames, ~42 KiB per fiber until growable contexts land).

- Language expressiveness. No generics — the cache stores Text and tells you to json.encode; no function values or closures (doctrine, but it's why every handler is a class with one method); byte-based strings with no Unicode awareness; no Result-style error values (traps + try only); pattern matching is a switch, not destructuring. Some of this is deliberate rejection, but "deliberate" doesn't make the expressiveness appear.
- Concurrency holes the arc hasn't closed. send is one-way — no reply/request-response primitive (my own benchmarks couldn't await the actor and had to sleep); no supervision, links, or actor death (actors live until process end); unbounded mailboxes with zero backpressure; no timers beyond sleep; round-robin placement with no work stealing; multi-shard DB access still traps (stage 3 unbuilt); accept lives on one shard.
- Production plumbing. No TLS anywhere (proxy-mandated forever), no HTTP/2 or WebSockets yet, no crypto primitives (blocked on the bit-ops-vs-builtins fork), observability is print/stderr — no metrics, tracing, or profiler; no debugger, no LSP (discussed, never built); deps are git-rev-only with no registry, no transitive resolution, no semver; blue-green deploy and schema migrations are recorded futures, not features.
- Proof maturity. 22's benchmark battery has never run — every number so far is a scratch measurement on one machine; TSan covers one demo; no fuzzing, no CI beyond local just, and the whole ecosystem is one framework, five samples, and one committed consumer. The honest summary: the architecture is ahead of the product — the doctrine bets (ownership+inference, actors, one binary, io_uring) are landing and measurable, while the surface a developer touches daily (types, tooling, ecosystem) is years behind the languages it benchmarks against.
