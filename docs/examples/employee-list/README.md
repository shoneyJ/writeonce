# employee-list — program B: attach, authenticate, read

> **Status: target workload — does not compile on today's toolchain.**
> Written ahead of iterations
> [9c (cross-program tables)](../../stories/language-runtime-database/refine/09c-cross-program-tables.md)
> and [9d (keypair attach auth)](../../stories/language-runtime-database/refine/09d-keypair-attach-auth.md),
> the way every acceptance sample here precedes its features. It also leans
> on 9/9b (the [employee sample](../employee/) it attaches to must run
> first).

Two programs, one database, one writer:

```
employee (A)                          employee-list (B)
  owns WO_DATA + WAL                    no database of its own
  [share] listen = unix:...sock         [connect.employee] ipc = unix:...sock
  [[share.clients]]                     public_key = <A's fingerprint, pinned>
    public_key = <B's fingerprint>      project    = ../employee  (shapes)
    rights     = "read"
        ▲                                     │
        └── every statement executes here ◄───┘   (typed, over the wire)
```

The manifests are the design: **A grants, B pins.** A's `[share]` names B's
public-key fingerprint with rights (`read` here); B's `[connect.employee]`
names A's IPC string AND A's fingerprint, so neither side talks to an
impostor. Fingerprints are printed by each program's `--identity` after
first boot (keys are generated into `WO_DATA`, never written into a toml)
and pasted — the `PASTE-…-HERE` placeholders mark exactly where. The
connect-section name is the code's namespace: `[connect.employee]` is why
the source says `employee.Employee`.

| Mode | What it proves |
| --- | --- |
| `employee-list list` | typed reads over the wire, `e.dept.name` ref navigation executing inside A |
| `employee-list report` | **byte-identical output to A's own `report`** — attach + GroupBy compose, the wire changes nothing |
| `employee-list staff <dept>` | unique-name index probe + `staff` backlink scan, both in A |
| `employee-list probe-write` | the rights matrix: registered read-only, so the insert traps with access-denied (caught, `DENIED …`, exit 4) and A's row count is unchanged |

The 9d acceptance drives the rest from the outside: wrong key, no key,
same-uid-wrong-key, impostor socket, handshake replay, key rotation — see
the iteration's criteria; this sample is the workload they run against.
