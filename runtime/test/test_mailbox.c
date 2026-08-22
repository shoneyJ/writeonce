/* test_mailbox — the fail-fast cap core (iteration 24). Reserve/release
 * arithmetic single-threaded, then two racing senders: successful
 * reserves never exceed the cap (each success must observe old < cap,
 * and increments are permanent — see wo_mbox_reserve). */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "t.h"
#include "vm.h"

static wo_actor A;

static void *hammer(void *arg) {
    (void)arg;
    long wins = 0;
    for (int i = 0; i < 1000; i++)
        if (wo_mbox_reserve(&A) == 0) wins++;
    return (void *)wins;
}

int main(void) {
    /* single-threaded: cap honored exactly, release frees a slot */
    wo_mailbox_cap = 4;
    memset(&A, 0, sizeof A);
    T_EQ(wo_mbox_reserve(&A), 0);
    T_EQ(wo_mbox_reserve(&A), 0);
    T_EQ(wo_mbox_reserve(&A), 0);
    T_EQ(wo_mbox_reserve(&A), 0);
    T_EQ(wo_mbox_reserve(&A), -1); /* full */
    wo_mbox_release(&A);
    T_EQ(wo_mbox_reserve(&A), 0);  /* freed slot reusable */
    T_EQ(wo_mbox_reserve(&A), -1);
    T_EQ(A.pending, 4);

    /* two racing senders against cap 8: exactly 8 wins, pending == 8 */
    wo_mailbox_cap = 8;
    memset(&A, 0, sizeof A);
    pthread_t t1, t2;
    void *w1, *w2;
    pthread_create(&t1, NULL, hammer, NULL);
    pthread_create(&t2, NULL, hammer, NULL);
    pthread_join(t1, &w1);
    pthread_join(t2, &w2);
    T_EQ((long)w1 + (long)w2, 8);
    T_EQ(A.pending, 8);
    /* drain and refill: the counter did not corrupt under the race */
    for (int i = 0; i < 8; i++) wo_mbox_release(&A);
    T_EQ(A.pending, 0);
    T_EQ(wo_mbox_reserve(&A), 0);

    return t_report("test_mailbox");
}
