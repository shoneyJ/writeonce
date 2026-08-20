/* builtin.h — runtime services bytecode can't express (spec §3/§5):
 * now/print/print_int/words plus the container bridge to cont.c. One
 * dispatcher: takes the VM, the current frame's registers, and the
 * instruction; 0 = ok, else a WO_T_* trap code with *msg set. */
#ifndef WO_BUILTIN_H
#define WO_BUILTIN_H

#include "vm.h"

/* Not a trap code: the stop flag (SIGTERM/SIGINT, armed by `env.stopping`)
 * was set when a BLOCKING call was interrupted, so the call does not restart
 * the syscall — it hands this back and the VM ends the program with it. A
 * service parked in `accept` otherwise never observes the flag and only
 * `kill -9` ends it. Negative so it cannot collide with a WO_T_* code, and
 * deliberately NOT catchable: `try` must not be able to swallow a stop. */
#define WO_SYS_STOPPED (-2)
/* arc T4: the builtin parked the calling fiber against the shard's I/O
 * plane (fb->park_* filled); the interpreter schedules another fiber. */
#define WO_SYS_PARKED (-3)

/* the stop flag, readable by the I/O plane's wait loop (park.c) */
int wo_sys_stop_pending(void);

int wo_builtin(wo_vm *vm, uint64_t *R, uint32_t ins, const char **msg);

/* The systems stdlib's OS half (runtime/src/sysio.c): same contract as
 * wo_builtin above — 0 on success, a WO_T_* code with *msg set on failure.
 * wo_builtin dispatches every id at or above WO_B_SYS_FIRST here. */
int wo_builtin_sys(wo_vm *vm, uint64_t *R, uint32_t ins, const char **msg);

/* json.encode / json.decode (runtime/src/json.c), same contract again. */
int wo_builtin_json(wo_vm *vm, uint64_t *R, uint32_t ins, const char **msg);

/* iteration 19: render a Float as text, SHORTEST form that reparses to the
 * same bits, into `out` (cap must be >= WO_FLOAT_TEXT_CAP); returns the
 * length. One renderer for three callers — WO_B_FLOAT_TO_TEXT, string
 * interpolation, and json.encode — because three spellings of the same
 * number is how a round-trip test starts passing while the product lies.
 *
 * The rendering always carries a '.' or an exponent, so a Float never prints
 * as `1` where an Int would: the two numeric worlds stay visibly distinct,
 * and `1.0` reparses to exactly the same bits as `1`. Non-finite values
 * render `nan` / `inf` / `-inf`; json.encode does NOT use those (JSON has no
 * such literals) and emits `null` instead, which it decides for itself. */
#define WO_FLOAT_TEXT_CAP 32u
size_t wo_float_text(double d, char *out, size_t cap);

/* iteration 19: decode base64 into a fresh Bytes. Two callers — the
 * `base64_decode` builtin and json.decode's Bytes boundary — and they must
 * agree byte for byte, so there is one implementation.
 *   0 = ok (*out is the Bytes word), -1 = malformed input, -2 = OOM.
 * Malformed is a return code rather than a trap because both callers treat
 * bad base64 as expected input from the network. */
int wo_base64_to_bytes(wo_rt *rt, const char *p, uint32_t len, uint64_t *out);

#endif /* WO_BUILTIN_H */
