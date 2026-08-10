/* builtin.h — runtime services bytecode can't express (spec §3/§5):
 * now/print/print_int/words plus the container bridge to cont.c. One
 * dispatcher: takes the VM, the current frame's registers, and the
 * instruction; 0 = ok, else a WO_T_* trap code with *msg set. */
#ifndef WO_BUILTIN_H
#define WO_BUILTIN_H

#include "vm.h"

int wo_builtin(wo_vm *vm, uint64_t *R, uint32_t ins, const char **msg);

#endif /* WO_BUILTIN_H */
