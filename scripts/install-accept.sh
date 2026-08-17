#!/usr/bin/env bash
# scripts/install-accept.sh — proves the dist tarball installs and works the way
# Go's does: extract to a prefix, put its bin on PATH, `woc version`, then build
# AND run a throwaway project from an unrelated cwd (so only woc's self-location
# can find wovm), and confirm the wo-version constraint refuses a newer toolchain.
# Run `just dist` first to produce the tarball.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ver="$(cat "$ROOT/VERSION")"
tarball="$ROOT/dist/writeonce-${ver}-linux-amd64.tar.gz"

pass=0
fail=0
ok()  { echo "ok   $1"; pass=$((pass + 1)); }
bad() { echo "FAIL $1 -- $2"; fail=$((fail + 1)); }

if [[ ! -f "$tarball" ]]; then
  echo "install-accept: $tarball missing -- build it first: just dist" >&2
  exit 1
fi

W="$(mktemp -d "${TMPDIR:-/tmp}/wo-install.XXXXXX")"
trap 'rm -rf "$W"' EXIT

# 1. extract exactly like `tar -C /usr/local` — archive root is `writeonce/`
tar -C "$W" -xzf "$tarball"
prefix="$W/writeonce"
if [[ -x "$prefix/bin/woc" && -x "$prefix/bin/wovm" ]]; then
  ok "tarball extracts writeonce/bin/{woc,wovm}"
else
  bad "extract" "binaries missing under $prefix/bin"
  echo; printf 'install-accept: %d checks, %d failures\n' "$((pass + fail))" "$fail"; exit 1
fi

export PATH="$prefix/bin:$PATH"

# 2. version reporting (the `go version` equivalent)
got="$(woc version)"
[[ "$got" == "writeonce $ver linux/amd64" ]] && ok "woc version" || bad "woc version" "$got"
got="$(wovm --version)"
[[ "$got" == "wovm $ver" ]] && ok "wovm --version" || bad "wovm --version" "$got"

# 3. build + run a project from an UNRELATED cwd (only self-location finds wovm)
proj="$W/proj"
mkdir -p "$proj"
cat > "$proj/wo.toml" <<EOF
name = "greet"
version = "0.1.0"

[runtime]
wo = ">= 0.1"
EOF
cat > "$proj/main.wo" <<'EOF'
fn main(args: multi Text) -> Int { print("installed writeonce works"); return 0; }
EOF
if ( cd /tmp && woc "$proj" ) >/dev/null 2>&1 && [[ -x "$proj/target/greet" ]]; then
  ok "woc builds a project (self-located wovm from an unrelated cwd)"
else
  bad "build" "no target/greet produced"
fi
out="$("$proj/target/greet" 2>&1)"
[[ "$out" == "installed writeonce works" ]] && ok "standalone binary runs" || bad "run" "$out"

# 4. the wo constraint refuses a toolchain older than required
sed -i 's/>= 0.1/>= 99.0/' "$proj/wo.toml"
( cd /tmp && woc "$proj" ) >/dev/null 2>&1
[[ $? -eq 2 ]] && ok "wo constraint refuses a newer requirement" || bad "constraint" "did not refuse >= 99.0"

echo
printf 'install-accept: %d checks, %d failures\n' "$((pass + fail))" "$fail"
[[ $fail -eq 0 ]]
