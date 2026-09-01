# Zen Browser parity — what a browser-class application actually asks of a language

Source: [`.dev/reference/zen-browser/`](../../../../.dev/reference/zen-browser/)
(shallow clone of zen-browser/desktop, surveyed 2026-09-01). Third study in the
series after [`alacritty`](../alacritty/00-alacritty-parity.md) and
[`tmux`](../tmux/00-tmux-parity.md) — and the one whose answer is different in
kind, which is why it is worth having.

## What Zen actually is (measured)

Zen is not a browser codebase. It is a **Firefox overlay**: `surfer.json` pins
`product: firefox, version: 154.0.1`, `npm run download` fetches the Firefox
source into `engine/` at build time, and the repo contributes **256 patch
files** plus a 33 MB `src/` tree that is copied over it — ~246,000 lines
counting JS/MJS/CSS/XHTML/patches (616 `.js` files, 568 Fluent localization
files, 48 CSS). There is no rendering, layout, JS-engine, or networking code
in the repository at all; Gecko (tens of millions of lines of C++/Rust)
arrives as a downloaded dependency and is built with Mozilla's own `mach`.

Zen's value-add lives in `src/zen/`: workspaces ("spaces"), split view,
compact mode, glance, folders, session store, sync — all UI composition and
state, written in JavaScript **because the engine ships a JavaScript host and
Zen's code runs inside it**. The engine is also the app platform.

## The lesson, stated plainly

Nobody — including a successful, well-staffed browser project — writes a
browser. They skin an engine. So "mature the language until it can build an
application such as this" resolves to three different questions, and only one
of them is real work for writeonce:

1. **Build the engine in `.wo`?** Not a maturity path, a decades-long refusal.
   The gap list it produces ("general FFI, C++ interop, a GPU pipeline, a JS
   VM…") is the alacritty study's stage-E fork multiplied by a thousand, with
   no intermediate shippable stage. Rejected as a driving workload — it cannot
   drive, only sink.
2. **Embed an engine (CEF/WebKitGTK) behind FFI?** The heaviest possible FFI
   consumer. Same fork as alacritty item 7, and this study deliberately does
   not promote it: an embedded engine's API surface is enormous and unstable,
   the worst first FFI customer imaginable.
3. **Drive an engine out-of-process.** Chromium and Firefox both expose a
   remote-debugging protocol (CDP; Firefox now speaks it too) over a local
   socket or a stdio pipe. A writeonce program that spawns a browser, connects,
   and drives it — kiosk shells, scrapers, site-acceptance drivers of the kind
   `site-accept.sh` fakes with curl today — is the browser-class workload that
   is actually reachable, and it is exactly the actor/protocol shape the
   runtime is built around. This is Playwright's architecture, minus the
   Node.js.

## What route 3 needs — mostly the existing list, one genuinely new gap

- **Bounded subprocess with stdio transport** — spawning the browser with a
  pipe transport is the third consumer of iteration 28's named gap (after the
  alacritty and tmux studies' PTY stages; this one does not even need a PTY).
- **`net.connect`** — iteration 38, already named, for the debugging socket.
- **NEW — WebSocket CLIENT.** CDP speaks WebSocket; iteration 24 landed the
  server side (`ws_accept`, frames) but nothing performs an outbound upgrade
  handshake. Small — the frame code is the hard half and already exists.
- **JSON both ways** — exists. Actor-per-session/per-tab supervision, timeouts
  (`time.after`), graceful teardown — all landed in iteration 24.

Notably absent: nothing graphical, no terminfo, no termios, no fd passing.
Route 3 is *closer* than the tmux stage.

## What Zen's own feature layer says about writeonce

Zen's ~250k-line overlay is session stores, workspace state, sync, and
declarative UI — in writeonce terms: `@table` rows (durable by declaration
since databasev2 2), and wo-html components. The project's existing bet —
that the browser is the *client* and the application lives server-side in
porch — already covers the useful half of what Zen builds. The other half of
Zen's lesson is about hosting: Gecko wins as a platform because it embeds a
scripting language with capability boundaries, which is iteration 28's
skillhost direction (writeonce as the confined host, not the confined guest).

## Verdict for the maturity path

The browser study adds **one builtin-sized gap** (WebSocket client) and
**one workload** to the staged path from the alacritty study — call it
**stage C′, the browser driver**, parallel to stage C (it needs subprocess +
`net.connect` + ws-client, none of stage C's tty machinery). Proof when it
lands: replace a curl leg of `site-accept.sh` with a `.wo` driver that opens
writeonce.de in a real browser, asserts the rendered chapter list, and tears
the browser down cleanly — the site gate exercising the language's own
browser-automation story end to end. Engine-building and engine-embedding
stay refused, by name, with this study as the reason.
