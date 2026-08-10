# Learnings — what attempts taught

What the work actually taught, independent of whether it shipped. Recorded so
the same wall is not hit twice. Newest first within each section.

Status board: [`00-kanban.md`](00-kanban.md) · Rejections: [`discarded.md`](discarded.md)

## Testing and verification

**Plumbed is not enforced — and a status table will happily claim otherwise.**
`?T` passed every lexer, parser, and dump test while its semantics did not
exist: `WO-E211`/`E212`/`E213` were declared and never emitted, so
`fn take_it(b: Box) -> Int { return b.v; }` with `v: ?Int` exited **0**. The
plan doc had marked the typechecker ✅. Assert on *behavior* — did the
diagnostic fire, what was the exit code — never on the presence of plumbing.
(2026-08-10, nullable-types audit.)

**A declared error-code constant is not a feature.** Ten `WO-E2xx` constants
existed in `types.ml` with no emission site anywhere, including
`unsatisfied_interface` — so structural interface satisfaction was unenforced
while the module's own doc comment claimed the satisfaction set was produced.
The error catalog now lists emitted and reserved codes separately.

**A golden test can pass vacuously.** The first diagnostic-ordering fixture put
both errors in the same pipeline stage, so the collector's insertion order
already equalled the required output order — the fixture would have passed with
the sort deleted. A fixture must **fail** when its mechanism is removed; prove
that by removing it once. The replacement splits the errors across stages so
insertion order is the reverse of output order.

**Exit-0-with-wrong-output is the worst failure mode, and only absence-testing
catches it.** Two instances in one plan: a class field named `on`, `service`, or
`policy` was silently absorbed by the skip-on-block dispatch (field gone, exit
0, empty stderr), and a dangling backslash at EOF inside a string was swallowed
with no diagnostic. Both were found by review, not by the suite, because no test
asserted that something *should* appear.

**Make memory correctness machine-checked.** Test classes given ~130 fields
exceed the arena's 1024-byte size-class ceiling and take the malloc path, so any
missed free becomes a hard ASan report instead of an invisible slop. Used
throughout the VM's drop, RC, cycle-collector, and trap-unwinding tests.

**A blind `bless` absorbs regressions.** `WOC_BLESS=1` rewrites every
`.expected` in every stage, not just the fixture you were thinking about. Prefer
hand-editing a predictable golden — then a passing test *proves* the prediction
— and if you do bless, diff the changed-file list against what you intended.

## Analysis and design

**"The runtime check will catch it" is false if nothing reaches the runtime.**
A double-`mut` reached through let-bound aliases (`let r = bag.items[i]; let s =
bag.items[k]; swap(r, s)`) escaped the static check *and* produced no residual
site — so the VM's borrow word, which only guards sites the table names, was
never engaged. Fixed by canonicalizing places through borrow bindings before
asking the aliasing question. Lesson: when a hybrid design defers a check to
runtime, verify the deferral actually lands in the table that drives it.

**Removals that look load-bearing may not be.** `owner.ml`'s
`is_abstract_type` call sat in a branch returning `Copy` — and its `else` branch
already returned `Copy` for unknown names, so deleting it was provably
behavior-neutral. Read the fallthrough before assuming a call site matters.

**Conservative joins leak.** Marking a conditionally-moved value `Moved` at an
`if`-join is safe against double-free but drops it from every later drop set, so
the not-moved path leaks — which an ASan gate would have caught only in plan 3.
Normalizing instead (drop at the non-moving branch's end) keeps the table shape
and costs only an earlier death on that path.

**Contract notes must live where the consumer will read them.** Two obligations
for the bytecode emitter — coalesce borrow guards per operand, and the
conditional-move drop rule — were first disclosed only in agent report files
that plan 3 will never open. They now live in `dump.ml`'s format-contract
comments beside the tables they constrain.

## Process and tooling

**Check that a new document is actually tracked.** `docs/plan/oop-vm/` was
swallowed by a blanket `docs/plan/` ignore rule, so the error catalog — a
normative compiler↔VM contract both plan tracks cite — existed only on local
disk and appeared in no diff. `.gitignore` now carves that directory back out.

**A status board that covers one track hides the other.** The kanban tracked
only the four Rust-runtime tracks while the entire OOP track (VM core shipped,
compiler front shipped) was invisible on it — so "what is next?" required
reading code and ledgers. Hence the six-bucket format and the next-plan pointer
at the top of the board.

**Prose reports are not a handoff.** Agent-written reports and SDD ledgers hold
the reasoning, but only files a developer opens by habit — the board, the plan,
the format contract — actually transfer it.

## Runtime, from the C proving ground

**Reference-implement first in C, then port.** The C proving ground (phases A–F)
hit 859k reads/s and 618k durable commits/s, and found the ack-ordering and
fd-ABA bugs the Rust port then avoided entirely. Building the risky thing twice,
cheaply first, was faster than building it once carefully.

**Budget the collector, don't stop the world.** Per-shard heaps plus per-shard
cycle-candidate buffers mean no global pause can even be expressed — the
worst case is a bounded slice of one shard's tick. Deferring all frees until
after the trial-deletion phases removed every dangling-candidate hazard that
incremental freeing had introduced.

**Validate once at the trust boundary, then trust it.** The `.wob` loader
bounds-checks every index and copies everything out into aligned structures, so
the interpreter's hot loop carries no static checks at all. Only the checks that
*cannot* be static — the borrow word, runtime-indexed bounds, map keys — remain,
and those always trap rather than corrupt.
