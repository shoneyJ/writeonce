/* park.h — the per-shard I/O plane (the 8+11 arc, stage 1 Task 4).
 *
 * One event loop per shard, io_uring-FIRST (developer directive; the linux
 * reference project's "single event loop" card): parked fibers wait as
 * POLL_ADD (fd readiness — resume RE-EXECUTES the now-ready builtin) or
 * TIMEOUT (sleep — resume continues PAST the builtin) submissions on a raw
 * ring. epoll is the PORTABILITY fallback, selected by a startup probe
 * (seccomp'd containers routinely deny io_uring) or forced with
 * WO_IO=uring|epoll so CI proves both paths on one kernel.
 *
 * Raw io_uring_setup/io_uring_enter syscalls, libc-only — struct layouts
 * mirrored from include/uapi/linux/io_uring.h. Ops restricted to the
 * TIMEOUT floor (Linux 5.4); multishot variants are recorded future work.
 */
#ifndef WO_PARK_H
#define WO_PARK_H

#include "vm.h"

/* Probe (or WO_IO-force) the backend. 0 ok; nonzero = no backend (fatal). */
int wo_io_init(wo_vm *vm);
void wo_io_destroy(wo_vm *vm);

/* Register the just-parked fiber's wait (fb->park_* already filled by the
 * builtin). 0 ok; -1 = arming failed (caller traps the builtin as IO). */
int wo_io_arm(wo_vm *vm, wo_fiber *fb);

/* Block until at least one parked fiber wakes; woken fibers move to the
 * run queue. 0 = something woke; WO_IO_STOP = the stop flag interrupted
 * the wait (caller unwinds everything); -1 = fatal backend error. */
#define WO_IO_STOP (-2)
int wo_io_wait(wo_vm *vm);

#endif /* WO_PARK_H */
