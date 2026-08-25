#!/usr/bin/env bash
# scripts/site-accept.sh — the writeonce.de tutorial site's gate: TWO deps
# (serve + view) resolved from run-time file:// remotes, build,
# serve, the page matrix (render/escape/404/401/authed edit), SIGTERM,
# and WAL restart persistence of an admin edit.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WOC="$ROOT/compiler/_build/default/bin/woc"
WOVM="$ROOT/runtime/wovm"

pass=0; fail=0
ok() { echo "ok   $1"; pass=$((pass + 1)); }
bad() { echo "FAIL $1 -- $2"; fail=$((fail + 1)); }

if [[ ! -x "$WOC" || ! -x "$WOVM" ]]; then
  echo "site-accept: build woc and wovm first (just woc-build; just wovm-build)" >&2
  exit 1
fi

W="$(mktemp -d "${TMPDIR:-/tmp}/site-accept.XXXXXX")"
SRV=""
cleanup() {
  [[ -n "$SRV" ]] && kill -9 "$SRV" 2>/dev/null
  rm -rf "$W"
}
trap cleanup EXIT

# ---- both deps as git remotes; the app pointed at them ----
cp -r "$ROOT/docs/examples/writeonce-serve" "$W/fw"
cp -r "$ROOT/docs/examples/writeonce-view" "$W/lib"
for d in "$W/fw" "$W/lib"; do
  git -C "$d" init -q
  git -C "$d" add -A
  git -C "$d" -c user.email=t@t -c user.name=t commit -qm v01
  git -C "$d" tag v0.1.0
done
cp -r "$ROOT/docs/examples/site" "$W/app"
sed -i "s|https://github.com/shoneyj/writeonce-serve|file://$W/fw|; s|https://github.com/shoneyj/writeonce-view|file://$W/lib|" "$W/app/wo.toml"
printf '[build]\nruntime = "%s"\n' "$WOVM" >> "$W/app/wo.toml"

# a stand-in release tarball so /dl can be exercised without running
# `just dist` first — the bytes do not matter, the serving path does
mkdir -p "$W/app/dist"
printf 'not-a-real-tarball' | gzip > "$W/app/dist/writeonce-0.1.0-linux-amd64.tar.gz"
( cd "$W/app/dist" && sha256sum writeonce-0.1.0-linux-amd64.tar.gz > writeonce-0.1.0-linux-amd64.tar.gz.sha256 )

# ---- 1. two-dep fetch + lock + build ----
if "$WOC" "$W/app" >"$W/build.out" 2>&1 && [[ -x "$W/app/target/site" && -f "$W/app/wo.lock" ]]; then
  ok "deps chain: two remotes fetched + wo.lock + build"
else
  bad "build" "$(grep -m1 "error" "$W/build.out" || head -1 "$W/build.out")"
  printf 'site-accept: %d checks, %d failures\n' "$((pass + fail))" "$fail"
  exit 1
fi

PORT=$((8500 + RANDOM % 400))
DATA="$W/data"; mkdir -p "$DATA"

hit() { # path [method] [data] [token] -> "STATUS|BODY" (redirects not followed)
  python3 - "$PORT" "$1" "${2:-GET}" "${3:-}" "${4:-}" <<'PYEOF'
import sys, urllib.request, urllib.error
port, path, method, data, token = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a, **k): return None
req = urllib.request.Request(f"http://127.0.0.1:{port}{path}",
                             data=data.encode() if data else None, method=method)
if data: req.add_header("content-type", "application/x-www-form-urlencoded")
if token: req.add_header("authorization", "Bearer " + token)
try:
    r = urllib.request.build_opener(NoRedirect).open(req, timeout=5)
    print(f"{r.status}|{r.read().decode()}")
except urllib.error.HTTPError as e:
    print(f"{e.code}|{e.read().decode()}")
PYEOF
}

# binary-safe: status + content-type + byte count, no decoding. The
# release tarball is gzip, which `hit` cannot represent.
hit_bin() {
  python3 - "$PORT" "$1" <<'PYEOF'
import sys, urllib.request, urllib.error
port, path = sys.argv[1], sys.argv[2]
try:
    r = urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=5)
    print(f"{r.status}|{r.headers.get('content-type','')}|{len(r.read())}")
except urllib.error.HTTPError as e:
    print(f"{e.code}|{e.headers.get('content-type','')}|{len(e.read())}")
PYEOF
}

expect() { # name got want_status want_substr
  local name="$1" got="$2" want="$3" sub="$4"
  local st="${got%%|*}" body="${got#*|}"
  if [[ "$st" == "$want" && "$body" == *"$sub"* ]]; then ok "$name"
  else bad "$name" "status=$st body=$(printf '%.90s' "$body")"; fi
}

serve() {
  SITE_TOKEN=s3cr3t WO_DATA="$DATA" "$W/app/target/site" "$PORT" >>"$W/srv.out" 2>&1 &
  SRV=$!
  for _ in $(seq 1 40); do
    [[ "$(hit /health 2>/dev/null)" == 200* ]] && return 0
    sleep 0.25
  done
  return 1
}

# ---- 2..8 the page matrix ----
serve || bad "serve" "server never answered /health"
expect "home renders the tutorial"      "$(hit /)"           200 "Learn writeonce"
expect "tailwind sheet inlined"         "$(hit /)"           200 ".btn{"
expect "chapter renders a code sample"  "$(hit /ch/hello)"   200 "fn main"
expect "escaped interpolation visible"  "$(hit /ch/values)"  200 '${port}'
expect "unknown chapter is a 404 page"  "$(hit /ch/nope)"    404 "No such chapter"
expect "install guide renders"          "$(hit /install)"    200 "tar -C /usr/local"
expect "packages index lists both"      "$(hit /packages)"   200 "/packages/serve"
expect "package detail shows its dep"   "$(hit /packages/view)" 200 "writeonce-view"
expect "unknown package is a 404 page"  "$(hit /packages/nope)" 404 "No such package"
expect "favicon is served as svg"       "$(hit /favicon.svg)" 200 "<svg"
expect "install lists supported systems" "$(hit /install)"   200 "glibc 2.35 or newer"
got="$(hit_bin /dl/writeonce-0.1.0-linux-amd64.tar.gz)"
if [[ "$got" == 200\|application/gzip\|* && "${got##*|}" -gt 0 ]]; then
  ok "the release tarball downloads as gzip"
else bad "tarball download" "$got"; fi
expect "its checksum downloads"          "$(hit /dl/writeonce-0.1.0-linux-amd64.tar.gz.sha256)" 200 "writeonce-0.1.0-linux-amd64.tar.gz"
expect "traversal out of /dl is a 404"   "$(hit /dl/../wo.toml)" 404 ""
expect "the logo is inline in the nav"  "$(hit /)"           200 "aria-label=\"writeonce\""
expect "admin without token is 401"     "$(hit /admin/ch/hello POST "title=X")"  401 "unauthorized"
expect "admin edit answers a redirect"  "$(hit /admin/ch/hello POST "title=Hello v2" s3cr3t)" 302 ""
expect "the edit is live"               "$(hit /ch/hello)"   200 "Hello v2"

# ---- 9. SIGTERM stops it ----
kill -TERM "$SRV"
stopped=1
for _ in $(seq 1 30); do kill -0 "$SRV" 2>/dev/null || { stopped=0; break; }; sleep 0.1; done
[[ $stopped -eq 0 ]] && ok "SIGTERM stops the server" || bad "stop" "still running"
SRV=""

# ---- 10. restart persistence: the edit replayed from the WAL ----
serve || bad "re-serve" "server never answered /health after restart"
expect "edit survives a restart (WAL)"  "$(hit /ch/hello)"   200 "Hello v2"
kill -TERM "$SRV" 2>/dev/null; SRV=""

echo
printf 'site-accept: %d checks, %d failures\n' "$((pass + fail))" "$fail"
[[ $fail -eq 0 ]]
