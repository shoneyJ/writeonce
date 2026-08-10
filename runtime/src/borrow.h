/* borrow.h — the residual-check primitive of the hybrid model (spec §4).
 * The header's borrow word counts shared readers; WO_BORROW_EXCL means
 * exclusively borrowed. Acquires return 0 or -1 (the VM maps -1 to
 * WO_T_BORROW); releases are unconditional — the compiler emits them
 * balanced. Small on purpose: this module is the semantic heart of the
 * hybrid model and the compiler's emit rules cite it. */
#ifndef WO_BORROW_H
#define WO_BORROW_H

#include "wob.h"

int wo_borrow_shared(wo_hdr *o); /* fails only against exclusive */
int wo_borrow_excl(wo_hdr *o);   /* fails unless completely free */
void wo_release_shared(wo_hdr *o);
void wo_release_excl(wo_hdr *o);

#endif /* WO_BORROW_H */
