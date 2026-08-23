/* park.c — the per-shard I/O plane (arc stage 1 Task 4). See park.h.
 *
 * The uring structs below mirror include/uapi/linux/io_uring.h exactly
 * (the fields this file touches; trailing space is padded to the kernel's
 * sizes). The layout is part of the kernel ABI — stable by contract. */
#define _GNU_SOURCE /* syscall(), struct layouts */
#include "park.h"

#include <errno.h>
#include <stdio.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "builtin.h"

/* ---- raw io_uring ABI (uapi mirror, the subset used) ------------------ */

struct io_sqring_offsets {
    uint32_t head, tail, ring_mask, ring_entries, flags, dropped, array;
    uint32_t resv1;
    uint64_t user_addr;
};
struct io_cqring_offsets {
    uint32_t head, tail, ring_mask, ring_entries, overflow, cqes, flags;
    uint32_t resv1;
    uint64_t user_addr;
};
struct io_uring_params {
    uint32_t sq_entries, cq_entries, flags, sq_thread_cpu, sq_thread_idle;
    uint32_t features, wq_fd, resv[3];
    struct io_sqring_offsets sq_off;
    struct io_cqring_offsets cq_off;
};
struct io_uring_sqe {
    uint8_t opcode, flags;
    uint16_t ioprio;
    int32_t fd;
    uint64_t off, addr;
    uint32_t len;
    union {
        uint32_t poll32_events; /* POLL_ADD (little-endian u32 of poll mask) */
        uint32_t timeout_flags; /* TIMEOUT */
        uint32_t rw_flags;
    };
    uint64_t user_data;
    uint64_t pad2[3];
};
struct io_uring_cqe {
    uint64_t user_data;
    int32_t res;
    uint32_t flags;
};

#define IORING_OP_POLL_ADD 6
#define IORING_OP_TIMEOUT 11
#define IORING_ENTER_GETEVENTS 1u
#define IORING_OFF_SQ_RING 0ULL
#define IORING_OFF_CQ_RING 0x8000000ULL
#define IORING_OFF_SQES 0x10000000ULL

/* the sq/cq ring pointers, resolved once from the params offsets */
typedef struct {
    uint32_t *sq_head, *sq_tail, *sq_mask, *sq_array;
    uint32_t *cq_head, *cq_tail, *cq_mask;
    struct io_uring_cqe *cqes;
    struct io_uring_sqe *sqes;
} rings;

/* Ring params live INSIDE each vm (vm.h io_params, opaque bytes) — arc
 * stage 3 fix: this WAS one shared static ("offsets survive init"), and a
 * worker's LAZY uring_init memset+refilled it on the worker thread while
 * another shard was reading ring offsets out of it — submits landed at
 * garbage offsets, the kernel saw no sqe (enter returned 0, treated as
 * ok), and a parked fiber's TIMEOUT silently never existed. Per-vm storage
 * ends the race by construction (a vm's params are only ever touched by
 * its own thread); the short-submit check below turns any relapse into a
 * loud trap instead of a lost wake. NOTE: not indexed by shard_id — the
 * lazy wo_vm_init runs while the worker's shard_id is transiently 0. */
_Static_assert(sizeof(struct io_uring_params) <= sizeof(((wo_vm *)0)->io_params),
               "io_params too small");

static rings ring_ptrs(const wo_vm *vm) {
    const struct io_uring_params *p = (const struct io_uring_params *)vm->io_params;
    rings r;
    uint8_t *sq = (uint8_t *)vm->io_sq, *cq = (uint8_t *)vm->io_cq;
    r.sq_head = (uint32_t *)(sq + p->sq_off.head);
    r.sq_tail = (uint32_t *)(sq + p->sq_off.tail);
    r.sq_mask = (uint32_t *)(sq + p->sq_off.ring_mask);
    r.sq_array = (uint32_t *)(sq + p->sq_off.array);
    r.cq_head = (uint32_t *)(cq + p->cq_off.head);
    r.cq_tail = (uint32_t *)(cq + p->cq_off.tail);
    r.cq_mask = (uint32_t *)(cq + p->cq_off.ring_mask);
    r.cqes = (struct io_uring_cqe *)(cq + p->cq_off.cqes);
    r.sqes = (struct io_uring_sqe *)vm->io_sqes;
    return r;
}

static int uring_init(wo_vm *vm) {
    struct io_uring_params *p = (struct io_uring_params *)vm->io_params;
    memset(p, 0, sizeof *p);
    long fd = syscall(SYS_io_uring_setup, 64u, p);
    if (fd < 0) return -1;
    size_t sq_len = p->sq_off.array + p->sq_entries * sizeof(uint32_t);
    size_t cq_len = p->cq_off.cqes + p->cq_entries * sizeof(struct io_uring_cqe);
    size_t sqes_len = p->sq_entries * sizeof(struct io_uring_sqe);
    void *sq = mmap(NULL, sq_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, (int)fd,
                    IORING_OFF_SQ_RING);
    void *cq = mmap(NULL, cq_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, (int)fd,
                    IORING_OFF_CQ_RING);
    void *sqes = mmap(NULL, sqes_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, (int)fd,
                      IORING_OFF_SQES);
    if (sq == MAP_FAILED || cq == MAP_FAILED || sqes == MAP_FAILED) {
        if (sq != MAP_FAILED) munmap(sq, sq_len);
        if (cq != MAP_FAILED) munmap(cq, cq_len);
        if (sqes != MAP_FAILED) munmap(sqes, sqes_len);
        close((int)fd);
        return -1;
    }
    vm->io_kind = 0;
    vm->io_fd = (int)fd;
    vm->io_sq = sq;
    vm->io_cq = cq;
    vm->io_sqes = sqes;
    vm->io_sq_len = sq_len;
    vm->io_cq_len = cq_len;
    vm->io_sqes_len = sqes_len;
    return 0;
}

static int uring_submit(wo_vm *vm, const struct io_uring_sqe *sqe) {
    rings r = ring_ptrs(vm);
    uint32_t tail = *r.sq_tail;
    uint32_t idx = tail & *r.sq_mask;
    r.sqes[idx] = *sqe;
    r.sq_array[idx] = idx;
    __atomic_store_n(r.sq_tail, tail + 1, __ATOMIC_RELEASE);
    long rc = syscall(SYS_io_uring_enter, vm->io_fd, 1u, 0u, 0u, NULL, 0);
    /* a short submit is a LOST WAKE, never a success (the g_params race
     * above hid behind rc >= 0 for a whole debugging session) */
    return rc == 1 ? 0 : -1;
}

/* ---- backend-neutral helpers ------------------------------------------ */

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void parked_unlink(wo_vm *vm, wo_fiber *fb) {
    wo_fiber **pp = &vm->parked;
    while (*pp && *pp != fb) pp = &(*pp)->pnext;
    if (*pp) {
        *pp = fb->pnext;
        fb->pnext = NULL;
        vm->nparked--;
    }
}

/* wake: parked -> run queue (fib_enqueue lives in vm.c; the tiny mirror
 * here keeps park.c free of vm.c internals) */
static void wake(wo_vm *vm, wo_fiber *fb) {
    parked_unlink(vm, fb);
    fb->state = WO_FIB_RUNNABLE;
    fb->next = NULL;
    if (vm->qtail) vm->qtail->next = fb;
    else vm->qhead = fb;
    vm->qtail = fb;
}

void wo_io_unpark(wo_vm *vm, wo_fiber *fb) {
    if (fb->state == WO_FIB_PARKED) wake(vm, fb);
}

/* ---- API --------------------------------------------------------------- */

int wo_io_init(wo_vm *vm) {
    const char *force = getenv("WO_IO");
    vm->io_fd = -1;
    if (!force || strcmp(force, "epoll") != 0) {
        if (uring_init(vm) == 0) return 0;
        if (force && strcmp(force, "uring") == 0) return -1; /* forced, absent */
    }
    int ep = epoll_create1(0);
    if (ep < 0) return -1;
    vm->io_kind = 1;
    vm->io_fd = ep;
    return 0;
}

void wo_io_destroy(wo_vm *vm) {
    if (vm->io_fd < 0) return;
    if (vm->io_kind == 0) {
        munmap(vm->io_sq, vm->io_sq_len);
        munmap(vm->io_cq, vm->io_cq_len);
        munmap(vm->io_sqes, vm->io_sqes_len);
    }
    close(vm->io_fd);
    vm->io_fd = -1;
}

int wo_io_arm(wo_vm *vm, wo_fiber *fb) {
    fb->state = WO_FIB_PARKED;
    fb->pnext = vm->parked;
    vm->parked = fb;
    vm->nparked++;
    /* arc stage 3: an inbox-wait fiber holds no plane wait at all — the
     * wake is wo_io_unpark from the envelope drain */
    if (fb->park_fd == WO_PARK_INBOX) return 0;
    if (vm->io_kind == 0) {
        struct io_uring_sqe sqe;
        memset(&sqe, 0, sizeof sqe);
        sqe.user_data = (uint64_t)(uintptr_t)fb;
        if (fb->park_fd >= 0) {
            sqe.opcode = IORING_OP_POLL_ADD;
            sqe.fd = fb->park_fd;
            sqe.poll32_events = (uint32_t)(uint16_t)fb->park_events;
        } else {
            int64_t rel = fb->park_deadline - now_ms();
            if (rel < 0) rel = 0;
            fb->park_ts.sec = rel / 1000;
            fb->park_ts.nsec = (rel % 1000) * 1000000LL;
            sqe.opcode = IORING_OP_TIMEOUT;
            sqe.fd = -1;
            sqe.addr = (uint64_t)(uintptr_t)&fb->park_ts;
            sqe.len = 1;
        }
        if (uring_submit(vm, &sqe) != 0) {
            parked_unlink(vm, fb);
            return -1;
        }
        return 0;
    }
    /* epoll fallback: fds registered oneshot; deadlines live on the parked
     * list and become the wait timeout */
    if (fb->park_fd >= 0) {
        struct epoll_event ev;
        memset(&ev, 0, sizeof ev);
        ev.events = (uint32_t)(uint16_t)fb->park_events | EPOLLONESHOT;
        ev.data.ptr = fb;
        if (epoll_ctl(vm->io_fd, EPOLL_CTL_ADD, fb->park_fd, &ev) != 0
            && (errno != EEXIST || epoll_ctl(vm->io_fd, EPOLL_CTL_MOD, fb->park_fd, &ev) != 0)) {
            parked_unlink(vm, fb);
            return -1;
        }
    }
    return 0;
}

/* user_data sentinel for the wake-eventfd's own readiness (fibers are
 * heap pointers, never 1) */
#define EFD_SENTINEL 1ull
/* iteration 35: the shard deadline tick (one TIMEOUT op armed for the
 * nearest fd-park deadline) and the tombstone POLL_REMOVE's own CQE.
 * Sentinels, never pointers — a late completion can never dangle. */
#define TICK_SENTINEL 2ull
#define CANCEL_SENTINEL 3ull
#define IORING_OP_POLL_REMOVE 7

/* iteration 35: wake every fd-park whose deadline passed and tombstone
 * its POLL op (the resumed builtin answers nil — the timeout result).
 * The removed poll's CQE (-ECANCELED, user_data = the fiber) arrives
 * later and is ignored: the fiber is RUNNABLE by then, and even a
 * recycled fiber just takes a benign spurious wake (the park protocol
 * re-executes the builtin, which re-checks). Returns woke-count. */
static int deadline_sweep_uring(wo_vm *vm, int64_t now) {
    int woke = 0;
    for (wo_fiber *fb = vm->parked; fb; fb = fb->pnext) {
        if (fb->state == WO_FIB_PARKED && fb->park_fd >= 0
            && fb->park_deadline > 0 && fb->park_deadline <= now) {
            struct io_uring_sqe sqe;
            memset(&sqe, 0, sizeof sqe);
            sqe.opcode = IORING_OP_POLL_REMOVE;
            sqe.fd = -1;
            sqe.addr = (uint64_t)(uintptr_t)fb; /* match the poll's user_data */
            sqe.user_data = CANCEL_SENTINEL;
            (void)uring_submit(vm, &sqe);
            wake(vm, fb);
            woke++;
        }
    }
    return woke;
}

/* Arm (or re-arm) the tick for the nearest fd-park deadline. Cheap
 * over-arming is fine: a tick firing with nothing expired just re-arms. */
static void tick_arm_uring(wo_vm *vm, int64_t now) {
    int64_t next = 0;
    for (wo_fiber *fb = vm->parked; fb; fb = fb->pnext)
        if (fb->state == WO_FIB_PARKED && fb->park_fd >= 0 && fb->park_deadline > 0)
            if (next == 0 || fb->park_deadline < next) next = fb->park_deadline;
    int64_t tn = wo_vm_timers_next(vm); /* iteration 24 T5: armed timers */
    if (tn > 0 && (next == 0 || tn < next)) next = tn;
    if (next == 0) return;
    if (vm->tick_armed && vm->tick_at <= next) return;
    int64_t rel = next - now;
    if (rel < 0) rel = 0;
    vm->tick_ts.sec = rel / 1000;
    vm->tick_ts.nsec = (rel % 1000) * 1000000LL;
    struct io_uring_sqe sqe;
    memset(&sqe, 0, sizeof sqe);
    sqe.opcode = IORING_OP_TIMEOUT;
    sqe.fd = -1;
    sqe.addr = (uint64_t)(uintptr_t)&vm->tick_ts;
    sqe.len = 1;
    sqe.user_data = TICK_SENTINEL;
    if (uring_submit(vm, &sqe) == 0) {
        vm->tick_armed = 1;
        vm->tick_at = next;
    }
}

static void efd_drain(wo_vm *vm) {
    uint64_t v = 0;
    ssize_t n = read(vm->wake_efd, &v, sizeof v);
    (void)n;
}

int wo_io_wait(wo_vm *vm) {
    for (;;) {
        if (wo_sys_stop_pending()) return WO_IO_STOP;
        if (vm->io_kind == 0) {
            /* keep the wake eventfd armed (oneshot POLL_ADD, re-armed
             * after each firing) so inbox pushes interrupt the wait */
            if (vm->wake_efd >= 0 && !vm->efd_armed) {
                struct io_uring_sqe sqe;
                memset(&sqe, 0, sizeof sqe);
                sqe.opcode = IORING_OP_POLL_ADD;
                sqe.fd = vm->wake_efd;
                sqe.poll32_events = POLLIN;
                sqe.user_data = EFD_SENTINEL;
                if (uring_submit(vm, &sqe) == 0) vm->efd_armed = 1;
            }
            tick_arm_uring(vm, now_ms());
            rings r = ring_ptrs(vm);
            uint32_t head = *r.cq_head;
            uint32_t tail = __atomic_load_n(r.cq_tail, __ATOMIC_ACQUIRE);
            if (head == tail) {
                long rc = syscall(SYS_io_uring_enter, vm->io_fd, 0u, 1u,
                                  IORING_ENTER_GETEVENTS, NULL, 0);
                if (rc < 0 && errno == EINTR) continue; /* stop checked on loop */
                if (rc < 0) {
                    fprintf(stderr, "DBG uring_enter shard=%u errno=%d\n", vm->shard_id, errno);
                    return -1;
                }
                tail = __atomic_load_n(r.cq_tail, __ATOMIC_ACQUIRE);
            }
            int woke = 0;
            while (head != tail) {
                struct io_uring_cqe *cqe = &r.cqes[head & *r.cq_mask];
                if (cqe->user_data == EFD_SENTINEL) {
                    vm->efd_armed = 0;
                    efd_drain(vm);
                    woke = 2; /* inbox wake: the caller adopts */
                } else if (cqe->user_data == TICK_SENTINEL) {
                    vm->tick_armed = 0; /* the sweep below decides who expired */
                } else if (cqe->user_data == CANCEL_SENTINEL) {
                    /* the tombstone's own completion: nothing to do */
                } else {
                    wo_fiber *fb = (wo_fiber *)(uintptr_t)cqe->user_data;
                    if (fb && fb->state == WO_FIB_PARKED) {
                        wake(vm, fb);
                        if (!woke) woke = 1;
                    }
                }
                head++;
            }
            __atomic_store_n(r.cq_head, head, __ATOMIC_RELEASE);
            int64_t swnow = now_ms();
            if (wo_vm_timers_fire(vm, swnow) && woke != 2) woke = 1;
            if (deadline_sweep_uring(vm, swnow) && woke != 2) woke = 1;
            if (woke == 2) return 1; /* adopt-needed */
            if (woke) return 0;
            continue;
        }
        /* epoll: the wake eventfd is registered once, level-triggered
         * (data.ptr NULL = the sentinel) */
        if (vm->wake_efd >= 0 && !vm->efd_armed) {
            struct epoll_event ev;
            memset(&ev, 0, sizeof ev);
            ev.events = EPOLLIN;
            ev.data.ptr = NULL;
            if (epoll_ctl(vm->io_fd, EPOLL_CTL_ADD, vm->wake_efd, &ev) == 0
                || errno == EEXIST)
                vm->efd_armed = 1;
        }
        int timeout = -1;
        int64_t now = now_ms();
        {
            int64_t tn = wo_vm_timers_next(vm); /* iteration 24 T5 */
            if (tn > 0) {
                int64_t rel = tn - now;
                if (rel < 0) rel = 0;
                timeout = (int)rel;
            }
        }
        for (wo_fiber *fb = vm->parked; fb; fb = fb->pnext)
            if (fb->park_fd == -1
                || (fb->park_fd >= 0 && fb->park_deadline > 0)) {
                /* sleeps AND deadline'd fd-parks (iteration 35); never INBOX */
                int64_t rel = fb->park_deadline - now;
                if (rel < 0) rel = 0;
                if (timeout < 0 || rel < timeout) timeout = (int)rel;
            }
        struct epoll_event evs[16];
        int n = epoll_wait(vm->io_fd, evs, 16, timeout);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            fprintf(stderr, "DBG epoll_wait shard=%u errno=%d\n", vm->shard_id, errno);
            return -1;
        }
        int woke = 0;
        for (int i = 0; i < n; i++) {
            wo_fiber *fb = (wo_fiber *)evs[i].data.ptr;
            if (!fb) { /* the wake eventfd: adopt-needed */
                efd_drain(vm);
                woke = 2;
            } else if (fb->state == WO_FIB_PARKED) {
                epoll_ctl(vm->io_fd, EPOLL_CTL_DEL, fb->park_fd, NULL);
                wake(vm, fb);
                if (!woke) woke = 1;
            }
        }
        now = now_ms();
        if (wo_vm_timers_fire(vm, now)) woke = 1;
        wo_fiber *fb = vm->parked;
        while (fb) {
            wo_fiber *nx = fb->pnext;
            if ((fb->park_fd == -1
                 || (fb->park_fd >= 0 && fb->park_deadline > 0))
                && fb->park_deadline <= now) {
                if (fb->park_fd >= 0)
                    epoll_ctl(vm->io_fd, EPOLL_CTL_DEL, fb->park_fd, NULL);
                wake(vm, fb);
                woke = 1;
            }
            fb = nx;
        }
        if (woke == 2) return 1; /* adopt-needed */
        if (woke) return 0;
    }
}
