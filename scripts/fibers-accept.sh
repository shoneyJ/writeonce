#!/usr/bin/env bash
# scripts/fibers-accept.sh — the hybrid-scheduler demo's gate: part 1 is
# byte-exact (budget accounting, timing-free); part 2 asserts ordering
# invariants (a parked sleeper blocks nobody) on BOTH I/O backends and
# under ASan.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WOC="$ROOT/compiler/_build/default/bin/woc"
WOVM="$ROOT/runtime/wovm"
DIR="$ROOT/docs/examples/fibers"

pass=0; fail=0
ok() { echo "ok   $1"; pass=$((pass + 1)); }
bad() { echo "FAIL $1 -- $2"; fail=$((fail + 1)); }

if [[ ! -x "$WOC" || ! -x "$WOVM" ]]; then
  echo "fibers-accept: build woc and wovm first" >&2; exit 1
fi

if WO_RUNTIME="$WOVM" "$WOC" "$DIR" >/dev/null 2>&1 && [[ -x "$DIR/target/fibers" ]]; then
  ok "builds"
else
  bad "build" "woc failed"; echo "fibers-accept: 1 checks, 1 failures"; exit 1
fi

check_run() { # name [env pairs...]
  local name="$1"; shift
  local out
  out="$(env "$@" "$DIR/target/fibers" 2>&1)"
  local want_p1=$'count +1 = 1\ncount +2 = 3\ncount +3 = 6\npart1 done'
  if [[ "$(printf '%s\n' "$out" | head -4)" == "$want_p1" ]]; then
    ok "$name: part 1 byte-exact (budget interleave)"
  else
    bad "$name part1" "$(printf '%s' "$out" | head -4 | tr '\n' '|')"
  fi
  local down_ln up_ln done_ln ticks
  down_ln=$(printf '%s\n' "$out" | grep -n "sleeper: down" | cut -d: -f1)
  up_ln=$(printf '%s\n' "$out" | grep -n "sleeper: up" | cut -d: -f1)
  done_ln=$(printf '%s\n' "$out" | grep -n "part2 done" | cut -d: -f1)
  ticks=$(printf '%s\n' "$out" | grep -c "main tick")
  if [[ -n "$down_ln" && -n "$up_ln" && -n "$done_ln" && "$ticks" == "8" ]]; then
    local before
    before=$(printf '%s\n' "$out" | sed -n "${down_ln},${up_ln}p" | grep -c "main tick")
    if [[ "$before" -ge 3 && "$up_ln" -lt "$done_ln" ]]; then
      ok "$name: part 2 — parked sleeper blocked nobody ($before ticks while down)"
    else
      bad "$name part2" "only $before ticks before wake"
    fi
  else
    bad "$name part2" "down=$down_ln up=$up_ln done=$done_ln ticks=$ticks"
  fi
}

check_run "auto"
check_run "uring" WO_IO=uring
check_run "epoll" WO_IO=epoll

# ASan flavor: rebuild the binary against the ASan runtime and repeat once
make -C "$ROOT/runtime" wovm-asan -s >/dev/null 2>&1
if "$WOC" build "$DIR" -o "$DIR/target/fibers_asan" --runtime "$ROOT/runtime/build/wovm_asan" >/dev/null 2>&1; then
  out="$("$DIR/target/fibers_asan" 2>&1)"
  if printf '%s' "$out" | grep -q "part2 done" && ! printf '%s' "$out" | grep -qi "sanitizer\|leak"; then
    ok "ASan run clean"
  else
    bad "asan" "$(printf '%s' "$out" | tail -2 | tr '\n' '|')"
  fi
else
  bad "asan" "build failed"
fi

echo
printf 'fibers-accept: %d checks, %d failures\n' "$((pass + fail))" "$fail"
[[ $fail -eq 0 ]]
