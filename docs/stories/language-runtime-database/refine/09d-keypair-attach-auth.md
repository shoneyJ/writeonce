# Iteration 9d — keypair authentication for cross-program attach

> Format: fiberloom `product/story-iteration-template`. Part of
> [Story — one language, one runtime, one database, one binary](../00-story.md).
>
> **Inserted 2026-08-15.** Promotes iteration 9c's identity fork (Info,
> fork 3) to its own iteration: the name + unix-uid lean is the milestone
> bootstrap, and THIS is what replaces it — program identity is a keypair,
> and an attachment is granted to a public key, not to a process that
> happens to share a uid. It follows 9c (there is nothing to authenticate
> until attach exists) and stays same-machine; the same handshake is what
> a future remote channel would reuse, which is the point of doing it
> properly now.
>
> **No spec exists yet.** The forks in *Info* are genuine decisions.

## Goals

- **A program's identity is a keypair.** Each writeonce program owns a
  private key (generated once, stored beside its data, never in the
  manifest) and a public key it can print/export. Identity stops being
  "whoever reached the socket first with the right uid".
- **Grants name public keys.** A's `[share]` registers a client by its
  public key (fingerprint), with rights exactly as 9c defined them; B's
  `[connect.a]` **pins A's public key** beside the IPC string. Both sides
  authenticate: A proves it is A before B sends a byte of intent, B proves
  it is B before A executes a statement.
- **The handshake is mutual challenge–response, replay-proof**: fresh
  nonces each attach, signatures over the nonce + channel binding, no
  secret ever crosses the channel. A failed handshake refuses the
  attachment with a catchable trap on the connecting side and one log line
  naming the offered fingerprint on the listening side.

## Acceptance Criteria

- What to achieve?
    - **Given** A's `[share]` registering B's public-key fingerprint with
      read+write, and B's `[connect.a]` pinning A's public key,
    - **when** B attaches,
    - **then** the mutual handshake completes, the attachment carries B's
      granted rights, and every 9c acceptance behavior (statements, traps,
      refusals) holds unchanged on top of it.
- What to achieve?
    - **Given** a client presenting a keypair A never registered,
    - **when** it attempts the handshake,
    - **then** the attach is refused before any statement is read, the
      client sees the catchable authentication trap, and A logs the offered
      fingerprint (so granting it is a copy-paste, not an investigation).
- What to achieve?
    - **Given** a same-uid process (the 9c bootstrap's whole trust basis)
      presenting no key or the wrong key,
    - **when** it attempts to attach,
    - **then** it is refused — proving the uid check has been superseded,
      not merely supplemented.
- What to achieve?
    - **Given** an impostor listening on A's socket path (or a swapped
      socket file),
    - **when** B attaches and the impostor cannot sign A's challenge
      response with A's private key,
    - **then** B aborts before sending any statement or data, with a trap
      that names the fingerprint mismatch — the pinned-key check working in
      the B→A direction.
- What to achieve?
    - **Given** a recorded handshake transcript from a legitimate attach,
    - **when** it is replayed against A,
    - **then** the attach is refused — the nonce is fresh per handshake and
      a signature over an old nonce proves nothing.
- What to achieve?
    - **Given** A rotates B's registered key (manifest update + restart),
    - **when** B attaches with the old key and then with the new one,
    - **then** the old key is refused and the new one works — rotation is a
      config change, exactly like the grant itself.

## Out Of Scope

- **Transport encryption.** Same-machine unix sockets; the kernel is the
  wire. Session encryption (and the key exchange it needs) arrives with a
  remote channel, if one ever ships — this iteration's handshake is
  designed not to preclude it, nothing more.
- **Certificate hierarchies, expiry, revocation lists.** A grant is a
  public key in a manifest; revocation is deleting the line and
  restarting. CA machinery has no workload here.
- **Key escrow / multi-key identities / agent forwarding.** One program,
  one keypair.
- **Protecting the private key from a root attacker or from the program's
  own uid.** File permissions (0600) are the boundary this iteration
  claims; anything stronger (TPM, keyring) is explicitly not promised.

## Info

Forks the spec must settle:

**1. Where does the crypto come from?** The runtime is libc-only by
doctrine, and hand-rolling signature crypto is the one wheel nobody gets to
reinvent. The realistic options: **vendor a compact, audited Ed25519**
implementation (TweetNaCl-lineage, a few files, no allocation, no OS
dependencies) into `database/src/` or a new `vendor/`; or take libsodium as
the first external dependency and break the doctrine openly. Leaning:
vendored compact Ed25519, recorded as the single sanctioned vendored
component with its provenance pinned in the tree — the doctrine's spirit is
"no dependency sprawl", not "write your own constant-time field
arithmetic".

**2. Key generation and storage.** Options: a `woc keygen` subcommand
(keys are a toolchain concern), or first-boot generation by the runtime
into the data directory (keys are a runtime concern, zero setup). Leaning:
first-boot generation into `WO_DATA` (0600, alongside the WAL — a program
with a persistent database already has the directory), plus a way to print
the public fingerprint (`program --identity` or a stdlib call) so the
operator can paste it into A's `[share]`. A program without `WO_DATA` has
no identity and cannot attach anywhere — which is coherent: attach is a
database feature.

**3. What exactly gets signed.** A bare nonce signature is vulnerable to
cross-protocol reuse; the lean is signing a transcript hash: protocol tag,
both fingerprints, both nonces, and the channel identity — so a signature
from this handshake means nothing in any other context. The spec should
write the exact byte layout down (the WAL encoding conventions apply: the
format is normative, little-endian, versioned by the protocol tag).

**4. Does the uid check survive at all?** Options: keys only (one
mechanism, one story), or keys AND peer-cred as defense in depth. Leaning:
keys only — two mechanisms invite "it worked because of the other one"
confusion in exactly the code that must never be confusing; `SO_PEERCRED`
remains a log-line enrichment (who was that fingerprint), never an
authorization input.

## Proposed Solution

- **Brainstorm the spec** settling the four forks, then fold the plan into
  9c's implementation plan as its authentication tasks — one plan, because
  9c without 9d ships a placeholder identity and 9d without 9c has nothing
  to authenticate. The 9c milestone may still land first with the uid
  bootstrap, flagged loudly as pre-9d.
- **Acceptance extends the 9c workload**: the employee-A /
  employee-list-B pair (`docs/examples/employee-list`, pre-authored
  2026-08-15) carries the key exchange in both manifests — A's
  `[[share.clients]]` names B's fingerprint, B's `[connect.employee]` pins
  A's; the acceptance script adds the wrong-key, no-key,
  same-uid-wrong-key, replay, impostor-socket, and rotation checks above,
  each asserting the exact trap/refusal.
- Expected shape: handshake module beside the channel code (both ends),
  `[share]`/`[connect]` manifest keys for fingerprints, first-boot keygen
  in the runtime's data-directory setup, vendored signature primitive with
  its own unit suite (known-answer tests from the algorithm's reference
  vectors).
