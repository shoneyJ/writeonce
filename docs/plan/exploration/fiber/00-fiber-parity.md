# Fiber parity study — what a mainstream Go web framework ships that `writeonce-serve` does not

Reference: [gofiber/fiber](https://github.com/gofiber/fiber) **v3.5.0**, read
2026-08-26 from a shallow clone at `.dev/reference/fiber` (gitignored — re-clone
with `git clone --depth 1 https://github.com/gofiber/fiber .dev/reference/fiber`).
Read against `docs/examples/writeonce-serve` as it stands the same day.

Why Fiber and not Express or Axum: it is the closest structural analogue in the
reference set. Compiled language, no runtime, one binary, an explicit
`Ctx`-per-request, and middleware as an ordered chain — the same shape
`writeonce-serve` already has. Where it differs, the difference is a feature
decision rather than a paradigm gap, which is what makes the comparison useful.
Express would have contributed mostly "you have no closures".

What was actually read: `app.go` (routing methods, the 53-field `Config`),
`ctx.go` / `req.go` / `res.go` (the request and response surface), `bind.go`
(binding), `hooks.go` (lifecycle), and the `Config` struct of every one of the
**32** packages under `middleware/`.

**Headline: `writeonce-serve` is further along than its size suggests.** Of
Fiber's 32 middleware packages, 9 already have a working `writeonce-serve`
counterpart (CORS, basic auth, key/bearer auth, helmet-style security headers,
ETag, static files, logger, host authorization, recover-as-500). The gaps are
real but they are mostly *breadth*, and they cluster around four things:
**cookies** (absent entirely, and half the list sits on top of them),
**streaming** (absent, and SSE/compression/chunked all sit on that),
**binding** (blocked by doctrine until `@derive`), and **one missing runtime
primitive nobody had noticed**.

---

## 0. The blocker the existing ledger gets wrong

`docs/examples/writeonce-serve/README.md`'s crypto row currently reads:

> Unlocks (signed cookies, CSRF, session integrity, webhook verification, JWT
> HS256) | ⬜ **UNBLOCKED** (the primitives exist since iteration 34)

**That is not true for three of the five.** SHA-256 and HMAC-SHA256 let you
*authenticate* a token. They do not let you *mint* one, because
**writeonce has no source of randomness at all** — `grep -inE
'random|rand|urandom|getrandom'` over `compiler/src/types.ml`,
`runtime/src/wob.h`, `sysio.c` and `crypto.c` returns nothing. There is no
`getrandom(2)`, no `/dev/urandom` read (`fs.read_at` could reach it, but `fs`
has no way to open a character device meaningfully and the result would be a
`Text` of raw bytes with no API contract), and no CSPRNG builtin.

Fiber's session store defaults its `KeyGenerator` to a UUID; its CSRF
middleware mints a token per request. Both are unguessability requirements, not
integrity requirements. An HMAC over a *predictable* session id is not a
session — it is a signed guess.

So the honest dependency order is: **a random-bytes builtin comes before
sessions and CSRF, not after them.** Signed cookies over an
application-supplied value and webhook verification (where the secret comes
from config and the nonce comes from the *sender*) genuinely are unblocked; JWT
HS256 is unblocked for verification and blocked for issuing anything with a
random `jti`.

This is the study's most valuable single finding and it is the reason iteration
39 leads with the primitive rather than the middleware.

---

## 1. Cookies — absent, and foundational

`writeonce-serve` has **no cookie support in either direction**. `Req` has
`headers: map<Text, Text>` and nothing parses `Cookie:`; `Resp` has
`headers: map<Text, Text>` and there is no `Set-Cookie` builder — and because
`Resp.headers` is a *map*, it structurally cannot carry the two `Set-Cookie`
lines a login-plus-flash response needs. That map is a real design constraint
this iteration has to confront, not a missing function.

Fiber, for comparison: `Req.Cookies(key)`, `Res.Cookie(*Cookie)` with
`ClearCookie`, and a `Cookie` struct carrying Path, Domain, MaxAge, Expires,
Secure, HTTPOnly, SameSite, Partitioned and SessionOnly.

| Piece | Fiber | writeonce-serve |
| --- | --- | --- |
| read request cookies | `Req.Cookies(key)` | — |
| set a response cookie | `Res.Cookie(&Cookie{...})` | — |
| clear | `Res.ClearCookie(key...)` | — |
| attributes | Path/Domain/MaxAge/Expires/Secure/HTTPOnly/SameSite/Partitioned | — |
| multiple `Set-Cookie` per response | native (header list) | **impossible** — `Resp.headers` is `map<Text,Text>` |
| signed / encrypted | `encryptcookie` middleware | — (HMAC exists; see §0 for the minting problem) |

Everything in §2 depends on this section landing first.

## 2. Session, CSRF, rate limiting, idempotency — the store-backed chain

All four are one shape in Fiber: a middleware plus a `Storage` interface. All
four are pure-`.wo` work in writeonce *once cookies and randomness exist*, and
writeonce has an unusual advantage here — `@table` gives a **durable,
WAL-backed, crash-recoverable** store for free, where Fiber ships an in-memory
default and makes you bolt on Redis for anything real.

| Middleware | Fiber's config knobs (the shape to translate) | writeonce status |
| --- | --- | --- |
| `session` | Storage, KeyGenerator, IdleTimeout, AbsoluteTimeout, CookieDomain/Path/SameSite/Secure/HTTPOnly/SessionOnly, Extractor | absent; needs §0 + §1 |
| `csrf` | Storage, Session, KeyGenerator, TrustedOrigins, SingleUseToken, CookieName + the cookie attrs, IdleTimeout, Extractor | absent; needs §0 + §1 |
| `limiter` | Storage, Max, Expiration, KeyGenerator, LimitReached, SkipFailed/SkipSuccessful, DisableHeaders | absent; needs only a store + `time.ticks` — **unblocked today** |
| `idempotency` | Storage, Lock, KeyHeader + validator, Lifetime, KeepResponseHeaders | absent; store + `time.ticks` — **unblocked today** |

`limiter` and `idempotency` are the two cheapest real wins in this whole
document: no new primitive, no cookie, just a `@table` and a clock that already
exists.

## 3. Streaming — absent, and three features sit on it

`internal/serve.wo` builds a whole response as one `Text` and writes it with a
single `net.write`; `serialize()` always emits `Content-Length`. There is no
flush, no chunked framing, no way to write a response incrementally.
`internal/parse.wo:153-157` **explicitly refuses** chunked request bodies, with
a correct note that silently treating a chunked request as body-less is request
smuggling — that refusal is good engineering and should stay until chunked is
implemented properly.

Blocked on this one seam:

- **SSE** (Fiber: `middleware/sse` with Retry, HeartbeatInterval, OnClose) —
  the natural fit for writeonce's actor model, since a room actor already has
  the fan-out shape. Wants iteration 24's chat work beside it.
- **Compression** (Fiber: `middleware/compress`, Level) — gzip/deflate/brotli.
  Iteration 36 landed the bitwise operators, so a pure-`.wo` DEFLATE is now
  *expressible*; whether it should be `.wo` or a C builtin is a genuine fork,
  and the honest answer probably depends on whether anything else ever wants
  zlib.
- **`SendFile` / `SendStream` / byte ranges** — Fiber's static ships ByteRange,
  Browse, MaxAge, CacheDuration, IndexNames, Download. `http/files.wo` serves
  whole small files only. Range requests are what make video and large
  downloads work.

The framework README already lists "lazy body streaming + backpressure ·
streaming responses · explicit commit point" as ⏸ unblocked-by-the-arc. This
study's contribution is naming what *else* falls out of it.

## 4. Binding — blocked by doctrine, and that is fine

Fiber's `Bind` is 16 methods: `Body`, `JSON`, `XML`, `CBOR`, `MsgPack`, `Form`,
`Query`, `URI`, `Header`, `Cookie`, `RespHeader`, `All`, `Custom`, plus
validator hooks. It fills a struct by reflecting over tags.

writeonce has `json.decode(t) as T` for JSON bodies and **nothing** for query,
path params, form or header — those hand you `map<Text, Text>` and you assign
field by field. Principle 13 forbids reflection, so this cannot be closed the
way Fiber closes it.

The right owner is **[iteration 29, `@derive`](../../../stories/language-runtime-database/29-compile-time-metaprogramming.md)**:
compile-time generation from the class table gives typed binding with no
runtime reflection. This is a genuine parity gap with a real answer that is
already on the roadmap, so iteration 39 records it and does not attempt it.

Same verdict, same reason, for the codec spread: Fiber ships XML, CBOR and
MsgPack encoders/decoders in `Config`. writeonce ships JSON. That is the small
stdlib doing its job, not a defect.

## 5. Routing and response ergonomics — mostly sugar, cheap to close

| Gap | Fiber | writeonce-serve |
| --- | --- | --- |
| method helpers | Get/Post/Put/Delete/Patch/Head/Options/Trace/Connect/All/Add | `get`/`post`/`put`/`delete_` only — a `Route { method: "PATCH" }` literal works, so this is registration sugar, but its absence is felt |
| route names + URL building | `Name()`, `GetRouteURL()` | — (no named routes, no reverse routing) |
| route introspection | `GetRoutes()`, `Stack()`, `HandlersCount()` | — |
| mount / sub-app | `Use(prefix, subApp)` | `Group { prefix }` covers the common case ✅ |
| case sensitivity / strict slash | `CaseSensitive`, `StrictRouting` | — (always case-sensitive, always lenient) |
| per-route body limit | `Config.BodyLimit` | `const BODY_MAX = 1048576` in `internal/parse.wo` — one compile-time number for the whole server |
| per-handler timeout | `middleware/timeout` (Timeout, OnTimeout) | conn-level `read_ms`/`idle_ms` only; bounding a *handler* needs cancellation, which the README already parks |
| `Location`, `Vary`, `Links`, `Append`, `Attachment`/`Download` | ✅ each a `Res` method | — (`set_header` by hand) |
| `Format`/`AutoFormat` content negotiation on the way out | ✅ | `accepts()` exists; no format dispatch helper |
| q-value **ranking** | ✅ | 🔶 already in the ledger: q-values stripped, not ranked |
| request id | `middleware/requestid` (Header, Generator) | `req.ctx` bag exists to carry it; no generator — and see §0 |
| healthcheck / favicon / redirect / rewrite / skip | five small middleware | — (each a handful of lines) |
| `earlydata`, `paginate`, `responsetime`, `envvar`, `expvar`, `pprof` | ✅ | — (`expvar`/`pprof` belong to iteration 30, which has no story file) |
| lifecycle hooks | 11 hook families (OnRoute, OnListen, OnPreShutdown, OnPostShutdown, …) | — README already notes "no user teardown hooks yet" 🔶 |
| `proxy` middleware | ✅ | **impossible today** — no `net.connect`; owned by [iteration 38](../../../stories/language-runtime-database/38-content-platform-capabilities.md) |
| `recover` with stack trace | `EnableStackTrace`, `StackTraceHandler` | trap → 500 and the server lives ✅; no backtrace primitive exists |

## 6. Deliberate divergences — listed so nobody re-opens them

Not gaps. Each was decided and the reasoning is on file.

| Fiber feature | writeonce position |
| --- | --- |
| `Views` / `Render` / `ReloadViews` / `PassLocalsToViews` — a runtime template engine | **Rejected.** Markup is a compile-time literal (iteration 37's raw text literal + `writeonce-view`'s `Component`) or it does not exist. A per-request file read is the already-rejected engine — see `docs/plan/discarded.md`. |
| TLS config, `SetTLSHandler`, HTTP/2 | **Proxy-terminated, forever** (principle: TLS is not the app's job). h2c stays parked behind iteration 23. |
| `adaptor` (net/http interop) | No FFI, no foreign handler ecosystem to adapt to. |
| `Concurrency`, `ReadBufferSize`, prefork/`OnFork` | The shard-actor runtime owns placement; there is no worker-pool knob to expose. |
| Closures as handlers | Handlers are classes satisfying `Handler` (`router/router.wo`'s own note: "no function values in this language, by doctrine — a handler is a CLASS, its fields are the closure substitute"). |
| `SharedState` / `Locals` as an untyped bag | `req.ctx` is `map<Text,Text>` on purpose; typed per-request state is a `@table` row or a field on the handler class. |

## 7. What this study feeds

[**Iteration 39 — web framework parity**](../../../stories/language-runtime-database/39-web-framework-parity.md)
takes §0–§2 and the cheap half of §5, in that order, because §0 gates §2 and §1
gates most of it.

Explicitly *not* iteration 39's, with owners:

- streaming, SSE, compression, byte ranges (§3) — the parked streaming slice
- typed binding (§4) — [iteration 29](../../../stories/language-runtime-database/29-compile-time-metaprogramming.md)
- TTL cache middleware — [iteration 18](../../../stories/language-runtime-database/18-memory-db-features.md)
- `proxy` — [iteration 38](../../../stories/language-runtime-database/38-content-platform-capabilities.md)
- `pprof`/`expvar`/metrics — iteration 30
- everything in §6 — closed by doctrine
