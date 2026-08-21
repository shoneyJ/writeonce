# Discarded — settled rejections

Decisions that were considered and **rejected**, with the reason. This file
exists so a settled question is not re-proposed. If you want to revisit an
entry, argue against the reason recorded here — do not re-open it as if it were
new.

Status board: [`00-status.md`](../00-status.md) · Doctrine: [`../00-principles.md`](../00-principles.md)

## Language surface

| Rejected | Date | Reason |
| --- | --- | --- |
| **Inheritance** — `extends`, `super`, `override`, `implements` | plan 13 / OOP spec | No hierarchies, ever. Is-a is a tagged union, has-a is composition, polymorphism is structural interfaces. Hierarchies fossilize early guesses and make dispatch, ownership, and diagnostics all harder. Principle 4. |
| **`abstract` newtypes** (`abstract Money = Int`) | 2026-08-10 | Flipped **adopt → reject** in the systems-track verdict table. A distinct scalar type adds a conversion surface without buying safety this language needs; domain scalars stay plain `Int`/`Text`. The `Money`/`SKU` stopgap allowlist that stood in for the unbuilt feature proved the cost was real. Haxe-parity's abstract+`is` task was deleted outright — `abstract` rejected here, `is` cut for zero uses in the driving workload. |
| **`Money`, `SKU` as language types** | 2026-08-10 | Removed from `builtin_scalars` and from the abstract allowlist. `Money` → `Int` (minor units), `SKU` → `Text` everywhere. |
| **`Float` as a builtin scalar** | 2026-08-10 | Phantom: it was in `builtin_scalars` and documented as f64, but `token.ml` has no float literal and `wob.h` has no float kind — `ratio: Float` typechecked while no `Float` value could ever be written or represented. Re-add only when literals **and** a wob float kind land together. |
| **`Dynamic` / `untyped`** | systems-track spec | Static typing is the untagged-register VM's foundation. Typed `json.decode … as T -> ?T` covers the real use. Principle 13. |
| **`cast`** | systems-track spec | No unsafe casts. Conversions are typed; `as` exists only in the decode-target position. |
| **`macro`** | systems-track spec | Kills the fast-compile promise. Codegen belongs to `wo gen` tooling. |
| **`extern` / FFI** | systems-track spec | One FFI hole voids the whole memory-safety story. Capabilities are audited typed builtins. Principle 10. |
| **`operator` overloading, `overload`** | systems-track spec | One name, one signature. Keeps dispatch and diagnostics simple. |
| **`inline` functions** | systems-track spec | Optimization is the compiler's job. `const` compile-time values are adopted; inline *functions* are not. |
| **User-definable generic containers** | OOP spec §3 | `multi` and `map` are runtime-provided native classes implemented in C. The VM picks the backing data structure; the language exposes only the ADT's operations. |

## Compiler / VM architecture

| Rejected | Reason |
| --- | --- |
| **AOT compilation to C** | Kills hot reload and makes builds slow. A register bytecode interpreter with computed-goto dispatch ships first; a JIT stays possible later. |
| **Approach B — "Lua-shaped minimal" VM** | Defers the project's core risk (the hybrid borrow VM) and adds a Menhir dependency. |
| **Approach C — "Rust-lite static regions"** | Research-grade complexity; recreates exactly the Rust ergonomics pain that mutable value semantics exists to avoid. |
| **Menhir, ppx, any opam package** | OCaml stdlib only; handwritten lexer and recursive-descent parser. dune is a build runner, nothing more. |
| **First-class borrows** (storable, returnable) | Second-class borrows (Hylo/Val mutable value semantics) eliminate full lifetime inference — the compiler needs only per-function flow analysis. Long-lived links go through `ref T` ids or `@gc`. |
| **Empty-list stub for `is_abstract_type`** | 2026-08-10: a function that can never return true is dead code. Deleted outright instead. |

## Runtime / deployment

| Rejected | Reason |
| --- | --- |
| **`Arc<Mutex<Engine>>` / any shared mutable engine state** | Thread-per-core shards each own their engine, heap, and event loop; cross-shard work is an ownership-moving message send. Principle 5. |
| **External deployer daemon** | Violates two-VMs-in-one-runtime and splits the data plane. The management plane is a runtime module with a thin CLI. |
| **Self-hosted `.wo` deploy logic** | Bootstrap problem — the deploy path cannot be written in the language whose deployment it implements. Recorded as a much-later possibility. |
| **Script-based / destructive schema migrations (v1)** | Additive-only auto-diff is what makes rollback unconditional: the previous version ignores fields and classes it never knew. Destructive changes reject at propose time. |
| **Portability abstractions over Linux** | Targeting one kernel lets the runtime use its sharpest primitives directly instead of the lowest common denominator. Principle 9. |
| **Mirrors on the commit path** | The Postgres mirror is a reconstructible backup. RAM is authoritative; reads and acks never depend on it. Principle 7. |

## Process / docs

| Rejected | Reason |
| --- | --- |
| **Per-example `principle.md` files** | 2026-08-08: one canonical repo-level [`docs/00-principles.md`](../00-principles.md) instead; examples link to it. |
| **Minimal 3-file log-watcher sample** | Breaks the file-for-file `.hx` → `.wo` mapping and leaves the "could not express" column unproven — which is the sample's entire acceptance criterion. |
| **Raw code in plan documents** | Plans carry concept, reason, and required behavior in words; the executor writes the code. |
| **`##ui` / `.htmlx` LiveView frontend track** | 2026-08-17: removed the 9-doc `exploration/ui/` design set, the `14-mvc-ui-implementation` plan, and the `ui-htmlx-live` plan. All were built on the non-advancing Rust runtime (`.dev/reference/crates/wo-htmlx`, `cargo run`, WebSocket live-patches) and contradict the current woc/wovm direction. The 13d pricing-UI row went with them. Revisit only if a UI story is re-opened on the woc/wovm stack. |
| **Old-runtime "front door" + v1 design docs** | 2026-08-17: removed `writeonce-pl.md`, `runtime/wo-language.md`, `future-scope/ai-agents-content-management.md`, the numbered v1 set `02-recovery`/`03-data`/`04-ui`/`05-datalayer`/`06-markdown-render`/`07-ssl`, and `runtime/database/05-go-sdk.md`. They pitched the old Rust `wo` runtime (REST + LiveView + SQL/Cypher) as the current language and contradicted the shipped woc/wovm toolchain. |
| **The 2026-08-01 shard-actor plan (epoll-based)** | 2026-08-21: [`superpowers/plans/2026-08-01-shard-actor-vm-runtime.md`](../superpowers/plans/2026-08-01-shard-actor-vm-runtime.md) marked discarded, file kept as reference. Superseded by the arc plan of record ([`2026-08-20-shard-fiber-arc.md`](../superpowers/plans/2026-08-20-shard-fiber-arc.md), stages 1+2 landed): io_uring is a MUST and the epoll-based approach is discarded — the old plan's "epoll now / io_uring later" premise is inverted, and its substrate (`runtime/wo-rt.c`) left with the Rust track 2026-08-18. |
| **The entire Rust `wo` runtime track** | 2026-08-18: removed `crates/` (the Stage-2 Rust runtime), `Cargo.toml`/`Cargo.lock`, `prototypes/` (wo-rt-c stale duplicate + wo-db C++ ref), the `rt-c-*` justfile recipes, the Rust engineering plans (`docs/plan/05..16`, `docs/plan/done/`), and `docs/runtime/` (the old runtime overview + 7-phase DB design series + async/fibers/gc/surreal essays). It was the prior, abandoned architecture — fully independent of the woc/wovm stack. Master now reflects only the current single-language project; the removed track lives in git history if ever needed as reference. Kept: the syscall/postgres/assembly/c-runtime **exploration studies** (they fed the current C runtime) and the discarded/learnings registers. |
