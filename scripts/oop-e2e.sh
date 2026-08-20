#!/usr/bin/env bash
# scripts/oop-e2e.sh — the conformance harness (plan 3, Task 2).
#
# Walks tests/corpus/{run,compile-fail,trap,gc}/*/ and enforces one exact
# outcome per fixture kind (docs/plan/oop-vm/02-corpus.md has the
# contribution contract this script is the enforcement of):
#
#   run/          fixture.wo compiles with woc and runs with wovm;
#                 stdout must equal fixture.out BYTE-FOR-BYTE.
#   compile-fail/ fixture.wo must fail to compile with exactly the
#                 WO-E### named in fixture.code.
#   trap/         fixture.wo must compile, then wovm must trap with
#                 exactly the code named in fixture.trap, parsed from
#                 wovm's fixed stderr line
#                 ("trap N in METHOD at line L: MESSAGE").
#   gc/           fixture.wo compiles and runs with wovm under
#                 WO_GC_TRACE=1 (and WO_GC_BUDGET from the optional
#                 fixture.gc_budget); stdout must equal fixture.out
#                 byte-for-byte, and the gc pump's stderr trace must
#                 match fixture.trace's step count and total freed
#                 count exactly.
#
# Any other outcome — wrong code, unexpected success, a loader
# rejection, a crash, a hang — fails and names the fixture. One line
# per fixture, a final tally, nonzero exit if anything failed.

set -uo pipefail

# The corpus asserts EXACT outputs — deterministic single-shard semantics.
# Multi-shard scheduling is nondeterministic by nature (the arc's spec
# narrows determinism to output SETS there); the multi-shard/TSan proofs
# live in the fibers gate, not here.
export WO_SHARDS=1  # no -e: a failing fixture is handled explicitly, one at a time
shopt -s nullglob

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WOC="$ROOT/compiler/_build/default/bin/woc"
WOVM="$ROOT/runtime/wovm"
CORPUS="$ROOT/tests/corpus"

# A hang in either binary must not block `just oop-e2e`/CI forever with no
# diagnostic — every invocation below runs under `timeout`, and a kill
# (exit 124, timeout(1)'s own signal for "I killed it") is reported as its
# own named failure, never folded into "exited nonzero". Overridable for a
# slower box; fixtures in this corpus are small enough that the default is
# generous, not tight.
TIMEOUT="${OOP_E2E_TIMEOUT:-10}"

if [[ ! -x "$WOC" ]]; then
  echo "oop-e2e: woc is not built ($WOC) — run: just woc-build" >&2
  exit 1
fi
if [[ ! -x "$WOVM" ]]; then
  echo "oop-e2e: wovm is not built ($WOVM) — run: make -C runtime wovm" >&2
  exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/oop-e2e.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0

ok() {
  echo "ok   $1"
  pass=$((pass + 1))
}

bad() {
  echo "FAIL $1 -- $2"
  fail=$((fail + 1))
}

# One scratch-file prefix per fixture (kind+name is unique across the
# whole corpus), so no per-file mktemp calls are needed inside the loop.
tmp_prefix() {
  echo "$WORK/$(echo "$1" | tr '/' '-')"
}

# Every WO-E### a run actually reported, anchored on the diagnostic
# renderer's own text (diag.ml's `render`: "file:line:col: error CODE:
# message") -- not a bare substring search. This is what makes matching
# exact both ways: an expected "WO-E21" cannot match a reported "WO-E215"
# (Critical 1, review round 1), and an empty/garbled fixture.code cannot
# match a stray occurrence of the right digits inside a message body.
extract_error_codes() {
  grep -oE 'error (WO-E[0-9]+):' "$1" | sed -E 's/error (WO-E[0-9]+):/\1/'
}

# haxe-parity Task 1 review, IMPORTANT 3: compiling the fixture's own
# directory (not just fixture.wo) means woc's own multi-file discovery
# picks up *any* .wo file dropped there, not only fixture.wo -- a stray
# second .wo directly beside fixture.wo silently joins the compile as a
# second file in the SAME module (Task 8's own discovery contract: every
# file sharing a directory is unconditionally visible to every other),
# and its own free fns/classes are exactly as reachable as fixture.wo's
# own. Confirmed exploitable: an alphabetically-earlier stray `fn main`
# hijacks the fixture's entry point with zero diagnostics (free fns are
# excluded from the cross-file collision check, docs/plan/oop-vm/
# 01-error-catalog.md's WO-E214 row). A *subdirectory* full of .wo files
# is a different module on purpose -- that's the whole point of a
# module fixture (greet/, secret/, a/, b/, ...) -- so this only counts
# files directly inside $1, never recursing into subdirectories.
assert_one_top_level_wo() {
  local dir="$1" name="$2"
  local top_level=("$dir"/*.wo)
  if [[ ${#top_level[@]} -ne 1 ]]; then
    bad "$name" "expected exactly one top-level .wo file in $dir, found ${#top_level[@]} -- module fixtures belong in a subdirectory, not loose beside fixture.wo"
    return 1
  fi
  return 0
}

run_fixture() {
  local dir="$1" name="run/$(basename "$1")"
  local prefix wob out err rc

  if [[ ! -f "$dir/fixture.wo" ]]; then bad "$name" "missing fixture.wo"; return; fi
  if [[ ! -f "$dir/fixture.out" ]]; then bad "$name" "missing fixture.out"; return; fi
  assert_one_top_level_wo "$dir" "$name" || return

  prefix="$(tmp_prefix "$name")"
  wob="$prefix.wob" out="$prefix.out" err="$prefix.err"

  # Compile the fixture's own directory, not just fixture.wo directly:
  # for a fixture with no other .wo file beside fixture.wo (every
  # pre-haxe-parity fixture), woc's directory discovery finds exactly
  # that one file, so this is behavior-identical to compiling
  # "$dir/fixture.wo" on its own. It's what lets a fixture exercise a
  # real cross-module `use` (haxe-parity Task 1) by adding a nested
  # module directory (e.g. `greet/greet.wo`) beside fixture.wo — woc's
  # own multi-file discovery (Task 8) then compiles both as one
  # program, exactly like a real project layout would.
  timeout "$TIMEOUT" "$WOC" --emit "$dir" -o "$wob" >/dev/null 2>"$err"
  rc=$?
  if [[ $rc -eq 124 ]]; then
    bad "$name" "woc timed out after ${TIMEOUT}s"
    return
  elif [[ $rc -gt 128 ]]; then
    bad "$name" "woc crashed (signal $((rc - 128)))"
    return
  elif [[ $rc -ne 0 ]]; then
    bad "$name" "compile failed (exit $rc): $(head -1 "$err")"
    return
  fi

  timeout "$TIMEOUT" "$WOVM" "$wob" >"$out" 2>"$err"
  rc=$?
  if [[ $rc -eq 124 ]]; then
    bad "$name" "wovm timed out after ${TIMEOUT}s"
    return
  elif [[ $rc -eq 1 ]]; then
    bad "$name" "unexpected trap: $(head -1 "$err")"
    return
  elif [[ $rc -eq 2 ]]; then
    bad "$name" "loader rejection: $(head -1 "$err")"
    return
  elif [[ $rc -gt 128 ]]; then
    bad "$name" "wovm crashed (signal $((rc - 128)))"
    return
  elif [[ $rc -ne 0 ]]; then
    bad "$name" "wovm exited $rc: $(head -1 "$err")"
    return
  fi

  if ! diff -q "$dir/fixture.out" "$out" >/dev/null 2>&1; then
    bad "$name" "stdout mismatch"
    diff -u "$dir/fixture.out" "$out" | sed 's/^/         /'
    return
  fi
  ok "$name"
}

gc_fixture() {
  local dir="$1" name="gc/$(basename "$1")"
  local prefix wob out err rc exp_steps exp_freed got_steps got_freed budget

  if [[ ! -f "$dir/fixture.wo" ]]; then bad "$name" "missing fixture.wo"; return; fi
  if [[ ! -f "$dir/fixture.out" ]]; then bad "$name" "missing fixture.out"; return; fi
  if [[ ! -f "$dir/fixture.trace" ]]; then bad "$name" "missing fixture.trace"; return; fi

  prefix="$(tmp_prefix "$name")"
  wob="$prefix.wob" out="$prefix.out" err="$prefix.err"

  timeout "$TIMEOUT" "$WOC" --emit "$dir/fixture.wo" -o "$wob" >/dev/null 2>"$err"
  rc=$?
  if [[ $rc -eq 124 ]]; then
    bad "$name" "woc timed out after ${TIMEOUT}s"
    return
  elif [[ $rc -gt 128 ]]; then
    bad "$name" "woc crashed (signal $((rc - 128)))"
    return
  elif [[ $rc -ne 0 ]]; then
    bad "$name" "compile failed (exit $rc): $(head -1 "$err")"
    return
  fi

  exp_steps="$(grep -oE 'steps=[0-9]+' "$dir/fixture.trace" | head -1 | cut -d= -f2)"
  exp_freed="$(grep -oE 'freed=[0-9]+' "$dir/fixture.trace" | head -1 | cut -d= -f2)"
  if [[ -z "$exp_steps" || -z "$exp_freed" ]]; then
    bad "$name" "fixture.trace missing steps=/freed="
    return
  fi

  # WO_GC_TRACE is always on so the pump's stderr trace can be checked;
  # WO_GC_BUDGET is only set when the fixture names one (fixture.gc_budget
  # is optional — absent means "use the pump's own default").
  if [[ -f "$dir/fixture.gc_budget" ]]; then
    budget="$(tr -d '[:space:]' <"$dir/fixture.gc_budget")"
    timeout "$TIMEOUT" env WO_GC_TRACE=1 WO_GC_BUDGET="$budget" "$WOVM" "$wob" >"$out" 2>"$err"
  else
    timeout "$TIMEOUT" env WO_GC_TRACE=1 "$WOVM" "$wob" >"$out" 2>"$err"
  fi
  rc=$?
  if [[ $rc -eq 124 ]]; then
    bad "$name" "wovm timed out after ${TIMEOUT}s"
    return
  elif [[ $rc -eq 1 ]]; then
    bad "$name" "unexpected trap: $(head -1 "$err")"
    return
  elif [[ $rc -eq 2 ]]; then
    bad "$name" "loader rejection: $(head -1 "$err")"
    return
  elif [[ $rc -gt 128 ]]; then
    bad "$name" "wovm crashed (signal $((rc - 128)))"
    return
  elif [[ $rc -ne 0 ]]; then
    bad "$name" "wovm exited $rc: $(head -1 "$err")"
    return
  fi

  if ! diff -q "$dir/fixture.out" "$out" >/dev/null 2>&1; then
    bad "$name" "stdout mismatch"
    diff -u "$dir/fixture.out" "$out" | sed 's/^/         /'
    return
  fi

  got_steps="$(grep -c '^gc: step ' "$err")"
  got_freed="$(grep -oE 'freed=[0-9]+' "$err" | cut -d= -f2 | awk '{s += $1} END {print s + 0}')"
  if [[ "$got_steps" != "$exp_steps" || "$got_freed" != "$exp_freed" ]]; then
    bad "$name" "gc trace mismatch: expected steps=$exp_steps freed=$exp_freed, got steps=$got_steps freed=$got_freed"
    return
  fi

  ok "$name"
}

compile_fail_fixture() {
  local dir="$1" name="compile-fail/$(basename "$1")"
  local prefix wob err rc expected

  if [[ ! -f "$dir/fixture.wo" ]]; then bad "$name" "missing fixture.wo"; return; fi
  if [[ ! -f "$dir/fixture.code" ]]; then bad "$name" "missing fixture.code"; return; fi
  assert_one_top_level_wo "$dir" "$name" || return

  prefix="$(tmp_prefix "$name")"
  wob="$prefix.wob" err="$prefix.err"
  expected="$(tr -d '[:space:]' <"$dir/fixture.code")"

  if [[ -z "$expected" ]]; then
    bad "$name" "fixture.code is empty"
    return
  fi

  # Compile the fixture's own directory, not just fixture.wo directly:
  # for a fixture with no other .wo file beside fixture.wo (every
  # pre-haxe-parity fixture), woc's directory discovery finds exactly
  # that one file, so this is behavior-identical to compiling
  # "$dir/fixture.wo" on its own. It's what lets a fixture exercise a
  # real cross-module `use` (haxe-parity Task 1) by adding a nested
  # module directory (e.g. `greet/greet.wo`) beside fixture.wo — woc's
  # own multi-file discovery (Task 8) then compiles both as one
  # program, exactly like a real project layout would.
  timeout "$TIMEOUT" "$WOC" --emit "$dir" -o "$wob" >/dev/null 2>"$err"
  rc=$?

  if [[ $rc -eq 124 ]]; then
    bad "$name" "expected $expected, woc timed out after ${TIMEOUT}s"
  elif [[ $rc -eq 0 ]]; then
    bad "$name" "expected $expected, compiled clean (unexpected success)"
  elif [[ $rc -gt 128 ]]; then
    bad "$name" "expected $expected, woc crashed (signal $((rc - 128)))"
  elif [[ $rc -eq 2 ]]; then
    bad "$name" "expected $expected, got a usage/IO error: $(head -1 "$err")"
  elif [[ $rc -ne 1 ]]; then
    bad "$name" "expected $expected, woc exited $rc"
  elif ! extract_error_codes "$err" | grep -qxF "$expected"; then
    bad "$name" "expected $expected, got: $(head -1 "$err")"
  else
    ok "$name"
  fi
}

trap_fixture() {
  local dir="$1" name="trap/$(basename "$1")"
  local prefix wob out err rc expected got

  if [[ ! -f "$dir/fixture.wo" ]]; then bad "$name" "missing fixture.wo"; return; fi
  if [[ ! -f "$dir/fixture.trap" ]]; then bad "$name" "missing fixture.trap"; return; fi

  prefix="$(tmp_prefix "$name")"
  wob="$prefix.wob" out="$prefix.out" err="$prefix.err"
  expected="$(tr -d '[:space:]' <"$dir/fixture.trap")"

  if [[ -z "$expected" ]]; then
    bad "$name" "fixture.trap is empty"
    return
  fi

  timeout "$TIMEOUT" "$WOC" --emit "$dir/fixture.wo" -o "$wob" >/dev/null 2>"$err"
  rc=$?
  if [[ $rc -eq 124 ]]; then
    bad "$name" "expected trap $expected, woc timed out after ${TIMEOUT}s"
    return
  elif [[ $rc -gt 128 ]]; then
    bad "$name" "expected trap $expected, woc crashed (signal $((rc - 128)))"
    return
  elif [[ $rc -ne 0 ]]; then
    bad "$name" "expected trap $expected, compile failed: $(head -1 "$err")"
    return
  fi

  timeout "$TIMEOUT" "$WOVM" "$wob" >"$out" 2>"$err"
  rc=$?
  if [[ $rc -eq 124 ]]; then
    bad "$name" "expected trap $expected, wovm timed out after ${TIMEOUT}s"
    return
  elif [[ $rc -eq 0 ]]; then
    bad "$name" "expected trap $expected, ran to completion"
    return
  elif [[ $rc -eq 2 ]]; then
    bad "$name" "expected trap $expected, got a loader rejection: $(head -1 "$err")"
    return
  elif [[ $rc -gt 128 ]]; then
    bad "$name" "expected trap $expected, wovm crashed (signal $((rc - 128)))"
    return
  elif [[ $rc -ne 1 ]]; then
    bad "$name" "expected trap $expected, wovm exited $rc"
    return
  fi

  # wovm's fixed stderr line: "trap N in METHOD at line L: MESSAGE"
  got="$(sed -n 's/^trap \([0-9][0-9]*\) in .*/\1/p' "$err" | head -1)"
  if [[ -z "$got" ]]; then
    bad "$name" "exit 1 but no parseable trap line: $(head -1 "$err")"
  elif [[ "$got" != "$expected" ]]; then
    bad "$name" "expected trap $expected, got trap $got"
  else
    ok "$name"
  fi
}

walk() {
  local kind="$1" fn="$2" dir
  for dir in "$CORPUS/$kind"/*/; do
    [[ -d "$dir" ]] || continue
    "$fn" "${dir%/}"
  done
}

walk run run_fixture
walk compile-fail compile_fail_fixture
walk trap trap_fixture
walk gc gc_fixture

# ---- single-binary smoke (Task 6, plan 3) -----------------------------
# `woc build` end to end (relocate outside the repo, run, diff; corrupt
# the trailer, confirm a clear failure) doesn't fit the fixture-walk
# shape above, so it's a dedicated script -- its ok/FAIL lines fold into
# this harness's own tally the same way a fixture's would.
SB_LOG="$WORK/single-binary-smoke.log"
"$ROOT/scripts/single-binary-smoke.sh" | tee "$SB_LOG"
pass=$((pass + $(grep -c '^ok   ' "$SB_LOG")))
fail=$((fail + $(grep -c '^FAIL ' "$SB_LOG")))

echo
total=$((pass + fail))
printf 'oop-e2e: %d checks, %d failures\n' "$total" "$fail"

if [[ $total -eq 0 ]]; then
  echo "oop-e2e: no fixtures found under $CORPUS/{run,compile-fail,trap,gc} — harness misconfigured?" >&2
  exit 1
fi
[[ $fail -eq 0 ]]
