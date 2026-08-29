/* vm.h — the register interpreter (spec §5): Lua-style window-overlap
 * calls, dual-flavor dispatch (computed goto / WO_ISO_C switch), structured
 * trap errors with line lookup, drop-map unwinding on trap. */
#ifndef WO_VM_H
#define WO_VM_H

#include "loader.h"

/* structured trap error (spec §6): one shape forever — the CLI prints it,
 * the future service layer maps it to HTTP */
typedef struct wo_err {
    uint32_t code;    /* WO_T_* */
    uint32_t line;    /* source line at the trapping pc; 0 = unknown */
    char method[64];  /* name of the trapping method */
    char msg[96];     /* human-readable reason */
} wo_err;

typedef struct wo_frame {
    uint32_t method; /* method index */
    uint32_t pc;     /* saved resume pc (next instruction) */
    uint32_t base;   /* register-window base in the value stack */
} wo_frame;

/* One live `try` region (haxe-parity compiler Task 5, WOP_TRY). `depth`
 * is the frame depth that registered it, so a trap raised deeper unwinds
 * every frame above that one and lands here; `pc` is the handler's
 * instruction in that frame's method; `reg` is the window-relative
 * register the error record is built into. */
typedef struct wo_catch {
    uint32_t depth;
    uint32_t pc;
    uint32_t reg;
} wo_catch;

#define WO_MAX_CATCH 64u

/* The concurrency arc (iterations 8+11, stage 1): a FIBER is exactly the
 * interpreter state the vm used to hold inline — the register window, the
 * frame stack, the catch stack, and the caught-error slot. The vm owns
 * the shard-wide pieces (module, runtime, and which fiber is live).
 * Stage 1 Task 1 is a pure extraction: one embedded fiber, `cur` always
 * points at it, behavior byte-identical. */
typedef enum {
    WO_FIB_RUNNABLE = 0,
    WO_FIB_PARKED = 1, /* stage 1 Task 4: waiting on an fd/deadline */
    WO_FIB_DONE = 2,
} wo_fib_state;

typedef struct wo_fiber {
    uint64_t regs[WO_STACK_SLOTS];
    wo_frame frames[WO_MAX_FRAMES];
    uint32_t depth;
    /* the catch stack, innermost last; ncatch = 0 means every trap is
     * the uncaught kind and behaves exactly as it did before Task 5 */
    wo_catch catches[WO_MAX_CATCH];
    uint32_t ncatch;
    /* the error a caught trap landed with, read by WO_B_ERR_FILL while
     * the catch arm builds its record */
    wo_err caught;
    wo_fib_state state;
    struct wo_fiber *next; /* intrusive FIFO link (run queue) */
    /* parking (arc T4): what this fiber waits on while PARKED. park_done
     * says how it resumes — 0 = re-execute the builtin (fd readiness:
     * accept/read/write retry, now ready), 1 = continue PAST it (sleep:
     * the result was preset before parking). park_wr_at carries a partial
     * net.write's progress across the retry. park_ts must outlive the
     * ring submission (TIMEOUT reads it asynchronously). */
    struct wo_fiber *pnext; /* parked-list link */
    int park_fd;            /* -1 = deadline-only (sleep) */
    short park_events;      /* POLLIN / POLLOUT */
    int park_done;
    int64_t park_deadline;  /* wall ms, sleep only */
    uint32_t park_wr_at;
    struct {
        long long sec, nsec;
    } park_ts;
    /* arc actors: when this fiber is an actor's delivery fiber, `actor`
     * points at it and `cur_msg` is the message the current receive call
     * borrows — the RUNTIME owns it and drops it after the call returns. */
    struct wo_actor *actor;
    uint64_t cur_msg;
    /* arc stage 3: the in-flight DB request while parked on the DB actor's
     * reply (a wo_db_req*, opaque here; vm.c owns the protocol) */
    void *dbreq;
    /* iteration 24, caller side of call(): 0 = no call in flight,
     * 1 = parked awaiting the reply, 2 = reply landed (call_reply is the
     * scalar), 3 = the callee was/went dead (the re-executed builtin
     * traps WO_T_ACTOR). Set on the caller's own thread or under its
     * shard's inbox drain — never concurrently with the fiber running. */
    int call_state;
    uint64_t call_reply;
    /* iteration 24, delivery side: the CURRENT message's caller (NULL for
     * a plain send) — where FIBER_DONE ships the receive's return value. */
    struct wo_fiber *msg_caller;
    uint32_t msg_caller_shard;
    /* iteration 35: the in-flight per-CALL deadline (_dl builtins). Set on
     * the builtin's first entry, cleared when it answers — the park/retry
     * protocol re-executes the builtin, and this is how the retry knows
     * the original deadline. */
    int dl_active;
    int64_t dl_at; /* wall ms */
} wo_fiber;

/* arc stage 3: park_fd sentinel — PARKED with NO plane wait; the wake is
 * an inbox envelope (the DB actor's reply). Excluded from the deadline
 * scans, which key on park_fd == -1 exactly. */
#define WO_PARK_INBOX (-2)

/* iteration 24: one mailbox slot. A plain send has caller == NULL; a
 * call carries the parked caller so the delivery's return value can
 * route home as a kind-6 envelope (or a same-shard unpark). */
typedef struct wo_msg {
    uint64_t payload;
    struct wo_fiber *caller; /* NULL = send */
    uint32_t caller_shard;
} wo_msg;

/* An actor: moved-in state, its receive method, a FIFO mailbox, and at
 * most one delivery fiber at a time (one message at a time — the actor
 * guarantee). Death (iteration 24): a receive trapping uncaught marks
 * the actor dead — sends to it drop silently, calls trap, queued
 * callers are error-unparked; the state and mailbox are released. */
/* iteration 24 T4: one death-notice registration. The runtime owns the
 * moved-in notice message until delivery (or drops it if the observer is
 * unreachable). The list lives on the WATCHED actor, owned by its home
 * thread. */
typedef struct wo_monitor {
    struct wo_actor *observer;
    uint64_t msg;
    struct wo_monitor *next;
} wo_monitor;

/* iteration 24 T5: one armed one-shot timer — fires as an ordinary
 * runtime send of the moved message when `at` passes. The list lives on
 * the ARMING fiber's shard and is scanned by the same deadline machinery
 * that serves fd-park deadlines. */
typedef struct wo_timer {
    int64_t at; /* wall ms */
    struct wo_actor *target;
    uint64_t msg;
    struct wo_timer *next;
} wo_timer;

typedef struct wo_actor {
    uint64_t instance;   /* the moved-in state object (runtime-owned) */
    uint32_t method;     /* receive's method index (self + msg = 2 args) */
    uint32_t home;       /* the shard whose thread owns mailbox + delivery */
    int dead;            /* set on the home thread when a receive traps */
    wo_msg *msgs;        /* FIFO ring, growable up to the cap */
    uint32_t mhead, mlen, mcap;
    /* iteration 24: sent-but-not-delivered count, incremented by the
     * SENDER on any shard (the cap check), decremented by the home
     * thread at delivery pop. Accessed ONLY through __atomic builtins
     * (wo_mbox_reserve/release) because senders race; the cap can
     * overshoot by at most the number of in-flight sends — disclosed. */
    uint32_t pending;
    wo_fiber *active;    /* the delivery fiber, NULL when idle */
    wo_monitor *monitors; /* iteration 24 T4: who wants the death notice */
    struct wo_actor *next_all; /* the vm's all-actors list */
} wo_actor;

/* iteration 24: the one mailbox cap (default 1024, WO_MAILBOX overrides
 * at boot — soak tests shrink it to force the fail-fast policy). */
extern uint32_t wo_mailbox_cap;
int wo_mbox_reserve(wo_actor *a);   /* 0 = slot reserved; -1 = full */
void wo_mbox_release(wo_actor *a);  /* delivery pop / failed enqueue */

typedef struct wo_vm {
    const wo_module *mod;
    wo_rt rt;
    /* the arc's stage 2: which shard this vm IS. Shard 0 is the primary
     * (runs the entry, owns the database); workers run wo_vm_serve. */
    uint32_t shard_id;
    int is_primary;
    int wake_efd; /* wakes this shard's I/O wait (inbox arrivals, shutdown) */
    /* the cross-shard inbox (arc T6): OTHER shards push envelopes here
     * under in_mu and write wake_efd; only the OWNING thread pops. A
     * mutex-guarded list, not the spec's lock-free ring — disclosed
     * deviation, rings arrive when 9e measures the mutex. */
    void *in_mu;         /* pthread_mutex_t*, opaque here */
    struct wo_envelope *in_head, *in_tail;
    /* home-routed frees: objects owned by THIS shard's arena, dropped on
     * another shard, come back here to die (header shard_id routes) */
    struct wo_envelope *free_head;
    wo_fiber f0;    /* fiber 0: main — embedded; spawned fibers are calloc'd */
    wo_fiber *cur;  /* the live fiber — every interpreter access goes here */
    wo_fiber *qhead, *qtail; /* RUNNABLE fibers awaiting the interpreter */
    uint32_t nfibers;        /* live fibers besides main */
    int64_t budget0;         /* reductions per slice (WO_REDUCTIONS, default 4000) */
    int64_t budget;          /* countdown for the live fiber */
    wo_actor *actors;        /* every spawned actor (torn down at destroy) */
    /* the I/O plane (arc T4, park.c): io_uring primary, epoll fallback */
    wo_fiber *parked;        /* fibers waiting on the plane */
    uint32_t nparked;
    /* iteration 35: dead fibers are POOLED, never freed mid-run — a stale
     * plane completion (the loser of a poll-vs-deadline race, consumed one
     * wait later) may still read the fiber's `state` word, and reading
     * freed memory is the UAF this prevents. Steady-state pool size = the
     * peak live fiber count; the pool dies with the vm. */
    wo_fiber *fib_pool;
    /* iteration 24 T5: this shard's armed timers (unsorted list — the
     * deadline scan is already linear; a wheel is measured-later work) */
    wo_timer *timers;
    /* iteration 35, uring backend: the shard's ONE deadline tick — a
     * TIMEOUT op with a sentinel user_data armed for the nearest fd-park
     * deadline (fd parks keep exactly one POLL op each; expiry wakes them
     * from the scan and POLL_REMOVE tombstones the poll). */
    int tick_armed;
    int64_t tick_at;
    struct {
        long long sec, nsec;
    } tick_ts;
    int io_kind;             /* 0 = uring, 1 = epoll */
    int efd_armed;           /* wake_efd registered on the plane (uring oneshot) */
    int io_fd;               /* ring fd or epoll fd */
    void *io_sq, *io_cq, *io_sqes; /* uring mmaps (NULL under epoll) */
    size_t io_sq_len, io_cq_len, io_sqes_len;
    /* THIS ring's io_uring_params (opaque bytes; park.c owns the type).
     * Arc stage 3 fix: a single file-static params was rewritten by every
     * shard's lazy init while other shards read ring offsets out of it —
     * submits landed at garbage offsets and parked fibers lost their
     * wakes. Per-vm storage ends the race by construction. */
    unsigned char io_params[256];
} wo_vm;

/* arc: the spawn/send builtins' runtime halves (vm.c owns the scheduler). */
int wo_vm_actor_spawn(wo_vm *vm, uint64_t instance, uint32_t method_idx,
                      uint64_t *out_addr, const char **msg);
int wo_vm_actor_send(wo_vm *vm, uint64_t addr, uint64_t msg_val, const char **msg);
/* iteration 24: send-that-waits. First entry enqueues the message with the
 * caller attached and parks (WO_SYS_PARKED); the re-execution consumes the
 * scalar reply into R[A] (vm.c owns the protocol, builtin.c dispatches). */
int wo_vm_actor_call(wo_vm *vm, uint64_t *R, uint32_t ins, const char **msg);
/* iteration 24 T4: register a death notice — monitor(watched, observer,
 * msg). The msg MOVES to the runtime; an already-dead watched actor
 * delivers it immediately. */
int wo_vm_actor_monitor(wo_vm *vm, uint64_t watched, uint64_t observer,
                        uint64_t msg_val, const char **msg);
/* iteration 24 T5: arm a one-shot timer on THIS shard — time.after(ms,
 * addr, msg). ms <= 0 delivers now. */
int wo_vm_timer_after(wo_vm *vm, int64_t ms, uint64_t addr, uint64_t msg_val,
                      const char **msg);
/* iteration 24 T5: fire every timer at or past `now` (park.c's deadline
 * machinery calls this beside the fd-park sweep). Returns fired count. */
int wo_vm_timers_fire(wo_vm *vm, int64_t now);
/* The nearest armed timer's deadline, 0 = none (park.c's tick/timeout). */
int64_t wo_vm_timers_next(wo_vm *vm);

/* ---- the shard engine (arc stage 2) ------------------------------------
 * One pinned thread per shard, each a full wo_vm (own arena, GC, I/O
 * plane). Shard 0 is the caller's (main's); workers idle on their wake
 * eventfd until fibers arrive (stage 2 T6) or shutdown. Count: WO_SHARDS
 * or all cores (the arc's default). */
typedef struct wo_engine {
    wo_vm *shards; /* [nshards]; index 0 = primary */
    void *threads; /* pthread_t[nshards-1], opaque here (libc-only header) */
    uint32_t nshards;
} wo_engine;

extern wo_engine wo_eng; /* the process's one engine (vm.c) */

/* inbox envelope kinds (arc T6; 3/4 = stage 3's transparent DB RPC) */
typedef struct wo_envelope {
    struct wo_envelope *next;
    int kind; /* 0 = SEND (actor, payload), 1 = SPAWN-ADOPT (actor),
               * 2 = FREE (payload = wo_hdr*),
               * 3 = DB_REQ (payload = wo_db_req*, to shard 0),
               * 4 = DB_RESP (payload = wo_db_req*, back to the requester),
               * 5 = CALL (iteration 24: actor, payload = moved message,
               *     from_shard/from_fiber = the parked caller),
               * 6 = CALL_REPLY (payload = the SCALAR reply, from_fiber =
               *     the caller to unpark; status 0 = ok, WO_T_ACTOR =
               *     the callee was/went dead — the caller traps),
               * 7 = MONITOR (iteration 24 T4: actor = the WATCHED one,
               *     from_fiber REUSED as the observer wo_actor*, payload =
               *     the moved notice — registered on the watched actor's
               *     home thread; already-dead delivers the notice now) */
    struct wo_actor *actor;
    uint64_t payload;
    uint32_t from_shard;
    struct wo_fiber *from_fiber;
    int status;
} wo_envelope;

/* arc stage 3: the requester half of the transparent DB RPC (vm.c). Called
 * by the builtin dispatcher on a worker shard whose rt.db is NULL: first
 * entry marshals + parks (WO_SYS_PARKED), the re-execution after the reply
 * consumes it. */
int wo_db_rpc(wo_vm *vm, uint64_t *R, uint32_t ins, const char **msg);

/* the shard whose thread we are on (thread-local; obj.c stamps and gc.c
 * routes with it). NULL only before main's vm exists. */
wo_vm *wo_tls_vm(void);
void wo_tls_set(wo_vm *vm);

/* Start shards 1..n-1 (0 is the caller's, already init'ed in shards[0]).
 * 0 ok. Stop joins every worker and destroys their vms. */
int wo_engine_start(const wo_module *mod, size_t heap_cap, uint32_t nshards);
int wo_engine_primary_inbox(int wake_efd);
void wo_engine_stop(void);

/* Spawn a fiber that will run method_idx(args) — the runtime half the
 * `spawn` expression lowers onto (stage 1 Task 3); Task 2's tests drive it
 * directly. The fiber is RUNNABLE and queued; it runs when the scheduler
 * reaches it. Returns NULL on allocation failure or bad method/arity. */
wo_fiber *wo_vm_spawn_fiber(wo_vm *vm, uint32_t method_idx, const uint64_t *args,
                            uint32_t argc);

/* heap_cap = arena byte capacity (the CLI's WO_HEAP_MB feeds this) */
int wo_vm_init(wo_vm *vm, const wo_module *mod, size_t heap_cap);
void wo_vm_destroy(wo_vm *vm);

/* Call a method with raw argument words. argc must equal the method's
 * declared arity. 0 = done, *ret filled; -1 = trapped, *err filled and the
 * stack fully unwound (depth 0); 1 = STOPPED — a blocking stdlib call was
 * interrupted with the stop flag set (builtin.h's WO_SYS_STOPPED), the stack
 * is unwound the same way, *ret and *err are untouched, and there is nothing
 * to report: the program was told to stop and did. */
int wo_vm_call(wo_vm *vm, uint32_t method_idx, const uint64_t *args,
               uint32_t argc, uint64_t *ret, wo_err *err);

uint32_t wo_vm_depth(const wo_vm *vm);

#endif /* WO_VM_H */
