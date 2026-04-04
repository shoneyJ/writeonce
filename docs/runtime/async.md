# Creating an Async Runtime Environment

## Goal

Build a minimal async runtime from scratch using Linux kernel primitives. The runtime is not specific to writeonce — it is a general-purpose event loop that any project can use to multiplex I/O without threads, without tokio, and without any external async framework.

The writeonce project uses this runtime (the `wo-event` and `wo-rt` crates), but the concepts apply to any server, daemon, or event-driven application on Linux.

## What Is a Runtime?

A runtime is the loop that decides **what code runs next**. In a synchronous program, the OS scheduler picks the next thread. In an async runtime, a single thread asks the kernel: "which of my file descriptors are ready?" — and runs the corresponding handler.

```
loop {
    ready_fds = ask_kernel_which_fds_are_ready()
    for fd in ready_fds {
        run_handler(fd)
    }
}
```

That's the entire concept. Everything else — epoll, tokens, interest flags — is implementation detail around this loop.

## Explaining It with C

Before Rust, before abstractions, here is a minimal async runtime in C that watches two file descriptors on one thread.

### Step 1: Create an epoll instance

```c
#include <sys/epoll.h>
#include <unistd.h>
#include <stdio.h>

int main() {
    // Create the event loop
    int epoll_fd = epoll_create1(0);

    // This single fd is the "runtime" — all other fds register on it
    printf("epoll fd: %d\n", epoll_fd);
}
```

`epoll_create1` returns a file descriptor. This fd *is* the runtime. Every other fd in the system registers itself on this one fd, and the kernel tracks readiness for all of them.

### Step 2: Register file descriptors

```c
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <time.h>

int main() {
    int epoll_fd = epoll_create1(0);

    // Create an eventfd (like a semaphore as a file descriptor)
    int event_fd = eventfd(0, EFD_NONBLOCK);

    // Create a timerfd (fires every 2 seconds)
    int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    struct itimerspec spec = {
        .it_interval = { .tv_sec = 2, .tv_nsec = 0 },
        .it_value    = { .tv_sec = 2, .tv_nsec = 0 }
    };
    timerfd_settime(timer_fd, 0, &spec, NULL);

    // Register both on epoll
    struct epoll_event ev1 = { .events = EPOLLIN, .data.fd = event_fd };
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, event_fd, &ev1);

    struct epoll_event ev2 = { .events = EPOLLIN, .data.fd = timer_fd };
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timer_fd, &ev2);
}
```

Two different kinds of fd — an event signal and a timer — both registered on the same epoll instance. The kernel will wake us when either is ready.

### Step 3: The event loop

```c
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

int main() {
    int epoll_fd = epoll_create1(0);

    int event_fd = eventfd(0, EFD_NONBLOCK);
    int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);

    struct itimerspec spec = {
        .it_interval = { .tv_sec = 2, .tv_nsec = 0 },
        .it_value    = { .tv_sec = 2, .tv_nsec = 0 }
    };
    timerfd_settime(timer_fd, 0, &spec, NULL);

    struct epoll_event ev1 = { .events = EPOLLIN, .data.fd = event_fd };
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, event_fd, &ev1);

    struct epoll_event ev2 = { .events = EPOLLIN, .data.fd = timer_fd };
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timer_fd, &ev2);

    printf("Runtime started. Timer fires every 2s.\n");
    printf("Write to eventfd to trigger it: echo 1 > /proc/%d/fd/%d\n",
           getpid(), event_fd);

    // The event loop — this IS the runtime
    struct epoll_event events[10];
    while (1) {
        int n = epoll_wait(epoll_fd, events, 10, -1);  // block until ready

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == timer_fd) {
                uint64_t expirations;
                read(timer_fd, &expirations, sizeof(expirations));
                printf("[timer] fired (%lu expirations)\n", expirations);
            }
            else if (fd == event_fd) {
                uint64_t val;
                read(event_fd, &val, sizeof(val));
                printf("[event] signaled (value: %lu)\n", val);
            }
        }
    }

    close(epoll_fd);
    close(event_fd);
    close(timer_fd);
    return 0;
}
```

Compile and run:

```bash
gcc -o runtime runtime.c
./runtime
```

Output:

```
Runtime started. Timer fires every 2s.
[timer] fired (1 expirations)
[timer] fired (1 expirations)
[timer] fired (1 expirations)
...
```

One thread. Two fd types. One loop. The kernel does the scheduling.

### Step 4: Add a TCP server to the same loop

```c
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>

int main() {
    int epoll_fd = epoll_create1(0);

    // Create a non-blocking TCP listener
    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(8080),
        .sin_addr.s_addr = INADDR_ANY
    };
    bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 128);

    // Register listener on epoll
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = listen_fd };
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

    printf("Listening on port 8080\n");

    struct epoll_event events[64];
    while (1) {
        int n = epoll_wait(epoll_fd, events, 64, -1);

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                // Accept new connection
                int client_fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK);
                if (client_fd >= 0) {
                    struct epoll_event cev = { .events = EPOLLIN, .data.fd = client_fd };
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &cev);
                    printf("[accept] client fd=%d\n", client_fd);
                }
            } else {
                // Read from client
                char buf[4096];
                int nbytes = read(fd, buf, sizeof(buf));
                if (nbytes <= 0) {
                    // Client disconnected
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                    printf("[close] fd=%d\n", fd);
                } else {
                    // Echo response
                    const char *response =
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Length: 13\r\n"
                        "\r\n"
                        "Hello, world!";
                    write(fd, response, strlen(response));
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                }
            }
        }
    }
}
```

This is a complete HTTP server — no threads, no framework, no library. One `epoll_wait` drives accept, read, write, and close for every connection.

## From C to Rust: The writeonce Runtime

The writeonce `wo-event` crate wraps these same syscalls in safe Rust:

| C syscall | Rust wrapper | Crate |
|-----------|-------------|-------|
| `epoll_create1` | `EventLoop::new()` | wo-event |
| `epoll_ctl(ADD)` | `EventLoop::register(fd, interest, token)` | wo-event |
| `epoll_ctl(MOD)` | `EventLoop::modify(fd, interest, token)` | wo-event |
| `epoll_ctl(DEL)` | `EventLoop::deregister(fd)` | wo-event |
| `epoll_wait` | `EventLoop::poll(timeout)` | wo-event |
| `eventfd` | `EventFd::new()` | wo-event |
| `timerfd_create` | `TimerFd::new()` | wo-event |
| `signalfd` | `SignalFd::new()` | wo-event |

The key difference from the C examples: instead of matching on raw fd numbers, the Rust runtime assigns a **token** (u64) to each fd. The event loop returns tokens, and the runtime dispatches on them:

```rust
let events = event_loop.poll(Some(Duration::from_millis(500)))?;

for event in events {
    match event.token {
        TOKEN_WATCHER  => handle_file_change(),
        TOKEN_SIGNAL   => handle_shutdown(),
        TOKEN_TIMER    => handle_periodic_task(),
        TOKEN_LISTENER => handle_new_connection(),
        token if token >= 10000 => handle_http(token),
        _ => {}
    }
}
```

## Why Not tokio?

tokio is a production-grade async runtime. It handles everything — epoll, thread pools, work stealing, timers, I/O drivers. So why build a custom one?

| Concern | tokio | Custom runtime |
|---------|-------|---------------|
| Binary size | Adds ~2-3 MB | Zero — just libc syscalls |
| Dependencies | 50+ transitive crates | 1 crate (libc) |
| Complexity | Work-stealing scheduler, multi-threaded executor | Single-threaded loop, ~200 lines |
| Control | Opaque — runtime internals hidden behind `.await` | Every fd, every syscall, every state transition is explicit |
| Learning | Abstracts the kernel away | Forces understanding of what the kernel actually does |

For a content platform serving markdown files, the workload is: accept connection, read request, query in-memory index, render template, write response. This is microseconds of work per request. A single-threaded event loop handles thousands of concurrent connections without the complexity of a multi-threaded executor.

## Reusing the Runtime in Other Projects

The `wo-event` crate has no dependency on writeonce. It provides:

- `EventLoop` — epoll wrapper with register/deregister/poll
- `EventFd` — lightweight signaling
- `TimerFd` — periodic timers as fds
- `SignalFd` — SIGINT/SIGTERM as fd events
- `Event` — readable/writable/hangup status
- `Token` — u64 identifier for dispatch

Any project that needs non-blocking I/O on Linux can use it:

```rust
use wo_event::{EventLoop, EventFd, Interest};
use std::time::Duration;

fn main() {
    let eloop = EventLoop::new().unwrap();
    let efd = EventFd::new().unwrap();

    eloop.register(efd.fd(), Interest::Readable, 1).unwrap();

    // Signal from another thread
    std::thread::spawn(move || {
        std::thread::sleep(Duration::from_secs(1));
        efd.write(42).unwrap();
    });

    // Wait for the signal
    let events = eloop.poll(Some(Duration::from_secs(5))).unwrap();
    assert_eq!(events[0].token, 1);
    println!("Event received!");
}
```

## The Mental Model

```
    ┌─────────────────────────────────┐
    │           your code             │
    │  (handlers, business logic)     │
    └──────────────┬──────────────────┘
                   │ dispatches on token
    ┌──────────────┴──────────────────┐
    │         event loop              │
    │    epoll_wait → Vec<Event>      │
    └──────────────┬──────────────────┘
                   │ registered fds
    ┌──────────────┴──────────────────┐
    │        Linux kernel             │
    │  tracks readiness for all fds   │
    │  inotify, sockets, timers,      │
    │  signals, eventfds — all fds    │
    └─────────────────────────────────┘
```

The kernel is the scheduler. The event loop is the dispatcher. Your code is the handler. Everything in the system — files, sockets, timers, signals — is a file descriptor. One loop to rule them all.
