# `tests/corpus/` — how to add a conformance fixture

> The contribution path every later sub-project's corpus (`actor/`,
> `db/`, `lang/`, `sys/`, `sample-logwatcher/`) follows, and the one
> `gc/` (below) already uses. Enforced by
> [`scripts/oop-e2e.sh`](../../../scripts/oop-e2e.sh) (plan 3, Task 2), run
> via `just oop-e2e`. What a `.wo` fixture may actually say is
> [`08-builtin-surface.md`](08-builtin-surface.md)'s contract, not this
> doc's — read that first, or you will write fixtures against the
> compiler's internals instead of its source-language contract and waste
> time chasing `WO-E403`s that were never about your fixture's intent.

## Layout: one directory per fixture, fixed filenames

Every fixture is its own directory under its kind (`run/`, `compile-fail/`,
`trap/`, `gc/`), named for what it exercises (kebab-case, e.g.
`interface-dispatch`, not `test3`). Inside, filenames are fixed so the
harness can walk every kind the same way:

```
tests/corpus/run/<name>/fixture.wo
tests/corpus/run/<name>/fixture.out

tests/corpus/compile-fail/<name>/fixture.wo
tests/corpus/compile-fail/<name>/fixture.code

tests/corpus/trap/<name>/fixture.wo
tests/corpus/trap/<name>/fixture.trap

tests/corpus/gc/<name>/fixture.wo
tests/corpus/gc/<name>/fixture.out
tests/corpus/gc/<name>/fixture.trace
tests/corpus/gc/<name>/fixture.gc_budget   -- optional
```

`scripts/oop-e2e.sh` globs `tests/corpus/<kind>/*/`, so a stray `.wo` file
placed directly inside a kind directory (not in its own subdirectory) is
never picked up — no error, no run, it just silently does not exist as a
fixture. If a fixture stops appearing in the tally, check that first.

**`run/` and `compile-fail/` compile the fixture's own *directory*, not
just `fixture.wo`** (haxe-parity Task 1, modules) — `woc --emit
<fixture-dir> -o <scratch>.wob`, letting `woc`'s own multi-file discovery
find every `.wo` file under it. For a fixture with no other `.wo` file
beside `fixture.wo` (every fixture that predates modules, and the large
majority since) this is behavior-identical to compiling `fixture.wo`
alone. What it's *for*: a module fixture puts its extra module(s) in a
**subdirectory** (`greet/greet.wo`, `secret/secret.wo`, `a/a.wo`, ...) —
each subdirectory is its own module (a `.wo` file's module is its
directory), so this is how a `run`/`compile-fail` fixture exercises a
real `use` across module boundaries at all.

**Guarded, not open season**: exactly one top-level `.wo` file
(`fixture.wo` itself) is required directly inside the fixture's own
directory — `scripts/oop-e2e.sh`'s `assert_one_top_level_wo` counts
`<fixture-dir>/*.wo` (never recursing into subdirectories) and fails the
fixture by name, before compiling anything, if that count isn't exactly
1. Without this, a second `.wo` file dropped loose beside `fixture.wo`
(not a module fixture's intentional subdirectory — a mistake, or worse)
silently joins the compile as a second file in the *same* module (Task
8's own discovery contract: every same-directory file is unconditionally
visible to every other) — confirmed exploitable: an alphabetically-
earlier stray `fn main` hijacks the fixture's own entry point with zero
diagnostics, since free fns are excluded from the cross-file collision
check (`01-error-catalog.md`'s WO-E214 row is classes/interfaces only).
A subdirectory full of `.wo` files is unaffected by this guard — that's
a different module by construction, exactly the shape a module fixture
is supposed to have.

## `run/` — compiles, runs, exact stdout

**Files:** `fixture.wo`, `fixture.out`.

**Rule:** `woc --emit fixture.wo -o <scratch>.wob` must exit 0, then
`wovm <scratch>.wob` must exit 0 with stdout **byte-for-byte identical**
to `fixture.out` — trailing newline included, since `print`/`print_int`
are newline-terminated (`08-builtin-surface.md`). No substring match, no
trimming. Generate `fixture.out` by actually running the fixture, not by
hand-typing what you expect the output to be:

```sh
just woc-build   # compiler/_build/default/bin/woc
make -C runtime wovm
compiler/_build/default/bin/woc --emit tests/corpus/run/<name>/fixture.wo -o /tmp/f.wob
runtime/wovm /tmp/f.wob > tests/corpus/run/<name>/fixture.out
```

Then read `fixture.out` back and sanity-check it says what you meant —
a byte-exact copy of a wrong run is still wrong, just consistently so.

The four seed fixtures (`hello`, `arithmetic`, `methods`, `interface`)
cover: `print`/`print_int`; arithmetic and control flow, including the
two operators the v1 instruction set lowers rather than gives an opcode
(`%`, `!=`); a direct method call (`CALL` by method index, receiver's
declared type is a concrete class); and structural interface dispatch
(`ICALL` by vtable slot, receiver's declared type is an interface, no
`implements` keyword). Look at these before writing a new one — they are
proof that a given construct actually round-trips through the real
`wovm`, not just through `--dump-bc`.

## `compile-fail/` — must fail with exactly one code

**Files:** `fixture.wo`, `fixture.code`.

**Rule:** `fixture.code` names exactly one diagnostic code (whitespace is
stripped, so `WO-E215` on its own line is enough). `woc --emit fixture.wo
-o <scratch>.wob` must exit 1 with that code appearing in stderr. Exit 0
(compiled clean), exit 2 (a usage/IO failure, not a diagnostic), or exit 1
with a *different* code are all failures — the harness names which.

Use `--emit`, not the bare `woc <path>` check-only form, when hand-testing
a fixture: `--emit` runs the full pipeline including the emitter, so it
also catches `WO-E4xx` cases (register budget, unlowerable constructs)
that check-only mode never reaches. `scripts/oop-e2e.sh` always uses
`--emit` for this kind for the same reason.

Every code in [`01-error-catalog.md`](01-error-catalog.md)'s main tables
is a legitimate `compile-fail/` target; the codes under "Reserved, not yet
emitted" are not — there is no call site to trigger them yet.

## `trap/` — must compile, then trap with exactly one code

**Files:** `fixture.wo`, `fixture.trap`.

**Rule:** `fixture.trap` names exactly one integer trap code (again,
whitespace-stripped). `woc --emit` must exit 0 (a `trap/` fixture that
fails to *compile* is a `compile-fail/` fixture wearing the wrong hat —
move it). Then `wovm <scratch>.wob` must exit 1, with stderr's one fixed
line

```
trap CODE in METHOD at line L: MESSAGE
```

giving exactly the `CODE` named in `fixture.trap`. Exit 0 (ran to
completion instead of trapping), exit 2 (a loader rejection — the image
was malformed, not merely trapped at runtime), or exit 1 with a different
`CODE` are all failures.

## `gc/` — must compile, run to completion, and drive the collector exactly

**Files:** `fixture.wo`, `fixture.out`, `fixture.trace`, optionally
`fixture.gc_budget`.

**Rule:** `woc --emit` must exit 0, then `wovm <scratch>.wob` must exit 0
with `WO_GC_TRACE=1` set (and `WO_GC_BUDGET` set from `fixture.gc_budget`
if the fixture has one). Two things are then checked exactly, both
generated by actually running the fixture, never hand-typed:

- **stdout**, byte-for-byte against `fixture.out` — same rule as `run/`.
  This is the fixture proving it executed the intended shape (e.g. a
  container's element count) before anything is abandoned.
- **the gc pump's stderr trace**, against `fixture.trace`. The pump
  (`runtime/src/main.c`) prints one `gc: step N budget=B freed=F
  visited=V remaining=R` line per collection step; `fixture.trace` names
  the exact total step count and the exact total freed count across every
  step, as two `key=value` lines:

  ```
  steps=1
  freed=2
  ```

  The harness counts `^gc: step ` lines in stderr for `steps=`, and sums
  every step's `freed=` value for `freed=`. A wrong count either way — an
  object freed that should have survived, one that should have been
  freed but wasn't, or a sweep that didn't slice the way the fixture's
  budget says it should — is a named failure, exactly like a wrong
  `WO-E###` or trap code.

**Why not assert via ASan/LeakSanitizer instead:** a sanitizer *is* how
each `gc/` fixture was actually verified (see below) and is the right
tool for proving a freed object was genuinely freed, not recycled inside
the arena's own freelist where nothing external can observe it. But
LeakSanitizer's leak scan is conservative — it can find a stray bit
pattern in the VM's own register file that happens to alias a live heap
address and treat an object as "reachable" that the collector's own
bookkeeping would not — so its *exact* output is not stable enough to
assert byte-for-byte in an automated regression gate. The trace's
`steps=`/`freed=` counts come straight from the collector's own
accounting (`wo_gc_step`'s return value and the public cycle-candidate
buffer length in `runtime/src/obj.h`), so they are exactly reproducible;
running the whole corpus under an ASan+UBSan `wovm` (`make -C runtime
wovm-asan`) is a supplementary, manual check, not something
`scripts/oop-e2e.sh` automates.

**Retired: `gc/held-cycle` (milestone-1 criterion-3 closure).** An earlier
fixture returned a `@gc` cycle from `main` to model an *externally held*
cycle — a root the pump must not collect. It could never actually prove
that: the program entry's return value is the process exit code
(`docs/superpowers/specs/2026-08-01-systems-track-design.md:70`), and
`runtime/src/main.c` never releases it, so the fixture's "hold" was
really a permanent, un-freeable refcount inflation — indistinguishable
from a leak, and confirmed as exactly that: `runtime/build/wovm_asan`
reported it as a genuine LeakSanitizer definite leak. `WO-E405`
(`compiler/src/emit.ml`, `01-error-catalog.md`) now rejects a non-`Int`
entry return type at compile time, which makes the fixture's own
premise inexpressible — a post-exit pump has no live roots once the
entry returns, by construction, so an *externally held* cycle cannot be
modeled from inside a `.wo` program at all. The scenario this fixture
meant to cover — a cycle kept alive by a real external root — is
already covered properly by
`test_externally_held_cycle_survives_then_dies` in
`runtime/test/test_cycle.c`, which holds its root the honest way (a C
local variable, not a leaked return value). Story iteration 7b (tracing
GC design) schedules a proper in-flight fixture for this shape once the
runtime has a way to express an external root without going through
`main`'s return. `gc/abandoned-cycle` and `gc/budget-steps` are
unaffected — neither depends on an externally-held root.

**Why a `multi` field, not a plain `@gc`-typed field, closes the cycle:**
milestone-1 has no nil literal and a constructor literal requires every
field, so two classes that mandatorily reference each other can never
be built — whichever is constructed first needs an instance of the
other that does not exist yet. A `multi` field sidesteps this: it starts
empty (`multi_new()`), so both objects can be constructed *before*
either references the other, and `push` closes the cycle afterward. This
is also why fixture classes carry ~130 `Int` filler fields alongside the
one `multi` field that matters — an object under 1024 bytes
(`WO_ARENA_MAX_CLASS`, `runtime/src/obj.h`) allocates through the arena's
own bump/freelist, where a sanitizer can never observe its free; over
that size, `wo_arena_alloc` routes to plain `malloc`, which is what lets
ASan prove the frees `runtime/test/test_cycle.c` already relies on the
same way (`BIG = 130`).

## Why exact-match, not substring or "any failure"

A fixture that merely checks "did *something* go wrong" degrades silently
the day the front end starts failing for the *wrong* reason — the fixture
stays green while the bug it was written for comes back under a different
code path. Naming the exact code (`WO-E###` or trap `N`) means a
regression that changes *which* diagnostic fires is caught exactly as
reliably as one that stops firing at all.

## Running the harness

```sh
just woc-build             # builds compiler/_build/default/bin/woc
make -C runtime wovm       # builds runtime/wovm
just oop-e2e               # walks the corpus, one line per fixture, a final tally

make -C runtime wovm-asan  # optional: builds runtime/build/wovm_asan, for
                            # manually re-running gc/ fixtures under ASan+UBSan
```

`just oop-e2e` fails loudly and names the missing binary (and the command
to build it) if either prerequisite above hasn't been built — it does not
build them for you. `wovm-asan` is not one of those prerequisites: the
automated harness runs every kind, `gc/` included, against the plain
`wovm`; the sanitizer build is a manual supplementary check (see `gc/`
above).
