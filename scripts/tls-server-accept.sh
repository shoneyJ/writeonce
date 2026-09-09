#!/usr/bin/env bash
# scripts/tls-server-accept.sh — runtime-v2 9 phase G's gate: inbound TLS 1.3.
# The runtime suite proves the server FSM offline (loopback against the client
# driver); this proves interop with a real client — `openssl s_client`
# validating our hand-rolled server handshake and exchanging application data,
# for both an ECDSA-P256 and an RSA server certificate. Log: /tmp/tls-server.log.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WOC="$ROOT/compiler/_build/default/bin/woc"
WOVM="$ROOT/runtime/wovm"
APP="$ROOT/docs/examples/tls-server"
PORT="${TLS_SERVER_PORT:-18553}"
LOG=/tmp/tls-server.log

pass=0; fail=0
ok()  { echo "ok   $1"; pass=$((pass + 1)); }
bad() { echo "FAIL $1"; fail=$((fail + 1)); }

WORK="$(mktemp -d)"
SRV_PID=""
cleanup() { [[ -n "$SRV_PID" ]] && kill -KILL "$SRV_PID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT
mkdir -p "$WORK/data"

[[ -x "$WOVM" ]] || { echo "tls-server: wovm not built — run: make -C runtime wovm" >&2; exit 1; }
[[ -x "$WOC" ]]  || { echo "tls-server: woc not built — run: just woc-build" >&2; exit 1; }
command -v openssl >/dev/null || { echo "tls-server: needs openssl" >&2; exit 1; }
python3 -c 'import cryptography' 2>/dev/null || { echo "tls-server: needs python3 cryptography" >&2; exit 1; }

# ---- 1. a test CA + an EC leaf and an RSA leaf (SAN localhost) ------------
cat > "$WORK/mk.py" <<'PY'
import datetime, sys
from cryptography import x509
from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID
from cryptography.hazmat.primitives import hashes, serialization as ser
from cryptography.hazmat.primitives.asymmetric import ec, rsa
d = sys.argv[1]
NB = datetime.datetime(2020,1,1); NA = datetime.datetime(2035,1,1)
def nm(cn): return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)])
cak = ec.generate_private_key(ec.SECP256R1())
ca = (x509.CertificateBuilder().subject_name(nm("wo-test-ca")).issuer_name(nm("wo-test-ca"))
      .public_key(cak.public_key()).serial_number(x509.random_serial_number())
      .not_valid_before(NB).not_valid_after(NA)
      .add_extension(x509.BasicConstraints(ca=True, path_length=None), True).sign(cak, hashes.SHA256()))
open(d+"/ca.pem","wb").write(ca.public_bytes(ser.Encoding.PEM))
def leaf(key, tag):
    c = (x509.CertificateBuilder().subject_name(nm("localhost")).issuer_name(ca.subject)
         .public_key(key.public_key()).serial_number(x509.random_serial_number())
         .not_valid_before(NB).not_valid_after(NA)
         .add_extension(x509.SubjectAlternativeName([x509.DNSName("localhost")]), False)
         .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), False)
         .sign(cak, hashes.SHA256()))
    open(d+"/"+tag+".pem","wb").write(c.public_bytes(ser.Encoding.PEM))
    open(d+"/"+tag+".key","wb").write(key.private_bytes(ser.Encoding.PEM,
        ser.PrivateFormat.PKCS8, ser.NoEncryption()))
leaf(ec.generate_private_key(ec.SECP256R1()), "ec")
leaf(rsa.generate_private_key(public_exponent=65537, key_size=2048), "rsa")
PY
python3 "$WORK/mk.py" "$WORK" || { bad "cert generation"; echo "tls-server: $fail failures"; exit 1; }
ok "test CA + EC leaf + RSA leaf (SAN localhost, EKU serverAuth) generated"

# ---- 2. build the server -------------------------------------------------
{ echo; echo "== tls-server-accept $(date -Is) port $PORT =="; } >>"$LOG"
if "$WOC" --emit "$APP" -o "$WORK/srv.wob" 2>"$WORK/cerr"; then
  ok "build: tls-server compiles"
else
  bad "build: $(head -3 "$WORK/cerr")"; echo "tls-server: $fail failures"; exit 1
fi

# ---- 3. openssl s_client interop, per key type ---------------------------
probe() {  # $1 = tag (ec|rsa) ; $2 = port (distinct per probe — avoids a bind race)
  local p="$2"
  kill -KILL "$SRV_PID" 2>/dev/null
  WO_DATA="$WORK/data" "$WOVM" "$WORK/srv.wob" "$p" "$WORK/$1.pem" "$WORK/$1.key" >>"$LOG" 2>&1 &
  SRV_PID=$!; disown "$SRV_PID" 2>/dev/null || true
  for _ in $(seq 1 100); do
    if ( exec 3<>"/dev/tcp/127.0.0.1/$p" ) 2>/dev/null; then break; fi
    sleep 0.05
  done
  local out
  out="$({ printf 'GET / HTTP/1.0\r\n\r\n'; sleep 1; } | \
    timeout 12 openssl s_client -connect "127.0.0.1:$p" -CAfile "$WORK/ca.pem" \
      -servername localhost -tls1_3 -verify_return_error -quiet 2>/dev/null)"
  if [[ "$out" == *"hello-wo-tls"* ]]; then
    ok "$1: openssl s_client validated the cert + got the reply"
  else
    bad "$1: no reply (out: ${out:0:80})"
  fi
}
probe ec "$PORT"
probe rsa "$((PORT + 1))"

echo "tls-server: $((pass + fail)) checks, $fail failures"
[[ $fail -eq 0 ]]
