# The writeonce principles

The doctrine in one page. Every design argument in this repo eventually
lands on one of these thirteen; later documents link here instead of
re-arguing them. Each principle: what it is, why it holds, where it is
enforced.

## 1. One binary is the whole system

The application, the database, the API, and (later) the UI ship as a single
deployable — there is nothing else to install, operate, or version-skew.
*Why:* the assembled-stack tax (app + DB server + proxy + glue) is the
problem writeonce exists to delete.
*Enforced by:* [`01-problem.md`](01-problem.md), the single-binary story in
[the OOP spec](superpowers/specs/2026-08-01-oop-compiler-vm-design.md).

## 2. Zero dependencies — kernel primitives only

The runtime is C on libc; the compiler is OCaml on its stdlib; everything
else is epoll/io_uring, inotify, eventfd, signalfd, sendfile, mmap. The
kernel is the framework.
*Why:* every dependency is a supply chain, an upgrade treadmill, and a
black box in the one binary that must be understood end to end.
*Enforced by:* [the kernel-primitives catalogue](plan/exploration/linux/00-linux.md),
the dependency doctrine in [the OOP spec](superpowers/specs/2026-08-01-oop-compiler-vm-design.md).

## 3. Memory safety without a GC tax

Objects are owned values: one owner, moves on assignment, second-class
borrows checked mostly at compile time (mutable value semantics — the
Rust-borrow shape without lifetime inference). GC-ness is **inferred** by
the compiler (iteration 7b): a class in a reference cycle, or one whose
values must escape as long-lived aliases, is traced by an incremental
per-shard tri-color mark-sweep collector in budgeted slices — the developer
writes no memory annotation, and no global pause exists by construction.
*Why:* deterministic memory for the default case, aliasing freedom where
the design wants it, and never a stop-the-world in a runtime that is also
the database.
*Enforced by:* [the OOP spec §4](superpowers/specs/2026-08-01-oop-compiler-vm-design.md).

## 4. No inheritance, ever

No `extends`, no `override`, no virtual hierarchies. Is-a is a tagged
union; has-a is composition; polymorphism is structural interfaces.
*Why:* hierarchies fossilize early guesses and make dispatch, ownership,
and diagnostics all harder; composition keeps every unit flat and movable.
*Enforced by:* [the OOP spec](superpowers/specs/2026-08-01-oop-compiler-vm-design.md),
the reject rows of [the systems-track verdict table](superpowers/specs/2026-08-01-systems-track-design.md).

## 5. Thread-per-core shards; ownership moves, data never shares

One pinned worker per core, each owning its engine, heap, and event loop.
Cross-shard work is a message send that moves ownership. There is no
`Arc<Mutex<…>>` anywhere and never will be.
*Why:* sharing mutable state buys contention, locks, and heisenbugs;
moving ownership buys linear scaling and per-shard GC.
*Enforced by:* [plan 09](plan/09-concurrency-scaleout.md) (shipped on the
Rust runtime), [the shard-actor plan](superpowers/plans/2026-08-01-shard-actor-vm-runtime.md).

## 6. The runtime never stops

The executable is a systemd service that deploys without restarting: two
VM slots (Blue/Green), in-runtime compile of an approved proposal, atomic
dispatch switch, previous version resident for instant rollback — and the
binary embeds its own source, so prod is always self-describing.
*Why:* restarts drop connections, dump caches, and turn deploys into
events; a database that is also the app must not blink.
*Enforced by:* [the blue-green spec](superpowers/specs/2026-08-03-blue-green-vm-design.md).

## 7. RAM is authoritative; the WAL makes it durable

All reads serve from memory. Every mutation is WAL-logged and fsynced
before acknowledgment; boot replays the log. Mirrors (Postgres) are
reconstructible backups that reads and acks never depend on.
*Why:* one source of truth with predictable latency; durability is a
sequential append, not a storage engine bolted to the side.
*Enforced by:* [plan 11](plan/11-wal-and-recovery.md),
[plan 16](plan/16-postgres-mirror.md) (mirror-is-backup doctrine).

## 8. Samples force the grammar

Language features exist when a sample program exercises them; the examples
directory is the de facto integration suite, and new surface is proven by
re-expressing real workloads (blog, ecommerce, pricing, log-watcher).
*Why:* grammars designed in the abstract grow features nobody needs and
miss the ones real programs demand.
*Enforced by:* [the blog sample](examples/blog/README.md),
the sample-workload acceptance in [the systems-track spec](superpowers/specs/2026-08-01-systems-track-design.md).

## 9. Linux is the target

Not POSIX, not portable-someday: Linux syscalls, Linux fd semantics,
systemd as the process manager. Portability abstractions are refused.
*Why:* targeting one kernel lets the runtime use its sharpest primitives
directly instead of the lowest common denominator.
*Enforced by:* [the kernel-primitives catalogue](plan/exploration/linux/00-linux.md).

## 10. Capabilities are typed builtins — no FFI

Programs reach the system only through audited stdlib builtins (`fs`,
`proc`, `net`, `time`, `json`): bounded reads, args-array-only process
runs, handles that close on drop. There is no `extern`, no escape hatch.
*Why:* one FFI hole voids the entire memory-safety and security story;
typed capabilities make the safe path the only path.
*Enforced by:* [the systems-track spec Parts 2–3](superpowers/specs/2026-08-01-systems-track-design.md).

## 11. Plain diagnostics are the product

Stable `WO-E###` codes, `file:line:col`, source excerpts, ownership errors
naming both sites, many errors per run.
*Why:* mutable value semantics only beats Rust ergonomics if the errors
read like sentences; the compiler's error text is a first-class feature.
*Enforced by:* [the OOP spec §6](superpowers/specs/2026-08-01-oop-compiler-vm-design.md),
[the compiler architecture doctrine](plan/compiler/architecture.md).

## 12. The runtime is a recipe box

Transports, fibers, routing, subscriptions, the DB engine, deploy
machinery — each stays a separable capability. A web framework or a custom
database experience is a `.wo` library composing them; the runtime itself
stays framework-agnostic.
*Why:* the next stories (web framework, richer database surfaces) must be
buildable *on* the runtime without forking it.
*Enforced by:* [the blue-green vision §2](plan/exploration/blue-green-vm/00-vision.md).

## 13. Statically typed, all the way to the register

Every slot's type is known at compile time: no `Dynamic`, no `untyped`, no
`cast`, no runtime reflection. The VM runs untagged 64-bit registers
because the compiler already knows; JSON enters through checked decodes
(`as T` yielding `?T`), never through dynamic objects.
*Why:* the type system is the foundation the untagged VM, the borrow
checker, and the annotation ORM (`@table` classes, `ref`/`multi`
relations) all stand on — one dynamic hole collapses all three. The Haxe
reference workload shows the alternative: its transcompiled C++ pays a
hashed `__Field` lookup on every typedef access.
*Enforced by:* the `Dynamic`/`untyped`/`cast` reject rows of
[the systems-track verdict table](superpowers/specs/2026-08-01-systems-track-design.md),
untagged registers in [the OOP spec §5](superpowers/specs/2026-08-01-oop-compiler-vm-design.md).
