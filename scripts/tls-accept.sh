#!/usr/bin/env bash
# scripts/tls-accept.sh — runtime-v2 9 F3c-net's gate: outbound TLS 1.3.
# The runtime suite (test_tls/test_crypto) proves the crypto + handshake +
# chain validation offline against RFC 8448 / real certs; this proves the half
# only a real program shows: net.connect_tls called FROM .wo through the
# compiler, dialing a live TLS 1.3 server, validating the chain to a trust
# anchor and the hostname, exchanging application bytes — and refusing loudly
# when the chain is untrusted or the hostname does not match. No live network:
# the server is a local stub with a test CA. Log: /tmp/tls.log (tail -F it).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WOC="$ROOT/compiler/_build/default/bin/woc"
WOVM="$ROOT/runtime/wovm"
APP="$ROOT/docs/examples/tls-client"
PORT="${TLS_PORT:-18443}"
LOG=/tmp/tls.log

pass=0; fail=0
ok()  { echo "ok   $1"; pass=$((pass + 1)); }
bad() { echo "FAIL $1"; fail=$((fail + 1)); }

WORK="$(mktemp -d)"
SRV_PID=""
cleanup() {
  [[ -n "$SRV_PID" ]] && kill -KILL "$SRV_PID" 2>/dev/null
  rm -rf "$WORK"
}
trap cleanup EXIT
mkdir -p "$WORK/data"

# ---- 0. prerequisites ----------------------------------------------------
[[ -x "$WOVM" ]] || { echo "tls-accept: wovm not built — run: make -C runtime wovm" >&2; exit 1; }
[[ -x "$WOC" ]]  || { echo "tls-accept: woc not built — run: just woc-build" >&2; exit 1; }
python3 -c 'import cryptography, ssl' 2>/dev/null || { echo "tls-accept: needs python3 cryptography + ssl" >&2; exit 1; }

# ---- 1. test CA + certs --------------------------------------------------
cat > "$WORK/mkcerts.py" <<'PY'
import datetime, sys
from cryptography import x509
from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
d = sys.argv[1]
NB = datetime.datetime(2020, 1, 1); NA = datetime.datetime(2035, 1, 1)
def nm(cn): return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)])
def leaf(cak, ca, cn, san):
    k = ec.generate_private_key(ec.SECP256R1())
    c = (x509.CertificateBuilder().subject_name(nm(cn)).issuer_name(ca.subject)
         .public_key(k.public_key()).serial_number(x509.random_serial_number())
         .not_valid_before(NB).not_valid_after(NA)
         .add_extension(x509.SubjectAlternativeName([x509.DNSName(san)]), False)
         .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), False)
         .sign(cak, hashes.SHA256()))
    return k, c
def ca(cn):
    k = ec.generate_private_key(ec.SECP256R1())
    c = (x509.CertificateBuilder().subject_name(nm(cn)).issuer_name(nm(cn))
         .public_key(k.public_key()).serial_number(x509.random_serial_number())
         .not_valid_before(NB).not_valid_after(NA)
         .add_extension(x509.BasicConstraints(ca=True, path_length=None), True)
         .sign(k, hashes.SHA256()))
    return k, c
def wpem(p, c): open(p, "wb").write(c.public_bytes(serialization.Encoding.PEM))
def wkey(p, k): open(p, "wb").write(k.private_bytes(serialization.Encoding.PEM,
    serialization.PrivateFormat.TraditionalOpenSSL, serialization.NoEncryption()))
cak, cac = ca("wo-test-ca")
lk, lc = leaf(cak, cac, "localhost", "localhost")
wk, wc = leaf(cak, cac, "wrong", "wrong.example")
_, oc = ca("wo-other-ca")
wpem(d + "/ca.pem", cac); wpem(d + "/leaf.pem", lc); wkey(d + "/leaf.key", lk)
wpem(d + "/wrong.pem", wc); wkey(d + "/wrong.key", wk); wpem(d + "/other.pem", oc)
PY
python3 "$WORK/mkcerts.py" "$WORK" || { bad "cert generation"; echo "tls-accept: $fail failures"; exit 1; }
ok "test CA + leaf (SAN localhost) + wrong-host + unrelated-CA generated"

# ---- 2. TLS 1.3 stub server ----------------------------------------------
cat > "$WORK/stub.py" <<'PY'
import socket, ssl, sys, threading, time
host, port, cert, key = "127.0.0.1", int(sys.argv[1]), sys.argv[2], sys.argv[3]
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.minimum_version = ssl.TLSVersion.TLSv1_3
ctx.maximum_version = ssl.TLSVersion.TLSv1_3
ctx.load_cert_chain(certfile=cert, keyfile=key)
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((host, port)); srv.listen(8)
print("READY", flush=True)
def serve():
    while True:
        try: c, _ = srv.accept()
        except OSError: return
        try:
            s = ctx.wrap_socket(c, server_side=True)
            print("SUITE", s.cipher()[0], flush=True)   # which AEAD ran over the wire
            s.recv(4096); s.sendall(b"wo-tls-ok\n"); s.close()
        except Exception as e:
            print("stub-err", e, flush=True)
threading.Thread(target=serve, daemon=True).start()
time.sleep(60)
PY

start_stub() {  # $1 = cert, $2 = key
  [[ -n "$SRV_PID" ]] && kill -KILL "$SRV_PID" 2>/dev/null
  : > "$WORK/stub.log"
  python3 "$WORK/stub.py" "$PORT" "$1" "$2" >"$WORK/stub.log" 2>&1 &
  SRV_PID=$!
  disown "$SRV_PID" 2>/dev/null || true   # keep job-control noise out of gate output
  for _ in $(seq 1 100); do grep -q READY "$WORK/stub.log" 2>/dev/null && return 0; sleep 0.1; done
  return 1
}

run_client() {  # $1 = CA bundle ; stdout: client output ; return: client exit
  WO_CA_BUNDLE="$1" WO_DATA="$WORK/data" timeout 20 "$WOVM" "$WORK/app.wob" localhost "$PORT" 2>>"$LOG"
}

# ---- 3. build the client -------------------------------------------------
{ echo; echo "== tls-accept $(date -Is) port $PORT =="; } >>"$LOG"
if "$WOC" --emit "$APP" -o "$WORK/app.wob" 2>"$WORK/cerr"; then
  ok "build: tls-client compiles"
else
  bad "build: $(head -3 "$WORK/cerr")"; echo "tls-accept: $fail failures"; exit 1
fi

# ---- 4. happy path: trusted chain + matching host ------------------------
start_stub "$WORK/leaf.pem" "$WORK/leaf.key" || { bad "stub server did not start"; echo "tls-accept: $fail failures"; exit 1; }
out="$(run_client "$WORK/ca.pem")"; rc=$?
if [[ $rc -eq 0 && "$out" == *"wo-tls-ok"* ]]; then
  ok "handshake + trusted chain + host match + app round-trip [$(grep -m1 '^SUITE' "$WORK/stub.log" | cut -d' ' -f2)]"
else
  bad "happy path (exit $rc): $out"
fi

# ---- 5. negative: untrusted chain (bundle = unrelated CA) -----------------
out="$(run_client "$WORK/other.pem")"; rc=$?
if [[ $rc -eq 1 && "$out" == *"refused"* ]]; then
  ok "untrusted chain refused"
else
  bad "untrusted chain not refused (exit $rc): $out"
fi

# ---- 6. negative: hostname mismatch (cert SAN=wrong.example) --------------
start_stub "$WORK/wrong.pem" "$WORK/wrong.key" || { bad "stub restart (wrong cert)"; }
out="$(run_client "$WORK/ca.pem")"; rc=$?
if [[ $rc -eq 1 && "$out" == *"refused"* ]]; then
  ok "hostname mismatch refused"
else
  bad "hostname mismatch not refused (exit $rc): $out"
fi

echo "tls-accept: $((pass + fail)) checks, $fail failures"
[[ $fail -eq 0 ]]
