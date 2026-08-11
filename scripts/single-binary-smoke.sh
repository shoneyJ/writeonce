#!/usr/bin/env bash
# scripts/single-binary-smoke.sh — Task 6 (plan 3): proves `woc build`
# produces a genuinely self-contained executable, and that a corrupted
# trailer (docs/plan/oop-vm/00-wob-format.md, "single-binary trailer"
# section) fails clearly instead of crashing or misbehaving silently.
#
# Steps: build the hello fixture into one file; copy it to a temp
# directory OUTSIDE the repo (hardcoded under /tmp, not $TMPDIR — the
# whole point is proving nothing repo-relative is needed at run time);
# run it there with no arguments; diff stdout byte-for-byte. Then corrupt
# the trailer's payload_len field and confirm a clear exit-2 error, not a
# crash or a hang.
#
# Called by scripts/oop-e2e.sh (its ok/FAIL lines fold into that
# harness's tally); also runnable standalone. Same output shape as
# oop-e2e.sh: "ok   NAME" / "FAIL NAME -- reason", one line per check.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WOC="$ROOT/compiler/_build/default/bin/woc"
WOVM="$ROOT/runtime/wovm"
FIXTURE="$ROOT/tests/corpus/run/hello/fixture.wo"
EXPECTED="$ROOT/tests/corpus/run/hello/fixture.out"
TIMEOUT="${OOP_E2E_TIMEOUT:-10}"

if [[ ! -x "$WOC" ]]; then
  echo "single-binary-smoke: woc is not built ($WOC) — run: just woc-build" >&2
  exit 1
fi
if [[ ! -x "$WOVM" ]]; then
  echo "single-binary-smoke: wovm is not built ($WOVM) — run: make -C runtime wovm" >&2
  exit 1
fi

pass=0
fail=0
ok()  { echo "ok   $1"; pass=$((pass + 1)); }
bad() { echo "FAIL $1 -- $2"; fail=$((fail + 1)); }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/wo-single-binary-smoke.XXXXXX")"
# Deliberately NOT under $WORK / $TMPDIR: this directory is the actual
# proof of self-containment, so it must be unambiguously outside the repo
# regardless of how TMPDIR is set in the environment running the harness.
ELSEWHERE="$(mktemp -d /tmp/wo-single-binary-elsewhere.XXXXXX)"
trap 'rm -rf "$WORK" "$ELSEWHERE"' EXIT
echo "single-binary-smoke: relocation dir = $ELSEWHERE"

# ---- build the hello fixture into one file --------------------------------
SRC="$WORK/src"
mkdir -p "$SRC"
cp "$FIXTURE" "$SRC/fixture.wo"
APP="$WORK/app"

if ! timeout "$TIMEOUT" "$WOC" build "$SRC" -o "$APP" --runtime "$WOVM" \
    >"$WORK/build.out" 2>"$WORK/build.err"; then
  rc=$?
  bad "single-binary/build" "woc build exited $rc: $(head -1 "$WORK/build.err")"
elif [[ ! -x "$APP" ]]; then
  bad "single-binary/build" "output missing or not executable: $APP"
else
  ok "single-binary/build"
fi

# ---- relocate outside the repo, run with no args, diff --------------------
if [[ -x "$APP" ]]; then
  cp "$APP" "$ELSEWHERE/app"
  chmod +x "$ELSEWHERE/app"
  ( cd / && timeout "$TIMEOUT" "$ELSEWHERE/app" ) >"$WORK/relocated.out" 2>"$WORK/relocated.err"
  rc=$?
  if [[ $rc -ne 0 ]]; then
    bad "single-binary/relocated-run" "exit $rc: $(head -1 "$WORK/relocated.err")"
  elif ! diff -q "$EXPECTED" "$WORK/relocated.out" >/dev/null 2>&1; then
    bad "single-binary/relocated-run" "stdout mismatch"
    diff -u "$EXPECTED" "$WORK/relocated.out" | sed 's/^/         /'
  else
    ok "single-binary/relocated-run"
  fi
else
  bad "single-binary/relocated-run" "skipped: no app to relocate"
fi

# ---- corrupt the trailer's payload_len field, confirm a clear failure -----
# Trailer is the last 20 bytes (payload_off u64, payload_len u64, magic
# u32): payload_len sits 12 bytes from EOF. Overwriting it with all-0xFF
# breaks the reader's "payload_off + payload_len == file_size - 20" check
# without touching the magic, so this exercises the "recognized trailer,
# bad bounds" path specifically (not the "no trailer at all" fallback).
if [[ -f "$ELSEWHERE/app" ]]; then
  CORRUPT="$WORK/corrupt-app"
  cp "$ELSEWHERE/app" "$CORRUPT"
  chmod +x "$CORRUPT"
  size=$(stat -c%s "$CORRUPT")
  printf '\xff\xff\xff\xff\xff\xff\xff\xff' \
    | dd of="$CORRUPT" bs=1 seek=$((size - 12)) count=8 conv=notrunc status=none

  timeout "$TIMEOUT" "$CORRUPT" >"$WORK/corrupt.out" 2>"$WORK/corrupt.err"
  rc=$?
  if [[ $rc -eq 124 ]]; then
    bad "single-binary/corrupt-trailer" "timed out after ${TIMEOUT}s instead of failing cleanly"
  elif [[ $rc -gt 128 ]]; then
    bad "single-binary/corrupt-trailer" "crashed (signal $((rc - 128))) instead of failing cleanly"
  elif [[ $rc -ne 2 ]]; then
    bad "single-binary/corrupt-trailer" "expected exit 2, got $rc: $(head -1 "$WORK/corrupt.err")"
  elif [[ -s "$WORK/corrupt.out" ]]; then
    bad "single-binary/corrupt-trailer" "exit 2 but stdout was not empty (partial run before failing?)"
  elif ! grep -qi "corrupt trailer" "$WORK/corrupt.err"; then
    bad "single-binary/corrupt-trailer" "exit 2 but no clear message: $(head -1 "$WORK/corrupt.err")"
  else
    ok "single-binary/corrupt-trailer"
  fi
else
  bad "single-binary/corrupt-trailer" "skipped: no relocated app to corrupt"
fi

total=$((pass + fail))
printf 'single-binary-smoke: %d checks, %d failures\n' "$total" "$fail"
[[ $fail -eq 0 ]]
