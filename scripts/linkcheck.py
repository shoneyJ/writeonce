#!/usr/bin/env python3
"""Scan every repo-authored .md, extract links, report broken local targets + bad anchors.

`.dev/` and `.superpowers/` are excluded: both are gitignored, per-developer
material this repo does not author -- vendored plugin-skill copies and cloned
reference projects (whose own docs use site-build-relative links that cannot
resolve on disk). Including them made the gate report the same ~23 failures
forever, which is a gate nobody reads. Everything the repo actually ships is
still scanned.
"""
import os, re, sys, urllib.parse
from collections import defaultdict

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")

SKIP_DIRS = {".git", "node_modules", "_build", "target", "dist", ".dev", ".superpowers"}

INLINE = re.compile(r'(?<!!)\[([^\]\n]*)\]\(\s*<?([^)\s>]+)>?(?:\s+"[^"]*")?\s*\)')
REFDEF = re.compile(r'^\s{0,3}\[([^\]]+)\]:\s*<?(\S+)>?', re.M)
FENCE = re.compile(r'(^```.*?^```|^~~~.*?^~~~)', re.M | re.S)
INLINE_CODE = re.compile(r'`[^`\n]*`')
ATX = re.compile(r'^(#{1,6})\s+(.*?)\s*#*\s*$', re.M)
HTML_ANCHOR = re.compile(r'<a\s+[^>]*(?:name|id)=["\']([^"\']+)["\']', re.I)

def strip_code(text):
    text = FENCE.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    return INLINE_CODE.sub("", text)

def slugify(heading):
    # GitHub-style slug
    h = re.sub(r'<[^>]+>', '', heading)
    h = re.sub(r'!?\[([^\]]*)\]\([^)]*\)', r'\1', h)   # links -> text
    h = h.replace('`', '').replace('*', '').replace('_', '')
    h = h.strip().lower()
    h = re.sub(r'[^\w\- ]', '', h, flags=re.UNICODE)
    return h.replace(' ', '-')

md_files = []
for dirpath, dirnames, filenames in os.walk(ROOT):
    dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
    for f in filenames:
        if f.lower().endswith((".md", ".markdown")):
            md_files.append(os.path.join(dirpath, f))
md_files.sort()

anchors = {}
def get_anchors(path):
    if path in anchors:
        return anchors[path]
    try:
        raw = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        anchors[path] = set()
        return anchors[path]
    body = strip_code(raw)
    s = set()
    seen = defaultdict(int)
    for _, head in ATX.findall(body):
        base = slugify(head)
        if not base:
            continue
        n = seen[base]
        seen[base] += 1
        s.add(base if n == 0 else f"{base}-{n}")
    s |= set(HTML_ANCHOR.findall(raw))
    anchors[path] = s
    return s

broken = []
anchor_bad = []
stats = defaultdict(int)

for md in md_files:
    raw = open(md, encoding="utf-8", errors="replace").read()
    body = strip_code(raw)
    lines = body.split("\n")
    targets = []
    for m in INLINE.finditer(body):
        targets.append((body[:m.start()].count("\n") + 1, m.group(2)))
    for m in REFDEF.finditer(body):
        targets.append((body[:m.start()].count("\n") + 1, m.group(2)))

    for lineno, target in targets:
        t = target.strip()
        if not t:
            continue
        low = t.lower()
        if low.startswith(("http://", "https://", "mailto:", "ftp://", "tel:", "data:")):
            stats["external"] += 1
            continue
        if t.startswith("#"):
            stats["anchor-local"] += 1
            frag = urllib.parse.unquote(t[1:])
            if frag and frag not in get_anchors(md):
                anchor_bad.append((md, lineno, t))
            continue
        stats["local"] += 1
        pathpart, _, frag = t.partition("#")
        pathpart = urllib.parse.unquote(pathpart)
        frag = urllib.parse.unquote(frag)
        if pathpart.startswith("/"):
            cand = os.path.join(ROOT, pathpart.lstrip("/"))
        else:
            cand = os.path.normpath(os.path.join(os.path.dirname(md), pathpart))
        if not os.path.exists(cand):
            broken.append((md, lineno, t))
            continue
        if frag and cand.lower().endswith((".md", ".markdown")):
            if frag not in get_anchors(cand):
                anchor_bad.append((md, lineno, t))

rel = lambda p: os.path.relpath(p, ROOT)
print(f"files={len(md_files)} links: local={stats['local']} external={stats['external']} same-file-anchor={stats['anchor-local']}")
print(f"\n== BROKEN PATHS ({len(broken)}) ==")
for md, ln, t in broken:
    print(f"{rel(md)}:{ln}  ->  {t}")
print(f"\n== BAD ANCHORS ({len(anchor_bad)}) ==")
for md, ln, t in anchor_bad:
    print(f"{rel(md)}:{ln}  ->  {t}")
