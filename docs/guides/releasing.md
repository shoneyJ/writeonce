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

## Two routes

**Automated (preferred).** `.github/workflows/release.yml` builds,
verifies and publishes on a `v*` tag push. It needs no `gh auth login`
and no secret: GitHub injects a per-job `GITHUB_TOKEN`, and the single
line `permissions: contents: write` is what lets that token create a
release. The token expires when the job ends, so there is nothing to
rotate or leak. Skip to *Releasing from the pipeline* below.

**Manual.** Everything from §0 onward — the path for a first release, or
when the pipeline is broken and you need to ship anyway.

## Making the pipeline ready (first time only)

It does **not** build on your machine, and pushing to `master` does not
release anything. The job runs on a GitHub-hosted runner, and only a
`v*` TAG push starts it. Ordinary commits, PRs and branch pushes are
ignored by this workflow.

```
 1  Get the workflow onto GitHub — Actions only sees files in the repo.
      git checkout master
      git merge site-homepage          # or open a PR and merge it
      git push origin master

 2  Repo -> Actions tab. If it offers to enable workflows, enable them.

 3  Repo -> Settings -> Actions -> General -> "Allow actions":
      must permit actions/checkout and ocaml/setup-ocaml.
      On "Allow select actions", add:  ocaml/setup-ocaml@*

 4  Same page -> "Workflow permissions". The workflow asks for
    contents: write explicitly, which is normally enough. If the
    publish step later fails with 403, come back and select
    "Read and write permissions".

 5  REHEARSE with a dry run — no tag, no publish, no cleanup.
    Actions tab -> "release" -> "Run workflow" -> master.
    A workflow_dispatch run skips the tag guard and the publish step,
    so it builds, verifies, smoke-tests the extracted tarball and
    reports the glibc floor, and stops there.

 6  (nothing to undo — a dry run creates no tag and no release)

 7  Watch it: the Actions tab, or `gh run watch` once gh is
    authenticated.

 8  Read the "Report the glibc floor" step. It prints what the
    RUNNER-built binaries actually require. Expect 2.35-ish from
    ubuntu-22.04, versus 2.38 from this dev machine.

 9  Update docs/examples/site/install/view.wo's supported-systems list
    to whatever step 8 printed, and the distros that follow from it.
    Publishing binaries whose floor differs from the page is the one
    failure a user cannot debug.

    Shipping that change to writeonce.de is its own runbook:
    docs/guides/deploying-site.md. Note especially that a new or
    renumbered CHAPTER does not appear on a host that already has a
    WO_DATA directory — the seed only fills an empty table.

    docs/examples/site is a SUBMODULE (github.com/shoneyJ/writeonce-site),
    so this edit is a commit in THAT repo, pushed there, and then a
    second commit here moving the submodule pointer. Editing the files
    and committing only in this repo records nothing — the change lives
    in a directory this repo tracks by revision, not by content.

10  Ship for real:
      git tag -a v0.1.0 -m "writeonce 0.1.0"
      git push origin v0.1.0

11  Verify the link a stranger clicks (should print 200):
      curl -sIL -o /dev/null -w '%{http_code}\n' \
        https://github.com/shoneyj/writeonce/releases/download/v0.1.0/writeonce-0.1.0-linux-amd64.tar.gz

12  Refresh the site's mirror from $WO_DIST (section 7 below).
```

There is no rehearsal to clean up and no draft flag to remove: a
`workflow_dispatch` run creates neither a tag nor a release, and
`gh release create` in the workflow publishes directly — the file has never
carried `--draft`. If you *want* a draft first, add `--draft` to that step
yourself and remember to remove it again.

Known first-run risks, in the order they are likely to bite:
`ocaml/setup-ocaml@v3` resolving OCaml 4.14 on the 22.04 image; the
`objdump` in the glibc-floor step needing `binutils` (present on GitHub
runners, absent in slim containers); and a 403 on publish, which is
step 4.

## What the hosted runner costs

**Public repository: nothing.** Standard GitHub-hosted runners are free
with unlimited minutes on public repos. Only *larger* runners (4-core
and up) are billed there, and this workflow does not ask for one.

**Private repository:** each plan includes monthly minutes — 2,000 on
Free, 3,000 on Pro and Team, 50,000 on Enterprise Cloud — then bills
per minute. Linux counts ×1, Windows ×2, macOS ×10, so staying on Linux
is also the cheap choice. Each *job* is rounded up to the next minute.

This job is small either way. A cold `scripts/mkdist.sh` measured 3.4s
on a 20-core workstation; on a 2-core runner call it well under a
minute. The real cost is `ocaml/setup-ocaml`, which is minutes cold and
under one when its opam cache hits — so budget roughly 3–10 minutes per
release, and it only ever runs on a tag. Ten releases a month is a
couple of percent of the smallest free allowance.

Release assets do **not** count against Actions artifact storage; they
live with the release. (Rates and allowances change — check the current
billing page before making a decision that depends on them.)

## Why the release job is NOT on a self-hosted runner

A self-hosted runner would work — it needs no inbound ports, polls
GitHub over outbound HTTPS, and honours `HTTPS_PROXY`/`NO_PROXY`, so a
box behind a proxy is fine. Two reasons not to use one for THIS job:

**It defeats the point of pinning the runner.** The release binaries
link glibc dynamically, so the build host sets the floor every user
must clear. `ubuntu-22.04` was chosen to keep that floor near 2.35. A
runner on a developer machine (glibc 2.39 here) puts it back at 2.38+
and silently drops Ubuntu 22.04, Debian 12 and RHEL 9 — the exact
regression the pinned image prevents. If a self-hosted runner is
unavoidable, build inside a container pinned to the oldest glibc you
intend to support, not on the host.

**A release built on a workstation is unattested.** "It built on my
machine" is what a pipeline exists to stop being true.

### If you do self-host, what you are accepting

A runner executes workflow code **as the user that started it**, with
that user's filesystem and network reach. On a personal workstation
that means `~/.ssh`, `~/.config/gh`, cloud and cluster credentials,
browser profiles, and every host reachable from it — including LAN
services and anything named in `~/.ssh/config`, which on a work laptop
is usually production. A job does not need to be malicious to leak;
it needs to be careless once.

The risk is highest on a **public** repository, where a pull request
from a stranger can run arbitrary code on the runner. GitHub's own
guidance is not to use self-hosted runners with public repositories.
On a private repository the blast radius is smaller but not zero:
anyone with write access, or one compromised token, reaches the same
shell.

If it is still the right call, make it boring:

- a dedicated VM or container, never a workstation, on a network
  segment that cannot reach production;
- a separate unprivileged user with no SSH keys, no cloud config, and
  no credentials of its own;
- `--ephemeral` registration so each job gets a clean runner and a
  poisoned toolchain cannot outlive one build;
- `.credentials` under the runner's home is a long-lived credential to
  act as that runner — treat the box as holding a secret;
- restrict egress if the workload allows; the same outbound HTTPS the
  runner needs is what exfiltration would use.

A reasonable split: self-hosted for tests that need private-network
access or unusual hardware, GitHub-hosted for the release artifact.

## Releasing from the pipeline

```
git tag -a v0.1.0 -m "writeonce 0.1.0"
git push origin v0.1.0
```

That is the whole release. The workflow then, in order: checks the tag
matches `VERSION`, builds via `scripts/mkdist.sh`, asserts the produced
filename is the one `/install` links, verifies the `.sha256`, extracts
the tarball and builds a hello project **with the binaries inside it**,
prints the glibc floor of what is about to ship, and publishes both
files with `gh release create`.

Two things about that file are deliberate:

- **`runs-on: ubuntu-22.04`, not `ubuntu-latest`.** The binaries link
  glibc dynamically, so the build host's glibc caps the symbol versions
  they can import — and that cap becomes the minimum glibc every user
  needs. On 24.04 (glibc 2.39) the floor is 2.39; on 22.04 (2.35) it is
  2.35. That is the difference between excluding and including Ubuntu
  22.04, Debian 12 and RHEL 9. Changing the image changes who can run
  the release, so change `install/view.wo`'s supported-systems list in
  the same commit.
- **It fails rather than publishes** when the tag, `VERSION` and the
  asset name disagree, because those three are what the download URL on
  `/install` is built from.

### Other CI (GitLab, Jenkins, Buildkite)

No `gh auth login` there either — `gh` reads a token from the
environment:

```
GH_TOKEN=$MY_SECRET gh release create v0.1.0 dist/*.tar.gz dist/*.sha256
```

The secret is a PAT with `repo` scope (classic) or **Contents: read and
write** (fine-grained). Outside GitHub it is a long-lived credential you
own and must rotate — which is exactly the cost `GITHUB_TOKEN` avoids,
and the reason to prefer Actions for this one job even if the rest of
your CI lives elsewhere.

## 0. Authenticate `gh` (manual route only)

`gh` keeps its own credential, separate from git's. SSH keys let you
`git push`; they do **not** let `gh` call the API, so a machine that
pushes fine can still fail to create a release.

Check first — if this names your account, skip the rest of this section:

```
gh auth status
```

### The interactive flow (what to pick)

```
gh auth login
```

It asks five things:

| prompt | answer |
| --- | --- |
| What account do you want to log into? | **GitHub.com** |
| Preferred protocol for Git operations? | **SSH** — this repo's remote is already `git@github.com:shoneyJ/writeonce.git`, and answering HTTPS here rewrites how git talks to GitHub for every repo on the host |
| Upload your SSH public key? | pick `~/.ssh/id_ed25519.pub`, or **Skip** if that key is already on your account |
| How would you like to authenticate? | **Login with a web browser** |
| One-time code | gh prints something like `ABCD-1234`; press Enter, paste it at <https://github.com/login/device>, authorise |

The token lands in the system credential store, or in a plain file if
there is no store (`gh auth status` prints the location; `--insecure-storage`
forces the file). `-w`/`--web` skips straight to the browser step.

**In a Claude Code session, prefix it with `!`** — `! gh auth login` — so
the prompts are yours to answer and the output lands in the conversation.
An agent cannot complete a device flow on your behalf.

### No browser on that machine (server, container, CI)

Create a personal access token at <https://github.com/settings/tokens>.
Classic tokens need the scopes `repo`, `read:org` and `gist`;
a fine-grained token needs **Contents: read and write** on the repo,
which is what release assets are written through. Then:

```
printf '%s' "$TOKEN" | gh auth login --with-token
```

Read it from a `chmod 600` file rather than typing it inline — an
argument on the command line lands in shell history and in `ps`. For
automation, skip login entirely and export `GH_TOKEN`; gh picks it up and
stores nothing.

### Verify before you rely on it

```
gh auth status
gh repo view shoneyJ/writeonce --json name,visibility
```

The second call is the real check: it proves the token can reach *this*
repo, which a valid-but-wrong-account token would not.

## 1. Before you build

Check the tree is clean and the version is what you mean to ship —
`VERSION` is the single source, and `mkdist.sh` refuses to build if
`woc version` or `wovm --version` disagree with it:

```
git status --porcelain     # expect empty
cat VERSION                # e.g. 0.1.0
```

## 2. Build the artifact

```
just dist
```

That builds both release binaries, runs the version drift guard, and
writes two files:

```
dist/writeonce-<ver>-linux-amd64.tar.gz
dist/writeonce-<ver>-linux-amd64.tar.gz.sha256
```

## 3. Verify it before anyone else can

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

## 4. Tag the commit

The tag is what the download URL points at, so tag the exact commit the
binaries were built from:

```
git tag -a v0.1.0 -m "writeonce 0.1.0"
git push origin v0.1.0
```

## 5. Publish and upload

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

## 6. Check the link the site actually uses

```
curl -sIL -o /dev/null -w '%{http_code} %{url_effective}\n' \
  https://github.com/shoneyj/writeonce/releases/download/v0.1.0/writeonce-0.1.0-linux-amd64.tar.gz
```

A `200` means `/install`'s download button works for a stranger. Anything
else means the tag, the asset name, or the repo path disagrees with the
link in `docs/examples/site/install/view.wo`.

## 7. Refresh the site's mirror

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
  text, so grep for the old number before you publish. That file is in
  the `writeonce-site` submodule — commit and push it there, then bump
  the pointer here.
- **One target today.** `mkdist.sh` builds `linux-amd64` only; a cross
  matrix is future work. Do not add architectures to the release notes
  that no build produces.
