//! Single-threaded event loop on Linux kernel primitives.
//!
//! Phase 02 of the runtime plan — see `docs/plan/02-event-loop-epoll.md`.
//! Wraps `epoll`, `eventfd`, `timerfd`, and `signalfd` directly via `libc`,
//! with no `tokio` / `mio` / `nix` involvement. Every fd is registered
//! edge-triggered (`EPOLLET`); the loop reads to `EAGAIN`.
//!
//! Lives alongside the tokio-backed `server` module until phase 04 cuts
//! the binary over.
//!
//! Filename convention mirrors Go's `src/runtime/netpoll_<flavour>.go`;
//! when phase 03 adds io_uring it lands as `netpoll_io_uring.rs` next door.

mod eventfd;
mod netpoll_epoll;
mod signalfd;
mod timerfd;

pub use eventfd::EventFd;
pub use netpoll_epoll::{Event, EventLoop, Interest, Token};
pub use signalfd::SignalFd;
pub use timerfd::TimerFd;
