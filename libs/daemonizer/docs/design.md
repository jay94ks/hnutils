# daemonizer design

## Ownership and lifecycle

`IDaemon`, `IPipe`, and `IPipeServer` expose behavior only. The default
`CDaemon` and immutable `CDaemonContext` hide their implementations. Derived
daemon classes override async hooks without accessing native handles or the
base class's lifecycle fields. Options are caller-owned values copied before
launch; `SSignal` has private metadata and read-only accessors.

The lifecycle is `onStart(context)`, then an application `onRun(context)` and
a serialized signal loop running independently, followed by `onStop()`.
Startup must finish before signal hooks begin. The application body must
cooperate with `requestStop()`; `waitForStop()` suspends without consuming a
thread. The runtime joins the application body before cleanup. Startup,
application, and signal failures still invoke cleanup and return failure.

Only one launch may own process-wide signal handling at a time. Native pipe
completions may resume coroutines on different reactors; `context.work()`
returns continuations to reactor zero after executing the worker callback.
Application state shared between the
body and signal hooks needs its own synchronization. Do not block reactors
or destroy the daemon while launch is active.

## Process creation

`max_fork` counts additional managed children, not the root or Unix daemon
bootstrap processes. Every process receives a separate runtime created after
fork/re-exec. The parent retains one pipe endpoint per child; a child retains
only its parent endpoint and closes inherited sibling descriptors. Unused
inherited endpoints are closed without `shutdown`, because shutdown would
affect the other process's shared open-file description.

Windows re-executes the exact filename from `args[0]`, resolved against the
original current directory, and appends the reserved
`--daemonz-internal-child-v1` flag with a slot, inherited pipe handle, inherited
stop-event handle, and parent PID. `STARTUPINFOEX` restricts inherited handles
to that explicit allowlist. Arguments are individually quoted using Windows
CRT rules, including empty strings, embedded quotes, and trailing backslashes.
User arguments must pass through to `launch()` so it can consume its internal
flags. The child reconstructs application state by rerunning `main` and its
normal constructors; it does not inherit a C++ memory snapshot.

Unix uses native fork before creating any runtime threads. `detach=true`
performs a bootstrap double fork, `setsid()`, and `umask(027)`. Standard
streams are redirected to `/dev/null` when `redirect_stdio` is true. An
optional working directory is applied during the instance lifecycle, then
restored in foreground callers. Other application-owned descriptors are not
blanket-closed; initialize process-local resources in `onStart()`.

Windows detachment uses a separate `--daemonz-internal-root-v1` marker and
`DETACHED_PROCESS`; managed children use `CREATE_NO_WINDOW`. This library
does not install a Windows SCM service or a system service unit.

## Thread budget and async I/O

The launching thread is reactor zero. The runtime starts exactly
`max_reactors - 1` additional reactor threads and `max_workers` worker threads.
Standalone `IPipeServer::create(N)` instead owns N background reactors.
No signal-wait thread or per-pipe blocking reader is created.

Reactors implement the asyncpp dispatcher contract. Unix endpoints are
non-blocking Unix-domain socketpairs; Windows endpoints are duplex byte-mode
named pipes using overlapped I/O and per-operation events. A reactor performs
non-blocking readiness/completion probes with a bounded 5 ms polling interval
while operations are pending; idle background reactors wait on a condition
variable. This initial backend does not use socketpp's epoll/kqueue/IOCP
watcher and does not claim kernel-event-only wakeups for daemon pipe I/O.
The worker queue is separate from the I/O reactors and limited by options.

Pipes are streams, not message transports. One read and one write may be
pending concurrently per endpoint; a duplicate operation in the same
direction throws `std::logic_error`. `readExact` returns false for clean EOF
before any data and throws for truncated input. `writeAll` retries partial
writes. Operation buffers and endpoint owners must remain alive until their
tasks complete. Do not destroy a task that is suspended on active I/O.

Closing an endpoint cancels pending I/O. Kernel handles are released only
after their outstanding operations finish; an overlapped record is never
destroyed while the Windows kernel still references it. Server destruction
closes its endpoints and drains cancellation before stopping its own runtime.

## Signal delivery and shutdown

Unix blocks SIGINT, SIGTERM, SIGHUP, and SIGCHLD before threads start. The
launching thread drains pending signals with `sigtimedwait` where available,
or `sigpending` plus `sigwait` on other Unix systems. Sender PID metadata is
available with the timed-wait backend; otherwise it is zero. Windows console
control callbacks record atomic event bits, which normal runtime code wraps
as `SSignal`. Native handlers do not invoke virtual application callbacks.

The default signal hook requests stop for interrupt, termination, or console
close. Reload and child events are available for application overrides.
`postSignal()` also supports explicit application event injection. The OS may
coalesce repeated native signals; the library does not promise one callback
per repeated signal sent before it is consumed.

On parent shutdown, Unix sends SIGTERM to owned children. Windows signals the
explicitly inherited stop events; child launch loops translate those events
into termination signals, enabling the same async hook path. Child exit
statuses are collected, and a nonzero status is preserved when the root
otherwise succeeds. Children still alive after `shutdown_timeout` are
terminated and reaped. No unrelated processes are targeted. This timeout
does not forcibly abort arbitrary user callbacks in the root process.

The public shutdown model is cooperative: a custom `onRun` should finish
after stop, and a custom signal hook should cancel its application's pending
operations if necessary. Runtime-managed buffers and tasks must be drained
before releasing application state.

## Platform references

- [Windows process creation](https://learn.microsoft.com/en-us/windows/win32/procthread/creating-processes)
- [Windows inherited resources](https://learn.microsoft.com/en-us/windows/win32/procthread/inheritance)
- [Windows overlapped named pipes](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-server-using-completion-routines)
- [Linux fork semantics](https://man7.org/linux/man-pages/man2/fork.2.html)
- [Linux synchronous signal consumption](https://man7.org/linux/man-pages/man2/sigwaitinfo.2.html)

Windows and Linux are exercised by the integration tests. The Unix portability
branches for other BSD-family systems are not yet validated on native hosts.
