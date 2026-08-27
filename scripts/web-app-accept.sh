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

echo
printf 'web-app-accept: %d checks, %d failures\n' "$((pass + fail))" "$fail"
[[ $fail -eq 0 ]]
