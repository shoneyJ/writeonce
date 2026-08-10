/* test_borrow — the borrow-word state machine (spec section 4). */
#include "borrow.h"
#include "t.h"

int main(void) {
    wo_hdr o = {0};

    /* free -> shared -> shared: readers stack */
    T_EQ(wo_borrow_shared(&o), 0);
    T_EQ(wo_borrow_shared(&o), 0);
    T_EQ(o.borrow, 2);

    /* shared blocks exclusive */
    T_EQ(wo_borrow_excl(&o), -1);

    /* release readers back to free */
    wo_release_shared(&o);
    wo_release_shared(&o);
    T_EQ(o.borrow, WO_BORROW_FREE);

    /* free -> exclusive */
    T_EQ(wo_borrow_excl(&o), 0);
    T_EQ(o.borrow, WO_BORROW_EXCL);

    /* exclusive blocks both */
    T_EQ(wo_borrow_excl(&o), -1);
    T_EQ(wo_borrow_shared(&o), -1);

    /* release restores free; full cycle works again */
    wo_release_excl(&o);
    T_EQ(o.borrow, WO_BORROW_FREE);
    T_EQ(wo_borrow_shared(&o), 0);
    wo_release_shared(&o);
    T_EQ(o.borrow, WO_BORROW_FREE);

    return t_report("test_borrow");
}
