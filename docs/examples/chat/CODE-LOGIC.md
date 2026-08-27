# `docs/examples/chat` — how the sample is put together

Iteration 24's acceptance workload: rooms, presence and broadcast over
WebSocket, actors on fibers across shards, one binary, no broker. It exists to
*drive* the actor work, so nearly every shape here is chosen to exercise
something the runtime claims.

Gate: `just chat` (`scripts/chat-accept.sh`), which logs to `/tmp/chat.log` —
`tail -F` it while the gate runs.

## The actors

| Actor | Owns | Answers |
| --- | --- | --- |
| `Registry` | name → room map, a fallback room | a `call` returning the room's address; spawns rooms on demand |
| `Room` | its member list (writer address + name) | join, leave, a text line, shutdown |
| `Reader` | the read half of one connection | nothing — it loops on the fd and sends onward |
| `Writer` | the **fd**, and the write half | text, pong, close |
| `ConnWorker` | one accepted connection | runs the HTTP layer over that fd |

`Registry` is the first honest consumer of `call`: the handler runs on the
connection worker's shard, the registry lives wherever placement put it, and
the reply is a scalar — the room's address. That is the cross-shard `call`
proof the gate asserts, not a contrivance added for it.

## Two actors per connection, not one

One fd, two directions, and they block independently. A single actor would have
to be inside `read` to notice the client, and inside `write` to deliver a
broadcast — it cannot be in both, so a broadcast would stall behind a quiet
client's read. Splitting them buys three things:

1. **The `Writer` is the sole writer of that fd.** Frames can never interleave,
   which for a framed protocol is a correctness property and not a nicety.
2. **The `Reader` may block as long as it likes.** It sits in `read_dl` with a
   30 s idle deadline and nothing else is waiting on it.
3. **The `Writer`'s mailbox becomes the backpressure point.** A slow client
   stops draining its socket, its `Writer` blocks in `write_dl`, its mailbox
   fills, and the room's next broadcast to it raises a catchable `WO_T_ACTOR`.
   The room catches that and drops the member. **This is the whole reason the
   mailbox cap is fail-fast** — the room survives its slowest member, and the
   gate's `WO_MAILBOX=8` leg proves the path fires rather than assuming it.

`Room.say` is written around that: it shifts every member, tries the send, and
keeps only the members whose send succeeded — a failed one is sent a close and
dropped. So fan-out and eviction are the same pass.

## Who owns the fd

The `Writer`. It closes it, in every branch: a failed write sets `dead` and
closes; a close message writes the close frame and closes. The `Reader` closes
the fd itself in exactly one case — when its `send_close` to the writer traps,
meaning the writer is unreachable and nobody else will. Without that the fd
would leak on a dead-writer path.

`Writer.dead` guards against a second close, which matters because two
independent paths can decide a connection is finished (the reader seeing EOF,
and the room broadcasting shutdown).

## Shutdown choreography

On `env.stopping()` the accept loop stops and `main` sends one message to the
`Registry`, which fans out to every room; each room shifts its members and
sends each `Writer` a close; each writer writes the close frame and closes the
fd. `main` then spins — it may **not** park, because a park after the stop flag
unwinds — and returns, which is what stops the engine.

Independently, every `Reader` notices `env.stopping()` at its loop head and
runs its tail: leave the room, close the writer.

Both paths exist and that is deliberate: the reader path covers a connection
whose room is already gone, the room path covers a reader parked in a read that
has not come back yet.

**This is where iteration 40 came from.** The room path used to be unreliable:
a `Room` whose shard was idle at `SIGTERM` never adopted the shutdown message,
because an idle worker abandoned its inbox on stop. Clients that still got a
close frame were being saved by the reader path alone — which is why the
failure looked random and why a warmed-up server hid it. The engine now
guarantees that a send issued before the stop flag is delivered, so both paths
work as written. Nothing in this file changed to fix it, and that is the point:
the sample was right and the runtime was not.
