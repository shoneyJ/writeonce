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
    /* arc actors: when this fiber is an actor's delivery fiber, `actor`
     * points at it and `cur_msg` is the message the current receive call
     * borrows — the RUNTIME owns it and drops it after the call returns. */
    struct wo_actor *actor;
    uint64_t cur_msg;
} wo_fiber;

/* An actor: moved-in state, its receive method, a FIFO mailbox, and at
 * most one delivery fiber at a time (one message at a time — the actor
 * guarantee). Actors live until program end (v1: no actor death). */
typedef struct wo_actor {
    uint64_t instance;   /* the moved-in state object (runtime-owned) */
    uint32_t method;     /* receive's method index (self + msg = 2 args) */
    uint64_t *msgs;      /* FIFO ring, growable */
    uint32_t mhead, mlen, mcap;
    wo_fiber *active;    /* the delivery fiber, NULL when idle */
    struct wo_actor *next_all; /* the vm's all-actors list */
} wo_actor;

typedef struct wo_vm {
    const wo_module *mod;
    wo_rt rt;
    wo_fiber f0;    /* fiber 0: main — embedded; spawned fibers are calloc'd */
    wo_fiber *cur;  /* the live fiber — every interpreter access goes here */
    wo_fiber *qhead, *qtail; /* RUNNABLE fibers awaiting the interpreter */
    uint32_t nfibers;        /* live fibers besides main */
    int64_t budget0;         /* reductions per slice (WO_REDUCTIONS, default 4000) */
    int64_t budget;          /* countdown for the live fiber */
    wo_actor *actors;        /* every spawned actor (torn down at destroy) */
} wo_vm;

/* arc: the spawn/send builtins' runtime halves (vm.c owns the scheduler). */
int wo_vm_actor_spawn(wo_vm *vm, uint64_t instance, uint32_t method_idx,
                      uint64_t *out_addr, const char **msg);
int wo_vm_actor_send(wo_vm *vm, uint64_t addr, uint64_t msg_val, const char **msg);

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
