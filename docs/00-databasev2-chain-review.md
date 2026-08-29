# databasev2 — chain and dependency review

Reviewed 2026-08-29 against story frontmatter, the track index's sequence
table, and the code as it stands on `dev`. Six findings, ordered by how much
damage each could do if acted on.

## The recorded picture

`chain` is a **cross-track** field (positions 1–6, defined in
`docs/stories/board-views.md`), not a databasev2 one. Only two databasev2
iterations carry it:

| chain | Story | status |
| --- | --- | --- |
| 1 | language 08 shard-actor runtime, 11 fibers | done |
| 2 | language 22 durability/throughput/scale | done |
| 3 | language 31 actor lifecycle, 40 shutdown drain | done |
| 4 | language 24 chat/websocket workload | done |
| 5 | **databasev2 4** io_uring group commit | in-progress |
| 6 | **databasev2 3** WAL checkpoint | done |

The track's own sequence lives in `00-story.md` as a Needs column plus an
ASCII graph. The two disagree with each other, with the chain field, and with
what happened.

## Finding 1 — the graph contradicts the chain field and the history

`00-story.md` draws `3 ──▶ 4`: iteration 3 before iteration 4. The chain field
says the opposite — iteration 4 is chain 5, iteration 3 is chain 6, so 4 comes
first. History settles it: **4's part A landed 2026-08-28, 3 landed
2026-08-29.** The chain field and the history agree; the graph is wrong.

Worth fixing rather than shrugging at, because the graph is the artefact
someone reads when choosing what to start.

## Finding 2 — the graph contradicts its own prose about direction

The order rationale states "**3 and 4 matter to 2**" — that is, 2 depends on 3
and 4. The graph draws an edge *from* 2 *to* 3, which reads as the reverse.
One of the two is backwards, and the prose is the one that matches the code:
`resident: keys` needed the checkpoint, not the other way round.

## Finding 3 — a retired path is still drawn

The graph still shows `2 ──▶ 5 ──▶ 6`. The 2026-08-27 amendment directly below
it says iteration 6 is largely superseded by 2, and that **5 is no longer a
prerequisite for anything on the critical path**. The prose retired the path;
the picture kept it.

## Finding 4 — the chain metadata omits the iteration doing the work

Iteration 2 is on the critical path, is `in-progress`, and is where 5c/5d just
landed — and it carries **no `chain` field**. The board-views query "the
concurrency chain, in execution order" filters `WHERE chain`, so iteration 2 is
invisible to it. Either 2 belongs on the chain and should say so, or the chain
is genuinely a concurrency artefact that databasev2 2 sits outside — in which
case 3 and 4 carrying it while 2 does not deserves a one-line explanation.

## Finding 5 — iteration 3's hazard section is stale, and was incomplete

This is the one with teeth.

`03-wal-checkpoint.md` carries a "Hazard: compaction invalidates every
`resident: keys` offset" section and a matching Outstanding entry. Both are now
**stale**: the Outstanding entry says "**Nothing fails today** because
iteration 2's storage half is unimplemented", which stopped being true when
5c/5d landed (`125bd09`, `08abd09`, `0c97fa4`, `f606fc9`). The hazard also
offers two shapes and says "the first is almost certainly right" — the first
*was* implemented, and the section should now record that as settled rather
than as an open fork.

More importantly, **the recorded hazard named only half the danger.** It
described stored offsets becoming wrong: a pointer into a rewritten file.
Implementation found a second, worse failure it did not anticipate — compaction
walked the slab **bitmap**, and a keys-resident row has no bitmap bit, because
its slot is returned to the free list when the payload is dropped. Every such
row would therefore have been **omitted from the new log entirely**. That is
silent data loss, not a bad pointer, and no amount of offset-rebuilding would
have caught it.

Both failure modes are now pinned by `test_keys_resident_survives_compaction`,
which rewrites rows in hash order so offsets genuinely move and a missing
re-point cannot pass by luck.

## Finding 6 — the coupling is now bidirectional, and undocumented in that direction

The docs record 2 depending on 3. After 5d, **3's own deliverable depends on
2's API**: `wo_wal_compact` in `database/src/wal.c` now calls
`wo_row_next_id`, `wo_row_offset1`, `wo_row_set_offset` and
`wo_table_is_keys_resident` — all iteration 2 surface. Compaction can no longer
be described as a pure file operation, which is exactly what the hazard section
predicted and no dependency table records.

Minor, same family: iteration 6 is "largely superseded" and to be revisited
"only with a measurement showing the page cache insufficient" — a hold
condition — yet its status is `pending` while genuinely parked iterations 8, 9
and 10 are `hold`.

## What is actually blocked

Nothing in databasev2 is blocked on anything else in databasev2. Iteration 2's
remaining tasks 6 and 7 depend only on iteration 2. Iteration 4's part B is not
blocked either — it is unstarted with an invalidated premise, which is a
re-brainstorm, not a dependency.
