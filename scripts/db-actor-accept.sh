#!/usr/bin/env bash
# scripts/db-actor-accept.sh — arc stage 3's gate: actors on worker shards
# read and write the database through the transparent DB actor. Multi-shard
# output is asserted as a SET (scheduling orders the writer lines); the
# main line and the single-shard run are exact. The WO_DATA pair proves a
# worker's write rides the owner's WAL and replays.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WOC="$ROOT/compiler/_build/default/bin/woc"
WOVM="$ROOT/runtime/wovm"
DIR="$ROOT/docs/examples/db-actor"

pass=0; fail=0
ok() { echo "ok   $1"; pass=$((pass + 1)); }
bad() { echo "FAIL $1 -- $2"; fail=$((fail + 1)); }

if [[ ! -x "$WOC" || ! -x "$WOVM" ]]; then
  echo "db-actor-accept: build woc and wovm first" >&2; exit 1
fi

if "$WOC" build "$DIR" -o "$DIR/target/db-actor" --runtime "$WOVM" >/dev/null 2>&1; then
  ok "builds"
else
  bad "build" "woc failed"; echo "db-actor-accept: 1 checks, 1 failures"; exit 1
fi

check_set() { # name [env pairs...]
  local name="$1"; shift
  local out
  out="$(env "$@" timeout 30 "$DIR/target/db-actor" 2>&1)"
  if printf '%s' "$out" | grep -q "writer 1 sees sum" \
     && printf '%s' "$out" | grep -q "writer 2 sees sum" \
     && printf '%s' "$out" | grep -q "^main sees 2 rows, sum 3$" ; then
    ok "$name: both writers wrote and read cross-shard; main exact"
  else
    bad "$name" "$(printf '%s' "$out" | tr '\n' '|')"
  fi
}

# multi-shard (default = all cores): the RPC path under test, three rounds
check_set "multi #1"
check_set "multi #2"
check_set "multi #3"
# forced backends: the reply park is plane-independent
check_set "multi uring" WO_IO=uring
check_set "multi epoll" WO_IO=epoll

# single-shard: byte-exact — the local path is untouched
sout="$(WO_SHARDS=1 timeout 30 "$DIR/target/db-actor" 2>&1)"
want=$'writer 1 sees sum 1\nwriter 2 sees sum 3\nmain sees 2 rows, sum 3'
if [[ "$sout" == "$want" ]]; then
  ok "single-shard byte-exact"
else
  bad "single-shard" "$(printf '%s' "$sout" | tr '\n' '|')"
fi

# durability through the RPC: a worker's insert commits on the owner's WAL
# before the ack; a restart replays it (2 rows, then 2+2)
DATA="$(mktemp -d)"
r1="$(WO_DATA="$DATA" timeout 30 "$DIR/target/db-actor" 2>&1 | tail -1)"
r2="$(WO_DATA="$DATA" timeout 30 "$DIR/target/db-actor" 2>&1 | tail -1)"
rm -rf "$DATA"
if [[ "$r1" == "main sees 2 rows, sum 3" && "$r2" == "main sees 4 rows, sum 6" ]]; then
  ok "WAL: worker writes ack-after-durable, replay doubles the store"
else
  bad "WAL replay" "r1=$r1 r2=$r2"
fi

# ---- language 41: an unadopted shard must not impersonate shard 0 ---------
# A worker shard's runtime is initialised lazily, when it adopts its first
# fiber -- but INBOX_READY is set at thread creation. A shard that never
# adopts therefore still gets settled at shutdown, and before the fix its
# rt.shard_id was left 0 by the memset. It then impersonated shard 0:
# wo_drop_obj saw 0 == 0 for anything the primary allocated, took the "we
# are home" branch instead of routing, and called class_free against a
# class table lazy init never filled -- &rt->classes[id] off a NULL base.
#
# Needs MULTIPLE SHARDS (the corpus runner pins WO_SHARDS=1, which is why
# this lives here) and the ASan build, because the arena is one hand-managed
# malloc block: intra-arena reuse is invisible to ASan, so the failure
# surfaces as a bare SEGV rather than a use-after-free report.
L41_W="$(mktemp -d)"
trap 'rm -rf "$L41_W"' EXIT
L41_WOC="$ROOT/compiler/_build/default/bin/woc"
L41_VM="$ROOT/runtime/build/wovm_asan"
L41_SRC="$ROOT/tests/regress/lang-41/shard-settle-crash.wo"
if [[ ! -x "$L41_VM" ]]; then
  bad "lang-41 shard settle" "build it first: make -C runtime wovm-asan"
elif "$L41_WOC" --emit "$L41_SRC" -o "$L41_W/l41.wob" >/dev/null 2>&1; then
  mkdir -p "$L41_W/l41data"
  l41_out="$(WO_DATA="$L41_W/l41data" WO_SHARDS=4 timeout 60 "$L41_VM" "$L41_W/l41.wob" 2>&1)"
  if grep -q 'SEGV\|AddressSanitizer' <<<"$l41_out"; then
    bad "lang-41 shard settle" "$(grep -m1 'ERROR' <<<"$l41_out")"
  elif grep -q 'dispatched' <<<"$l41_out"; then
    ok "lang-41: an unadopted shard routes instead of impersonating shard 0"
  else
    bad "lang-41 shard settle" "no output: $(head -c 120 <<<"$l41_out")"
  fi
else
  bad "lang-41 shard settle" "fixture did not compile"
fi

# lang-41 phase C: a cross-shard message carrying an OWNED SUBTREE (multi<Text>)
# sent + called into an actor on another shard. Pre-marshal-fix this pointer-
# shared the subtree across arenas -> a double free (ASan abort or settle hang);
# the marshal fix copies it per crossing. Run repeatedly — the failure was
# intermittent (~1 in 6).
L41_CSM="$ROOT/tests/regress/lang-41/cross-shard-marshal.wo"
if [[ -x "$L41_VM" ]] && "$L41_WOC" --emit "$L41_CSM" -o "$L41_W/csm.wob" >/dev/null 2>&1; then
  mkdir -p "$L41_W/csmdata"
  csm_bad=""
  for _ in 1 2 3 4 5; do
    csm_out="$(WO_DATA="$L41_W/csmdata" WO_SHARDS=4 timeout 60 "$L41_VM" "$L41_W/csm.wob" 2>&1)"
    if grep -q 'SEGV\|AddressSanitizer\|FATAL' <<<"$csm_out" || ! grep -q 'dispatched' <<<"$csm_out"; then
      csm_bad="$(grep -m1 'ERROR\|FATAL' <<<"$csm_out"); ${csm_bad}"
    fi
  done
  if [[ -z "$csm_bad" ]]; then
    ok "lang-41: cross-shard owned-subtree message marshals (no double free, 5x)"
  else
    bad "lang-41 cross-shard marshal" "$csm_bad"
  fi
else
  bad "lang-41 cross-shard marshal" "fixture did not build (need wovm-asan)"
fi

echo
echo "db-actor-accept: $((pass + fail)) checks, $fail failures"
[[ $fail -eq 0 ]] || exit 1
