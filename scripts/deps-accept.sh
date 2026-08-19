#!/usr/bin/env bash
# scripts/deps-accept.sh — iteration 15's acceptance gate: wo.toml [deps] +
# git fetch + wo.lock, proven against local file:// remotes built at run time
# (network-free). One check per spec behavior, log-watcher-accept style.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WOC="$ROOT/compiler/_build/default/bin/woc"
WOVM="$ROOT/runtime/wovm"

pass=0
fail=0
ok() { echo "ok   $1"; pass=$((pass + 1)); }
bad() { echo "FAIL $1 -- $2"; fail=$((fail + 1)); }

if [[ ! -x "$WOC" || ! -x "$WOVM" ]]; then
  echo "deps-accept: build woc and wovm first (just woc-build; just wovm-build)" >&2
  exit 1
fi

W="$(mktemp -d "${TMPDIR:-/tmp}/deps-accept.XXXXXX")"
cleanup() { [[ -n "${DEPS_ACCEPT_KEEP:-}" ]] && echo "kept $W" || rm -rf "$W"; }
trap cleanup EXIT
G() { git -C "$1" -c user.email=t@t -c user.name=t "${@:2}"; }

# ---- the framework remote: a real git repo with a tag ----
mkdir -p "$W/fwsrc/strutil"
cat > "$W/fwsrc/wo.toml" <<'EOF'
name = "niceframework"
version = "0.1.0"
EOF
cat > "$W/fwsrc/greet.wo" <<'EOF'
pub fn greeting(who: Text) -> Text {
  return "hello, " .. who
}
fn main(args: multi Text) -> Int {
  return 99
}
EOF
cat > "$W/fwsrc/strutil/shout.wo" <<'EOF'
pub fn quiet(t: Text) -> Text {
  return to_lower(t)
}
EOF
git -C "$W/fwsrc" init -q
G "$W/fwsrc" add -A
G "$W/fwsrc" commit -qm one
G "$W/fwsrc" tag v0.1.0

# ---- the consuming app ----
mkdir -p "$W/app"
cat > "$W/app/wo.toml" <<EOF
name = "app"
version = "0.1.0"
[build]
runtime = "$WOVM"
[deps]
niceframework = { git = "file://$W/fwsrc", rev = "v0.1.0" }
EOF
cat > "$W/app/main.wo" <<'EOF'
use niceframework
use niceframework/strutil

fn main(args: multi Text) -> Int {
  print(greeting("web"));
  print(quiet("QUIET"));
  return 0;
}
EOF

# ---- 1. cold fetch + lock + build + run (dep main must NOT be the entry:
#         the dep's main returns 99; the app's prints and returns 0) ----
out="$(cd /tmp && "$WOC" "$W/app" 2>&1 && "$W/app/target/app")"
rc=$?
if [[ $rc -eq 0 && "$out" == "hello, web
quiet" && -f "$W/app/wo.lock" ]]; then
  ok "cold fetch + lock + use <dep> and <dep>/sub + app entry wins"
else
  bad "cold build" "rc=$rc out=$(printf '%s' "$out" | tr '\n' '|')"
fi
locked_sha="$(awk '!/^#/ {print $2}' "$W/app/wo.lock")"

# ---- 2. offline rebuild: remote removed, lock + cache satisfy ----
mv "$W/fwsrc" "$W/fwsrc.hidden"
rm -rf "$W/app/target"
if "$WOC" "$W/app" >/dev/null 2>&1 && [[ "$("$W/app/target/app")" == "hello, web
quiet" ]]; then
  ok "offline rebuild (remote deleted, lock satisfied)"
else
  bad "offline rebuild" "failed with the remote gone"
fi
mv "$W/fwsrc.hidden" "$W/fwsrc"

# ---- 3. moved tag: cold cache + lock -> rebuilt at the LOCKED sha ----
G "$W/fwsrc" commit -qm two --allow-empty
G "$W/fwsrc" tag -f v0.1.0 >/dev/null 2>&1
rm -rf "$W/app/.wo-deps"
"$WOC" "$W/app" >/dev/null 2>&1
now_sha="$(git -C "$W/app/.wo-deps/niceframework" rev-parse HEAD 2>/dev/null)"
if [[ "$now_sha" == "$locked_sha" ]]; then
  ok "moved tag: lock wins (checkout pinned to the locked SHA)"
else
  bad "moved tag" "expected $locked_sha got $now_sha"
fi

# ---- 4. --update-deps follows the tag and rewrites the lock ----
"$WOC" --update-deps "$W/app" >/dev/null 2>&1
new_lock="$(awk '!/^#/ {print $2}' "$W/app/wo.lock")"
tag_sha="$(git -C "$W/fwsrc" rev-parse v0.1.0)"
if [[ "$new_lock" == "$tag_sha" && "$new_lock" != "$locked_sha" ]]; then
  ok "--update-deps refreshes the lock to the moved tag"
else
  bad "--update-deps" "lock=$new_lock tag=$tag_sha"
fi

# ---- 5. cache/lock drift is a named diagnostic ----
G "$W/app/.wo-deps/niceframework" commit -qm local --allow-empty
out="$("$WOC" "$W/app" 2>&1)"
if [[ "$out" == *"WO-E106"* && "$out" == *"lock drift"* ]]; then
  ok "cache/lock drift diagnosed (WO-E106)"
else
  bad "drift" "$(printf '%s' "$out" | head -1)"
fi
git -C "$W/app/.wo-deps/niceframework" reset -q --hard HEAD~1

# ---- 6. transitive [deps] refused ----
printf '[deps]\nx = { git = "file:///nowhere", rev = "v1" }\n' >> "$W/fwsrc/wo.toml"
G "$W/fwsrc" commit -qam trans
G "$W/fwsrc" tag -f v0.1.0 >/dev/null 2>&1
rm -rf "$W/app/.wo-deps" "$W/app/wo.lock"
out="$("$WOC" "$W/app" 2>&1)"
if [[ "$out" == *"WO-E106"* && "$out" == *"transitive"* ]]; then
  ok "transitive [deps] refused (flat-only)"
else
  bad "transitive" "$(printf '%s' "$out" | head -1)"
fi
G "$W/fwsrc" revert --no-edit HEAD >/dev/null 2>&1
G "$W/fwsrc" tag -f v0.1.0 >/dev/null 2>&1

# ---- 7. dep name colliding with a local module directory ----
mkdir -p "$W/app/niceframework"
touch "$W/app/niceframework/x.wo"
rm -rf "$W/app/.wo-deps" "$W/app/wo.lock"
out="$("$WOC" "$W/app" 2>&1)"
if [[ "$out" == *"WO-E107"* ]]; then
  ok "dep/local module-name collision diagnosed (WO-E107)"
else
  bad "collision" "$(printf '%s' "$out" | head -1)"
fi
rm -rf "$W/app/niceframework"

# ---- 8. manifest shape: a dep without `rev` is refused at parse ----
mkdir -p "$W/norev"
printf 'name = "norev"\n[deps]\nfw = { git = "file:///x" }\n' > "$W/norev/wo.toml"
printf 'fn main(args: multi Text) -> Int { return 0 }\n' > "$W/norev/main.wo"
out="$("$WOC" "$W/norev" 2>&1)"
if [[ "$out" == *"needs both"* ]]; then
  ok "missing rev refused at manifest parse"
else
  bad "missing rev" "$(printf '%s' "$out" | head -1)"
fi

echo
printf 'deps-accept: %d checks, %d failures\n' "$((pass + fail))" "$fail"
[[ $fail -eq 0 ]]
