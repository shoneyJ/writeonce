# Iteration 19 — the missing scalar types: Float and Bytes

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](../00-story.md).
>
> **Inserted 2026-08-20, forks settled the same day** (developer
> decisions below). Next: spec, then plan — a new storage kind touches
> the `.wob`/WAL formats, so this one earns its written spec.
>
> **LANDED 2026-08-20.** Both scalars shipped full-stack as `.wob` v5. The
> format decisions the story asked a spec for are recorded normatively in
> [`docs/plan/oop-vm/00-wob-format.md`](../../../plan/oop-vm/00-wob-format.md)
> §"v5: Float and Bytes" — written in the same change as the code, per this
> iteration's own last acceptance criterion — with the reasoning-under-the-code
> in `runtime/src/CODE-LOGIC.md` and `compiler/src/CODE-LOGIC.md`. No separate
> spec document was authored; the deviation is deliberate and noted here.
>
> Gates: corpus **103/0** (four new fixtures — `float-arithmetic`,
> `bytes-carrier`, `float-json-storage`, `float-table-column`),
> `test_wal` **156/0** (bit-exact Float/Bytes replay), `just web-app`
> **23/0** with the storefront price a real Float, `just oop-accept` ALL
> CRITERIA MET, and every other sample gate unchanged.
>
> One judgment call worth flagging: the shortest-round-trip renderer prefers
> FIXED notation over exponential across `1e-6 … 1e21`. Pure "shortest" is
> what `%g` does, and it renders a price of `900.0` as `9e+02` — correct and
> useless. Both forms carry the same significant digits, so round-tripping is
> unaffected.

## Why this iteration exists

The scalar inventory is `Int`, `Bool`, `Text` (+`Timestamp`/`Id`,
Int-shaped conveniences) — and `runtime/src/json.c` says the quiet part
out loud: "a fraction or an exponent is MALFORMED for an Int field (the
language has no [float])". Any JSON body containing `1.5` fails decode
today; a web framework that cannot accept `{"price": 9.99}` has a hole,
not a style. Separately, binary payloads (multipart file parts, the
coming WebSocket frames, crypto digests) ride Text-as-bytes — proven to
work, but it blurs every json/interpolation boundary Text has.

## Settled decisions (2026-08-20)

1. **Float, full stack in one iteration**: literal lexing, f64
   arithmetic, an `@table` column kind + WAL slot, json decode/encode
   fidelity (the forcing function), interpolation, `parse_float`. A
   language-only Float would leave the visible blocker standing.
2. **IEEE 754 quiet semantics, exactly**: Float division never traps
   (Inf/NaN flow), `NaN != NaN`, shortest-round-trip printing. Int keeps
   its DIV0 trap — two numeric worlds, NO implicit mixing: `float(i)`
   and `trunc(f)` are the explicit bridges.
3. **Bytes ships alongside**: a distinct binary scalar (len/byte_at/
   slice/compare; base64 and future digests return it; net reads can
   fill it) — Text goes back to meaning text; iteration 24 and the
   crypto fork inherit a clean carrier.
4. **Recorded as iteration 19; spec before code.**

## Surveyed and deliberately NOT added (the rest of the missing-type list)

- **Result/Option** — expressible TODAY: `?T` plus payload unions
  (`type R = Ok(v: Int) | Err(msg: Text)`); nothing to add.
- **Tuples** — records cover them; "no tuples" is standing doctrine.
- **Decimal/Money** — cents-as-Int holds until a workload proves
  otherwise; recorded, not scheduled.
- **Char/Rune + Unicode-aware Text** — its own future story;
  `byte_at`/`char_of` carry today's workloads.
- **Set and the ADT roster** — parked post-12 by the 2026-08-08 scope
  directive, unchanged.
- **Scalar newtypes** (a real Duration/Timestamp distinct from Int) —
  needs `abstract`, which is a reject row; the ms-Int convention stays
  documented instead.

## Goals (draft — the spec refines)

- `1.5`, `-0.25`, `2e10` lex as Float literals; arithmetic/comparison
  lower to IEEE f64 ops (registers are u64 — bitcast, no layout change).
- A `Float` field is a real `@table` column: stored, WAL-logged,
  replayed, indexed comparisons defined (total order with NaN sorted
  last, the one deviation from raw IEEE that an index REQUIRES —
  spec'd loudly).
- `json.decode` accepts fractions/exponents into Float fields and
  `json.encode` round-trips shortest-form; Int fields still reject
  fractions (unchanged strictness).
- `Bytes`: literal-less (built from base64/net/slices), length-carrying,
  content-comparable; `@table` column + WAL; json boundary = base64
  string by convention (spec'd).
- The web-app grows a Float column (a real price) as the living proof.

## Acceptance Criteria (draft)

- **Given** `{"price": 9.99}` posted to the storefront, **when** the
  Float-column model decodes it, **then** insert/query/order-by work and
  the value round-trips json byte-identically.
- **Given** Float edge values (NaN, Inf, -0.0, 1e308), **when** they
  flow through arithmetic, storage, replay, and json, **then** IEEE
  semantics hold end-to-end and the WAL replays them bit-exact.
- **Given** a Bytes value from base64_decode, **when** it is stored,
  compared, sliced, and re-encoded, **then** content survives
  bit-exact and no Text builtin silently accepts it (or vice versa)
  where the spec says types differ.
- Every standing gate stays green; `.wob`/WAL format changes are
  version-bumped and documented in the format doc in the same change.

## Out Of Scope

f32; SIMD; Decimal; numeric literals with underscores; implicit
Int↔Float coercion anywhere (including `==` across the divide);
Unicode; hashing/ordering of Bytes in maps beyond equality (the map key
story stays Int/Text v1).

## Proposed Solution

Spec next: the two kinds' `.wob`/WAL encoding (format version bump),
the IEEE-with-indexed-NaN ordering rule, the json boundaries, the
builtin surface (`float`, `trunc`, `parse_float`, Bytes accessors), and
the web-app price-column proof as the gate. Plan follows the spec.
