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

typedef struct wo_vm {
    const wo_module *mod;
    wo_rt rt;
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
} wo_vm;

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
