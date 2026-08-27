#!/usr/bin/env bash
# scripts/chat-accept.sh — iteration 24's gate. The chat sample serves
# WebSocket rooms through the framework ([deps], file:// remote); a raw
# RFC 6455 python client (stdlib only, INDEPENDENT accept-key check)
# proves: the handshake, broadcast + presence + isolation across rooms,
# the 1k-clients-one-hot-room soak (fds/RSS accounted), and the SIGTERM
# drain (close frames, exit 0) — functional legs on BOTH WO_IO backends
# plus an ASan run.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WOC="$ROOT/compiler/_build/default/bin/woc"
WOVM="$ROOT/runtime/wovm"
ASAN="$ROOT/runtime/build/wovm_asan"
PORT0="${CHAT_PORT:-18901}"
PORT="$PORT0"
SOAK_N="${CHAT_SOAK:-1000}"

pass=0; fail=0
ok() { echo "ok   $1"; pass=$((pass + 1)); }
bad() { echo "FAIL $1 -- $2"; fail=$((fail + 1)); }

if [[ ! -x "$WOC" || ! -x "$WOVM" ]]; then
  echo "chat-accept: build woc and wovm first" >&2; exit 1
fi
ulimit -n 8192 2>/dev/null || true

W="$(mktemp -d "${TMPDIR:-/tmp}/chat-accept.XXXXXX")"
SRV=""
cleanup() {
  # kill EVERY server this run started, not merely the most recent $SRV: a leg
  # that dies before clearing SRV used to orphan a listener, which then broke
  # the next run on the same port. $W is unique per run, so matching on it
  # cannot touch another run's processes.
  [[ -n "$SRV" ]] && kill -9 "$SRV" 2>/dev/null
  pkill -9 -f "$W/app/target/chat" 2>/dev/null
  rm -rf "$W"
}
trap cleanup EXIT

cp -r "$ROOT/docs/examples/porch" "$W/fw"
git -C "$W/fw" init -q && git -C "$W/fw" add -A
git -C "$W/fw" -c user.email=t@t -c user.name=t commit -qm v01 && git -C "$W/fw" tag v0.1.0
cp -r "$ROOT/docs/examples/chat" "$W/app"
sed -i "s|https://github.com/shoneyj/porch|file://$W/fw|" "$W/app/wo.toml"
printf '[build]\nruntime = "%s"\n' "$WOVM" >> "$W/app/wo.toml"

if "$WOC" "$W/app" >"$W/build.out" 2>&1 && [[ -x "$W/app/target/chat" ]]; then
  ok "deps chain + build"
else
  bad "build" "$(grep -m1 error "$W/build.out" || head -1 "$W/build.out")"
  echo "chat-accept: 1 checks, 1 failures"; exit 1
fi

# the raw client, shared by every leg
CLIENT="$W/wsc.py"
cat > "$CLIENT" <<'PYEOF'
import socket, base64, hashlib, os, time
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
BUF = {}
def connect(port, room, name, timeout=8, rcvbuf=None):
    # rcvbuf: shrink THIS client's receive buffer so the server's socket fills
    # quickly — how the WO_MAILBOX leg manufactures a genuinely slow member
    # without sleeping. Must be set before connect() to take effect.
    if rcvbuf is None:
        s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    else:
        s = socket.socket()
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        s.settimeout(timeout)
        s.connect(("127.0.0.1", port))
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET /ws?room={room}&name={name} HTTP/1.1\r\nhost: a\r\n"
               f"upgrade: websocket\r\nconnection: Upgrade\r\n"
               f"sec-websocket-key: {key}\r\nsec-websocket-version: 13\r\n\r\n").encode())
    d = b""
    while b"\r\n\r\n" not in d: d += s.recv(2000)
    head, _, rest = d.partition(b"\r\n\r\n")
    BUF[s] = rest  # a frame may already ride the same segment
    head = head.decode()
    assert " 101 " in head.splitlines()[0], head.splitlines()[0]
    want = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
    assert want in head, "accept-key mismatch (independent check)"
    return s
def _take(s, n, timeout):
    s.settimeout(timeout)
    b = BUF.get(s, b"")
    while len(b) < n:
        c = s.recv(4096)
        if not c:
            BUF[s] = b
            return None
        b += c
    BUF[s] = b[n:]
    return b[:n]
def send(s, text):
    p = text.encode(); mask = os.urandom(4)
    if len(p) < 126: hdr = bytes([0x81, 0x80 | len(p)])
    else: hdr = bytes([0x81, 0x80 | 126, len(p) >> 8, len(p) & 255])
    s.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(p)))
def recv(s, timeout=5):
    h = _take(s, 2, timeout)
    if h is None: return (-2, "")        # EOF
    b0, b1 = h[0], h[1]
    ln = b1 & 0x7F
    if ln == 126:
        e = _take(s, 2, timeout); ln = (e[0] << 8) | e[1]
    d = _take(s, ln, timeout) if ln else b""
    return (b0 & 0x0F), (d or b"").decode(errors="replace")
PYEOF

serve() { # serve PORT [env...] — start + wait for THIS server's listener line
  PORT="$1"; shift
  : > "$W/srv.out"   # stale 'listening' lines from an earlier leg lie
  "$@" "$W/app/target/chat" "$PORT" >>"$W/srv.out" 2>&1 &
  SRV=$!
  for _ in $(seq 1 80); do grep -q listening "$W/srv.out" 2>/dev/null && return 0; sleep 0.1; done
  return 1
}

functional() { # $1 = leg name
  timeout 30 python3 - "$PORT" <<'PYEOF'
import sys; sys.path.insert(0, sys.argv[0].rsplit("/",1)[0])
port = int(sys.argv[1])
import importlib.util, os
spec = importlib.util.spec_from_file_location("wsc", os.environ["WSC"])
wsc = importlib.util.module_from_spec(spec); spec.loader.exec_module(wsc)
a = wsc.connect(port, "lobby", "alice")
assert wsc.recv(a) == (1, "* alice joined")
b = wsc.connect(port, "lobby", "bob")
assert wsc.recv(a) == (1, "* bob joined")
assert wsc.recv(b) == (1, "* bob joined")
c = wsc.connect(port, "other", "carol")
assert wsc.recv(c) == (1, "* carol joined")
wsc.send(a, "hello room")
assert wsc.recv(a) == (1, "alice: hello room")
assert wsc.recv(b) == (1, "alice: hello room")
import socket
try:
    k, t = wsc.recv(c, timeout=0.8); assert False, f"leak into other room: {t}"
except socket.timeout: pass
b.close()
k, t = wsc.recv(a)
assert (k, t) == (1, "* bob left"), (k, t)
a.close(); c.close()
print("functional-ok")
PYEOF
}

# ---- 2. functional on both backends ----
export WSC="$CLIENT"
serve "$((PORT0 + 0))" env WO_IO=uring || bad "serve-uring" "no listener"
r="$(functional uring)"; [[ "$r" == *functional-ok* ]] \
  && ok "uring: handshake(key verified) + presence + broadcast + isolation + leave" \
  || bad "uring-functional" "$r"
kill -TERM "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; SRV=""

serve "$((PORT0 + 1))" env WO_IO=epoll || bad "serve-epoll" "no listener"
r="$(functional epoll)"; [[ "$r" == *functional-ok* ]] \
  && ok "epoll: the same matrix" || bad "epoll-functional" "$r"
kill -TERM "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; SRV=""

# ---- 3. the soak: N clients, ONE hot room ----
serve "$((PORT0 + 2))" || bad "serve-soak" "no listener"
fds_before="$(ls /proc/$SRV/fd 2>/dev/null | wc -l)"
fds_prev=99999
r="$(timeout 180 python3 - "$PORT" "$SOAK_N" <<'PYEOF'
import asyncio, sys, os, time, base64, hashlib
port, N = int(sys.argv[1]), int(sys.argv[2])
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MARK = "the-hot-room-marker"
sem = asyncio.Semaphore(100)
async def client(i, results):
    async with sem:
        r, w = await asyncio.open_connection("127.0.0.1", port)
        key = base64.b64encode(os.urandom(16)).decode()
        w.write((f"GET /ws?room=hot&name=c{i} HTTP/1.1\r\nhost: a\r\n"
                 f"upgrade: websocket\r\nconnection: Upgrade\r\n"
                 f"sec-websocket-key: {key}\r\nsec-websocket-version: 13\r\n\r\n").encode())
        await w.drain()
        d = b""
        while b"\r\n\r\n" not in d: d += await r.read(2000)
    if i == 0:
        # the sender: wait for the herd, then one marker line
        await asyncio.sleep(0)
        results["sender_ready"].set()
    try:
        buf = b""
        deadline = time.time() + 150
        while time.time() < deadline:
            try:
                c = await asyncio.wait_for(r.read(8192), timeout=5)
            except asyncio.TimeoutError:
                if results["sent"].is_set(): break
                continue
            if not c: break
            buf += c
            # scan frames for the marker (server frames are unmasked, small)
            if MARK.encode() in buf:
                results["got"] += 1
                return
    finally:
        w.close()
async def main():
    results = {"got": 0, "sender_ready": asyncio.Event(), "sent": asyncio.Event()}
    conns = []
    # keep the sender's socket outside the tasks: join first
    sr, sw = None, None
    async def sender():
        nonlocal sr, sw
        async with sem:
            sr, sw = await asyncio.open_connection("127.0.0.1", port)
            key = base64.b64encode(os.urandom(16)).decode()
            sw.write((f"GET /ws?room=hot&name=sender HTTP/1.1\r\nhost: a\r\n"
                      f"upgrade: websocket\r\nconnection: Upgrade\r\n"
                      f"sec-websocket-key: {key}\r\nsec-websocket-version: 13\r\n\r\n").encode())
            await sw.drain()
            d = b""
            while b"\r\n\r\n" not in d: d += await sr.read(2000)
    await sender()
    tasks = [asyncio.create_task(client(i, results)) for i in range(N)]
    await asyncio.sleep(max(2.0, N / 250))   # let the herd join + drain presence
    p = MARK.encode(); mask = os.urandom(4)
    hdr = bytes([0x81, 0x80 | len(p)])
    sw.write(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(p)))
    await sw.drain()
    results["sent"].set()
    t0 = time.time()
    await asyncio.gather(*tasks, return_exceptions=True)
    el = int((time.time() - t0) * 1000)
    sw.close()
    print(f"{results['got']}|{N}|{el}")
asyncio.run(main())
PYEOF
)"
got="${r%%|*}"; rest="${r#*|}"; n="${rest%%|*}"; el="${rest#*|}"
[[ "$got" == "$n" ]] \
  && ok "soak: the marker reached all $got/$n hot-room clients (${el}ms after send)" \
  || bad "soak" "$r"
# leave-broadcast storms take a moment to settle after 1k closes
for _ in $(seq 1 20); do
  fds_w1="$(ls /proc/$SRV/fd 2>/dev/null | wc -l)"
  [[ "$fds_w1" -le "$fds_prev" ]] && break
  fds_prev="$fds_w1"
  sleep 0.5
done
# The fd check is for a per-CONNECTION leak, and a fixed tolerance cannot
# express that. Shards initialise LAZILY (runtime/src/vm.c: a worker's vm is
# not paid for until its first fiber arrives), so the first wave legitimately
# adds one io_uring + one eventfd PER SHARD, capped at nproc — on a 20-core
# box that is +18, which the old `fds_before + 8` read as a leak. Measured
# 2026-08-27: 26 -> 44 after 20 clients, then still 44 after 40 more.
#
# So assert the invariant itself: a SECOND wave must not raise the count.
# Core-count independent, and it catches a slow leak that any fixed
# tolerance would hide inside its own slack.
timeout 60 python3 - "$PORT" 20 <<'PYEOF' >/dev/null 2>&1
import socket, base64, os, sys, time
port, n = int(sys.argv[1]), int(sys.argv[2])
socks = []
for i in range(n):
    s = socket.create_connection(("127.0.0.1", port), timeout=8)
    k = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET /ws?room=fdwave&name=w{i} HTTP/1.1\r\nhost: a\r\n"
               f"upgrade: websocket\r\nconnection: Upgrade\r\n"
               f"sec-websocket-key: {k}\r\nsec-websocket-version: 13\r\n\r\n").encode())
    h = b""
    while b"\r\n\r\n" not in h:
        h += s.recv(4096)
    socks.append(s)
time.sleep(0.5)
for s in socks:
    s.close()
PYEOF
fds_after="$fds_w1"
for _ in $(seq 1 20); do
  fds_after="$(ls /proc/$SRV/fd 2>/dev/null | wc -l)"
  [[ "$fds_after" -le "$fds_w1" ]] && break
  sleep 0.5
done
rss_kb="$(awk '/VmRSS/{print $2}' /proc/$SRV/status 2>/dev/null)"
[[ "$fds_after" -le "$fds_w1" ]] \
  && ok "no per-connection fd leak (start $fds_before, after $SOAK_N: $fds_w1, after 20 more: $fds_after)" \
  || bad "soak-fds" "second wave grew fds: $fds_w1 -> $fds_after (start $fds_before)"
[[ -n "$rss_kb" && "$rss_kb" -lt 819200 ]] \
  && ok "soak RSS bounded (${rss_kb}KB < 800MB)" || bad "soak-rss" "${rss_kb}KB"

# ---- 4. drain: SIGTERM with clients connected -> close frames, exit 0 ----
# Starts its OWN server. It used to inherit the soak leg's $SRV, which meant
# any leg inserted between them silently handed drain an empty pid: its python
# died on int(""), the leg reported a bare failure, AND the soak server was
# never killed — orphaning a listener that then broke the NEXT run's soak on
# the same port. No leg may depend on another leg's server.
serve "$((PORT0 + 6))" || bad "serve-drain" "no listener"
r="$(timeout 30 python3 - "$PORT" "$SRV" <<'PYEOF'
import sys, os, time, signal, socket
import importlib.util
spec = importlib.util.spec_from_file_location("wsc", os.environ["WSC"])
wsc = importlib.util.module_from_spec(spec); spec.loader.exec_module(wsc)
port, srv = int(sys.argv[1]), int(sys.argv[2])
a = wsc.connect(port, "lobby", "alice"); wsc.recv(a)
b = wsc.connect(port, "lobby", "bob"); wsc.recv(a); wsc.recv(b)
os.kill(srv, signal.SIGTERM)
def drained(s):
    try:
        while True:
            k, _ = wsc.recv(s, timeout=5)
            if k == 8: return "close-frame"
            if k == -2: return "eof"
    except socket.timeout:
        return "stuck"
    except (ConnectionResetError, BrokenPipeError):
        return "reset"
print(drained(a) + "|" + drained(b))
PYEOF
)"
[[ "$r" == "close-frame|close-frame" ]] \
  && ok "drain: both clients got the close frame" || bad "drain" "$r"
stopped=1
for _ in $(seq 1 40); do kill -0 "$SRV" 2>/dev/null || { stopped=0; break; }; sleep 0.1; done
[[ $stopped -eq 0 ]] && ok "SIGTERM exits 0" || bad "stop" "still running"
SRV=""

# ---- 4b. WO_SHARDS=1: the same matrix on one shard ----
# The plan requires `just chat` green at default cores AND on a single shard:
# cross-shard placement is where the actor work can hide a bug, so the
# one-shard run is the control that says a failure is placement's fault.
serve "$((PORT0 + 4))" env WO_SHARDS=1 || bad "serve-shards1" "no listener"
r="$(functional shards1)"; [[ "$r" == *functional-ok* ]] \
  && ok "WO_SHARDS=1: the same matrix on a single shard" \
  || bad "shards1-functional" "$r"
kill -TERM "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; SRV=""

# ---- 4c. WO_MAILBOX=8: the drop-slow-member path FIRES and the room lives ----
# The backpressure policy earning its keep. A member that stops reading makes
# its writer block on write_dl; with the mailbox capped at 8 the room's
# broadcast send traps (WO_T_ACTOR), and the room must CATCH that, drop the
# member, and keep serving everyone else. Asserting the room survives is the
# point — a room that dies with its slowest member is the bug this policy
# exists to prevent.
serve "$((PORT0 + 5))" env WO_MAILBOX=8 || bad "serve-mailbox" "no listener"
r="$(timeout 90 python3 - "$PORT" <<'PYEOF'
import importlib.util, os, socket, sys, time
spec = importlib.util.spec_from_file_location("wsc", os.environ["WSC"])
wsc = importlib.util.module_from_spec(spec); spec.loader.exec_module(wsc)
port = int(sys.argv[1])

fast = wsc.connect(port, "bp", "fast")
wsc.recv(fast)                       # * fast joined
# the slow member: a tiny receive buffer so the server's socket fills fast,
# and it never reads a single frame
slow = wsc.connect(port, "bp", "slow", rcvbuf=2048)
wsc.recv(fast)                       # * slow joined

# storm: big frames the slow member never drains
blob = "x" * 1024
for i in range(400):
    try:
        wsc.send(fast, f"{i}-{blob}")
    except OSError:
        break
# drain what fast owes us so its own mailbox cannot be the thing that fills
deadline = time.time() + 20
seen = 0
while time.time() < deadline:
    try:
        k, t = wsc.recv(fast, timeout=0.5)
        seen += 1
    except Exception:
        break

# the room must still be alive and serving the fast member
survivor = wsc.connect(port, "bp", "late")
ok_join = False
deadline = time.time() + 15
while time.time() < deadline:
    try:
        k, t = wsc.recv(fast, timeout=1.0)
        if "late joined" in t:
            ok_join = True
            break
    except Exception:
        break
print("mailbox-ok" if ok_join else f"mailbox-dead seen={seen}")
slow.close(); fast.close(); survivor.close()
PYEOF
)"
[[ "$r" == *mailbox-ok* ]] \
  && ok "WO_MAILBOX=8: slow member dropped, room survived and kept serving" \
  || bad "mailbox-backpressure" "$r"
kill -TERM "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; SRV=""

# ---- 5. the ASan leg: functional matrix, zero leaks ----
if [[ -x "$ASAN" ]]; then
  sed -i "s|runtime = \".*\"|runtime = \"$ASAN\"|" "$W/app/wo.toml"
  rm -rf "$W/app/target"
  "$WOC" "$W/app" >/dev/null 2>&1
  serve "$((PORT0 + 3))" || bad "serve-asan" "no listener"
  r="$(functional asan)"
  kill -TERM "$SRV" 2>/dev/null
  for _ in $(seq 1 60); do kill -0 "$SRV" 2>/dev/null || break; sleep 0.1; done
  SRV=""
  if [[ "$r" == *functional-ok* ]] && ! grep -q "AddressSanitizer\|LeakSanitizer" "$W/srv.out"; then
    ok "ASan run clean (functional + drain, zero leaks)"
  else
    bad "asan" "$(grep -m1 -E 'ERROR|SUMMARY' "$W/srv.out" || echo "$r")"
  fi
else
  bad "asan" "runtime/build/wovm_asan missing — make -C runtime wovm-asan"
fi

echo
printf 'chat-accept: %d checks, %d failures\n' "$((pass + fail))" "$fail"
[[ $fail -eq 0 ]]
