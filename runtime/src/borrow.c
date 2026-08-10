#include "borrow.h"

int wo_borrow_shared(wo_hdr *o) {
    if (o->borrow == WO_BORROW_EXCL) return -1;
    o->borrow++;
    return 0;
}

int wo_borrow_excl(wo_hdr *o) {
    if (o->borrow != WO_BORROW_FREE) return -1;
    o->borrow = WO_BORROW_EXCL;
    return 0;
}

void wo_release_shared(wo_hdr *o) { o->borrow--; }

void wo_release_excl(wo_hdr *o) { o->borrow = WO_BORROW_FREE; }
