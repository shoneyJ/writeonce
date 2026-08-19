#include "vm.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "borrow.h"
#include "builtin.h"
#include "cont.h"
#include "gc.h"

uint32_t wo_vm_depth(const wo_vm *vm) { return vm->depth; }

int wo_vm_init(wo_vm *vm, const wo_module *mod, size_t heap_cap) {
    memset(vm, 0, sizeof(*vm));
    vm->mod = mod;
    return wo_rt_init(&vm->rt, heap_cap, mod->classes, mod->class_cnt);
}

void wo_vm_destroy(wo_vm *vm) { wo_rt_destroy(&vm->rt); }

/* The drop-table entry governing instruction [pc]: the last one recorded
 * at or before it. NULL = nothing live there. */
static const wo_dropent *vm_dropent(const wo_methodrec *me, uint32_t pc) {
    const wo_dropent *ent = NULL;
    for (uint32_t i = 0; i < me->drop_cnt && me->drops[i].pc <= pc; i++) ent = &me->drops[i];
    return ent;
}

/* Release what frame [d-1] owns at [pc] but no longer owns at [keep_pc] —
 * the values the abandoned region of that frame created. [keep_pc] =
 * UINT32_MAX means "keep nothing", which is the dying-frame case every
 * uncaught trap uses. A borrow held by a dying register does not block
 * its drop — the borrower IS the dying region. */
static void vm_release_frame(wo_vm *vm, uint32_t d, uint32_t pc, uint32_t keep_pc) {
    const wo_frame *f = &vm->frames[d - 1];
    const wo_methodrec *me = &vm->mod->methods[f->method];
    const wo_dropent *ent = vm_dropent(me, pc);
    if (!ent) return; /* no entry: nothing live in this frame */
    uint64_t keep_owned = 0, keep_gc = 0;
    if (keep_pc != UINT32_MAX) {
        const wo_dropent *k = vm_dropent(me, keep_pc);
        if (k) {
            keep_owned = k->owned;
            keep_gc = k->gc;
        }
    }
    uint64_t *R = vm->regs + f->base;
    for (uint32_t r = 0; r < me->reg_cnt; r++) {
        uint64_t bit = 1ull << r;
        if ((ent->owned & bit) && !(keep_owned & bit) && R[r]) {
            wo_drop_obj(&vm->rt, (wo_hdr *)(uintptr_t)R[r]);
            R[r] = 0;
        }
        if ((ent->gc & bit) && !(keep_gc & bit) && R[r]) {
            /* a traced reference dying with its frame: tracing owns the
             * lifetime, and a mid-cycle root snapshot already shaded it —
             * the register just goes away */
            R[r] = 0;
        }
    }
}

/* ---- collector integration (iteration 7b) -------------------------------
 * The root snapshot: shade every live frame's gc-masked registers (traced
 * objects) and walk its owned-masked registers' interiors (owned values
 * that may hold gcrefs). The governing drop entry per frame follows
 * vm_unwind's convention — the current instruction for the innermost
 * frame (the caller synced f->pc first), the CALL for outer ones. Runs
 * once, atomically, when a cycle begins: bounded by the stack, not the
 * heap. */
static void vm_gc_roots(wo_vm *vm) {
    for (uint32_t d = vm->depth; d > 0; d--) {
        const wo_frame *f = &vm->frames[d - 1];
        const wo_methodrec *me = &vm->mod->methods[f->method];
        uint32_t gpc = (d == vm->depth) ? f->pc : f->pc - 1;
        const wo_dropent *ent = vm_dropent(me, gpc);
        if (!ent) continue;
        const uint64_t *R = vm->regs + f->base;
        for (uint32_t r = 0; r < me->reg_cnt; r++) {
            uint64_t bit = 1ull << r;
            if (((ent->gc | ent->owned) & bit) && R[r])
                wo_gc_scan_root(&vm->rt, (wo_hdr *)(uintptr_t)R[r]);
        }
    }
}

/* One safepoint: start a cycle when the trigger says so (snapshot the
 * roots before the mutator resumes), then run one budgeted slice while a
 * cycle is live. The caller synced the innermost frame's pc first. */
static void vm_gc_safepoint(wo_vm *vm) {
    if (wo_gc_want_start(&vm->rt)) {
        wo_gc_begin(&vm->rt);
        vm_gc_roots(vm);
    }
    if (vm->rt.gc_phase != WO_GC_IDLE) wo_gc_slice(&vm->rt, vm->rt.gc_budget);
}

/* Trap unwinding — the spec's "traps never leak" promise (spec §6). Walk
 * frames innermost to outermost down to (not including) [stop_depth];
 * in each, the governing instruction is the trap pc for the innermost
 * frame and the instruction before the saved resume pc — i.e. the CALL —
 * for every outer frame. Window overlap is safe: a slot dropped by the
 * callee frame is nulled, so an outer mask covering the same physical
 * slot sees 0 and skips. stop_depth is 0 for an uncaught trap (the whole
 * stack dies) and the catching frame's depth for a caught one. */
static void vm_unwind(wo_vm *vm, uint32_t stop_depth) {
    for (uint32_t d = vm->depth; d > stop_depth; d--) {
        const wo_frame *f = &vm->frames[d - 1];
        vm_release_frame(vm, d, (d == vm->depth) ? f->pc : f->pc - 1, UINT32_MAX);
    }
    vm->depth = stop_depth;
}

/* Residual runtime checks the loader cannot do statically (registers are
 * untyped): non-null receiver, an actual class object (not a native
 * sentinel), field index inside the class. NULL return = trap BOUNDS with
 * *why naming the reason. */
static wo_hdr *recv_check(wo_vm *vm, uint64_t v, uint32_t fidx,
                          const char **why) {
    if (!v) {
        *why = "null receiver";
        return NULL;
    }
    wo_hdr *o = (wo_hdr *)(uintptr_t)v;
    if (o->class_id >= vm->mod->class_cnt) {
        *why = "native object has no fields";
        return NULL;
    }
    if (fidx >= vm->mod->classes[o->class_id].field_cnt) {
        *why = "field index out of range";
        return NULL;
    }
    return o;
}

static wo_str *str_check(uint64_t v, const char **why) {
    if (!v) {
        *why = "null text";
        return NULL;
    }
    wo_str *s = (wo_str *)(uintptr_t)v;
    if (s->h.class_id != WO_CLS_STR) {
        *why = "not a text value";
        return NULL;
    }
    return s;
}

/* Fills [out] with the trap's structured error (spec §6): the code, the
 * source line of the trapping pc, the trapping method's name, and the
 * message. One shape forever — the CLI prints it, and the catch arm of a
 * `try` binds exactly the same four fields. */
static void vm_fill_err(wo_vm *vm, wo_err *out, uint32_t tcode, const char *fmt, va_list ap) {
    const wo_module *mod = vm->mod;
    const wo_frame *f = &vm->frames[vm->depth - 1];
    const wo_methodrec *me = &mod->methods[f->method];
    out->code = tcode;
    out->line = 0; /* last line-table entry with pc <= trapping pc */
    for (uint32_t i = 0; i < me->line_cnt && me->lines[i].pc <= f->pc; i++)
        out->line = me->lines[i].line;
    const wo_str *nm = mod->consts[me->name].s;
    int nlen = nm->len < 63 ? (int)nm->len : 63;
    snprintf(out->method, sizeof(out->method), "%.*s", nlen, nm->data);
    vsnprintf(out->msg, sizeof(out->msg), fmt, ap);
}

/* 0 = the trap was caught: the stack is unwound down to the catching
 * frame, that frame's pc now points at the handler, and the caller must
 * reload and keep interpreting. -1 = uncaught: *err is filled and the
 * stack is fully unwound (depth 0), exactly as before Task 5. */
static int vm_trap(wo_vm *vm, wo_err *err, uint32_t tcode, const char *fmt,
                   ...) {
    /* The record the catch arm reads is always filled, even when the
     * caller passed no err: it is the value `catch (e)` binds. */
    va_list ap;
    va_start(ap, fmt);
    vm_fill_err(vm, &vm->caught, tcode, fmt, ap);
    va_end(ap);
    if (vm->ncatch) {
        const wo_catch *c = &vm->catches[vm->ncatch - 1];
        uint32_t cdepth = c->depth;
        uint32_t hpc = c->pc;
        /* Which instruction governs the catching frame's own live set has
         * to be decided before unwinding moves the depth: the trapping
         * instruction when the trap was raised in this very frame, the
         * CALL (pc - 1, the saved pc is the resume point) when it came
         * from deeper. */
        int trapped_here = (cdepth == vm->depth);
        vm->ncatch--;
        /* frames above the catching one die whole */
        vm_unwind(vm, cdepth);
        /* in the catching frame only the try region's own values die: the
         * handler's drop entry names what survives into the catch arm */
        wo_frame *cf = &vm->frames[cdepth - 1];
        vm_release_frame(vm, cdepth, trapped_here ? cf->pc : cf->pc - 1, hpc);
        cf->pc = hpc;
        return 0;
    }
    if (err) *err = vm->caught;
    vm_unwind(vm, 0);
    return -1;
}

static int vm_run(wo_vm *vm, uint64_t *ret, wo_err *err) {
    const wo_module *mod = vm->mod;
    const wo_methodrec *me;
    const uint32_t *code;
    uint32_t pc;
    uint64_t *R;
    uint32_t ins = 0;

#define RELOAD()                                                  \
    do {                                                          \
        me = &mod->methods[vm->frames[vm->depth - 1].method];     \
        code = me->code;                                          \
        pc = vm->frames[vm->depth - 1].pc;                        \
        R = vm->regs + vm->frames[vm->depth - 1].base;            \
    } while (0)

/* pc is post-incremented at dispatch: the trapping instruction is pc-1.
 * A caught trap (vm_trap == 0) has already unwound to the handler's frame
 * and pointed it at the handler, so the interpreter just reloads and
 * keeps going — the same macro serves both surfaces. */
#define TRAPF(tcode, ...)                                \
    do {                                                 \
        vm->frames[vm->depth - 1].pc = pc - 1;           \
        if (vm_trap(vm, err, tcode, __VA_ARGS__) == 0) { \
            RELOAD();                                    \
            NEXT();                                      \
        }                                                \
        return -1;                                       \
    } while (0)

/* Collector safepoint (iteration 7b): placed at allocations, calls, and
 * loop back-edges — the pcs that already carry drop-table entries, so the
 * root snapshot's masks are exact. Costs one predictable branch when the
 * collector is idle and the trigger is cold. */
#define GC_SAFEPOINT()                                                       \
    do {                                                                     \
        if (vm->rt.gc_phase != WO_GC_IDLE || wo_gc_want_start(&vm->rt)) {    \
            vm->frames[vm->depth - 1].pc = pc - 1;                           \
            vm_gc_safepoint(vm);                                             \
        }                                                                    \
    } while (0)

    RELOAD();

    /* dual-flavor dispatch, one shared case-body text (spec §5): computed
     * goto under GNU C, plain switch under -DWO_ISO_C — the ISO flavor has
     * its own make target so the fallback can never rot */
#ifndef WO_ISO_C
    static const void *JT[WOP_MAX + 1] = {
        [WOP_NOP] = &&L_NOP,           [WOP_LOADK] = &&L_LOADK,
        [WOP_MOVE] = &&L_MOVE,         [WOP_ADD] = &&L_ADD,
        [WOP_SUB] = &&L_SUB,           [WOP_MUL] = &&L_MUL,
        [WOP_DIV] = &&L_DIV,           [WOP_NEG] = &&L_NEG,
        [WOP_CONCAT] = &&L_CONCAT,     [WOP_EQ] = &&L_EQ,
        [WOP_LT] = &&L_LT,             [WOP_LE] = &&L_LE,
        [WOP_EQS] = &&L_EQS,           [WOP_JMP] = &&L_JMP,
        [WOP_JZ] = &&L_JZ,             [WOP_CALL] = &&L_CALL,
        [WOP_ICALL] = &&L_ICALL,       [WOP_RET] = &&L_RET,
        [WOP_RET0] = &&L_RET0,         [WOP_NEW] = &&L_NEW,
        [WOP_GETF] = &&L_GETF,         [WOP_SETF] = &&L_SETF,
        [WOP_DROP] = &&L_DROP,         [WOP_BORROW_S] = &&L_BORROW_S,
        [WOP_BORROW_X] = &&L_BORROW_X, [WOP_RELEASE_S] = &&L_RELEASE_S,
        [WOP_RELEASE_X] = &&L_RELEASE_X, [WOP_RC_INC] = &&L_RC_INC,
        [WOP_RC_DEC] = &&L_RC_DEC,     [WOP_BUILTIN] = &&L_BUILTIN,
        [WOP_DB_STUB] = &&L_DB_STUB,   [WOP_TRAP] = &&L_TRAP,
        [WOP_TRY] = &&L_TRY,           [WOP_ENDTRY] = &&L_ENDTRY,
    };
#define CASE(name) L_##name
#define NEXT()                        \
    do {                              \
        ins = code[pc++];             \
        goto *JT[wo_ins_op(ins)];     \
    } while (0)
    NEXT();
#else
#define CASE(name) case WOP_##name
#define NEXT() goto dispatch
dispatch:
    ins = code[pc++];
    switch (wo_ins_op(ins)) {
#endif

    CASE(NOP) : NEXT();

    CASE(LOADK) : {
        const wo_const *k = &mod->consts[wo_ins_bx(ins)];
        R[wo_ins_a(ins)] = k->tag == WOB_K_INT ? (uint64_t)k->i
                                               : (uint64_t)(uintptr_t)k->s;
        NEXT();
    }

    CASE(MOVE) : {
        /* for owned values this IS the move: the compiler guarantees the
         * source register is dead afterwards */
        R[wo_ins_a(ins)] = R[wo_ins_b(ins)];
        NEXT();
    }

    /* i64 arithmetic: two's-complement wrapping via unsigned math */
    CASE(ADD) : {
        R[wo_ins_a(ins)] = R[wo_ins_b(ins)] + R[wo_ins_c(ins)];
        NEXT();
    }
    CASE(SUB) : {
        R[wo_ins_a(ins)] = R[wo_ins_b(ins)] - R[wo_ins_c(ins)];
        NEXT();
    }
    CASE(MUL) : {
        R[wo_ins_a(ins)] = R[wo_ins_b(ins)] * R[wo_ins_c(ins)];
        NEXT();
    }
    CASE(DIV) : {
        int64_t x = (int64_t)R[wo_ins_b(ins)], y = (int64_t)R[wo_ins_c(ins)];
        if (y == 0) TRAPF(WO_T_DIV0, "division by zero");
        if (x == INT64_MIN && y == -1)
            TRAPF(WO_T_DIV0, "INT64_MIN / -1 overflows");
        R[wo_ins_a(ins)] = (uint64_t)(x / y);
        NEXT();
    }
    CASE(NEG) : {
        R[wo_ins_a(ins)] = 0u - R[wo_ins_b(ins)];
        NEXT();
    }

    CASE(EQ) : {
        R[wo_ins_a(ins)] = R[wo_ins_b(ins)] == R[wo_ins_c(ins)] ? 1 : 0;
        NEXT();
    }
    CASE(LT) : {
        R[wo_ins_a(ins)] =
            (int64_t)R[wo_ins_b(ins)] < (int64_t)R[wo_ins_c(ins)] ? 1 : 0;
        NEXT();
    }
    CASE(LE) : {
        R[wo_ins_a(ins)] =
            (int64_t)R[wo_ins_b(ins)] <= (int64_t)R[wo_ins_c(ins)] ? 1 : 0;
        NEXT();
    }

    CASE(JMP) : {
        if (wo_ins_sbx(ins) < 0) GC_SAFEPOINT(); /* loop back-edge */
        pc = (uint32_t)((int64_t)pc + wo_ins_sbx(ins));
        NEXT();
    }
    CASE(JZ) : {
        if (R[wo_ins_a(ins)] == 0)
            pc = (uint32_t)((int64_t)pc + wo_ins_sbx(ins));
        NEXT();
    }

    CASE(CALL) : {
        GC_SAFEPOINT();
        /* Lua-style window overlap: callee r0 = caller slot A; args sit at
         * A..A+argc-1; the return value lands back in slot A */
        const wo_methodrec *callee = &mod->methods[wo_ins_bx(ins)];
        uint32_t nbase = vm->frames[vm->depth - 1].base + wo_ins_a(ins);
        if (vm->depth >= WO_MAX_FRAMES)
            TRAPF(WO_T_STACK, "frame stack overflow (%u frames)",
                  (unsigned)WO_MAX_FRAMES);
        if (nbase + callee->reg_cnt > WO_STACK_SLOTS)
            TRAPF(WO_T_STACK, "value stack overflow");
        vm->frames[vm->depth - 1].pc = pc;
        vm->frames[vm->depth].method = wo_ins_bx(ins);
        vm->frames[vm->depth].pc = 0;
        vm->frames[vm->depth].base = nbase;
        vm->depth++;
        /* zero non-argument registers: drop masks must never see stale bits */
        memset(vm->regs + nbase + callee->arg_cnt, 0,
               (size_t)(callee->reg_cnt - callee->arg_cnt) * 8u);
        RELOAD();
        NEXT();
    }

/* A frame leaving takes its still-open try regions with it: a `return`
 * out of a try region never runs its ENDTRY, and a handler pc in a frame
 * that no longer exists would land the next trap on a dead window. */
#define DROP_CATCHES()                                                  \
    while (vm->ncatch && vm->catches[vm->ncatch - 1].depth > vm->depth) \
        vm->ncatch--

    CASE(RET) : {
        uint64_t rv = R[wo_ins_a(ins)];
        vm->regs[vm->frames[vm->depth - 1].base] = rv;
        vm->depth--;
        DROP_CATCHES();
        if (vm->depth == 0) {
            *ret = rv;
            return 0;
        }
        RELOAD();
        NEXT();
    }
    CASE(RET0) : {
        vm->regs[vm->frames[vm->depth - 1].base] = 0;
        vm->depth--;
        DROP_CATCHES();
        if (vm->depth == 0) {
            *ret = 0;
            return 0;
        }
        RELOAD();
        NEXT();
    }

    CASE(NEW) : {
        GC_SAFEPOINT(); /* allocation is the trigger's natural home */
        wo_hdr *o = wo_obj_new(&vm->rt, wo_ins_bx(ins));
        if (!o) TRAPF(WO_T_OOM, "out of memory");
        R[wo_ins_a(ins)] = (uint64_t)(uintptr_t)o;
        NEXT();
    }

    CASE(GETF) : {
        const char *why;
        wo_hdr *o = recv_check(vm, R[wo_ins_b(ins)], wo_ins_c(ins), &why);
        if (!o) TRAPF(WO_T_BOUNDS, "%s", why);
        R[wo_ins_a(ins)] = wo_fields(o)[wo_ins_c(ins)];
        NEXT();
    }

    CASE(SETF) : {
        /* overwriting a non-scalar field does NOT auto-drop the old value:
         * the compiler emits the drop (format doc).
         *
         * A TEXT field is COPIED into (2026-08-14), the same rule push/set
         * follow: the field's kind makes the object the owner of that string,
         * so storing a pointer the caller still owns would give it two owners.
         * It is also what lets `self.name = name` — a borrowed Text parameter
         * stored in a field, the most ordinary line there is — stay legal
         * without demanding `take`. A freshly built Text handed to a field is
         * still the caller's, and the compiler drops it at the store site. */
        const char *why;
        wo_hdr *o = recv_check(vm, R[wo_ins_a(ins)], wo_ins_b(ins), &why);
        if (!o) TRAPF(WO_T_BOUNDS, "%s", why);
        uint64_t v = R[wo_ins_c(ins)];
        /* Yuasa deletion barrier (iteration 7b): overwriting a gcref slot
         * while marking deletes an edge the snapshot may depend on — shade
         * the OLD target before the store. Inactive outside marking; owned
         * fields, scalars and text pay nothing. */
        if (vm->rt.gc_phase == WO_GC_MARK &&
            vm->mod->classes[o->class_id].kinds[wo_ins_b(ins)] == WO_K_GCREF) {
            uint64_t old = wo_fields(o)[wo_ins_b(ins)];
            if (old) wo_gc_shade(&vm->rt, (wo_hdr *)(uintptr_t)old);
        }
        if (v && vm->mod->classes[o->class_id].kinds[wo_ins_b(ins)] == WO_K_TEXT) {
            const wo_str *src = (const wo_str *)(uintptr_t)v;
            if (src->h.class_id != WO_CLS_STR) TRAPF(WO_T_BOUNDS, "not a text value");
            wo_str *cp = wo_str_new(&vm->rt, src->data, src->len);
            if (!cp) TRAPF(WO_T_OOM, "out of memory");
            v = (uint64_t)(uintptr_t)cp;
        }
        wo_fields(o)[wo_ins_b(ins)] = v;
        NEXT();
    }

    CASE(DROP) : {
        uint64_t v = R[wo_ins_a(ins)];
        if (v) wo_drop_obj(&vm->rt, (wo_hdr *)(uintptr_t)v);
        R[wo_ins_a(ins)] = 0; /* unwinding must never double-free */
        NEXT();
    }

    CASE(BORROW_S) : {
        uint64_t v = R[wo_ins_a(ins)];
        if (!v) TRAPF(WO_T_BOUNDS, "null receiver");
        if (wo_borrow_shared((wo_hdr *)(uintptr_t)v) != 0)
            TRAPF(WO_T_BORROW, "shared borrow of exclusively borrowed value");
        NEXT();
    }
    CASE(BORROW_X) : {
        uint64_t v = R[wo_ins_a(ins)];
        if (!v) TRAPF(WO_T_BOUNDS, "null receiver");
        if (wo_borrow_excl((wo_hdr *)(uintptr_t)v) != 0)
            TRAPF(WO_T_BORROW, "exclusive borrow of already borrowed value");
        NEXT();
    }
    CASE(RELEASE_S) : {
        /* releases are unconditional: the compiler emits them balanced */
        wo_release_shared((wo_hdr *)(uintptr_t)R[wo_ins_a(ins)]);
        NEXT();
    }
    CASE(RELEASE_X) : {
        wo_release_excl((wo_hdr *)(uintptr_t)R[wo_ins_a(ins)]);
        NEXT();
    }

    /* iteration 7b: reference counting is retired — tracing owns traced
     * lifetimes, so alias bookkeeping means nothing. The opcodes stay
     * accepted as no-ops until the emitter stops producing them and the
     * format reserves 27–28 (the .wob version bump); a no-op is also what
     * deletes the old RC_DEC-on-nil trap that broke `?Node` field stores. */
    CASE(RC_INC) : NEXT();
    CASE(RC_DEC) : NEXT();

    CASE(CONCAT) : {
        const char *why;
        wo_str *x = str_check(R[wo_ins_b(ins)], &why);
        if (!x) TRAPF(WO_T_BOUNDS, "%s", why);
        wo_str *y = str_check(R[wo_ins_c(ins)], &why);
        if (!y) TRAPF(WO_T_BOUNDS, "%s", why);
        wo_str *z = wo_str_concat(&vm->rt, x, y);
        if (!z) TRAPF(WO_T_OOM, "out of memory");
        R[wo_ins_a(ins)] = (uint64_t)(uintptr_t)z; /* new owned text */
        NEXT();
    }
    CASE(EQS) : {
        /* Text content equality — and the one comparison that must accept a
         * nil operand: two `?Text` values compare with this opcode, and the
         * language's answer is "both absent is equal, one absent is not"
         * (trapping instead would make `a != b` on optionals unusable). Only a
         * NON-nil value still has to actually be a Text. */
        uint64_t bv = R[wo_ins_b(ins)], cv = R[wo_ins_c(ins)];
        if (!bv || !cv) {
            R[wo_ins_a(ins)] = bv == cv ? 1 : 0;
            NEXT();
        }
        const char *why;
        wo_str *x = str_check(bv, &why);
        if (!x) TRAPF(WO_T_BOUNDS, "%s", why);
        wo_str *y = str_check(cv, &why);
        if (!y) TRAPF(WO_T_BOUNDS, "%s", why);
        R[wo_ins_a(ins)] = wo_str_eq(x, y) ? 1 : 0;
        NEXT();
    }

    CASE(BUILTIN) : {
        const char *bmsg = "builtin failed";
        int brc = wo_builtin(vm, R, ins, &bmsg);
        /* A stop is not a trap: no error record, no catch handler gets a
         * look (`try` must not be able to swallow SIGTERM), and no message.
         * The stack is unwound exactly as an uncaught trap unwinds it, so
         * every live value is still released on the way out; the CLI turns
         * this into the same exit status a clean `return 0` gives. */
        if (brc == WO_SYS_STOPPED) {
            vm->frames[vm->depth - 1].pc = pc - 1;
            vm->ncatch = 0;
            vm_unwind(vm, 0);
            return 1;
        }
        if (brc) TRAPF((uint32_t)brc, "%s", bmsg);
        NEXT();
    }

    CASE(ICALL) : {
        /* structural-interface dispatch (spec §2): binary search the sorted
         * (class, slot, method) triples by the RECEIVER's class. The
         * compiler's type checker makes a miss unreachable in compiled
         * code; the VM keeps the trap as defense (spec §6). */
        uint64_t v = R[wo_ins_a(ins)];
        if (!v) TRAPF(WO_T_BOUNDS, "null receiver");
        wo_hdr *o = (wo_hdr *)(uintptr_t)v;
        if (o->class_id >= mod->class_cnt)
            TRAPF(WO_T_BOUNDS, "interface call on a native value");
        uint32_t slot = wo_ins_bx(ins);
        const wo_vtabent *hit = NULL;
        for (uint32_t lo = 0, hi = mod->vtab_cnt; lo < hi;) {
            uint32_t mid = lo + (hi - lo) / 2;
            const wo_vtabent *e = &mod->vtabs[mid];
            if (e->class_id < o->class_id ||
                (e->class_id == o->class_id && e->slot < slot)) {
                lo = mid + 1;
            } else if (e->class_id == o->class_id && e->slot == slot) {
                hit = e;
                break;
            } else {
                hi = mid;
            }
        }
        if (!hit) TRAPF(WO_T_BOUNDS, "no vtable entry for receiver class");
        /* exactly the CALL sequence at the same window base: the receiver
         * already sits in slot A = callee's self */
        const wo_methodrec *callee = &mod->methods[hit->method];
        if ((uint32_t)wo_ins_a(ins) + callee->arg_cnt > me->reg_cnt)
            TRAPF(WO_T_STACK, "call window exceeds frame"); /* runtime: callee
                                                unknown to the loader here */
        uint32_t nbase = vm->frames[vm->depth - 1].base + wo_ins_a(ins);
        if (vm->depth >= WO_MAX_FRAMES)
            TRAPF(WO_T_STACK, "frame stack overflow (%u frames)",
                  (unsigned)WO_MAX_FRAMES);
        if (nbase + callee->reg_cnt > WO_STACK_SLOTS)
            TRAPF(WO_T_STACK, "value stack overflow");
        vm->frames[vm->depth - 1].pc = pc;
        vm->frames[vm->depth].method = hit->method;
        vm->frames[vm->depth].pc = 0;
        vm->frames[vm->depth].base = nbase;
        vm->depth++;
        memset(vm->regs + nbase + callee->arg_cnt, 0,
               (size_t)(callee->reg_cnt - callee->arg_cnt) * 8u);
        RELOAD();
        NEXT();
    }

    CASE(DB_STUB) : { TRAPF(WO_T_DB, "engine not linked"); }

    CASE(TRAP) : { TRAPF(wo_ins_bx(ins), "explicit trap"); }

    CASE(TRY) : {
        if (vm->ncatch >= WO_MAX_CATCH)
            TRAPF(WO_T_STACK, "catch stack overflow (%u regions)",
                  (unsigned)WO_MAX_CATCH);
        vm->catches[vm->ncatch].depth = vm->depth;
        vm->catches[vm->ncatch].pc = (uint32_t)((int64_t)pc + wo_ins_sbx(ins));
        vm->catches[vm->ncatch].reg = wo_ins_a(ins);
        vm->ncatch++;
        NEXT();
    }
    CASE(ENDTRY) : {
        /* the try region completed without trapping. Defensive on an
         * unpaired ENDTRY (a miscompile the loader cannot see): pop
         * nothing rather than corrupt the stack. */
        if (vm->ncatch) vm->ncatch--;
        NEXT();
    }

#ifdef WO_ISO_C
    default:
        TRAPF(WO_T_EXPLICIT, "unknown opcode"); /* unreachable: loader */
    }
#endif

#undef CASE
#undef NEXT
#undef RELOAD
#undef TRAPF
#undef GC_SAFEPOINT
#undef DROP_CATCHES
}

int wo_vm_call(wo_vm *vm, uint32_t method_idx, const uint64_t *args,
               uint32_t argc, uint64_t *ret, wo_err *err) {
    if (err) memset(err, 0, sizeof(*err));
    if (method_idx >= vm->mod->method_cnt) {
        if (err) {
            err->code = WO_T_EXPLICIT;
            snprintf(err->msg, sizeof(err->msg), "no such method");
        }
        return -1;
    }
    const wo_methodrec *me = &vm->mod->methods[method_idx];
    if (argc != me->arg_cnt) {
        if (err) {
            err->code = WO_T_EXPLICIT;
            snprintf(err->msg, sizeof(err->msg), "bad call arity");
        }
        return -1;
    }
    vm->depth = 1;
    vm->ncatch = 0; /* catch regions never survive a call boundary */
    vm->frames[0].method = method_idx;
    vm->frames[0].pc = 0;
    vm->frames[0].base = 0;
    if (argc) memcpy(vm->regs, args, (size_t)argc * 8u);
    memset(vm->regs + argc, 0, (size_t)(me->reg_cnt - argc) * 8u);
    return vm_run(vm, ret, err);
}
