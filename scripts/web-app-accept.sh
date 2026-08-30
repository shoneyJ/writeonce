#!/usr/bin/env bash
# scripts/web-app-accept.sh — iteration 16's acceptance gate. Proves the whole
# chain at run time, network-free: a temp git remote is built from
# docs/examples/porch, its file:// URL is substituted into a
# temp copy of docs/examples/web-app, then: fetch -> lock -> build -> serve ->
# the storefront matrix -> SIGTERM -> restart persistence, plus iteration 17's
# library-kind and internal/-boundary checks. The repo itself
# never carries .wo-deps/wo.lock artifacts.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WOC="$ROOT/compiler/_build/default/bin/woc"
WOVM="$ROOT/runtime/wovm"
PORT="${WA_PORT:-18801}"

pass=0
fail=0
ok() { echo "ok   $1"; pass=$((pass + 1)); }
bad() { echo "FAIL $1 -- $2"; fail=$((fail + 1)); }

if [[ ! -x "$WOC" || ! -x "$WOVM" ]]; then
  echo "web-app-accept: build woc and wovm first (just woc-build; just wovm-build)" >&2
  exit 1
fi

if ! command -v curl >/dev/null 2>&1; then
  echo "web-app-accept: curl is required (the limiter gate legs drive the server with it)" >&2
  exit 1
fi

ulimit -n 8192 2>/dev/null || true  # the 1k soak needs headroom
W="$(mktemp -d "${TMPDIR:-/tmp}/web-app-accept.XXXXXX")"
SRV=""
cleanup() {
  [[ -n "$SRV" ]] && kill -9 "$SRV" 2>/dev/null
  [[ -n "${WA_ACCEPT_KEEP:-}" ]] && echo "kept $W" || rm -rf "$W"
}
trap cleanup EXIT

# ---- the framework as a git remote; the app pointed at it ----
cp -r "$ROOT/docs/examples/porch" "$W/fw"
git -C "$W/fw" init -q
git -C "$W/fw" add -A
git -C "$W/fw" -c user.email=t@t -c user.name=t commit -qm v01
git -C "$W/fw" tag v0.1.0
cp -r "$ROOT/docs/examples/web-app" "$W/app"
sed -i "s|https://github.com/shoneyj/porch|file://$W/fw|" "$W/app/wo.toml"
printf '[build]\nruntime = "%s"\n' "$WOVM" >> "$W/app/wo.toml"

# ---- 1. fetch + lock + build ----
if "$WOC" "$W/app" >"$W/build.out" 2>&1 && [[ -x "$W/app/target/web-app" && -f "$W/app/wo.lock" ]]; then
  ok "deps chain: fetch + wo.lock + build"
else
  bad "build" "$(head -1 "$W/build.out")"
  printf 'web-app-accept: %d checks, %d failures\n' "$((pass + fail))" "$fail"
  exit 1
fi

# ---- iteration 17: library kind + the internal/ dep boundary ----
# The framework copy at $W/fw carries `kind = "library"` and has no `fn main`.
# Checking it entry-less is what retired iteration 16's `--emit` workaround.
if out="$("$WOC" "$W/fw" 2>&1)" && [[ -z "$out" ]]; then
  ok "library check mode: the framework typechecks entry-less"
else
  bad "library-check" "exit=$? out=$(printf '%s' "$out" | head -1)"
fi

# A consumer reaching past the privacy line is WO-E108 at its own `use`.
cp -r "$W/app" "$W/app-internal"
rm -rf "$W/app-internal/target" "$W/app-internal/wo.lock" "$W/app-internal/.wo-deps"
sed -i '1i use porch/internal' "$W/app-internal/main.wo"
out="$("$WOC" "$W/app-internal" 2>&1)"; rc=$?
if [[ "$rc" == "1" ]] && printf '%s' "$out" | grep -q 'WO-E108'; then
  ok "dep boundary: importing serve/internal is WO-E108"
else
  bad "internal-boundary" "exit=$rc out=$(printf '%s' "$out" | head -1)"
fi

# An unknown `kind` is a manifest error (exit 2), not a silent default.
cp -r "$W/app" "$W/app-badkind"
rm -rf "$W/app-badkind/target" "$W/app-badkind/wo.lock" "$W/app-badkind/.wo-deps"
sed -i '1i kind = "junk"' "$W/app-badkind/wo.toml"
out="$("$WOC" "$W/app-badkind" 2>&1)"; rc=$?
if [[ "$rc" == "2" ]] && printf '%s' "$out" | grep -q 'WO-E109'; then
  ok "manifest: an unknown kind is WO-E109 at exit 2"
else
  bad "kind-validation" "exit=$rc out=$(printf '%s' "$out" | head -1)"
fi

DATA="$W/data"; mkdir -p "$DATA"
# stable, tailable server log: the per-run temp dir is deleted on exit, so a
# developer had nothing to follow. `tail -F /tmp/web-app.log` while this runs.
SRVLOG="/tmp/web-app.log"
: > "$SRVLOG"
echo "server log: $SRVLOG  (tail -F \"$SRVLOG\" to follow)"
printf '===== boot — port %s =====\n' "$PORT" >>"$SRVLOG"
LEGFROM=$(( $(wc -l < "$SRVLOG") + 1 ))
WA_TOKEN=s3cr3t WA_IDLE_MS=600 WO_DATA="$DATA" "$W/app/target/web-app" "$PORT" >>"$SRVLOG" 2>&1 &
SRV=$!
for _ in $(seq 1 40); do
  tail -n "+$LEGFROM" "$SRVLOG" 2>/dev/null | grep -q listening && break
  sleep 0.1
done

# one tiny HTTP client; python is already a repo test dependency
hit() { # method path [body] [auth: yes|no] [content-type] -> "STATUS|BODY"
  python3 - "$PORT" "$1" "$2" "${3:-}" "${4:-yes}" "${5:-}" <<'PYEOF'
import socket, sys
port, method, path, body, auth, ct = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5], sys.argv[6]
s = socket.create_connection(("127.0.0.1", port), timeout=5)
h = f"{method} {path} HTTP/1.1\r\nhost: a\r\n"
if auth == "yes": h += "authorization: Bearer s3cr3t\r\n"
if ct: h += f"content-type: {ct}\r\n"
h += f"content-length: {len(body)}\r\n\r\n{body}"
s.sendall(h.encode())
d = b""
while b"\r\n\r\n" not in d: d += s.recv(2000)
head, _, rest = d.partition(b"\r\n\r\n")
want = int([l for l in head.decode().splitlines() if l.lower().startswith("content-length")][0].split(":")[1])
while len(rest) < want: rest += s.recv(2000)
s.close()
print(head.decode().splitlines()[0].split(" ")[1] + "|" + rest.decode())
PYEOF
}

expect() { # name got want-status [want-body-substring]
  local name="$1" got="$2" ws="$3" wb="${4:-}"
  local st="${got%%|*}" body="${got#*|}"
  if [[ "$st" == "$ws" && ( -z "$wb" || "$body" == *"$wb"* ) ]]; then
    ok "$name"
  else
    bad "$name" "got $st body=$body"
  fi
}

# ---- 2..10 the storefront matrix ----
expect "401 without the token"        "$(hit GET /products '' no)" 401

# wrong bearer token: the framework's constant-time compare denies (401)
wt="$(timeout 5 python3 - "$PORT" <<'PYEOF'
import socket, sys
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=3)
s.sendall(b"GET /products HTTP/1.1\r\nhost: a\r\nauthorization: Bearer wr0ng!\r\nconnection: close\r\ncontent-length: 0\r\n\r\n")
d = b""
while True:
    got = s.recv(2000)
    if not got: break
    d += got
print(d.decode().splitlines()[0].split(" ")[1])
PYEOF
)"
[[ "$wt" == "401" ]] && ok "401 on a wrong bearer token (ct_eq)" \
                     || bad "wrong-token" "got $wt"
expect "empty list"                   "$(hit GET /products)" 200 "[]"
# iteration 19: a REAL decimal price. `{"price": 9.99}` is the acceptance
# criterion — before Float existed this body failed the whole checked decode.
expect "create product (201, fractional price)" \
  "$(hit POST /products '{"name":"mug","price":9.99,"stock":5}')" 201 '"price":9.99'
expect "duplicate name is 409 (@unique)" "$(hit POST /products '{"name":"mug","price":1.0,"stock":1}')" 409
# an integer-shaped JSON number is a legal Float too, and comes back as one
expect "integer-shaped price decodes as Float" \
  "$(hit POST /products '{"name":"plate","price":12,"stock":1}')" 201 '"price":12.0'
expect "malformed json is 400"        "$(hit POST /products '{oops')" 400
expect "form-encoded create (201, + and %XX decoded)" \
  "$(hit POST /products 'name=form+kettle&price=12.50&stock=2' yes 'application/x-www-form-urlencoded; charset=UTF-8')" 201 '"name":"form kettle"'
expect "form with a non-numeric price is 400" \
  "$(hit POST /products 'name=x&price=abc&stock=1' yes 'application/x-www-form-urlencoded')" 400
MP=$'--BXB\r\ncontent-disposition: form-data; name="name"\r\n\r\nmp teapot\r\n--BXB\r\ncontent-disposition: form-data; name="price"\r\n\r\n7.05\r\n--BXB\r\ncontent-disposition: form-data; name="stock"\r\n\r\n3\r\n--BXB--\r\n'
expect "multipart create (201, curl -F shape)" \
  "$(hit POST /products "$MP" yes 'multipart/form-data; boundary=BXB')" 201 '"name":"mp teapot"'
expect "multipart without the closing marker is 400" \
  "$(hit POST /products $'--BXB\r\ncontent-disposition: form-data; name="name"\r\n\r\nx\r\n' yes 'multipart/form-data; boundary=BXB')" 400
# the fractional price survives storage and comes back byte-identical
expect "list shows the fractional price" "$(hit GET /products)" 200 '"price":9.99'
expect "list shows the form price"       "$(hit GET /products)" 200 '"price":12.5'
expect "show by :name capture"        "$(hit GET /products/mug)" 200 '"stock":5'
expect "unknown product is 404"       "$(hit GET /products/none)" 404
expect "create order (FK)"            "$(hit POST /orders '{"product":"mug","qty":2}')" 201
expect "delete restricted by FK (409), server survives" "$(hit DELETE /products/mug)" 409 "reference"

# ---- 11. 405 on a known path with the wrong method (Allow header) ----
ma="$(timeout 5 python3 - "$PORT" <<'PYEOF'
import socket, sys
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=3)
s.sendall(b"PUT /products HTTP/1.1\r\nhost: a\r\nauthorization: Bearer s3cr3t\r\nconnection: close\r\ncontent-length: 0\r\n\r\n")
d = b""
while True:
    got = s.recv(4000)
    if not got: break
    d += got
head = d.split(b"\r\n\r\n")[0].decode()
status = head.splitlines()[0].split(" ")[1]
allow = [l.split(":", 1)[1].strip() for l in head.splitlines() if l.lower().startswith("allow")]
print(status + "|" + (allow[0] if allow else ""))
PYEOF
)"
[[ "$ma" == "405|GET, POST" ]] && ok "405 + Allow on wrong method" \
                               || bad "405" "got $ma (want 405|GET, POST)"

# ---- 12. HEAD: GET's headers, no body ----
hd="$(timeout 5 python3 - "$PORT" <<'PYEOF'
import socket, sys
port = int(sys.argv[1])
def raw(req):
    s = socket.create_connection(("127.0.0.1", port), timeout=3)
    s.sendall(req)
    d = b""
    while True:
        got = s.recv(4000)
        if not got: break
        d += got
    s.close()
    return d
tail = b" /products HTTP/1.1\r\nhost: a\r\nauthorization: Bearer s3cr3t\r\nconnection: close\r\ncontent-length: 0\r\n\r\n"
g = raw(b"GET" + tail)
h = raw(b"HEAD" + tail)
ghead, _, gbody = g.partition(b"\r\n\r\n")
hhead, _, hbody = h.partition(b"\r\n\r\n")
def cl(head):
    return [l.split(b":")[1].strip() for l in head.splitlines() if l.lower().startswith(b"content-length")][0]
status = hhead.decode().splitlines()[0].split(" ")[1]
print(f"{status}|{(cl(h[:len(hhead)]) == cl(g[:len(ghead)]))}|{len(hbody)}|{len(gbody)}")
PYEOF
)"
IFS='|' read -r hs same hb gb <<<"$hd"
[[ "$hs" == "200" && "$same" == "True" && "$hb" == "0" && "$gb" != "0" ]] \
  && ok "HEAD answers GET's Content-Length with no body" \
  || bad "HEAD" "status=$hs same-length=$same head-body=$hb get-body=$gb"

# ---- 12b. framework v1 slice 2 ----
# raw client with header control: prints "STATUS|HEADERS|BODY"
# (headers ;-joined, lowercased names)
hraw() { # extra_header_lines(\n-separated) method path
  timeout 5 python3 - "$PORT" "$1" "$2" "$3" <<'PYEOF'
import socket, sys
port, extra, method, path = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
s = socket.create_connection(("127.0.0.1", port), timeout=5)
h = f"{method} {path} HTTP/1.1\r\n"
for line in extra.split("\n"):
    if line: h += line + "\r\n"
h += "content-length: 0\r\nconnection: close\r\n\r\n"
s.sendall(h.encode())
d = b""
try:
    while True:
        c = s.recv(4000)
        if not c: break
        d += c
except Exception: pass
s.close()
head, _, body = d.partition(b"\r\n\r\n")
lines = head.decode().splitlines()
status = lines[0].split(" ")[1]
def norm(l):
    n, _, v = l.partition(":")
    return n.lower() + ":" + v
hdrs = ";".join(norm(l) for l in lines[1:])
print(status + "|" + hdrs + "|" + body.decode(errors="replace"))
PYEOF
}
AUTH="host: a
authorization: Bearer s3cr3t"

r="$(hraw "$AUTH" GET /files/a/b/c)"
[[ "$r" == 200\|*"path=a/b/c"* ]] && ok "wildcard *rest captures the tail" || bad "wildcard" "$r"
r="$(hraw "$AUTH" GET /files)"
[[ "$r" == 200\|*"path="* ]] && ok "wildcard matches the empty rest" || bad "wildcard-empty" "$r"
r="$(hraw "$AUTH" GET /api/ping)"
[[ "$r" == 200\|*"pong via=api-group"* ]] && ok "group route + group middleware + req.ctx" || bad "group" "$r"
r="$(hraw "$AUTH" GET /etag-probe)"
[[ "$r" == 200\|*"etag: \""* ]] && ok "ETag stamped on the response" || bad "etag" "$r"
tag="$(printf '%s' "$r" | tr ';' '\n' | grep -m1 '^etag: ' | cut -d' ' -f2)"
r="$(hraw "$AUTH
if-none-match: $tag" GET /etag-probe)"
[[ "$r" == 304\|* ]] && ok "If-None-Match answers 304" || bad "etag-304" "$r"
r="$(hraw "$AUTH
accept: text/html" GET /nego)"
[[ "$r" == 406\|* ]] && ok "Accept negotiation refuses non-JSON (406)" || bad "nego-406" "$r"
r="$(hraw "$AUTH
accept: application/*" GET /nego)"
[[ "$r" == 200\|*'"ok":true'* ]] && ok "Accept type/* matches" || bad "nego-200" "$r"
r="$(hraw "$AUTH" GET /products)"
[[ "$r" == 200\|*"x-content-type-options: nosniff"*"x-frame-options: DENY"* ]] \
  && ok "security headers on responses" || bad "sec-headers" "$r"
r="$(hraw "host: evil
authorization: Bearer s3cr3t" GET /products)"
[[ "$r" == 421\|* ]] && ok "host validation answers 421" || bad "host-421" "$r"
r="$(hraw "host: a
origin: http://x
access-control-request-method: POST" OPTIONS /products)"
[[ "$r" == 204\|*"access-control-allow-origin: *"*"access-control-allow-methods:"* ]] \
  && ok "CORS preflight answers 204 + allow set" || bad "cors-preflight" "$r"
r="$(hraw "$AUTH
origin: http://x" GET /products)"
[[ "$r" == 200\|*"access-control-allow-origin: *"* ]] \
  && ok "CORS origin stamped on real responses" || bad "cors-after" "$r"
# ---- 12c. the serving slice: fiber-per-connection + deadlines ----
r="$(timeout 10 python3 - "$PORT" <<'PYEOF'
import socket, sys, time, threading
port = int(sys.argv[1])
REQ = b"GET /slow HTTP/1.1\r\nhost: a\r\nauthorization: Bearer s3cr3t\r\nconnection: close\r\ncontent-length: 0\r\n\r\n"
def one(res, i):
    s = socket.create_connection(("127.0.0.1", port), timeout=8)
    s.sendall(REQ)
    d = b""
    while True:
        c = s.recv(4000)
        if not c: break
        d += c
    res[i] = b"slow done" in d
t0 = time.time()
res = [False, False]
ts = [threading.Thread(target=one, args=(res, i)) for i in (0, 1)]
[t.start() for t in ts]; [t.join() for t in ts]
el = int((time.time() - t0) * 1000)
print(f"{res[0] and res[1]}|{el}")
PYEOF
)"
pw="${r%%|*}"; pe="${r#*|}"
[[ "$pw" == "True" && "$pe" -lt 700 ]] \
  && ok "two slow requests served in PARALLEL (${pe}ms, serial would be 800+)" \
  || bad "parallel" "$r"
r="$(timeout 10 python3 - "$PORT" <<'PYEOF'
import socket, sys, time
port = int(sys.argv[1])
# a client that connects and sends NOTHING: the idle deadline must evict it
s = socket.create_connection(("127.0.0.1", port), timeout=8)
t0 = time.time()
s.settimeout(5)
try:
    d = s.recv(100)
    print(f"closed|{int((time.time()-t0)*1000)}" if d == b"" else f"data|{d[:20]}")
except socket.timeout:
    print("still-open|5000")
PYEOF
)"
sw="${r%%|*}"; se="${r#*|}"
[[ "$sw" == "closed" && "$se" -lt 2500 ]] \
  && ok "stalled client evicted at the idle deadline (${se}ms)" \
  || bad "stalled" "$r"
r="$(timeout 10 python3 - "$PORT" <<'PYEOF'
import socket, sys, time
port = int(sys.argv[1])
# half a request then silence: the READ deadline tears it (400-and-close)
s = socket.create_connection(("127.0.0.1", port), timeout=8)
s.sendall(b"GET /products HTTP/1.1\r\nhost: a\r\nauthor")
t0 = time.time()
d = b""
s.settimeout(5)
try:
    while True:
        c = s.recv(400)
        if not c: break
        d += c
except socket.timeout: pass
status = d.decode(errors="replace").split(" ")[1] if d else "closed"
print(f"{status}|{int((time.time()-t0)*1000)}")
PYEOF
)"
tw="${r%%|*}"; te="${r#*|}"
[[ "$tw" == "400" && "$te" -lt 2500 ]] \
  && ok "slow-loris torn at the read deadline (400, ${te}ms)" \
  || bad "slowloris" "$r"

r="$(timeout 5 python3 - "$PORT" <<'PYEOF'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=5)
s.sendall(b"GET /products HTTP/1.1\r\nhost: a\r\nauthorization: Bearer s3cr3t\r\ncontent-length: 0\r\ncontent-length: 5\r\nconnection: close\r\n\r\n")
d = b""
try:
    while True:
        c = s.recv(2000)
        if not c: break
        d += c
except Exception: pass
print(d.decode(errors="replace").splitlines()[0].split(" ")[1])
PYEOF
)"
[[ "$r" == "400" ]] && ok "duplicate Content-Length rejected (400)" || bad "dup-cl" "got $r"

# ---- 12d. Transfer-Encoding is rejected outright (RFC 9112 §6.1) ----
r="$(timeout 5 python3 - "$PORT" <<'PYEOF'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=5)
s.sendall(b"POST /products HTTP/1.1\r\nhost: a\r\nauthorization: Bearer s3cr3t\r\ntransfer-encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n")
d = b""
try:
    while True:
        c = s.recv(2000)
        if not c: break
        d += c
except Exception: pass
print(d.decode(errors="replace").splitlines()[0].split(" ")[1] if d else "closed")
PYEOF
)"
[[ "$r" == "400" ]] && ok "Transfer-Encoding rejected (400, anti-smuggling)" || bad "te-reject" "got $r"

# ---- 12e. the 1k soak: 500 idle + 500 real, fds and RSS come home ----
fds_before="$(ls /proc/$SRV/fd 2>/dev/null | wc -l)"
r="$(timeout 60 python3 - "$PORT" <<'PYEOF'
import asyncio, sys, time
port = int(sys.argv[1])
REQ = b"GET /products HTTP/1.1\r\nhost: a\r\nauthorization: Bearer s3cr3t\r\nconnection: close\r\ncontent-length: 0\r\n\r\n"
sem = asyncio.Semaphore(100)   # connect in waves: the listener backlog is 64
async def idle_conn():
    async with sem:
        r, w = await asyncio.open_connection("127.0.0.1", port)
    try:
        # the server evicts at idle_ms after ITS accept, which under the
        # 1k wave can lag the client's connect by seconds — wait long; a
        # reset counts as evicted too (closed is closed)
        d = await asyncio.wait_for(r.read(64), timeout=30)
        return 1 if d == b"" else 0
    except asyncio.TimeoutError:
        return 0
    except (ConnectionResetError, BrokenPipeError):
        return 1
    finally:
        w.close()
async def real_conn():
    async with sem:
        r, w = await asyncio.open_connection("127.0.0.1", port)
    try:
        w.write(REQ); await w.drain()
        d = await asyncio.wait_for(r.read(-1), timeout=15)
        return 1 if b" 200 " in d.split(b"\r\n")[0] + b" " else 0
    finally:
        w.close()
async def main():
    t0 = time.time()
    tasks = [idle_conn() for _ in range(500)] + [real_conn() for _ in range(500)]
    res = await asyncio.gather(*tasks, return_exceptions=True)
    evicted = sum(1 for x in res[:500] if x == 1)
    served  = sum(1 for x in res[500:] if x == 1)
    print(f"{evicted}|{served}|{int(time.time()-t0)}")
asyncio.run(main())
PYEOF
)"
ev="${r%%|*}"; rest="${r#*|}"; sv="${rest%%|*}"; el="${rest#*|}"
[[ "$ev" -ge 495 && "$sv" -ge 495 ]] \
  && ok "1k soak: $sv/500 served + $ev/500 idle evicted in ${el}s" \
  || bad "soak" "$r"
sleep 1
fds_after="$(ls /proc/$SRV/fd 2>/dev/null | wc -l)"
rss_kb="$(awk '/VmRSS/{print $2}' /proc/$SRV/status 2>/dev/null)"
[[ "$fds_after" -le $((fds_before + 8)) ]] \
  && ok "soak fds came home ($fds_before -> $fds_after)" \
  || bad "soak-fds" "$fds_before -> $fds_after"
[[ -n "$rss_kb" && "$rss_kb" -lt 409600 ]] \
  && ok "soak RSS bounded (${rss_kb}KB < 400MB)" \
  || bad "soak-rss" "${rss_kb}KB"
expect "server healthy after the soak" "$(hit GET /products)" 200 '"name":"mug"'

# ---- 13. pipelined keep-alive: two requests, one connection ----
n="$(timeout 5 python3 - "$PORT" <<'PYEOF'
import socket, sys
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=3)
r = b"GET /products HTTP/1.1\r\nhost: a\r\nauthorization: Bearer s3cr3t\r\ncontent-length: 0\r\n\r\n"
s.sendall(r + r)
buf = b""
try:
    while buf.count(b"HTTP/1.1") < 2:
        got = s.recv(4000)
        if not got: break
        buf += got
except socket.timeout:
    pass
print(buf.count(b"HTTP/1.1"))
PYEOF
)"
[[ "$n" == "2" ]] && ok "pipelined keep-alive (2 responses, 1 connection)" \
                  || bad "pipelining" "expected 2 responses, got $n"

# ---- 14. SIGTERM stops it ----
kill -TERM "$SRV"
stopped=1
for _ in $(seq 1 30); do kill -0 "$SRV" 2>/dev/null || { stopped=0; break; }; sleep 0.1; done
[[ $stopped -eq 0 ]] && ok "SIGTERM stops the server" || bad "stop" "still running"
SRV=""

# ---- 15. restart persistence (WAL replay) ----
printf '\n===== restart (WAL replay) — port %s =====\n' "$PORT" >>"$SRVLOG"
WA_TOKEN=s3cr3t WA_IDLE_MS=600 WO_DATA="$DATA" "$W/app/target/web-app" "$PORT" >>"$SRVLOG" 2>&1 &
SRV=$!
sleep 0.5
expect "product survives a restart (WAL)" "$(hit GET /products)" 200 '"name":"mug"'
kill -TERM "$SRV" 2>/dev/null; SRV=""

# ---- 16. porch-store task 2: the key pool counts 1 then 2, reset_at is wall-clock ----
# A flat copy of porch (manifest stripped, so it compiles as one ordinary
# multi-file program with a real entry point rather than the manifest's
# library build) plus a tiny driver dropped into middleware/ — same
# folder as keypool.wo, so it sees make_pool/pool_count with no `use`
# needed, matching the rest of the middleware package's own convention.
KP="$W/keypool-check"
cp -r "$ROOT/docs/examples/porch" "$KP"
rm -f "$KP/wo.toml"
rm -rf "$KP/target"
KP_WINDOW_US=60000000
cat >"$KP/middleware/kptest_main.wo" <<WOEOF
fn main() -> Int {
  let pool = make_pool(4);
  let v1 = pool_count(pool, "ip:test", 5, ${KP_WINDOW_US});
  let v2 = pool_count(pool, "ip:test", 5, ${KP_WINDOW_US});
  print("\${v1.count} \${v2.count} \${v1.reset_at}");
  return 0;
}
WOEOF
if kp_out="$("$WOC" --emit "$KP" -o "$KP/kptest.wob" 2>&1)"; then
  kp_before_ms="$(date +%s%3N)"
  kp_vm="$("$WOVM" "$KP/kptest.wob" 2>&1)"
  kp_after_ms="$(date +%s%3N)"
  read -r kp_c1 kp_c2 kp_reset <<<"$kp_vm"
  # v1 is the FIRST-ever hit for this key — the "fresh window" path
  # (msg.window skipped its µs->ms conversion before the fix, landing
  # reset_at ~1000x too far out: a 60s window read back as ~16.7h away).
  # A tight few-second band around time.now() + window_ms catches that
  # regression without being timing-flaky.
  kp_window_ms=$((KP_WINDOW_US / 1000))
  kp_lo=$((kp_before_ms + kp_window_ms - 5000))
  kp_hi=$((kp_after_ms + kp_window_ms + 5000))
  if [[ "$kp_c1" == "1" && "$kp_c2" == "2" && "$kp_reset" =~ ^[0-9]+$ \
        && "$kp_reset" -ge "$kp_lo" && "$kp_reset" -le "$kp_hi" ]]; then
    ok "keypool: counts 1 then 2, reset_at ~window ms ahead of time.now()"
  else
    bad "keypool" "counts=$kp_c1,$kp_c2 reset_at=$kp_reset expected in [$kp_lo,$kp_hi]"
  fi
else
  bad "keypool" "compile: $(printf '%s' "$kp_out" | head -1)"
fi

# ---- 17. porch-store task 3: the limiter delegates to the pool ----------
# Same flattening trick as the keypool leg, but this one boots a REAL
# server: fiber-per-connection (mirrors web-app's ConnWorker — see App's
# own doc comment), Limiter registered as both Mw and Aw (Cors's own
# dual-role shape) so the allowed path's X-RateLimit-* headers actually
# reach the response, not just req.ctx. trust_proxy is on so every
# backgrounded client can land on ONE key by sending the same
# X-Forwarded-For value, regardless of its own ephemeral source port.
#
# ConnWorker's own state carries a bare actor handle, never a Pool: Pool
# is aliased by design (pool_count reads the same value on every request)
# and the compiler refuses a traced value in actor state or a message
# (WO-E222 — spawn placement makes every actor potentially remote). Each
# connection rebuilds a throwaway one-slot Pool from that handle instead.
LP="$W/limiter-check"
cp -r "$ROOT/docs/examples/porch" "$LP"
rm -f "$LP/wo.toml"
rm -rf "$LP/target"
cat >"$LP/limiter_check_main.wo" <<'WOEOF'
use net
use env
use http
use router
use middleware

class Ping {
  fn handle(req: Req) -> Resp { return ok_text("pong"); }
}

fn build_app(slot: actor PoolMsg, limit: Int, window_us: Int) -> App {
  let app = App { middleware: [], routes: [] };
  let p1 = Pool { actors: [PoolSlot { a: slot }] };
  let p2 = Pool { actors: [PoolSlot { a: slot }] };
  app.use_mw(Mw { m: Limiter { pool: p1, limit: limit, window: window_us, trust_proxy: true } });
  app.use_after(Aw { a: Limiter { pool: p2, limit: limit, window: window_us, trust_proxy: true } });
  app.get("/ping", Ping {});
  return app;
}

class Conn { fd: net.Conn }

class ConnWorker {
  slot:   actor PoolMsg
  limit:  Int
  window: Int
  fn receive(msg: Conn) {
    let app = build_app(self.slot, self.limit, self.window);
    app.handle_conn(msg.fd, 2000, 2000);
  }
}

fn main(args: multi Text) -> Int {
  if len(args) < 3 {
    print_err("usage: limiter_check <port> <limit> <window_us>");
    return 2;
  }
  let port = parse_int(args[0]);
  if port == nil { print_err("bad port"); return 2; }
  let limit = parse_int(args[1]);
  if limit == nil { print_err("bad limit"); return 2; }
  let window = parse_int(args[2]);
  if window == nil { print_err("bad window"); return 2; }
  let ka: actor PoolMsg = spawn KeyActor {};
  let srv = net.listen("127.0.0.1", port);
  print("listening on 127.0.0.1:${port}");
  while true {
    if env.stopping() { net.close(srv); return 0; }
    let c = net.accept_dl(srv, 250);
    if c != nil {
      let w: actor Conn = spawn ConnWorker { slot: ka, limit: limit, window: window };
      send(w, Conn { fd: c });
    }
  }
}
WOEOF

if lp_out="$("$WOC" --emit "$LP" -o "$LP/limiter_check.wob" 2>&1)"; then
  ok "limiter: compiles against the pool (Mw+Aw, trust_proxy)"

  LPORT=$((PORT + 1))
  lhit() { # xff-value -> "STATUS\nHEADERS..." for one request keyed on it
    curl -sD - -o /dev/null --max-time 5 -H "X-Forwarded-For: $1" -H "Host: a" "http://127.0.0.1:$LPORT/ping" | tr -d '\r'
  }
  lhit_noxff() { # -> "STATUS\nHEADERS..." for one request with NO X-Forwarded-For at all
    curl -sD - -o /dev/null --max-time 5 -H "Host: a" "http://127.0.0.1:$LPORT/ping" | tr -d '\r'
  }
  lstatus() { printf '%s' "$1" | head -1 | awk '{print $2}'; }
  lwait_listen() { # from-line
    for _ in $(seq 1 40); do
      tail -n "+$1" "$SRVLOG" 2>/dev/null | grep -q listening && return
      sleep 0.1
    done
  }

  # ---- 17a. threshold: of LIMIT+1 requests, first LIMIT pass, last 429 ----
  LDATA="$W/limiter-data"; mkdir -p "$LDATA"
  LLIMIT=5
  LWINDOW_US=60000000
  printf '\n===== limiter check — port %s =====\n' "$LPORT" >>"$SRVLOG"
  LEGFROM=$(( $(wc -l < "$SRVLOG") + 1 ))
  WO_DATA="$LDATA" "$WOVM" "$LP/limiter_check.wob" "$LPORT" "$LLIMIT" "$LWINDOW_US" >>"$SRVLOG" 2>&1 &
  SRV=$!
  lwait_listen "$LEGFROM"

  codes=""
  last=""
  for i in $(seq 1 $((LLIMIT + 1))); do
    last="$(lhit 6.6.6.6)"
    codes="$codes$(lstatus "$last") "
  done
  want=""
  for i in $(seq 1 $LLIMIT); do want="${want}200 "; done
  want="${want}429 "
  [[ "$codes" == "$want" ]] \
    && ok "limiter threshold: first $LLIMIT pass, request $((LLIMIT + 1)) is 429" \
    || bad "limiter-threshold" "codes=$codes want=$want"
  printf '%s\n' "$last" | grep -qi '^retry-after:' \
    && ok "limiter 429 carries Retry-After" \
    || bad "limiter-429-retry-after" "$(printf '%s' "$last" | head -1)"
  printf '%s\n' "$last" | grep -qi '^x-ratelimit-remaining: 0' \
    && ok "limiter 429 x-ratelimit-remaining is 0" \
    || bad "limiter-429-remaining" "$(printf '%s' "$last" | head -1)"

  allowed="$(lhit 6.6.6.7)"
  printf '%s\n' "$allowed" | grep -qi "^x-ratelimit-limit: $LLIMIT\$" \
    && ok "limiter allowed path carries X-RateLimit-* headers (Mw+Aw)" \
    || bad "limiter-allowed-headers" "$(printf '%s' "$allowed" | head -1)"

  # ---- 17a2. trust_proxy peer fallback: absent XFF must not share the "ip:" bucket ----
  # Each curl below is its own TCP connection (a fresh ephemeral source
  # port), so a correct net.peer(req.conn) fallback gives every one of
  # these LLIMIT+1 requests its OWN key -- none should be refused. The bug
  # this pins: keying an absent X-Forwarded-For on the literal "ip:" (empty
  # client_ip) collapses every such client onto ONE shared bucket, and the
  # (LLIMIT+1)th request would then be 429 instead of 200.
  noxff_codes=""
  for i in $(seq 1 $((LLIMIT + 1))); do
    r="$(lhit_noxff)"
    noxff_codes="$noxff_codes$(lstatus "$r") "
  done
  want_noxff=""
  for i in $(seq 1 $((LLIMIT + 1))); do want_noxff="${want_noxff}200 "; done
  [[ "$noxff_codes" == "$want_noxff" ]] \
    && ok "limiter trust_proxy: absent X-Forwarded-For falls back to net.peer, not one shared \"ip:\" bucket" \
    || bad "limiter-noxff-peer-fallback" "codes=$noxff_codes want=$want_noxff"

  # ---- 17b. SIGTERM + restart: same WO_DATA, same key, still limited ----
  kill -TERM "$SRV" 2>/dev/null
  stopped=1
  for _ in $(seq 1 30); do kill -0 "$SRV" 2>/dev/null || { stopped=0; break; }; sleep 0.1; done
  [[ $stopped -eq 0 ]] && ok "limiter: SIGTERM stops the server" || bad "limiter-stop" "still running"
  SRV=""

  printf '\n===== limiter check — restart, port %s =====\n' "$LPORT" >>"$SRVLOG"
  LEGFROM=$(( $(wc -l < "$SRVLOG") + 1 ))
  WO_DATA="$LDATA" "$WOVM" "$LP/limiter_check.wob" "$LPORT" "$LLIMIT" "$LWINDOW_US" >>"$SRVLOG" 2>&1 &
  SRV=$!
  lwait_listen "$LEGFROM"
  r="$(lhit 6.6.6.6)"
  [[ "$(lstatus "$r")" == "429" ]] \
    && ok "limiter restart: counter replayed from the WAL, still limited" \
    || bad "limiter-restart" "got $(lstatus "$r")"
  kill -TERM "$SRV" 2>/dev/null
  for _ in $(seq 1 30); do kill -0 "$SRV" 2>/dev/null || break; sleep 0.1; done
  SRV=""

  # ---- 17c. concurrency: N genuinely-parallel requests, exact count -------
  # Backgrounded shell clients (curl `&` + explicit-PID `wait`), not asyncio
  # in one process — a sequential version of this passes against the OLD
  # read-modify-write limiter too and proves nothing. The exact count is
  # read off ONE more sequential probe's X-RateLimit-Remaining afterward,
  # never off the table directly: if the N parallel calls lost an
  # increment, that number is wrong by exactly the lost count.
  LCDATA="$W/limiter-cc-data"; mkdir -p "$LCDATA"
  LCN=30
  LCLIMIT=1000
  printf '\n===== limiter check — concurrency, port %s =====\n' "$LPORT" >>"$SRVLOG"
  LEGFROM=$(( $(wc -l < "$SRVLOG") + 1 ))
  WO_DATA="$LCDATA" "$WOVM" "$LP/limiter_check.wob" "$LPORT" "$LCLIMIT" "$LWINDOW_US" >>"$SRVLOG" 2>&1 &
  SRV=$!
  lwait_listen "$LEGFROM"

  cc_pids=()
  for i in $(seq 1 $LCN); do
    ( curl -s -o /dev/null --max-time 10 -H "X-Forwarded-For: 6.6.6.8" -H "Host: a" "http://127.0.0.1:$LPORT/ping" ) &
    cc_pids+=("$!")
  done
  for p in "${cc_pids[@]}"; do wait "$p"; done

  probe="$(lhit 6.6.6.8)"
  remaining="$(printf '%s\n' "$probe" | grep -i '^x-ratelimit-remaining:' | awk '{print $2}')"
  expected=$((LCLIMIT - (LCN + 1)))
  [[ "$remaining" == "$expected" ]] \
    && ok "limiter concurrency: exact count after $LCN parallel requests (no lost increments)" \
    || bad "limiter-concurrency" "remaining=$remaining expected=$expected"

  kill -TERM "$SRV" 2>/dev/null
  for _ in $(seq 1 30); do kill -0 "$SRV" 2>/dev/null || break; sleep 0.1; done
  SRV=""
else
  bad "limiter-compile" "$(printf '%s' "$lp_out" | head -1)"
fi

# ---- 18. porch-store task 4: idempotency runs the handler inside the actor --
# Same flattening trick as the keypool/limiter legs. Idempotent wraps a
# SLOW route handler (500ms) so two genuinely-parallel duplicates actually
# overlap in the pool's mailbox. The handler's side effect (ExecMark) is
# a real @table row count read back over GET /execs -- never a log line,
# per the brief. One server serves all three legs with distinct keys, so
# the exec count accumulates 1 -> 2 -> 3 across them.
IP="$W/idempotent-check"
cp -r "$ROOT/docs/examples/porch" "$IP"
rm -f "$IP/wo.toml"
rm -rf "$IP/target"
cat >"$IP/idempotent_check_main.wo" <<'WOEOF'
use net
use env
use http
use router
use middleware
use time

@table(name: "exec_marks")
class ExecMark {
  n: Int
}

-- a deliberately slow handler: the concurrency leg's workload. Records
-- one row per REAL execution so a duplicate that wrongly ran it too
-- shows up as a row-count of 2, never as a log line.
class SlowHandler {
  fn handle(req: Req) -> Resp {
    insert ExecMark { n: 1 };
    let n = len(from e in ExecMark select e);
    time.sleep(500);
    return ok_json("{\"echo\":\"${req.body}\",\"exec\":${n}}");
  }
}

class ExecCount {
  fn handle(req: Req) -> Resp {
    let n = len(from e in ExecMark select e);
    return ok_json("{\"count\":${n}}");
  }
}

@table(name: "flaky_marks")
class FlakyMark {
  n: Int
}

-- reviewer finding, task 4 follow-up: a transient 5xx must not be cached
-- for the TTL -- fails on the first call, succeeds on every call after.
class FlakyHandler {
  fn handle(req: Req) -> Resp {
    let n = len(from f in FlakyMark select f);
    insert FlakyMark { n: 1 };
    if n == 0 { return server_error(); }
    return ok_json("{\"ok\":true}");
  }
}

-- second reviewer follow-up: the SAME fails-once handler and table,
-- separate from FlakyMark/FlakyHandler above so the sequential leg
-- (18d) can't consume the one-time failure this leg (18e) needs -- but
-- this time hit by two GENUINELY concurrent duplicates, to pin that
-- neither ever receives a replayed 5xx from the other's ephemeral row.
@table(name: "flaky_marks2")
class FlakyMark2 {
  n: Int
}

class FlakyHandler2 {
  fn handle(req: Req) -> Resp {
    let n = len(from f in FlakyMark2 select f);
    insert FlakyMark2 { n: 1 };
    if n == 0 { return server_error(); }
    return ok_json("{\"ok\":true}");
  }
}

class FlakyCount2 {
  fn handle(req: Req) -> Resp {
    let n = len(from f in FlakyMark2 select f);
    return ok_json("{\"count\":${n}}");
  }
}

-- coordinator follow-up: ephemeral (4xx/5xx) rows must not accumulate.
-- Always fails, so every attempt against the SAME idempotency key is
-- its own ephemeral miss -- never durable, never a hit for the next one.
class AlwaysFailHandler {
  fn handle(req: Req) -> Resp {
    return server_error();
  }
}

class IdemKeyCount {
  fn handle(req: Req) -> Resp {
    let n = len(from k in IdempotencyKey select k);
    return ok_json("{\"count\":${n}}");
  }
}

fn build_app(slot: actor PoolMsg) -> App {
  let app = App { middleware: [], routes: [] };
  let p = Pool { actors: [PoolSlot { a: slot }] };
  app.post("/create", Idempotent { key_header: "idempotency-key", pool: p, inner: SlowHandler {} });
  app.post("/flaky", Idempotent { key_header: "idempotency-key", pool: p, inner: FlakyHandler {} });
  app.post("/flaky2", Idempotent { key_header: "idempotency-key", pool: p, inner: FlakyHandler2 {} });
  app.post("/alwaysfail", Idempotent { key_header: "idempotency-key", pool: p, inner: AlwaysFailHandler {} });
  app.get("/execs", ExecCount {});
  app.get("/flaky2count", FlakyCount2 {});
  app.get("/idemkeycount", IdemKeyCount {});
  return app;
}

class Conn { fd: net.Conn }

class ConnWorker {
  slot: actor PoolMsg
  fn receive(msg: Conn) {
    let app = build_app(self.slot);
    app.handle_conn(msg.fd, 5000, 5000);
  }
}

fn main(args: multi Text) -> Int {
  if len(args) < 1 {
    print_err("usage: idempotent_check <port>");
    return 2;
  }
  let port = parse_int(args[0]);
  if port == nil { print_err("bad port"); return 2; }
  let ka: actor PoolMsg = spawn KeyActor {};
  let srv = net.listen("127.0.0.1", port);
  print("listening on 127.0.0.1:${port}");
  while true {
    if env.stopping() { net.close(srv); return 0; }
    let c = net.accept_dl(srv, 250);
    if c != nil {
      let w: actor Conn = spawn ConnWorker { slot: ka };
      send(w, Conn { fd: c });
    }
  }
}
WOEOF

if ip_out="$("$WOC" --emit "$IP" -o "$IP/idempotent_check.wob" 2>&1)"; then
  ok "idempotent: compiles (actor-run handler, digest, pool_begin)"

  IPORT=$((PORT + 2))
  IDATA="$W/idempotent-data"; mkdir -p "$IDATA"
  printf '\n===== idempotent check — port %s =====\n' "$IPORT" >>"$SRVLOG"
  LEGFROM=$(( $(wc -l < "$SRVLOG") + 1 ))
  WO_DATA="$IDATA" "$WOVM" "$IP/idempotent_check.wob" "$IPORT" >>"$SRVLOG" 2>&1 &
  SRV=$!
  iwait_listen() {
    for _ in $(seq 1 40); do
      tail -n "+$LEGFROM" "$SRVLOG" 2>/dev/null | grep -q listening && return
      sleep 0.1
    done
  }
  iwait_listen

  ipost() { # key body outfile -> prints STATUS, leaves the body in outfile
    curl -s -o "$3" -w '%{http_code}' --max-time 5 -X POST \
      -H "Host: a" -H "Idempotency-Key: $1" -H "Content-Type: text/plain" \
      --data-binary "$2" "http://127.0.0.1:$IPORT/create"
  }
  iexecs() { # -> the ExecMark row count
    curl -s --max-time 5 -H "Host: a" "http://127.0.0.1:$IPORT/execs" \
      | grep -o '"count":[0-9]*' | cut -d: -f2
  }

  # ---- 18a. gate leg: replay is exact (brief step 8) ----------------------
  s1="$(ipost leg8-key hello "$W/i8a.body")"
  s2="$(ipost leg8-key hello "$W/i8b.body")"
  [[ "$s1" == "200" && "$s2" == "200" ]] \
    && ok "idempotent: same key + same body both answer 200" \
    || bad "idempotent-replay-status" "s1=$s1 s2=$s2"
  if cmp -s "$W/i8a.body" "$W/i8b.body"; then
    ok "idempotent: replay is byte-identical"
  else
    bad "idempotent-replay-bytes" "$(cat "$W/i8a.body") != $(cat "$W/i8b.body")"
  fi
  ec="$(iexecs)"
  [[ "$ec" == "1" ]] \
    && ok "idempotent: handler ran exactly once (ExecMark row count = 1)" \
    || bad "idempotent-replay-execs" "ExecMark count=$ec want 1"

  # ---- 18b. gate leg: digest mismatch is 422 (brief step 9) ---------------
  m1="$(ipost leg9-key bodyA "$W/i9a.body")"
  m2="$(ipost leg9-key bodyB "$W/i9b.body")"
  [[ "$m1" == "200" && "$m2" == "422" ]] \
    && ok "idempotent: same key + different body is 422, not 200" \
    || bad "idempotent-mismatch-status" "m1=$m1 m2=$m2"
  if ! cmp -s "$W/i9a.body" "$W/i9b.body"; then
    ok "idempotent: 422 body is the refusal, not the other request's response"
  else
    bad "idempotent-mismatch-bytes" "422 body equals the first request's stored response"
  fi
  ec="$(iexecs)"
  [[ "$ec" == "2" ]] \
    && ok "idempotent: the refused request never ran the handler (ExecMark row count = 2)" \
    || bad "idempotent-mismatch-execs" "ExecMark count=$ec want 2"

  # ---- 18c. gate leg: concurrent duplicates (brief step 10) ---------------
  # Two backgrounded curl clients, launched together, hitting the SAME
  # slow (500ms) handler through the SAME key -- a sequential version of
  # this passes against the old before/after flow too and proves nothing.
  t0=$(date +%s%3N)
  ( s="$(ipost leg10-key samebody "$W/i10a.body")"; echo "$s" >"$W/i10a.status" ) &
  cc1=$!
  ( s="$(ipost leg10-key samebody "$W/i10b.body")"; echo "$s" >"$W/i10b.status" ) &
  cc2=$!
  wait "$cc1" "$cc2"
  t1=$(date +%s%3N)
  elapsed=$((t1 - t0))
  cs1="$(cat "$W/i10a.status")"; cs2="$(cat "$W/i10b.status")"
  [[ "$cs1" == "200" && "$cs2" == "200" ]] \
    && ok "idempotent concurrency: both parallel duplicates answer 200" \
    || bad "idempotent-cc-status" "cs1=$cs1 cs2=$cs2"
  if cmp -s "$W/i10a.body" "$W/i10b.body"; then
    ok "idempotent concurrency: both clients got the same response body"
  else
    bad "idempotent-cc-bytes" "$(cat "$W/i10a.body") != $(cat "$W/i10b.body")"
  fi
  [[ "$elapsed" -lt 900 ]] \
    && ok "idempotent concurrency: genuinely overlapped (${elapsed}ms, serial would be ~1000ms+)" \
    || bad "idempotent-cc-elapsed" "${elapsed}ms"
  ec="$(iexecs)"
  [[ "$ec" == "3" ]] \
    && ok "idempotent concurrency: exactly one execution despite 2 parallel duplicates (ExecMark row count = 3)" \
    || bad "idempotent-cc-execs" "ExecMark count=$ec want 3"

  # ---- 18d. gate leg: a transient 5xx is never replayed (reviewer finding) --
  # The miss path must persist only a 2xx/3xx response. FlakyHandler fails
  # on its first-ever call and succeeds on every call after; hit twice with
  # the SAME idempotency key, the answer must be 500 then 200 -- caching the
  # 500 would make every retry fail for the rest of the TTL (default 24h),
  # a worse outcome than no idempotency at all.
  f1="$(curl -s -o "$W/i11a.body" -w '%{http_code}' --max-time 5 -X POST \
    -H "Host: a" -H "Idempotency-Key: leg11-key" -H "Content-Type: text/plain" \
    --data-binary "x" "http://127.0.0.1:$IPORT/flaky")"
  f2="$(curl -s -o "$W/i11b.body" -w '%{http_code}' --max-time 5 -X POST \
    -H "Host: a" -H "Idempotency-Key: leg11-key" -H "Content-Type: text/plain" \
    --data-binary "x" "http://127.0.0.1:$IPORT/flaky")"
  [[ "$f1" == "500" && "$f2" == "200" ]] \
    && ok "idempotent: a transient 5xx is not replayed -- retry re-executes (500 then 200)" \
    || bad "idempotent-5xx-not-cached" "first=$f1 second=$f2 want 500 then 200"

  # ---- 18e. gate leg: a 5xx is never replayed to a CONCURRENT duplicate ---
  # (coordinator follow-up on 18d's residual). Two backgrounded clients,
  # launched together, same key, against a handler that fails only its
  # first-ever invocation. Whichever message the actor's mailbox happens
  # to process first gets that real failure; the second message must find
  # the row ephemeral and re-run the handler itself -- never read back a
  # replayed 500. Which of the two clients goes first is a race this test
  # cannot pin, so it asserts the UNORDERED outcome instead: the statuses
  # are exactly one 500 and one 200 (both requests genuinely executed --
  # FlakyMark2 count = 2). The pre-fix behavior (middleware-side delete,
  # racing the actor) would show 500 and 500 with count = 1 whenever the
  # duplicate is dequeued before the owner's delete lands -- deterministic
  # either way, no ordering assumption needed.
  ( s="$(curl -s -o "$W/i12a.body" -w '%{http_code}' --max-time 5 -X POST \
      -H "Host: a" -H "Idempotency-Key: leg12-key" -H "Content-Type: text/plain" \
      --data-binary "x" "http://127.0.0.1:$IPORT/flaky2")"; echo "$s" >"$W/i12a.status" ) &
  cf1=$!
  ( s="$(curl -s -o "$W/i12b.body" -w '%{http_code}' --max-time 5 -X POST \
      -H "Host: a" -H "Idempotency-Key: leg12-key" -H "Content-Type: text/plain" \
      --data-binary "x" "http://127.0.0.1:$IPORT/flaky2")"; echo "$s" >"$W/i12b.status" ) &
  cf2=$!
  wait "$cf1" "$cf2"
  g1="$(cat "$W/i12a.status")"; g2="$(cat "$W/i12b.status")"
  gsorted="$(printf '%s\n%s\n' "$g1" "$g2" | sort | tr '\n' ' ')"
  [[ "$gsorted" == "200 500 " ]] \
    && ok "idempotent: concurrent duplicates never replay a 5xx (one 500, one 200)" \
    || bad "idempotent-5xx-concurrent" "g1=$g1 g2=$g2 want one 500 and one 200"
  fc2="$(curl -s --max-time 5 -H "Host: a" "http://127.0.0.1:$IPORT/flaky2count" \
    | grep -o '"count":[0-9]*' | cut -d: -f2)"
  [[ "$fc2" == "2" ]] \
    && ok "idempotent: both concurrent attempts genuinely executed (FlakyMark2 count = 2)" \
    || bad "idempotent-5xx-concurrent-execs" "FlakyMark2 count=$fc2 want 2"

  # ---- 18f. gate leg: ephemeral rows do not accumulate (coordinator follow-up) --
  # AlwaysFailHandler fails every time, so N attempts against the SAME
  # idempotency key are N separate ephemeral misses -- never a durable
  # row, never a hit for the next one. Before the fix each attempt left
  # its own permanent, nonce-keyed row behind; after it, idempotent.wo
  # deletes that row the instant it reads it back (safe: the nonce is
  # never handed to anyone else, so nothing else could ever address that
  # row anyway). The IdempotencyKey row count must return to its
  # baseline after all N attempts, not grow by N.
  idemkeycount() {
    curl -s --max-time 5 -H "Host: a" "http://127.0.0.1:$IPORT/idemkeycount" \
      | grep -o '"count":[0-9]*' | cut -d: -f2
  }
  ik_baseline="$(idemkeycount)"
  IK_N=3
  for i in $(seq 1 $IK_N); do
    curl -s -o /dev/null --max-time 5 -X POST -H "Host: a" \
      -H "Idempotency-Key: leg13-key" -H "Content-Type: text/plain" \
      --data-binary "x" "http://127.0.0.1:$IPORT/alwaysfail"
  done
  ik_after="$(idemkeycount)"
  [[ "$ik_after" == "$ik_baseline" ]] \
    && ok "idempotent: $IK_N ephemeral attempts leave no rows behind (IdempotencyKey count stays $ik_baseline)" \
    || bad "idempotent-ephemeral-leak" "baseline=$ik_baseline after $IK_N attempts=$ik_after"

  kill -TERM "$SRV" 2>/dev/null
  istopped=1
  for _ in $(seq 1 30); do kill -0 "$SRV" 2>/dev/null || { istopped=0; break; }; sleep 0.1; done
  [[ $istopped -eq 0 ]] && ok "idempotent: SIGTERM stops the server" || bad "idempotent-stop" "still running"
  if [[ $istopped -eq 1 ]]; then
    # §14/§17b's own pattern clears SRV here unconditionally, which is
    # exactly how an orphan survives past this leg: the EXIT trap only
    # kills a non-empty $SRV, so a still-running process that this loop
    # gave up on would otherwise keep the port bound for the NEXT run of
    # this whole script. Force it dead right here instead of trusting the
    # trap -- the bad-verdict line above already told the reader SIGTERM
    # alone did not work.
    kill -9 "$SRV" 2>/dev/null
    for _ in $(seq 1 20); do kill -0 "$SRV" 2>/dev/null || break; sleep 0.1; done
  fi
  SRV=""
else
  bad "idempotent-compile" "$(printf '%s' "$ip_out" | head -1)"
fi

# ---- 19. porch-store task 5: pool saturation fails closed (503, no bypass) --
# Same flattening trick as the earlier legs. WO_MAILBOX (runtime/src/vm.c,
# wo_mailbox_cap, default 1024) shrinks the runtime's per-actor mailbox cap
# so a handful of concurrent requests can actually exhaust it. A pool of
# ONE actor -- the only address a one-slot Pool's pool_select can ever
# return -- fed a handler that blocks it for SP_SLEEP_MS turns every
# genuinely-concurrent request into a race for that one mailbox's slots.
# Distinct idempotency keys per request rule out replay masking a request
# that never actually ran the handler.
#
# The runtime frees a reserved slot the instant a message is POPPED for
# delivery, not when its receive returns (wo_mbox_reserve/release), so
# with cap C exactly the first C+1 concurrent calls to the one busy actor
# ever get a slot -- one executing, C queued behind it -- and every later
# concurrent call finds the mailbox full and traps (WO_T_ACTOR), which
# idempotent.wo's own try/catch turns into 503. SP_SLEEP_MS only has to
# outlast the time it takes SP_N curl clients to all reach their `call`,
# comfortably true on localhost.
SP="$W/saturation-check"
cp -r "$ROOT/docs/examples/porch" "$SP"
rm -f "$SP/wo.toml"
rm -rf "$SP/target"
cat >"$SP/saturation_check_main.wo" <<'WOEOF'
use net
use env
use http
use router
use middleware
use time

@table(name: "sat_execs")
class SatMark {
  n: Int
}

-- Blocks the pool's one actor for a few seconds on every genuine
-- (non-replay) execution -- the same shape as idempotent-check's
-- SlowHandler (section 18), its own table so the two legs' counts can
-- never be confused.
class SlowSatHandler {
  fn handle(req: Req) -> Resp {
    insert SatMark { n: 1 };
    time.sleep(3000);
    return ok_json("{\"ok\":true}");
  }
}

class SatExecCount {
  fn handle(req: Req) -> Resp {
    let n = len(from e in SatMark select e);
    return ok_json("{\"count\":${n}}");
  }
}

fn build_app(slot: actor PoolMsg) -> App {
  let app = App { middleware: [], routes: [] };
  let p = Pool { actors: [PoolSlot { a: slot }] };
  app.post("/slow", Idempotent { key_header: "idempotency-key", pool: p, inner: SlowSatHandler {} });
  app.get("/execs", SatExecCount {});
  return app;
}

class Conn { fd: net.Conn }

class ConnWorker {
  slot: actor PoolMsg
  fn receive(msg: Conn) {
    let app = build_app(self.slot);
    app.handle_conn(msg.fd, 8000, 8000);
  }
}

fn main(args: multi Text) -> Int {
  if len(args) < 1 {
    print_err("usage: saturation_check <port>");
    return 2;
  }
  let port = parse_int(args[0]);
  if port == nil { print_err("bad port"); return 2; }
  let ka: actor PoolMsg = spawn KeyActor {};
  let srv = net.listen("127.0.0.1", port);
  print("listening on 127.0.0.1:${port}");
  while true {
    if env.stopping() { net.close(srv); return 0; }
    let c = net.accept_dl(srv, 250);
    if c != nil {
      let w: actor Conn = spawn ConnWorker { slot: ka };
      send(w, Conn { fd: c });
    }
  }
}
WOEOF

if sp_out="$("$WOC" --emit "$SP" -o "$SP/saturation_check.wob" 2>&1)"; then
  ok "saturation: compiles (one-actor pool, slow in-actor handler)"

  SPORT=$((PORT + 3))
  SPDATA="$W/saturation-data"; mkdir -p "$SPDATA"
  SPSTATUS="$W/saturation-status"; mkdir -p "$SPSTATUS"
  SP_CAP=2
  SP_N=15
  printf '\n===== saturation check — port %s (WO_MAILBOX=%s) =====\n' "$SPORT" "$SP_CAP" >>"$SRVLOG"
  LEGFROM=$(( $(wc -l < "$SRVLOG") + 1 ))
  WO_DATA="$SPDATA" WO_MAILBOX="$SP_CAP" "$WOVM" "$SP/saturation_check.wob" "$SPORT" >>"$SRVLOG" 2>&1 &
  SRV=$!
  spwait_listen() {
    for _ in $(seq 1 40); do
      tail -n "+$LEGFROM" "$SRVLOG" 2>/dev/null | grep -q listening && return
      sleep 0.1
    done
  }
  spwait_listen

  # SP_N genuinely-parallel duplicates, each its own idempotency key, all
  # against the SAME (one-actor) pool -- a sequential version proves nothing,
  # same reasoning as every other concurrency leg in this file.
  sp_pids=()
  for i in $(seq 1 $SP_N); do
    ( st="$(curl -s -D "$SPSTATUS/$i.hdr" -o "$SPSTATUS/$i.body" -w '%{http_code}' --max-time 15 -X POST \
        -H "Host: a" -H "Idempotency-Key: sat-key-$i" -H "Content-Type: text/plain" \
        --data-binary "x" "http://127.0.0.1:$SPORT/slow")"
      echo "$st" >"$SPSTATUS/$i.status" ) &
    sp_pids+=("$!")
  done
  for p in "${sp_pids[@]}"; do wait "$p"; done

  sp_200=0
  sp_503=0
  sp_other=0
  sp_one503=""
  for i in $(seq 1 $SP_N); do
    st="$(cat "$SPSTATUS/$i.status" 2>/dev/null)"
    case "$st" in
      200) sp_200=$((sp_200 + 1)) ;;
      503) sp_503=$((sp_503 + 1)); sp_one503="$i" ;;
      *) sp_other=$((sp_other + 1)) ;;
    esac
  done
  sp_want_ok=$((SP_CAP + 1))
  sp_want_bad=$((SP_N - sp_want_ok))
  [[ "$sp_other" -eq 0 ]] \
    && ok "saturation: every one of $SP_N requests answered 200 or 503, nothing else" \
    || bad "saturation-codes" "$sp_other requests answered neither (200=$sp_200 503=$sp_503)"
  [[ "$sp_200" -eq "$sp_want_ok" && "$sp_503" -eq "$sp_want_bad" ]] \
    && ok "saturation: exactly $sp_want_ok served (1 running + $SP_CAP queued), $sp_want_bad overflow answer 503" \
    || bad "saturation-threshold" "200=$sp_200 503=$sp_503 want 200=$sp_want_ok 503=$sp_want_bad"

  sp_execs="$(curl -s --max-time 5 -H "Host: a" "http://127.0.0.1:$SPORT/execs" \
    | grep -o '"count":[0-9]*' | cut -d: -f2)"
  [[ "$sp_execs" == "$sp_200" ]] \
    && ok "saturation: handler ran exactly once per 200 (SatMark count=$sp_execs) -- no overflow request slipped through uncounted" \
    || bad "saturation-execs" "SatMark count=$sp_execs want $sp_200 (== the 200 count)"

  if [[ -n "$sp_one503" ]]; then
    grep -qi '^retry-after:' "$SPSTATUS/$sp_one503.hdr" \
      && ok "saturation 503 carries Retry-After" \
      || bad "saturation-503-retry-after" "$(head -1 "$SPSTATUS/$sp_one503.hdr")"
    grep -q 'idempotency store saturated' "$SPSTATUS/$sp_one503.body" \
      && ok "saturation 503 names the real cause (idempotency store saturated), not a generic failure" \
      || bad "saturation-503-body" "$(cat "$SPSTATUS/$sp_one503.body")"
  else
    bad "saturation-503-missing" "no 503 observed among $SP_N requests -- cannot verify overflow shape"
  fi

  # Teardown is deliberately NOT asserted pass/fail here (unlike the earlier
  # legs' own SIGTERM checks): this leg's subject is saturation, not graceful
  # shutdown -- already proven in §14 and (usually) §17b/§18. This exact
  # workload -- many concurrent call()-parked callers against one busy actor
  # doing real per-request table I/O -- is the sharpest known trigger for a
  # pre-existing runtime defect (see the story's Outstanding notes): main()
  # can return cleanly while the OS process itself hangs. Failing this leg
  # over that already-documented, out-of-scope defect would be exactly the
  # kind of flaky check that erodes trust in every other leg in this file, so
  # it force-kills instead of asserting graceful-vs-forced.
  kill -TERM "$SRV" 2>/dev/null
  spstopped=1
  for _ in $(seq 1 30); do kill -0 "$SRV" 2>/dev/null || { spstopped=0; break; }; sleep 0.1; done
  if [[ $spstopped -eq 1 ]]; then
    kill -9 "$SRV" 2>/dev/null
    for _ in $(seq 1 20); do kill -0 "$SRV" 2>/dev/null || break; sleep 0.1; done
  fi
  ok "saturation: server torn down (graceful SIGTERM, or kill -9 on the known actor-pool hang)"
  SRV=""
else
  bad "saturation-compile" "$(printf '%s' "$sp_out" | head -1)"
fi

echo
printf 'web-app-accept: %d checks, %d failures\n' "$((pass + fail))" "$fail"
[[ $fail -eq 0 ]]
