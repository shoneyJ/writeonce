#define _GNU_SOURCE /* pthread_setaffinity_np, CPU_SET */
#include "vm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "borrow.h"
#include "builtin.h"
#include "cont.h"
#include "gc.h"
#include "park.h"

#include <assert.h>

#include "db.h"    /* arc stage 3: the transparent DB RPC (wo_db_req) */
#include "table.h" /* slot encode/decode for the RPC marshaling */
#include "wal.h"   /* databasev2 4: the drain issues the barrier */

#include <pthread.h>
#include <poll.h>
#include <sched.h>
#include <errno.h>
#include <sys/eventfd.h>
#include <unistd.h>

uint32_t wo_vm_depth(const wo_vm *vm) { return vm->cur->depth; }

/* ---- the shard engine (arc stage 2, T5: threads exist and idle) -------- */

wo_engine wo_eng = {0};

static _Atomic int eng_shutdown = 0;
static _Atomic int eng_teardown = 0; /* set once threads are joined: routed
    frees become no-ops (every arena dies wholesale) and envelopes are
    discarded, so teardown order cannot dangle a mutex */
static _Atomic uint32_t eng_rr = 0; /* round-robin spawn cursor */
static _Thread_local wo_vm *tls_vm = NULL;

wo_vm *wo_tls_vm(void) { return tls_vm; }
void wo_tls_set(wo_vm *vm) { tls_vm = vm; }

/* The cross-shard inbox lives OUTSIDE wo_vm, in engine-owned storage a
 * worker's lazy vm-init can never wipe: the second TSan/ASan round found
 * senders reading vm fields (in_mu, wake_efd, shard_id) through the
 * late-init memset's zero window. Senders touch ONLY this array; the vm's
 * own in_* fields are dead weight kept for layout stability. */
#define WO_ENG_MAX_SHARDS 64u
typedef struct {
    pthread_mutex_t mu;
    wo_envelope *head, *tail;
    int efd; /* duplicate of the shard's wake_efd, sender-visible, never wiped */
} wo_inbox;
static wo_inbox INBOX[WO_ENG_MAX_SHARDS];
static int INBOX_READY[WO_ENG_MAX_SHARDS];

/* push an envelope into a shard's inbox and wake it (any thread) */
static void inbox_push_to(uint32_t shard, wo_envelope *e) {
    wo_inbox *ib = &INBOX[shard % WO_ENG_MAX_SHARDS];
    pthread_mutex_lock(&ib->mu);
    e->next = NULL;
    if (ib->tail) ib->tail->next = e;
    else ib->head = e;
    ib->tail = e;
    int efd = ib->efd;
    pthread_mutex_unlock(&ib->mu);
    if (efd >= 0) {
        uint64_t one = 1;
        ssize_t n = write(efd, &one, sizeof one);
        (void)n;
    }
}

static void fib_enqueue(wo_vm *vm, wo_fiber *fb);
static int actor_activate(wo_vm *vm, wo_actor *a);
static void fib_reap_all(wo_vm *vm);
static int actor_push(wo_actor *a, wo_msg m);
static void call_reply_to(wo_vm *vm, wo_fiber *caller, uint32_t caller_shard,
                          uint64_t reply, int status);
static void actor_drop_payload(wo_vm *vm, uint64_t payload);
static void monitors_fire(wo_vm *vm, wo_actor *a);
static void runtime_notify(wo_vm *vm, wo_actor *target, uint64_t msg_val,
                           const char *what);

/* the owning thread drains its inbox: adopt actors, deliver sends,
 * execute home-routed frees. Returns how many envelopes were handled. */
static int wo_vm_adopt(wo_vm *vm) {
    wo_inbox *ib = &INBOX[vm->shard_id % WO_ENG_MAX_SHARDS];
    pthread_mutex_lock(&ib->mu);
    wo_envelope *e = ib->head;
    ib->head = ib->tail = NULL;
    pthread_mutex_unlock(&ib->mu);
    int n = 0;
    /* databasev2 4 (group commit): DB replies are HELD until one barrier has
     * covered the whole drain. Locals, not per-shard state: nothing here needs
     * to outlive the batch it describes. */
    wo_envelope *rhead = NULL, *rtail = NULL;
    uint32_t staged = 0;
    while (e) {
        wo_envelope *nx = e->next;
        switch (e->kind) {
        case 1: /* adopt a freshly spawned actor: link it, nothing runs yet */
            e->actor->next_all = vm->actors;
            vm->actors = e->actor;
            break;
        case 0: { /* a cross-shard send: mailbox + activation on the HOME
                   thread. The sender already reserved the cap slot; a failed
                   push (OOM) must hand it back or the slot leaks forever.
                   A dead target drops the moved message silently (the
                   send-to-dead rule) and frees the slot. */
            if (e->actor->dead) {
                actor_drop_payload(vm, e->payload);
                wo_mbox_release(e->actor);
                break;
            }
            wo_msg m0 = { e->payload, NULL, 0 };
            if (actor_push(e->actor, m0) == 0) {
                if (!e->actor->active) (void)actor_activate(vm, e->actor);
            } else {
                actor_drop_payload(vm, e->payload);
                wo_mbox_release(e->actor);
            }
            break;
        }
        case 5: { /* iteration 24: a cross-shard call — same enqueue as a
                   send, but the slot remembers the parked caller. A dead
                   target answers the error reply instead. */
            if (e->actor->dead) {
                actor_drop_payload(vm, e->payload);
                wo_mbox_release(e->actor);
                call_reply_to(vm, e->from_fiber, e->from_shard, 0, WO_T_ACTOR);
                break;
            }
            wo_msg mc = { e->payload, e->from_fiber, e->from_shard };
            if (actor_push(e->actor, mc) == 0) {
                if (!e->actor->active) (void)actor_activate(vm, e->actor);
            } else {
                actor_drop_payload(vm, e->payload);
                wo_mbox_release(e->actor);
                call_reply_to(vm, e->from_fiber, e->from_shard, 0, WO_T_ACTOR);
            }
            break;
        }
        case 7: { /* iteration 24 T4: a cross-shard monitor registration —
                   WE are the watched actor's home. Dead already = the
                   notice fires now; else it joins the list. */
            wo_actor *ob = (wo_actor *)(uintptr_t)e->from_fiber;
            if (e->actor->dead) {
                runtime_notify(vm, ob, e->payload, "death notice");
                break;
            }
            wo_monitor *mn = calloc(1, sizeof *mn);
            if (!mn) {
                actor_drop_payload(vm, e->payload);
                break;
            }
            mn->observer = ob;
            mn->msg = e->payload;
            mn->next = e->actor->monitors;
            e->actor->monitors = mn;
            break;
        }
        case 6: /* iteration 24: a call reply landing on the caller's shard —
                   fill the slot and wake the parked fiber; the re-executed
                   builtin consumes it (status != 0 makes it trap). */
            e->from_fiber->call_reply = e->payload;
            e->from_fiber->call_state = e->status ? 3 : 2;
            wo_io_unpark(vm, e->from_fiber);
            break;
        case 2: /* a home-routed free: this arena owns the object */
            wo_drop_obj(&vm->rt, (wo_hdr *)(uintptr_t)e->payload);
            break;
        case 3: { /* arc stage 3: a marshaled DB statement — WE are the DB
                   * actor (only shard 0 ever receives these). Execute
                   * serialized, right here on the owner thread, then ship
                   * the same request back as the reply. */
            wo_db_req *q = (wo_db_req *)(uintptr_t)e->payload;
            assert(vm->is_primary && "DB requests route to shard 0 only");
            wo_wal *dw = (wo_wal *)vm->rt.wal;
            size_t before = dw ? dw->len : 0;
            wo_db_exec_req(vm, q);
            q->done = 1;
            /* did this statement actually stage a record? Asking the buffer
             * beats guessing from the opcode, and the count is what the
             * failure diagnostic reports. */
            if (dw && dw->len > before) staged++;
            wo_envelope *re = calloc(1, sizeof *re);
            if (re) {
                re->kind = 4;
                re->payload = e->payload;
                re->next = NULL;
                if (dw && dw->len > before) {
                    /* This statement STAGED a record, so its reply is HELD:
                     * pushing it now would unpark the requester before its
                     * record is durable, which is the ack contract this
                     * iteration exists to make literally true. FIFO, so the
                     * first waiter is released first. */
                    if (rtail) rtail->next = re; else rhead = re;
                    rtail = re;
                } else {
                    /* A READ (or any statement that staged nothing) has no
                     * durability to wait for. Holding it too was measurably
                     * wrong: it parked readers behind an fsync they had no
                     * stake in, and durable.sN.mixread p99 rose ~4x
                     * (1043 -> 4057us) until this branch existed. */
                    inbox_push_to(q->from_shard, re);
                }
            } /* OOM: the requester stays parked until stop — leak, not UB */
            break;
        }
        case 4: { /* the DB actor's reply: wake the requesting fiber; the
                   * re-executed builtin consumes the request */
            wo_db_req *q = (wo_db_req *)(uintptr_t)e->payload;
            wo_io_unpark(vm, (wo_fiber *)q->fiber);
            break;
        }
        }
        free(e);
        n++;
        e = nx;
    }
    /* databasev2 4: ONE barrier for everything this drain staged, then every
     * held reply. Each requester therefore unparks having been acknowledged
     * after the barrier that carried ITS record. Commit unconditionally when
     * anything is staged — the inline path relies on finding the buffer empty
     * (see db.c), so a drain must never leave a record behind. */
    if (staged) {
        wo_wal *cw = (wo_wal *)vm->rt.wal;
        if (cw) {
            wo_wal_commit_fatal(cw, staged);
            /* databasev2 2 (5c): NOW the batch's offsets are readable, so any
             * keys-resident payload staged behind this barrier can be dropped.
             * Before the barrier those offsets pread zeros. */
            wo_db_flush_drops((wo_db *)vm->rt.db, cw);
        }
    }
    while (rhead) {
        wo_envelope *rn = rhead->next;
        wo_db_req *rq = (wo_db_req *)(uintptr_t)rhead->payload;
        rhead->next = NULL;
        inbox_push_to(rq->from_shard, rhead);
        rhead = rn;
    }
    /* databasev2 3: the ONE point where compaction is safe — the barrier above
     * just ran, so the staging buffer is empty. Anywhere else, a staged record
     * would be written into a file about to be replaced. This is a correctness
     * requirement, not a scheduling preference; wo_wal_compact also refuses a
     * non-empty buffer as a backstop.
     *
     * Replies are released FIRST, deliberately: their records are already
     * durable, and holding them across a stop-the-world rewrite would add the
     * rewrite's full duration to their latency for no benefit.
     *
     * The result is ignored because a failed compaction is a missed
     * optimisation, not a durability event — the original log is left intact
     * and the process carries on. */
    if (staged) {
        wo_wal *cw = (wo_wal *)vm->rt.wal;
        if (cw && wo_wal_should_compact(cw->off, cw->compacted_bytes,
                                        wo_wal_ckpt_floor, wo_wal_ckpt_ratio))
            (void)wo_wal_compact(cw, (wo_db *)vm->rt.db);
    }
    return n;
}

/* route a drop to the object's home shard (gc.c calls through this when
 * the header's shard id is not the current thread's) */
void wo_route_free(wo_hdr *h) {
    if (eng_teardown) return; /* arenas are torn down wholesale */
    wo_envelope *e = calloc(1, sizeof *e);
    if (!e) return; /* OOM on the free path: leak rather than crash */
    e->kind = 2;
    e->payload = (uint64_t)(uintptr_t)h;
    inbox_push_to(h->shard_id, e);
}

/* ---- arc stage 3: the requester half of the transparent DB RPC ---------
 * A worker shard's DB builtin lands here (its rt.db is NULL by design):
 * the args are ENCODED into engine slots on THIS thread — VM heaps are
 * never read cross-shard — the request rides an envelope to shard 0, and
 * the fiber parks with no plane wait (WO_PARK_INBOX). The reply unparks
 * the fiber, the builtin RE-EXECUTES, lands here again, and consumes the
 * answer. Every status/msg pair is the one the local path would trap. */
int wo_db_rpc(wo_vm *vm, uint64_t *R, uint32_t ins, const char **msg) {
    uint32_t A = wo_ins_a(ins), B = wo_ins_b(ins), C = wo_ins_c(ins);
    wo_fiber *fb = vm->cur;
    wo_db_req *q = (wo_db_req *)fb->dbreq;
    const wo_classdesc *classes = vm->mod->classes;

    if (q && q->done) { /* the reply: consume it and finish the builtin */
        fb->dbreq = NULL;
        int rc = q->status;
        if (rc) {
            *msg = q->msg;
        } else {
            switch (C) {
            case WO_B_DB_INSERT: R[A] = q->result; break;
            case WO_B_DB_UPDATE_FIELD:
            case WO_B_DB_DELETE: R[A] = 0; break;
            case WO_B_DB_SCAN:
            case WO_B_DB_PROBE: {
                wo_multi *ids = wo_multi_new(&vm->rt, WO_K_SCALAR);
                if (!ids) rc = WO_T_OOM;
                for (uint32_t i = 0; !rc && i < q->id_cnt; i++)
                    if (wo_multi_push(ids, q->ids[i]) != 0) rc = WO_T_OOM;
                if (!rc) R[A] = (uint64_t)(uintptr_t)ids;
                else *msg = "out of memory";
                break;
            }
            case WO_B_DB_GET_FIELD: {
                int ok = 1;
                uint64_t v = wo_val_decode_vm(NULL, &vm->rt, q->val_kind, q->val, &ok, msg);
                wo_db_val_free(NULL, q->val_kind, q->val);
                q->val = 0;
                if (!ok) rc = WO_T_OOM;
                else R[A] = v;
                break;
            }
            default:
                *msg = "unknown db builtin";
                rc = WO_T_DB;
            }
        }
        if (q->val) wo_db_val_free(NULL, q->val_kind, q->val);
        free(q->ids);
        free(q);
        return rc;
    }

    /* first entry: marshal on OUR thread, ship, park */
    q = calloc(1, sizeof *q);
    if (!q) {
        *msg = "out of memory";
        return WO_T_OOM;
    }
    q->op = C;
    q->from_shard = vm->shard_id;
    q->fiber = fb;
    int ok = 1;
    switch (C) {
    case WO_B_DB_INSERT: {
        q->cid = (uint32_t)R[B];
        if (q->cid >= vm->mod->class_cnt) {
            free(q);
            *msg = "no such class";
            return WO_T_DB;
        }
        const wo_classdesc *c = &classes[q->cid];
        q->slots = calloc(c->field_cnt ? c->field_cnt : 1, 8);
        if (!q->slots) {
            free(q);
            *msg = "out of memory";
            return WO_T_OOM;
        }
        q->slot_cnt = c->field_cnt;
        for (uint32_t i = 0; i < c->field_cnt; i++) {
            q->slots[i] = wo_db_val_encode(classes, c->kinds[i], R[B + 1 + i], &ok, msg);
            if (!ok) { /* GCREF (the compiler's reject, defensively) or OOM */
                for (uint32_t j = 0; j < i; j++)
                    wo_db_val_free(NULL, c->kinds[j], q->slots[j]);
                free(q->slots);
                free(q);
                return WO_T_DB;
            }
        }
        break;
    }
    case WO_B_DB_UPDATE_FIELD: {
        q->cid = (uint32_t)R[B];
        q->id = R[B + 1];
        q->field = (uint32_t)R[B + 2];
        if (q->cid >= vm->mod->class_cnt || q->field >= classes[q->cid].field_cnt) {
            free(q);
            *msg = "no such field";
            return WO_T_DB;
        }
        q->slots = calloc(1, 8);
        if (!q->slots) {
            free(q);
            *msg = "out of memory";
            return WO_T_OOM;
        }
        q->slot_cnt = 1;
        q->slots[0] =
            wo_db_val_encode(classes, classes[q->cid].kinds[q->field], R[B + 3], &ok, msg);
        if (!ok) {
            free(q->slots);
            free(q);
            return WO_T_DB;
        }
        break;
    }
    case WO_B_DB_DELETE:
        q->cid = (uint32_t)R[B];
        q->id = R[B + 1];
        break;
    case WO_B_DB_SCAN:
        q->cid = (uint32_t)R[B];
        break;
    case WO_B_DB_GET_FIELD:
        q->cid = (uint32_t)R[B];
        q->id = R[B + 1];
        q->field = (uint32_t)R[B + 2];
        break;
    case WO_B_DB_PROBE: {
        q->cid = (uint32_t)R[B];
        q->index = (uint32_t)R[B + 1];
        /* the key's kind comes from the class table's index metadata —
         * identical on every shard (one module). An index the metadata
         * does not know ships keyless; the owner answers empty, exactly
         * as the local path does. */
        if (q->cid < vm->mod->class_cnt && classes[q->cid].idx_meta &&
            q->index < classes[q->cid].idx_cnt) {
            const uint32_t *p = classes[q->cid].idx_meta;
            for (uint32_t k = 0; k < q->index; k++) p += 2 + p[1];
            uint8_t kind = classes[q->cid].kinds[p[2]];
            q->slots = calloc(1, 8);
            if (!q->slots) {
                free(q);
                *msg = "out of memory";
                return WO_T_OOM;
            }
            q->slot_cnt = 1;
            q->slots[0] = wo_db_val_encode(classes, kind, R[B + 2], &ok, msg);
            if (!ok) {
                free(q->slots);
                free(q);
                return WO_T_DB;
            }
        }
        break;
    }
    default:
        free(q);
        *msg = "unknown db builtin";
        return WO_T_DB;
    }
    wo_envelope *e = calloc(1, sizeof *e);
    if (!e) {
        if (q->slot_cnt && C == WO_B_DB_INSERT) {
            const wo_classdesc *c = &classes[q->cid];
            for (uint32_t j = 0; j < c->field_cnt; j++)
                wo_db_val_free(NULL, c->kinds[j], q->slots[j]);
        } else if (q->slot_cnt && C == WO_B_DB_UPDATE_FIELD) {
            wo_db_val_free(NULL, classes[q->cid].kinds[q->field], q->slots[0]);
        } /* a PROBE key leaks on this path: kind recompute not worth it */
        free(q->slots);
        free(q);
        *msg = "out of memory";
        return WO_T_OOM;
    }
    fb->dbreq = q;
    e->kind = 3;
    e->payload = (uint64_t)(uintptr_t)q;
    inbox_push_to(0, e);
    fb->park_fd = WO_PARK_INBOX;
    fb->park_done = 0; /* resume RE-EXECUTES the builtin: the consume path */
    return WO_SYS_PARKED;
}

/* A worker's whole life in T5: pinned, parked on its wake eventfd until
 * shutdown. T6 gives it an inbox to adopt fibers from and the serve loop
 * that runs them. */
static size_t eng_heap_cap = 0;

/* lazily give a worker its full vm (arena, GC, I/O plane) — paid on the
 * first envelope, not at boot (20 idle shards must stay ~free) */
static int worker_late_init(wo_vm *vm) {
    if (vm->rt.arena.base) return 0;
    /* the memset here is now HARMLESS to senders: every field they touch
     * lives in the engine-owned INBOX array, never in the vm (the second
     * TSan/ASan round found them reading through this wipe's zero window) */
    const wo_module *mod = vm->mod;
    uint32_t id = vm->shard_id;
    int efd = vm->wake_efd;
    int rc = wo_vm_init(vm, mod, eng_heap_cap);
    if (rc == 0) {
        vm->shard_id = id;
        vm->rt.shard_id = (uint16_t)id;
        vm->is_primary = 0;
        vm->wake_efd = efd;
        tls_vm = vm;
    }
    return rc;
}

int wo_vm_serve(wo_vm *vm); /* vm_run's worker flavor, defined below it */

/* one blocking wait for the FIRST envelope (the vm — and its I/O plane —
 * does not exist yet); after late init the plane's own wait watches the
 * eventfd and this poll never runs again */
static void worker_first_wait(wo_vm *vm) {
    struct pollfd p = { .fd = vm->wake_efd, .events = POLLIN };
    while (!eng_shutdown) {
        int n = poll(&p, 1, -1);
        if (n > 0 || (n < 0 && errno != EINTR)) return;
        if (wo_sys_stop_pending()) return;
    }
}

static void *shard_main(void *arg) {
    wo_vm *vm = (wo_vm *)arg;
    tls_vm = vm;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET((int)(vm->shard_id % 64u), &set);
    pthread_setaffinity_np(pthread_self(), sizeof set, &set);
    worker_first_wait(vm);
    if (eng_shutdown || worker_late_init(vm) != 0) return NULL;
    while (!eng_shutdown) {
        (void)wo_vm_adopt(vm);
        if (vm->qhead) {
            int rc = wo_vm_serve(vm); /* runs until drained (2) or stop */
            if (rc == 1) break;       /* stop: everything reaped inside */
        } else {
            int rc = wo_io_wait(vm); /* parked fibers AND the wake eventfd */
            if (rc == WO_IO_STOP) {
                /* iteration 40 — THE DRAIN GUARANTEE. A message sent before
                 * the stop flag is observed must be delivered and run before
                 * the engine stops.
                 *
                 * NEXT_RUNNABLE() already states this contract for a worker
                 * holding a live fiber: it returns 2 and keeps draining "so
                 * queued shutdown messages (close frames!) still run". This
                 * branch — the IDLE worker, empty run queue, waiting on the
                 * plane — used to reap and break instead, abandoning whatever
                 * sat in its inbox for wo_engine_stop() to free wholesale.
                 *
                 * An actor between messages is exactly that idle case, which
                 * is why a WARM server hid the bug: warm shards had live
                 * fibers and took the correct path. Measured 2026-08-27 on a
                 * fresh server: 5 of 16 SIGTERM drains left a WebSocket
                 * client at EOF with no close frame and no diagnostic.
                 *
                 * The window belongs to the PRIMARY and closes when it sets
                 * eng_shutdown (after main returns), so honour it here and
                 * only exit when the primary says so. Yield on an empty poll:
                 * a tight loop would burn a core per shard and starve the very
                 * actors the drain exists to let run. */
                if (!eng_shutdown) {
                    (void)wo_vm_adopt(vm);
                    if (!vm->qhead) sched_yield();
                    continue;
                }
                fib_reap_all(vm);
                break;
            }
        }
    }
    return NULL;
}

/* register the PRIMARY's inbox row (main.c calls it once its wake fd
 * exists); workers register theirs in wo_engine_start */
int wo_engine_primary_inbox(int wake_efd) {
    if (!INBOX_READY[0]) {
        if (pthread_mutex_init(&INBOX[0].mu, NULL) != 0) return -1;
        INBOX_READY[0] = 1;
    }
    INBOX[0].head = INBOX[0].tail = NULL;
    INBOX[0].efd = wake_efd;
    return 0;
}

/* iteration 24 teardown phase 1 (single-threaded, BEFORE eng_teardown):
 * dismantle one vm's actor world with real drops — container backings are
 * malloc'd, so wholesale arena death does NOT cover them (LSan, chat's
 * registry map). Cross-shard payloads route home through wo_route_free
 * (still live here); the routed kind-2 envelopes are settled by the
 * caller's inbox passes. */
static void vm_drop_actor_world(wo_vm *vm) {
    wo_actor *a = vm->actors;
    vm->actors = NULL;
    while (a) {
        wo_actor *nx = a->next_all;
        if (a->instance) wo_drop_obj(&vm->rt, (wo_hdr *)(uintptr_t)a->instance);
        for (uint32_t i = 0; i < a->mlen; i++) {
            uint64_t m = a->msgs[(a->mhead + i) % a->mcap].payload;
            if (m) wo_drop_obj(&vm->rt, (wo_hdr *)(uintptr_t)m);
        }
        wo_monitor *mo = a->monitors;
        while (mo) {
            wo_monitor *mnx = mo->next;
            if (mo->msg) wo_drop_obj(&vm->rt, (wo_hdr *)(uintptr_t)mo->msg);
            free(mo);
            mo = mnx;
        }
        free(a->msgs);
        free(a);
        a = nx;
    }
    wo_timer *tt = vm->timers;
    vm->timers = NULL;
    while (tt) {
        wo_timer *tnx = tt->next;
        if (tt->msg) wo_drop_obj(&vm->rt, (wo_hdr *)(uintptr_t)tt->msg);
        free(tt);
        tt = tnx;
    }
}

/* Settle every inbox after phase 1: home-routed frees execute on their
 * owner vm; payload-carrying strays drop (possibly routing again — the
 * outer loop runs until everything is quiet). Node memory always freed. */
static int eng_settle_inboxes(void) {
    int moved = 0;
    for (uint32_t i = 0; i < wo_eng.nshards && i < WO_ENG_MAX_SHARDS; i++) {
        if (!INBOX_READY[i]) continue;
        wo_vm *vm = &wo_eng.shards[i];
        wo_inbox *ib = &INBOX[i];
        wo_envelope *e = ib->head;
        ib->head = ib->tail = NULL;
        while (e) {
            wo_envelope *nx = e->next;
            switch (e->kind) {
            case 2: /* WE are home: the direct drop is the settlement */
                wo_drop_obj(&vm->rt, (wo_hdr *)(uintptr_t)e->payload);
                break;
            case 0:
            case 5:
            case 7: /* in-flight payloads: drop (may route -> next pass) */
                if (e->payload)
                    wo_drop_obj(&vm->rt, (wo_hdr *)(uintptr_t)e->payload);
                break;
            case 1: /* an unadopted actor shell */
                if (e->actor) {
                    if (e->actor->instance)
                        wo_drop_obj(&vm->rt, (wo_hdr *)(uintptr_t)e->actor->instance);
                    free(e->actor->msgs);
                    free(e->actor);
                }
                break;
            default: /* 3/4/6: scalar or engine-side payloads, node-only */
                break;
            }
            free(e);
            moved++;
            e = nx;
        }
    }
    return moved;
}

int wo_engine_start(const wo_module *mod, size_t heap_cap, uint32_t nshards) {
    wo_eng.nshards = nshards;
    eng_heap_cap = heap_cap;
    if (nshards <= 1) return 0; /* the one-shard degenerate case: no threads */
    pthread_t *ts = calloc(nshards - 1, sizeof(pthread_t));
    if (!ts) return -1;
    wo_eng.threads = ts;
    for (uint32_t i = 1; i < nshards; i++) {
        wo_vm *sv = &wo_eng.shards[i];
        /* LAZY: a worker's full vm (64 MiB arena and all) is not paid for
         * until its first fiber arrives (T6 adopts). T5 workers only need
         * an identity and a wake fd — 20 idle shards must not cost 1.25 GiB
         * (they did: the web-app gate flaked on exactly that). */
        memset(sv, 0, sizeof *sv);
        sv->mod = mod;
        sv->shard_id = i;
        /* language 41: stamp the RUNTIME's identity here too, not only at
         * lazy init. A worker's rt is initialised when it adopts its first
         * fiber (T6), but INBOX_READY[i] is set right below — at thread
         * creation. So a shard that never adopts still gets settled at
         * shutdown, with rt.shard_id left 0 by the memset above. It then
         * IMPERSONATES shard 0: wo_drop_obj compares o->shard_id against
         * rt->shard_id, sees 0 == 0 for any payload the primary allocated,
         * concludes "we are home" instead of routing, and calls class_free
         * against rt->classes — which lazy init never filled, so it is NULL.
         * That is the SIGSEGV: &rt->classes[class_id] off a null base.
         * An uninitialised shard has allocated nothing and therefore owns
         * nothing, so carrying its real id makes every payload correctly
         * foreign and routes it to the owner that can actually free it. */
        sv->rt.shard_id = (uint16_t)i;
        sv->is_primary = 0;
        sv->wake_efd = eventfd(0, EFD_NONBLOCK);
        if (sv->wake_efd < 0) return -1;
        {
            wo_inbox *ib = &INBOX[i % WO_ENG_MAX_SHARDS];
            if (!INBOX_READY[i % WO_ENG_MAX_SHARDS]) {
                if (pthread_mutex_init(&ib->mu, NULL) != 0) return -1;
                INBOX_READY[i % WO_ENG_MAX_SHARDS] = 1;
            }
            ib->head = ib->tail = NULL;
            ib->efd = sv->wake_efd;
        }
        if (pthread_create(&ts[i - 1], NULL, shard_main, sv) != 0) return -1;
    }
    (void)heap_cap; /* consumed at lazy init (T6) */
    return 0;
}

void wo_engine_stop(void) {
    if (wo_eng.nshards <= 1) return;
    eng_shutdown = 1;
    pthread_t *ts = (pthread_t *)wo_eng.threads;
    for (uint32_t i = 1; i < wo_eng.nshards; i++) {
        uint64_t one = 1;
        ssize_t n = write(wo_eng.shards[i].wake_efd, &one, sizeof one);
        (void)n;
    }
    for (uint32_t i = 1; i < wo_eng.nshards; i++) pthread_join(ts[i - 1], NULL);
    /* single-threaded from here: PHASE 1 — real drops while every arena
     * and the routing fabric are still alive (malloc'd container backings
     * inside actor state need them; iteration 24's registry map). Settle
     * passes run until routed frees stop appearing. */
    for (uint32_t i = 0; i < wo_eng.nshards && i < WO_ENG_MAX_SHARDS; i++)
        if (wo_eng.shards[i].rt.arena.base) vm_drop_actor_world(&wo_eng.shards[i]);
    while (eng_settle_inboxes() > 0) {}
    /* single-threaded from here. Every arena dies wholesale, so routed
     * frees and queued payloads need no per-object drops — DISCARD the
     * envelopes (freeing the malloc'd nodes/actors) and let the arenas
     * take their contents with them. The flag also turns any route_free
     * raised by the destroys below into a no-op, so no teardown ordering
     * can lock a freed mutex (the ASan SEGV this replaces). */
    eng_teardown = 1;
    for (uint32_t i = 0; i < wo_eng.nshards && i < WO_ENG_MAX_SHARDS; i++) {
        if (!INBOX_READY[i]) continue;
        wo_inbox *ib = &INBOX[i];
        wo_envelope *e = ib->head;
        ib->head = ib->tail = NULL;
        ib->efd = -1;
        while (e) {
            wo_envelope *nx = e->next;
            if (e->kind == 1 && e->actor) {
                free(e->actor->msgs);
                free(e->actor);
            }
            free(e);
            e = nx;
        }
    }
    for (uint32_t i = 1; i < wo_eng.nshards; i++) {
        close(wo_eng.shards[i].wake_efd);
        if (wo_eng.shards[i].rt.arena.base) /* lazily init'ed only */
            wo_vm_destroy(&wo_eng.shards[i]);

    }
    /* the primary's wake fd (its inbox row was drained in the loop above) */
    {
        wo_vm *pv = &wo_eng.shards[0];
        if (pv->wake_efd >= 0) {
            close(pv->wake_efd);
            pv->wake_efd = -1;
        }
    }
    free(ts);
    wo_eng.threads = NULL;
    wo_eng.nshards = 1;
}

int wo_vm_init(wo_vm *vm, const wo_module *mod, size_t heap_cap) {
    memset(vm, 0, sizeof(*vm));
    vm->mod = mod;
    vm->cur = &vm->f0; /* fiber 0: main — the one-fiber degenerate case */
    vm->wake_efd = -1; /* engines/main wire a real one; tests run without */
    vm->budget0 = 4000; /* reductions per slice, the BEAM-ish default */
    {
        const char *e = getenv("WO_REDUCTIONS");
        if (e && *e) {
            long v = atol(e);
            if (v > 0) vm->budget0 = v;
        }
    }
    vm->budget = vm->budget0;
    if (wo_io_init(vm) != 0) return -1; /* no I/O plane at all: fatal */
    return wo_rt_init(&vm->rt, heap_cap, mod->classes, mod->class_cnt);
}

void wo_vm_destroy(wo_vm *vm) {
    wo_proc_reap_all(vm); /* iteration 42: no child outlives its shard */
    /* iteration 35: the fiber pool dies with the vm */
    while (vm->fib_pool) {
        wo_fiber *fb = vm->fib_pool;
        vm->fib_pool = fb->next;
        free(fb);
    }
    /* actors first — dropping their state and queued messages needs the
     * runtime alive. BUT: once the engine is in teardown, arenas die
     * WHOLESALE (the standing doctrine) — a moved-in message's home arena
     * may belong to an ALREADY-destroyed shard, and even reading its
     * header is a use-after-free (ASan, chat's drain). Structures are
     * still freed; payload drops are skipped. */
    int drops_ok = !eng_teardown;
    wo_actor *a = vm->actors;
    while (a) {
        wo_actor *nx = a->next_all;
        if (drops_ok && a->instance)
            wo_drop_obj(&vm->rt, (wo_hdr *)(uintptr_t)a->instance);
        for (uint32_t i = 0; drops_ok && i < a->mlen; i++) {
            uint64_t m = a->msgs[(a->mhead + i) % a->mcap].payload;
            if (m) wo_drop_obj(&vm->rt, (wo_hdr *)(uintptr_t)m);
        }
        wo_monitor *mo = a->monitors;
        while (mo) { /* undelivered notices are the runtime's to drop */
            wo_monitor *mnx = mo->next;
            if (drops_ok && mo->msg)
                wo_drop_obj(&vm->rt, (wo_hdr *)(uintptr_t)mo->msg);
            free(mo);
            mo = mnx;
        }
        free(a->msgs);
        free(a);
        a = nx;
    }
    wo_timer *tt = vm->timers;
    vm->timers = NULL;
    while (tt) { /* unfired timers likewise */
        wo_timer *tnx = tt->next;
        if (drops_ok && tt->msg)
            wo_drop_obj(&vm->rt, (wo_hdr *)(uintptr_t)tt->msg);
        free(tt);
        tt = tnx;
    }
    vm->actors = NULL;
    wo_io_destroy(vm);
    wo_rt_destroy(&vm->rt);
}

/* ---- the run queue (stage 1 Task 2) ---------------------------------- */

static void vm_unwind(wo_vm *vm, uint32_t stop_depth);

static void fib_enqueue(wo_vm *vm, wo_fiber *fb) {
    fb->state = WO_FIB_RUNNABLE;
    fb->next = NULL;
    if (vm->qtail) vm->qtail->next = fb;
    else vm->qhead = fb;
    vm->qtail = fb;
}

static wo_fiber *fib_dequeue(wo_vm *vm) {
    wo_fiber *fb = vm->qhead;
    if (fb) {
        vm->qhead = fb->next;
        if (!vm->qhead) vm->qtail = NULL;
        fb->next = NULL;
    }
    return fb;
}

/* iteration 35: dead fibers pool instead of freeing (vm.h's UAF note).
 * next links the pool; a pooled fiber's state is DONE, so a stale plane
 * completion reading it is harmless. */
static void fib_retire(wo_vm *vm, wo_fiber *fb) {
    fb->state = WO_FIB_DONE;
    fb->next = vm->fib_pool;
    vm->fib_pool = fb;
}

wo_fiber *wo_vm_spawn_fiber(wo_vm *vm, uint32_t method_idx, const uint64_t *args,
                            uint32_t argc) {
    if (method_idx >= vm->mod->method_cnt) return NULL;
    const wo_methodrec *sme = &vm->mod->methods[method_idx];
    if (argc != sme->arg_cnt) return NULL;
    wo_fiber *fb;
    if (vm->fib_pool) {
        fb = vm->fib_pool;
        vm->fib_pool = fb->next;
        memset(fb, 0, sizeof(*fb));
    } else {
        fb = calloc(1, sizeof(*fb));
    }
    if (!fb) return NULL;
    fb->depth = 1;
    fb->frames[0].method = method_idx;
    fb->frames[0].pc = 0;
    fb->frames[0].base = 0;
    if (argc) memcpy(fb->regs, args, (size_t)argc * 8u);
    memset(fb->regs + argc, 0, (size_t)(sme->reg_cnt - argc) * 8u);
    vm->nfibers++;
    fib_enqueue(vm, fb);
    return fb;
}

/* Unwind and release one fiber's live frames (drop maps run — parked and
 * queued fibers die as cleanly as trapped ones), then free it if it is a
 * spawned one. `vm->cur` is borrowed to do it, restored after. */
static void fib_reap(wo_vm *vm, wo_fiber *fb) {
    wo_proc_abandon(vm, fb); /* iteration 42: its child dies with it */
    wo_fiber *save = vm->cur;
    vm->cur = fb;
    vm_unwind(vm, 0);
    vm->cur = save;
    if (fb->actor) {
        /* the in-flight message is the runtime's to drop */
        if (fb->cur_msg) wo_drop_obj(&vm->rt, (wo_hdr *)(uintptr_t)fb->cur_msg);
        fb->cur_msg = 0;
        fb->actor->active = NULL;
    }
    if (fb != &vm->f0) {
        vm->nfibers--;
        fib_retire(vm, fb);
    }
}

/* Main finished (return or stop): every remaining fiber — queued AND
 * parked — unwinds clean. */
static void fib_reap_all(wo_vm *vm) {
    wo_fiber *fb;
    while ((fb = fib_dequeue(vm)) != NULL) fib_reap(vm, fb);
    while ((fb = vm->parked) != NULL) {
        vm->parked = fb->pnext;
        fb->pnext = NULL;
        vm->nparked--;
        fib_reap(vm, fb);
    }
}

/* ---- actors (arc stage 1 Task 3) -------------------------------------- */

/* iteration 24: fail-fast backpressure. The SENDER reserves a slot before
 * anything is enqueued anywhere (same-shard push or cross-shard envelope);
 * the home thread releases it when the message is popped for delivery.
 * Reserve/release are the unit-testable core (test_mailbox.c). */
uint32_t wo_mailbox_cap = 1024;

int wo_mbox_reserve(wo_actor *a) {
    uint32_t old = __atomic_fetch_add(&a->pending, 1, __ATOMIC_ACQ_REL);
    if (old >= wo_mailbox_cap) {
        __atomic_fetch_sub(&a->pending, 1, __ATOMIC_ACQ_REL);
        return -1;
    }
    return 0;
}

void wo_mbox_release(wo_actor *a) {
    __atomic_fetch_sub(&a->pending, 1, __ATOMIC_ACQ_REL);
}

static wo_msg actor_pop(wo_actor *a) {
    wo_msg m = a->msgs[a->mhead];
    a->mhead = (a->mhead + 1) % a->mcap;
    a->mlen--;
    wo_mbox_release(a);
    return m;
}

static int actor_push(wo_actor *a, wo_msg m) {
    if (a->mlen == a->mcap) {
        uint32_t ncap = a->mcap ? a->mcap * 2 : 8;
        wo_msg *nm = malloc((size_t)ncap * sizeof(wo_msg));
        if (!nm) return -1;
        for (uint32_t i = 0; i < a->mlen; i++) nm[i] = a->msgs[(a->mhead + i) % a->mcap];
        free(a->msgs);
        a->msgs = nm;
        a->mhead = 0;
        a->mcap = ncap;
    }
    a->msgs[(a->mhead + a->mlen) % a->mcap] = m;
    a->mlen++;
    return 0;
}

/* iteration 24: a message the runtime must discard (dead target, failed
 * enqueue). Messages are class instances — wo_drop_obj routes a wrong-
 * shard drop home through the free envelope. */
static void actor_drop_payload(wo_vm *vm, uint64_t payload) {
    if (payload) wo_drop_obj(&vm->rt, (wo_hdr *)(uintptr_t)payload);
}

/* iteration 24: answer one parked caller. Same-shard callers unpark
 * directly; remote ones get a kind-6 envelope. status 0 delivers the
 * scalar reply; WO_T_ACTOR makes the caller's re-executed builtin trap. */
static void call_reply_to(wo_vm *vm, wo_fiber *caller, uint32_t caller_shard,
                          uint64_t reply, int status) {
    if (!caller) return;
    if (caller_shard == vm->shard_id) {
        caller->call_reply = reply;
        caller->call_state = status ? 3 : 2;
        wo_io_unpark(vm, caller);
        return;
    }
    wo_envelope *e = calloc(1, sizeof *e);
    if (!e) return; /* OOM: the caller stays parked until stop — leak, not UB */
    e->kind = 6;
    e->payload = reply;
    e->from_fiber = caller;
    e->status = status;
    inbox_push_to(caller_shard, e);
}

/* iteration 24: an actor dies (its receive trapped uncaught). Marked on
 * the HOME thread only. The in-flight caller and every QUEUED caller get
 * the dead error; queued payloads are the runtime's to drop; the moved-in
 * state is released. The wo_actor shell itself stays allocated forever
 * (addresses are copyable scalars that may still be sent to). */
static void actor_die(wo_vm *vm, wo_actor *a, wo_fiber *delivery) {
    a->dead = 1;
    wo_proc_abandon_actor(vm, a); /* runtime-v2 1: its children die with it */
    if (delivery->cur_msg) {
        wo_drop_obj(&vm->rt, (wo_hdr *)(uintptr_t)delivery->cur_msg);
        delivery->cur_msg = 0;
    }
    call_reply_to(vm, delivery->msg_caller, delivery->msg_caller_shard, 0, WO_T_ACTOR);
    delivery->msg_caller = NULL;
    while (a->mlen) {
        wo_msg m = actor_pop(a);
        if (m.payload) wo_drop_obj(&vm->rt, (wo_hdr *)(uintptr_t)m.payload);
        call_reply_to(vm, m.caller, m.caller_shard, 0, WO_T_ACTOR);
    }
    if (a->instance) {
        wo_drop_obj(&vm->rt, (wo_hdr *)(uintptr_t)a->instance);
        a->instance = 0;
    }
    a->active = NULL;
    monitors_fire(vm, a);
}

/* Mailbox nonempty, no delivery fiber: start one on the next message.
 * receive borrows both self and the message; the runtime keeps ownership
 * of the message (fiber->cur_msg) and drops it when the call returns. */
static int actor_activate(wo_vm *vm, wo_actor *a) {
    wo_msg m = actor_pop(a);
    uint64_t args[2] = { a->instance, m.payload };
    wo_fiber *fb = wo_vm_spawn_fiber(vm, a->method, args, 2);
    if (!fb) {
        actor_drop_payload(vm, m.payload);
        call_reply_to(vm, m.caller, m.caller_shard, 0, WO_T_ACTOR);
        return -1;
    }
    fb->actor = a;
    fb->cur_msg = m.payload;
    fb->msg_caller = m.caller;
    fb->msg_caller_shard = m.caller_shard;
    a->active = fb;
    return 0;
}

int wo_vm_actor_spawn(wo_vm *vm, uint64_t instance, uint32_t method_idx,
                      uint64_t *out_addr, const char **msg) {
    if (method_idx >= vm->mod->method_cnt
        || vm->mod->methods[method_idx].arg_cnt != 2) {
        *msg = "spawn: receive must take (self, msg)";
        return WO_T_BOUNDS;
    }
    if (!instance) {
        *msg = "spawn: nil instance";
        return WO_T_BOUNDS;
    }
    wo_actor *a = calloc(1, sizeof(*a));
    if (!a) {
        *msg = "out of memory";
        return WO_T_OOM;
    }
    a->instance = instance;
    a->method = method_idx;
    /* placement (arc T6): round-robin across shards; same-shard when the
     * engine is absent (tests) or single. The actor's list membership
     * belongs to its HOME thread — an adopt envelope carries it there. */
    uint32_t n = wo_eng.nshards ? wo_eng.nshards : 1;
    uint32_t home = n > 1 ? (eng_rr++ % n) : vm->shard_id;
    a->home = home;
    if (home == vm->shard_id) {
        a->next_all = vm->actors;
        vm->actors = a;
    } else {
        wo_envelope *e = calloc(1, sizeof *e);
        if (!e) {
            free(a);
            *msg = "out of memory";
            return WO_T_OOM;
        }
        e->kind = 1;
        e->actor = a;
        inbox_push_to(home, e);
    }
    *out_addr = (uint64_t)(uintptr_t)a;
    return 0;
}

int wo_vm_actor_send(wo_vm *vm, uint64_t addr, uint64_t msg_val, const char **msg) {
    wo_actor *a = (wo_actor *)(uintptr_t)addr;
    if (!a) {
        *msg = "send: nil actor address";
        return WO_T_BOUNDS;
    }
    if (!msg_val) {
        *msg = "send: nil message";
        return WO_T_BOUNDS;
    }
    /* send-to-dead is a silent drop (spec'd v1): the message moved to the
     * runtime, so the runtime discards it. The dead flag is written on the
     * home thread; a racing remote read at worst enqueues an envelope the
     * home drain then discards through its own dead check. */
    if (a->dead) {
        actor_drop_payload(vm, msg_val);
        return 0;
    }
    /* iteration 24: the cap check happens SENDER-side on every path, so
     * the sender always learns — fail-fast backpressure, catchable. */
    if (wo_mbox_reserve(a) != 0) {
        *msg = "actor mailbox full";
        return WO_T_ACTOR;
    }
    if (a->home != vm->shard_id) {
        /* cross-shard: the HOME thread owns the mailbox — send travels as
         * an inbox envelope, ownership moves with it (the mutex is the
         * happens-before edge TSan sees) */
        wo_envelope *e = calloc(1, sizeof *e);
        if (!e) {
            wo_mbox_release(a);
            *msg = "out of memory";
            return WO_T_OOM;
        }
        e->kind = 0;
        e->actor = a;
        e->payload = msg_val;
        inbox_push_to(a->home, e);
        return 0;
    }
    wo_msg m0 = { msg_val, NULL, 0 };
    if (actor_push(a, m0) != 0) {
        wo_mbox_release(a);
        *msg = "out of memory";
        return WO_T_OOM;
    }
    if (!a->active && actor_activate(vm, a) != 0) {
        *msg = "out of memory";
        return WO_T_OOM;
    }
    return 0;
}

/* iteration 24 T4/T5: a RUNTIME-sourced delivery (death notice, timer).
 * No fiber to trap: a full or dead target drops the message with a
 * stderr line (spec'd disclosure), never silently. Runs on any thread —
 * cross-shard targets ride the ordinary kind-0 envelope. */
static void runtime_notify(wo_vm *vm, wo_actor *target, uint64_t msg_val,
                           const char *what) {
    if (!target || !msg_val) return;
    if (target->dead) {
        actor_drop_payload(vm, msg_val);
        return; /* send-to-dead: silent by contract */
    }
    if (wo_mbox_reserve(target) != 0) {
        fprintf(stderr, "wovm: %s dropped — the observer's mailbox is full\n", what);
        actor_drop_payload(vm, msg_val);
        return;
    }
    if (target->home != vm->shard_id) {
        wo_envelope *e = calloc(1, sizeof *e);
        if (!e) {
            wo_mbox_release(target);
            actor_drop_payload(vm, msg_val);
            return;
        }
        e->kind = 0;
        e->actor = target;
        e->payload = msg_val;
        inbox_push_to(target->home, e);
        return;
    }
    wo_msg m0 = { msg_val, NULL, 0 };
    if (actor_push(target, m0) != 0) {
        wo_mbox_release(target);
        actor_drop_payload(vm, msg_val);
        return;
    }
    if (!target->active) (void)actor_activate(vm, target);
}

/* iteration 24 T4: the death walk — every registered observer gets its
 * chosen notice, then the list is gone (an actor dies once). */
static void monitors_fire(wo_vm *vm, wo_actor *a) {
    wo_monitor *m = a->monitors;
    a->monitors = NULL;
    while (m) {
        wo_monitor *nx = m->next;
        runtime_notify(vm, m->observer, m->msg, "death notice");
        free(m);
        m = nx;
    }
}

/* iteration 24: call — send that waits. First entry enqueues with the
 * caller attached and parks (WO_PARK_INBOX, the DB-RPC park); the resume
 * RE-EXECUTES this builtin and consumes the scalar reply. No hangs, ever:
 * call-to-dead traps immediately, callee-dies-mid-call error-unparks. */
int wo_vm_actor_call(wo_vm *vm, uint64_t *R, uint32_t ins, const char **msg) {
    uint32_t A = wo_ins_a(ins), B = wo_ins_b(ins);
    wo_fiber *fb = vm->cur;

    if (fb->call_state == 2) { /* the reply: consume it */
        fb->call_state = 0;
        R[A] = fb->call_reply;
        return 0;
    }
    if (fb->call_state == 3) { /* the callee was/went dead */
        fb->call_state = 0;
        *msg = "actor died during call";
        return WO_T_ACTOR;
    }

    wo_actor *a = (wo_actor *)(uintptr_t)R[B];
    uint64_t msg_val = R[B + 1];
    if (!a) {
        *msg = "call: nil actor address";
        return WO_T_BOUNDS;
    }
    if (!msg_val) {
        *msg = "call: nil message";
        return WO_T_BOUNDS;
    }
    if (a->dead) { /* unlike send, the caller MUST learn */
        actor_drop_payload(vm, msg_val);
        *msg = "actor died during call";
        return WO_T_ACTOR;
    }
    if (wo_mbox_reserve(a) != 0) {
        *msg = "actor mailbox full";
        return WO_T_ACTOR;
    }
    if (a->home != vm->shard_id) {
        wo_envelope *e = calloc(1, sizeof *e);
        if (!e) {
            wo_mbox_release(a);
            *msg = "out of memory";
            return WO_T_OOM;
        }
        e->kind = 5;
        e->actor = a;
        e->payload = msg_val;
        e->from_shard = vm->shard_id;
        e->from_fiber = fb;
        inbox_push_to(a->home, e);
    } else {
        wo_msg mc = { msg_val, fb, vm->shard_id };
        if (actor_push(a, mc) != 0) {
            wo_mbox_release(a);
            *msg = "out of memory";
            return WO_T_OOM;
        }
        if (!a->active && actor_activate(vm, a) != 0) {
            *msg = "out of memory";
            return WO_T_OOM;
        }
    }
    fb->call_state = 1;
    fb->park_fd = WO_PARK_INBOX;
    fb->park_done = 0; /* resume RE-EXECUTES the builtin: the consume path */
    return WO_SYS_PARKED;
}

int wo_vm_actor_monitor(wo_vm *vm, uint64_t watched, uint64_t observer,
                        uint64_t msg_val, const char **msg) {
    wo_actor *w = (wo_actor *)(uintptr_t)watched;
    wo_actor *o = (wo_actor *)(uintptr_t)observer;
    if (!w || !o) {
        *msg = "monitor: nil actor address";
        return WO_T_BOUNDS;
    }
    if (!msg_val) {
        *msg = "monitor: nil notice message";
        return WO_T_BOUNDS;
    }
    /* the registration belongs to the WATCHED actor's home thread */
    if (w->home != vm->shard_id) {
        wo_envelope *e = calloc(1, sizeof *e);
        if (!e) {
            actor_drop_payload(vm, msg_val);
            *msg = "out of memory";
            return WO_T_OOM;
        }
        e->kind = 7;
        e->actor = w;
        e->payload = msg_val;
        e->from_fiber = (wo_fiber *)o; /* reused slot: the observer */
        inbox_push_to(w->home, e);
        return 0;
    }
    if (w->dead) { /* monitoring the dead: the notice fires NOW */
        runtime_notify(vm, o, msg_val, "death notice");
        return 0;
    }
    wo_monitor *m = calloc(1, sizeof *m);
    if (!m) {
        actor_drop_payload(vm, msg_val);
        *msg = "out of memory";
        return WO_T_OOM;
    }
    m->observer = o;
    m->msg = msg_val;
    m->next = w->monitors;
    w->monitors = m;
    return 0;
}

int wo_vm_timer_after(wo_vm *vm, int64_t ms, uint64_t addr, uint64_t msg_val,
                      const char **msg) {
    wo_actor *a = (wo_actor *)(uintptr_t)addr;
    if (!a) {
        *msg = "time.after: nil actor address";
        return WO_T_BOUNDS;
    }
    if (!msg_val) {
        *msg = "time.after: nil message";
        return WO_T_BOUNDS;
    }
    if (ms <= 0) { /* no wait to arm: deliver now */
        runtime_notify(vm, a, msg_val, "timer message");
        return 0;
    }
    wo_timer *t = calloc(1, sizeof *t);
    if (!t) {
        actor_drop_payload(vm, msg_val);
        *msg = "out of memory";
        return WO_T_OOM;
    }
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    t->at = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 + ms;
    t->target = a;
    t->msg = msg_val;
    t->next = vm->timers;
    vm->timers = t;
    return 0;
}

/* runtime-v2 3: the signal drain's delivery path (sysio.c cannot see the
 * static runtime_notify) */
void wo_actor_notify(wo_vm *vm, wo_actor *target, uint64_t payload,
                     const char *what) {
    runtime_notify(vm, target, payload, what);
}

int wo_vm_timers_fire(wo_vm *vm, int64_t now) {
    int fired = 0;
    wo_timer **pp = &vm->timers;
    while (*pp) {
        wo_timer *t = *pp;
        if (t->at <= now) {
            *pp = t->next;
            runtime_notify(vm, t->target, t->msg, "timer message");
            free(t);
            fired++;
        } else {
            pp = &t->next;
        }
    }
    return fired;
}

int64_t wo_vm_timers_next(wo_vm *vm) {
    int64_t next = 0;
    for (wo_timer *t = vm->timers; t; t = t->next)
        if (next == 0 || t->at < next) next = t->at;
    return next;
}

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
    const wo_frame *f = &vm->cur->frames[d - 1];
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
    uint64_t *R = vm->cur->regs + f->base;
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
/* One fiber's frames as GC roots. EVERY fiber — running, queued, parked —
 * pins its values (the arc's rule); vm_gc_roots walks them all. Stage 1
 * Task 1: exactly one fiber exists. */
static void vm_gc_roots_fiber(wo_vm *vm, const wo_fiber *fb) {
    for (uint32_t d = fb->depth; d > 0; d--) {
        const wo_frame *f = &fb->frames[d - 1];
        const wo_methodrec *me = &vm->mod->methods[f->method];
        uint32_t gpc = (d == fb->depth) ? f->pc : f->pc - 1;
        const wo_dropent *ent = vm_dropent(me, gpc);
        if (!ent) continue;
        const uint64_t *R = fb->regs + f->base;
        for (uint32_t r = 0; r < me->reg_cnt; r++) {
            uint64_t bit = 1ull << r;
            if (((ent->gc | ent->owned) & bit) && R[r])
                wo_gc_scan_root(&vm->rt, (wo_hdr *)(uintptr_t)R[r]);
        }
    }
}

static void vm_gc_roots(wo_vm *vm) {
    /* the live fiber plus every queued one; fiber 0 is always one of
     * those two (parked fibers join here in stage 1 Task 4) */
    vm_gc_roots_fiber(vm, vm->cur);
    for (const wo_fiber *fb = vm->qhead; fb; fb = fb->next)
        vm_gc_roots_fiber(vm, fb);
    for (const wo_fiber *fb = vm->parked; fb; fb = fb->pnext)
        vm_gc_roots_fiber(vm, fb);
    /* actors: moved-in state, queued messages, and the in-flight message
     * are runtime-owned — none sits in any frame's masks */
    for (const wo_actor *a = vm->actors; a; a = a->next_all) {
        if (a->instance) wo_gc_scan_root(&vm->rt, (wo_hdr *)(uintptr_t)a->instance);
        for (uint32_t i = 0; i < a->mlen; i++) {
            uint64_t m = a->msgs[(a->mhead + i) % a->mcap].payload;
            if (m) wo_gc_scan_root(&vm->rt, (wo_hdr *)(uintptr_t)m);
        }
        if (a->active && a->active->cur_msg)
            wo_gc_scan_root(&vm->rt, (wo_hdr *)(uintptr_t)a->active->cur_msg);
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
    for (uint32_t d = vm->cur->depth; d > stop_depth; d--) {
        const wo_frame *f = &vm->cur->frames[d - 1];
        vm_release_frame(vm, d, (d == vm->cur->depth) ? f->pc : f->pc - 1, UINT32_MAX);
    }
    vm->cur->depth = stop_depth;
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
    const wo_frame *f = &vm->cur->frames[vm->cur->depth - 1];
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
    vm_fill_err(vm, &vm->cur->caught, tcode, fmt, ap);
    va_end(ap);
    if (vm->cur->ncatch) {
        const wo_catch *c = &vm->cur->catches[vm->cur->ncatch - 1];
        uint32_t cdepth = c->depth;
        uint32_t hpc = c->pc;
        /* Which instruction governs the catching frame's own live set has
         * to be decided before unwinding moves the depth: the trapping
         * instruction when the trap was raised in this very frame, the
         * CALL (pc - 1, the saved pc is the resume point) when it came
         * from deeper. */
        int trapped_here = (cdepth == vm->cur->depth);
        vm->cur->ncatch--;
        /* frames above the catching one die whole */
        vm_unwind(vm, cdepth);
        /* in the catching frame only the try region's own values die: the
         * handler's drop entry names what survives into the catch arm */
        wo_frame *cf = &vm->cur->frames[cdepth - 1];
        vm_release_frame(vm, cdepth, trapped_here ? cf->pc : cf->pc - 1, hpc);
        cf->pc = hpc;
        return 0;
    }
    if (err) *err = vm->cur->caught;
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
        me = &mod->methods[vm->cur->frames[vm->cur->depth - 1].method];     \
        code = me->code;                                          \
        pc = vm->cur->frames[vm->cur->depth - 1].pc;                        \
        R = vm->cur->regs + vm->cur->frames[vm->cur->depth - 1].base;            \
    } while (0)

/* pc is post-incremented at dispatch: the trapping instruction is pc-1.
 * A caught trap (vm_trap == 0) has already unwound to the handler's frame
 * and pointed it at the handler, so the interpreter just reloads and
 * keeps going — the same macro serves both surfaces. */
#define TRAPF(tcode, ...)                                \
    do {                                                 \
        vm->cur->frames[vm->cur->depth - 1].pc = pc - 1;           \
        if (vm_trap(vm, err, tcode, __VA_ARGS__) == 0) { \
            RELOAD();                                    \
            NEXT();                                      \
        }                                                \
        /* uncaught: the fiber's stack is already unwound. Main dying is \
         * the program dying (unchanged); a spawned fiber dies ALONE —   \
         * the report goes to stderr the uncaught-trap way and the       \
         * program lives (the arc's isolation rule). */                  \
        if (vm->cur != &vm->f0) {                        \
            if (err)                                     \
                fprintf(stderr,                          \
                        "wovm: fiber trap %d at %s:%d: %s\n", \
                        err->code, err->method, err->line, err->msg); \
            wo_fiber *dead = vm->cur;                    \
            /* iteration 24: a receive trapping uncaught kills the ACTOR, \
             * not just the fiber — the dead flag, the in-flight caller,  \
             * every queued caller, the state and the mailbox are all     \
             * settled here (before: cur_msg leaked and a->active         \
             * dangled — the actor's mailbox rotted forever). */          \
            if (dead->actor) actor_die(vm, dead->actor, dead);            \
            vm->nfibers--;                               \
            fib_retire(vm, dead);                        \
            NEXT_RUNNABLE();                             \
            RELOAD();                                    \
            NEXT();                                      \
        }                                                \
        fib_reap_all(vm);                                \
        return -1;                                       \
    } while (0)

/* Pick the next runnable fiber; when the queue is empty, wait on the I/O
 * plane for a parked one. A stop interrupting the wait unwinds EVERYTHING
 * and returns 1 (the WO_SYS_STOPPED contract). The queue-and-parked-both-
 * empty case cannot be reached from a live fiber (main is always one of
 * cur/queued/parked). */
#define NEXT_RUNNABLE()                              \
    do {                                             \
        if (INBOX_READY[vm->shard_id % WO_ENG_MAX_SHARDS]) (void)wo_vm_adopt(vm); \
        vm->cur = fib_dequeue(vm);                   \
        while (!vm->cur) {                           \
            if (!vm->is_primary && !vm->parked) {    \
                vm->cur = &vm->f0; /* parked-safe sentinel */ \
                return 2; /* worker drained: back to the serve loop */ \
            }                                        \
            if (!vm->is_primary && eng_shutdown) {   \
                fib_reap_all(vm);                    \
                vm->cur = &vm->f0;                   \
                return 1; /* engine stopping: die clean */ \
            }                                        \
            int iorc_ = wo_io_wait(vm);              \
            if (iorc_ == WO_IO_STOP) {               \
                /* iteration 24: a WORKER on stop keeps DRAINING — its    \
                 * serve loop spins adopting the inbox until the primary  \
                 * finishes the drain window and sets eng_shutdown, so    \
                 * queued shutdown messages (close frames!) still run.    \
                 * Only the PRIMARY's stop ends the program. */           \
                if (!vm->is_primary) {               \
                    vm->cur = &vm->f0;               \
                    return 2;                        \
                }                                    \
                fib_reap_all(vm);                    \
                vm->cur = &vm->f0;                   \
                return 1;                            \
            }                                        \
            if (iorc_ < 0) {                         \
                fib_reap_all(vm);                    \
                vm->cur = &vm->f0;                   \
                if (err) {                           \
                    err->code = WO_T_IO;             \
                    snprintf(err->msg, sizeof err->msg, "I/O plane failed"); \
                }                                    \
                return -1;                           \
            }                                        \
            if (INBOX_READY[vm->shard_id % WO_ENG_MAX_SHARDS]) (void)wo_vm_adopt(vm); \
            vm->cur = fib_dequeue(vm);               \
        }                                            \
        vm->budget = vm->budget0;                    \
    } while (0)

/* Collector safepoint (iteration 7b): placed at allocations, calls, and
 * loop back-edges — the pcs that already carry drop-table entries, so the
 * root snapshot's masks are exact. Costs one predictable branch when the
 * collector is idle and the trigger is cold. */
#define GC_SAFEPOINT()                                                       \
    do {                                                                     \
        if (vm->rt.gc_phase != WO_GC_IDLE || wo_gc_want_start(&vm->rt)) {    \
            vm->cur->frames[vm->cur->depth - 1].pc = pc - 1;                           \
            vm_gc_safepoint(vm);                                             \
        }                                                                    \
    } while (0)

/* Reduction budget (stage 1 Task 2). Checked ONLY at loop back-edges,
 * AFTER the jump has landed, so the saved pc is the loop head and resume
 * makes progress — a pre-instruction save at budget 1 would re-execute
 * the jump, hit the same decrement, and livelock. (Deviation from the
 * spec's "same three sites as the GC": NEW/CALL re-execution has the
 * identical livelock shape; back-edges alone bound every loop, which is
 * what preemption is for. Recorded in the arc plan.) */
#define FIBER_BUDGET()                                       \
    do {                                                     \
        if (--vm->budget <= 0) {                             \
            vm->budget = vm->budget0;                        \
            /* arc stage 3: a busy shard still serves its inbox once per  \
             * slice — bounds a DB request's wait on a computing primary  \
             * to one reduction budget */                    \
            if (INBOX_READY[vm->shard_id % WO_ENG_MAX_SHARDS]) \
                (void)wo_vm_adopt(vm);                       \
            if (vm->qhead) {                                 \
                vm->cur->frames[vm->cur->depth - 1].pc = pc; \
                fib_enqueue(vm, vm->cur);                    \
                vm->cur = fib_dequeue(vm);                   \
                RELOAD();                                    \
                NEXT();                                      \
            }                                                \
        }                                                    \
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
        [WOP_RELEASE_X] = &&L_RELEASE_X, [WOP_BUILTIN] = &&L_BUILTIN,
        [WOP_DB_STUB] = &&L_DB_STUB,   [WOP_TRAP] = &&L_TRAP,
        [WOP_TRY] = &&L_TRY,           [WOP_ENDTRY] = &&L_ENDTRY,
        /* iteration 19: the f64 world */
        [WOP_FADD] = &&L_FADD,         [WOP_FSUB] = &&L_FSUB,
        [WOP_FMUL] = &&L_FMUL,         [WOP_FDIV] = &&L_FDIV,
        [WOP_FNEG] = &&L_FNEG,         [WOP_FEQ] = &&L_FEQ,
        [WOP_FLT] = &&L_FLT,           [WOP_FLE] = &&L_FLE,
        /* iteration 36 (v6): the Int bitwise set */
        [WOP_BAND] = &&L_BAND,         [WOP_BOR] = &&L_BOR,
        [WOP_BXOR] = &&L_BXOR,         [WOP_SHL] = &&L_SHL,
        [WOP_SHR] = &&L_SHR,
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
        /* WOB_K_TEXT is the only pointer-shaped constant; INT and (iteration
         * 19) FLOAT both live in the same word, differing only in how the
         * ops that read them interpret it. */
        R[wo_ins_a(ins)] = k->tag == WOB_K_TEXT ? (uint64_t)(uintptr_t)k->s
                                                : (uint64_t)k->i;
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

    /* iteration 36 (v6): Int bitwise. AND/OR/XOR are sign-agnostic on
     * the u64 register word. SHL shifts the unsigned word (wrapping,
     * like ADD); SHR casts to int64_t first — ARITHMETIC, the sign bit
     * extends (gcc/clang define signed >> as arithmetic, the only
     * compilers this runtime targets). A count outside 0..63 traps
     * WO_T_SHIFT rather than silently masking like the hardware would;
     * literal counts were rejected at compile time (WO-E223). */
    CASE(BAND) : {
        R[wo_ins_a(ins)] = R[wo_ins_b(ins)] & R[wo_ins_c(ins)];
        NEXT();
    }
    CASE(BOR) : {
        R[wo_ins_a(ins)] = R[wo_ins_b(ins)] | R[wo_ins_c(ins)];
        NEXT();
    }
    CASE(BXOR) : {
        R[wo_ins_a(ins)] = R[wo_ins_b(ins)] ^ R[wo_ins_c(ins)];
        NEXT();
    }
    CASE(SHL) : {
        int64_t s = (int64_t)R[wo_ins_c(ins)];
        if (s < 0 || s > 63) TRAPF(WO_T_SHIFT, "shift count out of range 0..63");
        R[wo_ins_a(ins)] = R[wo_ins_b(ins)] << s;
        NEXT();
    }
    CASE(SHR) : {
        int64_t s = (int64_t)R[wo_ins_c(ins)];
        if (s < 0 || s > 63) TRAPF(WO_T_SHIFT, "shift count out of range 0..63");
        R[wo_ins_a(ins)] = (uint64_t)((int64_t)R[wo_ins_b(ins)] >> s);
        NEXT();
    }

    /* iteration 19: f64 arithmetic. Registers are u64, so each op bitcasts in
     * and out (wo_f64/wo_bits — memcpy-based, the only strict-aliasing-clean
     * way). Nothing here traps: IEEE 754 quiet semantics are the contract, so
     * x/0.0 yields ±Inf and 0.0/0.0 yields NaN instead of raising. The FPU's
     * own exception flags are left alone — the language never reads them. */
    CASE(FADD) : {
        R[wo_ins_a(ins)] = wo_bits(wo_f64(R[wo_ins_b(ins)]) + wo_f64(R[wo_ins_c(ins)]));
        NEXT();
    }
    CASE(FSUB) : {
        R[wo_ins_a(ins)] = wo_bits(wo_f64(R[wo_ins_b(ins)]) - wo_f64(R[wo_ins_c(ins)]));
        NEXT();
    }
    CASE(FMUL) : {
        R[wo_ins_a(ins)] = wo_bits(wo_f64(R[wo_ins_b(ins)]) * wo_f64(R[wo_ins_c(ins)]));
        NEXT();
    }
    CASE(FDIV) : {
        R[wo_ins_a(ins)] = wo_bits(wo_f64(R[wo_ins_b(ins)]) / wo_f64(R[wo_ins_c(ins)]));
        NEXT();
    }
    CASE(FNEG) : {
        /* sign flip, not 0.0 - x: only this reaches -0.0 from +0.0, and the
         * iteration's edge-case gate stores -0.0 and reads it back. */
        R[wo_ins_a(ins)] = wo_bits(-wo_f64(R[wo_ins_b(ins)]));
        NEXT();
    }
    /* IEEE comparisons, NOT the total order: every one of these is false when
     * either side is NaN, which is what makes `NaN != NaN` true in the
     * language. Indexes and order-by need a total order instead and call
     * WO_B_FLOAT_CMP for it. */
    CASE(FEQ) : {
        R[wo_ins_a(ins)] = wo_f64(R[wo_ins_b(ins)]) == wo_f64(R[wo_ins_c(ins)]) ? 1 : 0;
        NEXT();
    }
    CASE(FLT) : {
        R[wo_ins_a(ins)] = wo_f64(R[wo_ins_b(ins)]) < wo_f64(R[wo_ins_c(ins)]) ? 1 : 0;
        NEXT();
    }
    CASE(FLE) : {
        R[wo_ins_a(ins)] = wo_f64(R[wo_ins_b(ins)]) <= wo_f64(R[wo_ins_c(ins)]) ? 1 : 0;
        NEXT();
    }

    CASE(JMP) : {
        if (wo_ins_sbx(ins) < 0) {
            GC_SAFEPOINT(); /* loop back-edge */
            pc = (uint32_t)((int64_t)pc + wo_ins_sbx(ins));
            FIBER_BUDGET(); /* after the jump lands: resume = the loop head */
            NEXT();
        }
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
        uint32_t nbase = vm->cur->frames[vm->cur->depth - 1].base + wo_ins_a(ins);
        if (vm->cur->depth >= WO_MAX_FRAMES)
            TRAPF(WO_T_STACK, "frame stack overflow (%u frames)",
                  (unsigned)WO_MAX_FRAMES);
        if (nbase + callee->reg_cnt > WO_STACK_SLOTS)
            TRAPF(WO_T_STACK, "value stack overflow");
        vm->cur->frames[vm->cur->depth - 1].pc = pc;
        vm->cur->frames[vm->cur->depth].method = wo_ins_bx(ins);
        vm->cur->frames[vm->cur->depth].pc = 0;
        vm->cur->frames[vm->cur->depth].base = nbase;
        vm->cur->depth++;
        /* zero non-argument registers: drop masks must never see stale bits */
        memset(vm->cur->regs + nbase + callee->arg_cnt, 0,
               (size_t)(callee->reg_cnt - callee->arg_cnt) * 8u);
        RELOAD();
        NEXT();
    }

/* A frame leaving takes its still-open try regions with it: a `return`
 * out of a try region never runs its ENDTRY, and a handler pc in a frame
 * that no longer exists would land the next trap on a dead window. */
#define DROP_CATCHES()                                                  \
    while (vm->cur->ncatch && vm->cur->catches[vm->cur->ncatch - 1].depth > vm->cur->depth) \
        vm->cur->ncatch--


/* A fiber's last frame returned. Main ending IS the program ending: every
 * other fiber unwinds through its drop maps (clean, ASan-proven) and the
 * program's value is main's. A spawned fiber ending just leaves the
 * scheduler; its return value is discarded (the spawn surface's entry
 * wrapper returns nothing owned — Task 3's contract). The queue cannot be
 * empty when a spawned fiber ends: main never parks in stage 1, so it is
 * either live or queued. */
#define FIBER_DONE(rv)                                                    \
    do {                                                                  \
        if (vm->cur == &vm->f0) {                                         \
            fib_reap_all(vm);                                             \
            *ret = (rv);                                                  \
            return 0;                                                     \
        }                                                                 \
        wo_fiber *dead = vm->cur;                                         \
        if (dead->actor) {                                                \
            wo_actor *a = dead->actor;                                    \
            if (dead->cur_msg) {                                          \
                wo_drop_obj(&vm->rt, (wo_hdr *)(uintptr_t)dead->cur_msg); \
                dead->cur_msg = 0;                                        \
            }                                                             \
            /* iteration 24: the receive's return value IS the reply —   \
             * ship it before this context is reused or freed */         \
            call_reply_to(vm, dead->msg_caller, dead->msg_caller_shard,  \
                          (rv), 0);                                       \
            dead->msg_caller = NULL;                                     \
            if (a->mlen) {                                                \
                /* next message: REUSE this context, re-queued for       \
                 * fairness (one message per turn, never a monopolist) */ \
                wo_msg m_ = actor_pop(a);                                 \
                const wo_methodrec *sme_ = &vm->mod->methods[a->method];  \
                dead->depth = 1;                                          \
                dead->ncatch = 0;                                         \
                dead->frames[0].method = a->method;                       \
                dead->frames[0].pc = 0;                                   \
                dead->frames[0].base = 0;                                 \
                dead->regs[0] = a->instance;                              \
                dead->regs[1] = m_.payload;                               \
                memset(dead->regs + 2, 0, (size_t)(sme_->reg_cnt - 2) * 8u); \
                dead->cur_msg = m_.payload;                               \
                dead->msg_caller = m_.caller;                             \
                dead->msg_caller_shard = m_.caller_shard;                 \
                fib_enqueue(vm, dead);                                    \
                NEXT_RUNNABLE();                                          \
                RELOAD();                                                 \
                NEXT();                                                   \
            }                                                             \
            a->active = NULL;                                             \
        }                                                                 \
        vm->nfibers--;                                                    \
        fib_retire(vm, dead);                                             \
        NEXT_RUNNABLE();                                                  \
        RELOAD();                                                         \
        NEXT();                                                           \
    } while (0)

    CASE(RET) : {
        uint64_t rv = R[wo_ins_a(ins)];
        vm->cur->regs[vm->cur->frames[vm->cur->depth - 1].base] = rv;
        vm->cur->depth--;
        DROP_CATCHES();
        if (vm->cur->depth == 0) FIBER_DONE(rv);
        RELOAD();
        NEXT();
    }
    CASE(RET0) : {
        vm->cur->regs[vm->cur->frames[vm->cur->depth - 1].base] = 0;
        vm->cur->depth--;
        DROP_CATCHES();
        if (vm->cur->depth == 0) FIBER_DONE(0);
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
        if (brc == WO_SYS_PARKED) {
            /* arc T4: the builtin filled cur->park_*. Resume either
             * RE-EXECUTES it (park_done=0: fd readiness — accept/read/
             * write retry against a now-ready fd) or continues PAST it
             * (park_done=1: sleep — result preset before parking). */
            vm->cur->frames[vm->cur->depth - 1].pc = vm->cur->park_done ? pc : pc - 1;
            wo_fiber *pk = vm->cur;
            if (wo_io_arm(vm, pk) != 0) {
                pk->state = WO_FIB_RUNNABLE;
                TRAPF(WO_T_IO, "%s", "cannot arm the I/O wait");
            }
            NEXT_RUNNABLE();
            RELOAD();
            NEXT();
        }
        if (brc == WO_SYS_STOPPED) {
            vm->cur->frames[vm->cur->depth - 1].pc = pc - 1;
            vm->cur->ncatch = 0;
            vm_unwind(vm, 0);
            /* iteration 24 (the drain): a STOPPED wait on a NON-main fiber
             * unwinds that fiber ALONE — the rest of the program (main's
             * drain code, actors flushing close frames) keeps running.
             * Main's own STOPPED still ends the program, as ever. */
            if (vm->cur != &vm->f0) {
                wo_fiber *dead = vm->cur;
                if (dead->actor) {
                    wo_actor *da = dead->actor;
                    if (dead->cur_msg) {
                        wo_drop_obj(&vm->rt, (wo_hdr *)(uintptr_t)dead->cur_msg);
                        dead->cur_msg = 0;
                    }
                    call_reply_to(vm, dead->msg_caller, dead->msg_caller_shard,
                                  0, WO_T_ACTOR);
                    dead->msg_caller = NULL;
                    da->active = NULL;
                }
                vm->nfibers--;
                fib_retire(vm, dead);
                NEXT_RUNNABLE();
                RELOAD();
                NEXT();
            }
            /* main: a stop ends the PROGRAM — every remaining fiber
             * unwinds clean */
            if (vm->cur != &vm->f0) {
                wo_fiber *dead = vm->cur;
                vm->cur = &vm->f0;
                vm->nfibers--;
                fib_retire(vm, dead);
                if (vm->f0.depth) {
                    /* main was queued mid-run: release its frames too */
                    wo_fiber *q = vm->qhead, *prev = NULL;
                    while (q && q != &vm->f0) { prev = q; q = q->next; }
                    if (q) { /* unlink f0 from the queue */
                        if (prev) prev->next = q->next; else vm->qhead = q->next;
                        if (vm->qtail == q) vm->qtail = prev;
                        vm_unwind(vm, 0);
                    }
                }
            }
            fib_reap_all(vm);
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
        uint32_t nbase = vm->cur->frames[vm->cur->depth - 1].base + wo_ins_a(ins);
        if (vm->cur->depth >= WO_MAX_FRAMES)
            TRAPF(WO_T_STACK, "frame stack overflow (%u frames)",
                  (unsigned)WO_MAX_FRAMES);
        if (nbase + callee->reg_cnt > WO_STACK_SLOTS)
            TRAPF(WO_T_STACK, "value stack overflow");
        vm->cur->frames[vm->cur->depth - 1].pc = pc;
        vm->cur->frames[vm->cur->depth].method = hit->method;
        vm->cur->frames[vm->cur->depth].pc = 0;
        vm->cur->frames[vm->cur->depth].base = nbase;
        vm->cur->depth++;
        memset(vm->cur->regs + nbase + callee->arg_cnt, 0,
               (size_t)(callee->reg_cnt - callee->arg_cnt) * 8u);
        RELOAD();
        NEXT();
    }

    CASE(DB_STUB) : { TRAPF(WO_T_DB, "engine not linked"); }

    CASE(TRAP) : { TRAPF(wo_ins_bx(ins), "explicit trap"); }

    CASE(TRY) : {
        if (vm->cur->ncatch >= WO_MAX_CATCH)
            TRAPF(WO_T_STACK, "catch stack overflow (%u regions)",
                  (unsigned)WO_MAX_CATCH);
        vm->cur->catches[vm->cur->ncatch].depth = vm->cur->depth;
        vm->cur->catches[vm->cur->ncatch].pc = (uint32_t)((int64_t)pc + wo_ins_sbx(ins));
        vm->cur->catches[vm->cur->ncatch].reg = wo_ins_a(ins);
        vm->cur->ncatch++;
        NEXT();
    }
    CASE(ENDTRY) : {
        /* the try region completed without trapping. Defensive on an
         * unpaired ENDTRY (a miscompile the loader cannot see): pop
         * nothing rather than corrupt the stack. */
        if (vm->cur->ncatch) vm->cur->ncatch--;
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
#undef FIBER_BUDGET
#undef DROP_CATCHES
}

/* the worker flavor of wo_vm_call: no entry frame — run whatever the run
 * queue holds (adopted fibers, actor deliveries) until drained (rc 2),
 * stopped (1), or a fatal error (-1). */
int wo_vm_serve(wo_vm *vm) {
    tls_vm = vm;
    /* arc stage 3 obligation: a worker NEVER holds the engine or the WAL —
     * its DB statements marshal to shard 0 (wo_db_rpc). Replay finished on
     * the primary before wo_engine_start spawned this thread. */
    assert(!vm->rt.db && !vm->rt.wal);
    if (!vm->qhead) return 2;
    vm->cur = fib_dequeue(vm);
    vm->budget = vm->budget0;
    uint64_t ret = 0;
    wo_err err;
    return vm_run(vm, &ret, &err);
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
    vm->cur->depth = 1;
    vm->cur->ncatch = 0; /* catch regions never survive a call boundary */
    vm->cur->frames[0].method = method_idx;
    vm->cur->frames[0].pc = 0;
    vm->cur->frames[0].base = 0;
    if (argc) memcpy(vm->cur->regs, args, (size_t)argc * 8u);
    memset(vm->cur->regs + argc, 0, (size_t)(me->reg_cnt - argc) * 8u);
    return vm_run(vm, ret, err);
}
