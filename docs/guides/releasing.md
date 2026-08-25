# Cutting a release — building the tarball and publishing it on GitHub

The `/install` page on writeonce.de links a GitHub release asset by an
exact URL. Publishing is therefore not "upload a file somewhere": the
**asset filename has to match what the site links**, or the download
button 404s. This runbook keeps the two in step.

The URL the site links today:

```
https://github.com/shoneyj/writeonce/releases/download/v0.1.0/writeonce-0.1.0-linux-amd64.tar.gz
```

which decomposes as `<repo>/releases/download/<tag>/<asset-name>`. So the
tag must be `v0.1.0` and the asset must be named exactly
`writeonce-0.1.0-linux-amd64.tar.gz` — which is what `just dist` already
produces.

## 0. Before you start

`gh` must be authenticated. It is an interactive browser/device flow, so
run it yourself:

```
gh auth login
```

In a Claude Code session, type it with a leading `!` so the output lands
in the conversation: `! gh auth login`.

Check the tree is clean and the version is what you mean to ship —
`VERSION` is the single source, and `mkdist.sh` refuses to build if
`woc version` or `wovm --version` disagree with it:

```
git status --porcelain     # expect empty
cat VERSION                # e.g. 0.1.0
```

## 1. Build the artifact

```
just dist
```

That builds both release binaries, runs the version drift guard, and
writes two files:

```
dist/writeonce-<ver>-linux-amd64.tar.gz
dist/writeonce-<ver>-linux-amd64.tar.gz.sha256
```

## 2. Verify it before anyone else can

Two checks, both cheap, both worth it. First the digest:

```
cd dist && sha256sum -c writeonce-0.1.0-linux-amd64.tar.gz.sha256 && cd ..
```

Then prove the tarball actually works, using the binaries INSIDE it —
not the ones in your build tree:

```
tmp=$(mktemp -d)
tar -C "$tmp" -xzf dist/writeonce-0.1.0-linux-amd64.tar.gz
export PATH="$tmp/writeonce/bin:$PATH"
woc version && wovm --version

mkdir -p "$tmp/hello" && cd "$tmp/hello"
printf 'name    = "hello"\nversion = "0.1.0"\n\n[runtime]\nwo = ">= 0.1"\n' > wo.toml
printf 'fn main() -> Int {\n  print("hello, writeonce");\n  return 0;\n}\n' > main.wo
woc . && ./target/hello
```

If that prints `hello, writeonce`, the release is sound.

## 3. Tag the commit

The tag is what the download URL points at, so tag the exact commit the
binaries were built from:

```
git tag -a v0.1.0 -m "writeonce 0.1.0"
git push origin v0.1.0
```

## 4. Publish and upload

One command creates the release and attaches both files:

```
gh release create v0.1.0 \
  dist/writeonce-0.1.0-linux-amd64.tar.gz \
  dist/writeonce-0.1.0-linux-amd64.tar.gz.sha256 \
  --title "writeonce 0.1.0" \
  --notes-file - <<'EOF'
Linux x86-64, glibc 2.38 or newer. Two binaries — `woc` and `wovm` —
depending only on the system C library.

    rm -rf /usr/local/writeonce
    tar -C /usr/local -xzf writeonce-0.1.0-linux-amd64.tar.gz
    export PATH=$PATH:/usr/local/writeonce/bin

Verify with the published `.sha256` before extracting.
EOF
```

Add `--draft` to stage it without publishing, or `--prerelease` to mark
it as one. If the release already exists and you only need to attach (or
replace) files:

```
gh release upload v0.1.0 dist/writeonce-0.1.0-linux-amd64.tar.gz --clobber
```

Without `gh`, the same thing through the web UI: the repo's **Releases**
page → *Draft a new release* → choose tag `v0.1.0` → drag both files
into the attachment box → *Publish release*. Uploading by hand is where
the filename usually drifts, so paste it rather than retyping it.

## 5. Check the link the site actually uses

```
curl -sIL -o /dev/null -w '%{http_code} %{url_effective}\n' \
  https://github.com/shoneyj/writeonce/releases/download/v0.1.0/writeonce-0.1.0-linux-amd64.tar.gz
```

A `200` means `/install`'s download button works for a stranger. Anything
else means the tag, the asset name, or the repo path disagrees with the
link in `docs/examples/site/install/view.wo`.

## 6. Refresh the site's mirror

`/install` offers this site as a mirror beside the GitHub link, served by
the framework's `StaticFiles` out of `$WO_DIST` (default `./dist`). Copy
the same two files there so the mirror is not stale:

```
scp dist/writeonce-0.1.0-linux-amd64.tar.gz* user@host:/srv/writeonce/dist/
```

Both copies are the same bytes, and the published `.sha256` covers
either one — that is the point of shipping the digest beside the
archive.

## Notes

- **`dist/` is gitignored** (`.gitignore:103`). The tarball is a build
  artifact; the release is where it lives, not the repository.
- **Repo path case.** The git remote is `shoneyJ/writeonce` while the
  site links `shoneyj/writeonce`. GitHub paths are case-insensitive and
  redirect, so both resolve — but keep them consistent when either
  changes.
- **Bumping the version** means editing `VERSION`, rebuilding, and
  updating every place the site names the current release:
  `docs/examples/site/install/view.wo` (the download links, the tar
  command, and the `.sha256` link). The version appears there as literal
  text, so grep for the old number before you publish.
- **One target today.** `mkdist.sh` builds `linux-amd64` only; a cross
  matrix is future work. Do not add architectures to the release notes
  that no build produces.
