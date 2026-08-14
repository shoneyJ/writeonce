#!/usr/bin/env bash
# scripts/log-watcher-accept.sh — the language track's acceptance test.
#
# The corpus (scripts/oop-e2e.sh) gates individual behaviors; THIS gates the
# thing the whole track exists for: docs/examples/log-watcher, 1285 lines of
# .wo across 7 files, must compile with zero diagnostics and then actually run.
# Four checks, in the order a person would try them:
#
#   compile     woc --emit over the sample's directory, exit 0, no diagnostics
#   watch       tail a live file: an `error` line, then silence past the quiet
#               period, must print ALERT
#   run         a cron.d directory must parse and schedule (SCHEDULE line)
#   mcp         the MCP server must answer JSON-RPC over HTTP: initialize,
#               tools/list, and a 401 for a request with no bearer token
#
# Each check names what it wanted when it fails. Timeouts are generous but
# real: a hang is a failure, not a wait. No python, no curl — the HTTP client
# is bash's own /dev/tcp, so this runs wherever the runtime does.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WOC="$ROOT/compiler/_build/default/bin/woc"
WOVM="$ROOT/runtime/wovm"
SAMPLE="$ROOT/docs/examples/log-watcher"
# A per-run port by default: a fixed one collides with a server left behind by
# an earlier run (or by hand), and then the checks below silently talk to THAT
# process instead of the one this script started. Override with LW_ACCEPT_PORT.
PORT="${LW_ACCEPT_PORT:-$((18000 + ($$ % 900)))}"

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

if [[ ! -x "$WOC" ]]; then
  echo "log-watcher-accept: woc is not built ($WOC) — run: just woc-build" >&2
  exit 1
fi
if [[ ! -x "$WOVM" ]]; then
  echo "log-watcher-accept: wovm is not built ($WOVM) — run: just wovm-build" >&2
  exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/lw-accept.XXXXXX")"
# LW_ACCEPT_KEEP=1 leaves the work directory (image, logs, cron.d, the
# server's own stdout) in place — what you want the moment a check fails.
cleanup() {
  [[ -n "${SRV_PID:-}" ]] && kill -9 "$SRV_PID" 2>/dev/null
  [[ -n "${WATCH_PID:-}" ]] && kill -9 "$WATCH_PID" 2>/dev/null
  if [[ -n "${LW_ACCEPT_KEEP:-}" ]]; then
    echo "log-watcher-accept: kept $WORK"
  else
    rm -rf "$WORK"
  fi
}
trap cleanup EXIT

IMAGE="$WORK/log-watcher.wob"

# ---- 1. compile -------------------------------------------------------
if timeout 60 "$WOC" --emit "$SAMPLE" -o "$IMAGE" >"$WORK/compile.out" 2>"$WORK/compile.err"; then
  if [[ -s "$WORK/compile.err" ]]; then
    bad "compile" "exit 0 but diagnostics on stderr: $(head -1 "$WORK/compile.err")"
  else
    ok "compile ($(wc -c <"$IMAGE" | tr -d ' ') bytes)"
  fi
else
  bad "compile" "$(head -3 "$WORK/compile.err" | tr '\n' ' ')"
fi

if [[ ! -s "$IMAGE" ]]; then
  echo
  printf 'log-watcher-accept: %d checks, %d failures\n' "$((pass + fail))" "$((fail + 1))"
  echo "log-watcher-accept: no image, nothing to run" >&2
  exit 1
fi

# ---- 2. watch ---------------------------------------------------------
# quiet 2s, poll 1s: write an error line, then stay silent long enough for the
# watcher to decide the burst is over.
LOG="$WORK/app.log"
: >"$LOG"
timeout -k 2 12 "$WOVM" "$IMAGE" watch "$LOG" 2 1 >"$WORK/watch.out" 2>&1 &
WATCH_PID=$!
sleep 2
printf 'info service starting\n' >>"$LOG"
sleep 2
printf 'error disk full\n' >>"$LOG"
sleep 6
kill -9 "$WATCH_PID" 2>/dev/null
wait "$WATCH_PID" 2>/dev/null
WATCH_PID=""
if grep -q "^watching " "$WORK/watch.out" && grep -q "^ALERT .*last entry is error" "$WORK/watch.out"; then
  ok "watch (alerted on the error line)"
else
  bad "watch" "no ALERT line; got: $(tr '\n' '|' <"$WORK/watch.out" | cut -c1-160)"
fi

# ---- 3. run (supervisor) ---------------------------------------------
CRON="$WORK/cron.d"
mkdir -p "$CRON"
printf '* * * * * root /usr/bin/backup.sh > /var/log/backup.log 2>&1\n' >"$CRON/backup"
timeout 8 "$WOVM" "$IMAGE" run "$CRON" >"$WORK/run.out" 2>&1
if grep -q "^SCHEDULE /var/log/backup.log" "$WORK/run.out"; then
  ok "run (parsed and scheduled the cron entry)"
else
  bad "run" "no SCHEDULE line; got: $(tr '\n' '|' <"$WORK/run.out" | cut -c1-160)"
fi

# ---- 4. mcp -----------------------------------------------------------
cat >"$WORK/cfg.json" <<EOF
{ "pollInterval": 2, "quietPeriod": 5, "detections": "$WORK/detections.log",
  "mcp": { "port": $PORT, "apiKey": "s3cret" } }
EOF
# -k: `env.stopping()` installs a SIGTERM handler that only sets a flag, and
# the serve loop is blocked in accept(), so a plain TERM is swallowed — the
# process needs a KILL to actually stop (recorded in docs/00-status.md).
timeout -k 2 20 "$WOVM" "$IMAGE" mcp "$CRON" "$WORK/cfg.json" >"$WORK/mcp.out" 2>&1 &
SRV_PID=$!
sleep 2

# Did OUR server actually come up? Without this check a dead server (the usual
# cause: something else already on the port, which makes `net.listen` trap
# "Address already in use") is invisible — the requests below would answer
# from whatever else is listening, and "ok" would mean nothing. Liveness is
# "this process is still running AND the port accepts", checked with a short
# retry so a slow start is a wait, not a failure.
srv_ready=""
for _ in 1 2 3 4 5 6 7 8 9 10; do
  if ! kill -0 "$SRV_PID" 2>/dev/null; then break; fi
  if (exec 4<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
    exec 4<&- 2>/dev/null
    exec 4>&- 2>/dev/null
    srv_ready=1
    break
  fi
  sleep 0.5
done
if [[ -z "$srv_ready" ]]; then
  bad "mcp server start" "$(head -1 "$WORK/mcp.out" 2>/dev/null || echo 'no output') (port $PORT; set LW_ACCEPT_PORT to pick another)"
  echo
  printf 'log-watcher-accept: %d checks, %d failures\n' "$((pass + fail))" "$fail"
  exit 1
fi

# One request over bash's own TCP. The reply is read the way any HTTP client
# reads one — headers to the blank line, then exactly Content-Length bytes —
# and NOT by waiting for EOF: the sample never calls `net.close`, so the server
# holds the connection open after answering (a recorded gap, see
# docs/00-status.md). Reading to EOF meant a `timeout`-killed `head`/`cat`
# discarding its own buffer, which made this check flaky rather than false.
http_post() {
  local body="$1" auth="$2" line len="" hdr="" reply=""
  exec 3<>"/dev/tcp/127.0.0.1/$PORT" || return 1
  {
    printf 'POST /mcp HTTP/1.1\r\n'
    printf 'Host: 127.0.0.1\r\n'
    [[ -n "$auth" ]] && printf 'Authorization: Bearer %s\r\n' "$auth"
    printf 'Content-Type: application/json\r\n'
    printf 'Content-Length: %d\r\n' "${#body}"
    printf 'Connection: close\r\n\r\n'
    printf '%s' "$body"
  } >&3
  while IFS= read -r -t 5 line <&3; do
    line="${line%$'\r'}"
    [[ -z "$line" ]] && break
    hdr+="$line"$'\n'
    [[ "$line" == [Cc]ontent-[Ll]ength:* ]] && len="${line#*: }"
  done
  if [[ -n "$len" && "$len" != 0 ]]; then
    IFS= read -r -N "$len" -t 5 reply <&3
  fi
  exec 3<&-
  exec 3>&-
  printf '%s%s' "$hdr" "$reply"
}

INIT="$(http_post '{"jsonrpc":"2.0","id":1,"method":"initialize"}' s3cret)"
if [[ "$INIT" == *"200 OK"* && "$INIT" == *'"protocolVersion"'* && "$INIT" == *'"id":1'* ]]; then
  ok "mcp initialize (200, protocolVersion, unquoted id)"
else
  bad "mcp initialize" "got: $(printf '%s' "$INIT" | tr '\n' '|' | cut -c1-160)"
fi

TOOLS="$(http_post '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' s3cret)"
if [[ "$TOOLS" == *"200 OK"* && "$TOOLS" == *'"tail_log"'* && "$TOOLS" == *'"get_running_crons"'* ]]; then
  ok "mcp tools/list (the tool set encodes)"
else
  bad "mcp tools/list" "got: $(printf '%s' "$TOOLS" | tr '\n' '|' | cut -c1-160)"
fi

NOAUTH="$(http_post '{"jsonrpc":"2.0","id":3,"method":"tools/list"}' '')"
if [[ "$NOAUTH" == *"401 Unauthorized"* && "$NOAUTH" == *'"unauthorized"'* ]]; then
  ok "mcp auth (401 without a bearer token)"
else
  bad "mcp auth" "got: $(printf '%s' "$NOAUTH" | tr '\n' '|' | cut -c1-160)"
fi

kill -9 "$SRV_PID" 2>/dev/null
wait "$SRV_PID" 2>/dev/null
SRV_PID=""

echo
printf 'log-watcher-accept: %d checks, %d failures\n' "$((pass + fail))" "$fail"
[[ $fail -eq 0 ]]
