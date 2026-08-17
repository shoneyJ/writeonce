#!/usr/bin/env bash
# scripts/mkdist.sh — package the writeonce toolchain as an installable tarball,
# Go-style. Archive root is `writeonce/` (version-less, like Go's `go/`), so
# `tar -C /usr/local -xzf ...` drops it at /usr/local/writeonce. Builds both
# release binaries, asserts VERSION == `woc version` == `wovm --version` (drift
# guard), stages an install README, and emits dist/<name>.tar.gz + a sha256.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

ver="$(cat VERSION)"
triple="linux-amd64"          # the only target today; a cross matrix is future work
name="writeonce-${ver}-${triple}"

# release binaries
dune build --root compiler
make -C runtime wovm >/dev/null

woc="compiler/_build/default/bin/woc"
wovm="runtime/wovm"

# drift guard: the three version sources must agree, else the tarball would ship
# a `wo >= X` constraint nobody can honor
got_woc="$("$woc" version | awk '{print $2}')"
got_vm="$("$wovm" --version | awk '{print $2}')"
[[ "$got_woc" == "$ver" ]] || { echo "mkdist: woc reports $got_woc but VERSION says $ver" >&2; exit 1; }
[[ "$got_vm"  == "$ver" ]] || { echo "mkdist: wovm reports $got_vm but VERSION says $ver" >&2; exit 1; }

stage="dist/writeonce"
rm -rf "$stage"
mkdir -p "$stage/bin"
install -m 0755 "$woc"  "$stage/bin/woc"
install -m 0755 "$wovm" "$stage/bin/wovm"
cp VERSION "$stage/VERSION"
sed "s/@VER@/$ver/g; s/@TRIPLE@/$triple/g" scripts/install-readme.tmpl.md > "$stage/README.md"

tar -C dist -czf "dist/${name}.tar.gz" writeonce
rm -rf "$stage"
( cd dist && sha256sum "${name}.tar.gz" > "${name}.tar.gz.sha256" )

echo "built dist/${name}.tar.gz"
cat "dist/${name}.tar.gz.sha256"
