#!/usr/bin/env bash
# scripts/web-app-accept.sh — iteration 16's acceptance gate. Proves the whole
# chain at run time, network-free: a temp git remote is built from
# docs/examples/writeonce-framework, its file:// URL is substituted into a
# temp copy of docs/examples/web-app, then: fetch -> lock -> build -> serve ->
# the storefront matrix -> SIGTERM -> restart persistence. The repo itself
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

W="$(mktemp -d "${TMPDIR:-/tmp}/web-app-accept.XXXXXX")"
SRV=""
cleanup() {
  [[ -n "$SRV" ]] && kill -9 "$SRV" 2>/dev/null
  [[ -n "${WA_ACCEPT_KEEP:-}" ]] && echo "kept $W" || rm -rf "$W"
}
trap cleanup EXIT

# ---- the framework as a git remote; the app pointed at it ----
cp -r "$ROOT/docs/examples/writeonce-framework" "$W/fw"
git -C "$W/fw" init -q
git -C "$W/fw" add -A
git -C "$W/fw" -c user.email=t@t -c user.name=t commit -qm v01
git -C "$W/fw" tag v0.1.0
cp -r "$ROOT/docs/examples/web-app" "$W/app"
sed -i "s|https://github.com/shoneyj/writeonce-framework|file://$W/fw|" "$W/app/wo.toml"
printf '[build]\nruntime = "%s"\n' "$WOVM" >> "$W/app/wo.toml"

# ---- 1. fetch + lock + build ----
if "$WOC" "$W/app" >"$W/build.out" 2>&1 && [[ -x "$W/app/target/web-app" && -f "$W/app/wo.lock" ]]; then
  ok "deps chain: fetch + wo.lock + build"
else
  bad "build" "$(head -1 "$W/build.out")"
  printf 'web-app-accept: %d checks, %d failures\n' "$((pass + fail))" "$fail"
  exit 1
fi

DATA="$W/data"; mkdir -p "$DATA"
WA_TOKEN=s3cr3t WO_DATA="$DATA" "$W/app/target/web-app" "$PORT" >"$W/srv.out" 2>&1 &
SRV=$!
for _ in $(seq 1 40); do grep -q listening "$W/srv.out" 2>/dev/null && break; sleep 0.1; done

# one tiny HTTP client; python is already a repo test dependency
hit() { # method path [body] [auth: yes|no] -> "STATUS|BODY"
  python3 - "$PORT" "$1" "$2" "${3:-}" "${4:-yes}" <<'PYEOF'
import socket, sys
port, method, path, body, auth = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
s = socket.create_connection(("127.0.0.1", port), timeout=5)
h = f"{method} {path} HTTP/1.1\r\nhost: a\r\n"
if auth == "yes": h += "authorization: Bearer s3cr3t\r\n"
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
expect "create product (201)"         "$(hit POST /products '{"name":"mug","price":900,"stock":5}')" 201 '"name":"mug"'
expect "duplicate name is 409 (@unique)" "$(hit POST /products '{"name":"mug","price":1,"stock":1}')" 409
expect "malformed json is 400"        "$(hit POST /products '{oops')" 400
expect "list shows the product"       "$(hit GET /products)" 200 '"price":900'
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
WA_TOKEN=s3cr3t WO_DATA="$DATA" "$W/app/target/web-app" "$PORT" >>"$W/srv.out" 2>&1 &
SRV=$!
sleep 0.5
expect "product survives a restart (WAL)" "$(hit GET /products)" 200 '"name":"mug"'
kill -TERM "$SRV" 2>/dev/null; SRV=""

echo
printf 'web-app-accept: %d checks, %d failures\n' "$((pass + fail))" "$fail"
[[ $fail -eq 0 ]]
