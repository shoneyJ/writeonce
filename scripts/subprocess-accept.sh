#!/usr/bin/env bash
# scripts/subprocess-accept.sh — iteration 42's gate: bounded subprocess.
# The runtime suite (test_proc) proves the mechanics in-process; this
# proves the half only a real program shows: proc.run called FROM .wo
# through the compiler, bounds caught with try/catch in the language, a
# parked run blocking no other request, and the SIGTERM drain leaving no
# child behind. Service log: /tmp/subprocess.log (tail -F it live).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WOC="$ROOT/compiler/_build/default/bin/woc"
WOVM="$ROOT/runtime/wovm"
APP="$ROOT/docs/examples/subprocess/main.wo"
PORT="${SUBPROCESS_PORT:-18971}"
LOG=/tmp/subprocess.log

pass=0; fail=0
ok()  { echo "ok   $1"; pass=$((pass + 1)); }
bad() { echo "FAIL $1"; fail=$((fail + 1)); }

WORK="$(mktemp -d)"
APP_PID=""
cleanup() {
  [[ -n "$APP_PID" ]] && kill -KILL "$APP_PID" 2>/dev/null
  rm -rf "$WORK"
}
trap cleanup EXIT

now_ms() { echo $(( $(date +%s%N) / 1000000 )); }

# one request, one line back (the service closes after answering)
req() {
  local line=""
  if exec 3<>"/dev/tcp/127.0.0.1/$PORT" 2>/dev/null; then
    printf '%s\n' "$1" >&3
    IFS= read -r -t "${2:-8}" line <&3 || true
    exec 3>&- 2>/dev/null
  fi
  echo "$line"
}

# ---- 0. build ------------------------------------------------------------
if "$WOC" --emit "$APP" -o "$WORK/subprocess.wob" 2>"$WORK/cerr"; then
  ok "build: main.wo compiles"
else
  bad "build: $(head -3 "$WORK/cerr")"
  echo "subprocess-accept: $((pass + fail)) checks, $fail failures"
  exit 1
fi

# ---- 1. start, banner-separated log --------------------------------------
{ echo; echo "== subprocess-accept $(date -Is) port $PORT =="; } >>"$LOG"
"$WOVM" "$WORK/subprocess.wob" "$PORT" >>"$LOG" 2>&1 &
APP_PID=$!
up=0
for _ in $(seq 1 50); do
  if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then exec 3>&-; up=1; break; fi
  sleep 0.1
done
[[ $up == 1 ]] && ok "service answers on $PORT (log: $LOG)" \
               || bad "service never came up (see $LOG)"

# ---- 2. the default run --------------------------------------------------
got="$(req run)"
[[ "$got" == "code=0 out=hello" ]] && ok "proc.run: code=0 out=hello" \
                                   || bad "proc.run answered '$got'"

# ---- 3. deadline caught IN the language ----------------------------------
got="$(req deadline)"
if [[ "$got" == caught:*deadline* ]]; then
  ok "deadline trap caught in .wo: '$got'"
else
  bad "deadline leg answered '$got'"
fi

# ---- 4. stdout cap caught IN the language --------------------------------
got="$(req cap)"
if [[ "$got" == caught:*"stdout cap 1000"* ]]; then
  ok "cap trap caught in .wo: '$got'"
else
  bad "cap leg answered '$got'"
fi

# ---- 5. a parked run blocks nobody ---------------------------------------
slow_out="$WORK/slow"
( req slow 10 >"$slow_out" ) &
SLOW_JOB=$!
sleep 0.4    # the slow child (sleep 2) is now in flight
t0=$(now_ms)
got="$(req ping)"
t1=$(now_ms)
[[ "$got" == "pong" ]] && ok "ping answered while slow runs" \
                        || bad "ping answered '$got' while slow runs"
(( t1 - t0 < 1500 )) && ok "ping took $((t1 - t0)) ms (not slow's 2 s)" \
                      || bad "ping took $((t1 - t0)) ms — the shard was blocked"
wait "$SLOW_JOB"
got="$(cat "$slow_out")"
[[ "$got" == "slow-done code=0" ]] && ok "slow completed: '$got'" \
                                    || bad "slow answered '$got'"

# ---- 6. SIGTERM drain: no child survives ---------------------------------
got="$(req long)"
[[ "$got" == "long-started" ]] && ok "long child started" \
                                || bad "long answered '$got'"
CHILD=""
for _ in $(seq 1 30); do
  CHILD="$(pgrep -P "$APP_PID" -x sleep | head -1 || true)"
  [[ -n "$CHILD" ]] && break
  sleep 0.1
done
[[ -n "$CHILD" ]] && ok "live child found (sleep 30, pid $CHILD)" \
                   || bad "no sleep child appeared under the service"
kill -TERM "$APP_PID"
rc=-1
for _ in $(seq 1 50); do
  if ! kill -0 "$APP_PID" 2>/dev/null; then wait "$APP_PID"; rc=$?; break; fi
  sleep 0.1
done
[[ $rc == 0 ]] && ok "SIGTERM: clean exit 0" || bad "SIGTERM exit was $rc"
if [[ -n "$CHILD" ]]; then
  kill -0 "$CHILD" 2>/dev/null && bad "child $CHILD SURVIVED the stop" \
                                || ok "child $CHILD is gone with the service"
fi
APP_PID=""

echo "subprocess-accept: $((pass + fail)) checks, $fail failures"
[[ $fail == 0 ]]
