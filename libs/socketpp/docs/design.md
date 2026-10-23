# socketpp design

## Ownership boundary

`CSocketAddress`, `CSocket`, `CSocketWatcher`, and `CSocketAsync` each use a
private implementation object. Public headers contain no native socket type,
`sockaddr` storage, IOCP record, epoll descriptor, kqueue registration, or
reactor callback map. This keeps callers from mutating invariants behind the
wrapper and keeps platform headers out of downstream translation units.

`CSocket` is move-only and closes its owned socket on destruction.
`CSocketAsync` is also move-only and takes ownership of a `CSocket`; converting
a socket to asynchronous use therefore has an explicit ownership transfer.

Protocol-specific TCP, UDP, and IPv4 broadcast classes also use private
implementation objects. Their synchronous variants compose `CSocket`, and
their asynchronous variants compose `CSocketAsync`. Broadcast wrappers
compose the corresponding UDP wrapper and enable `SO_BROADCAST` on creation.
No variant exposes its wrapped socket. See [transports](transports.md) for
the complete API contract and [examples](../examples/README.md) for runnable
per-class programs.

## Reactor model

`CSocketWatcher` derives from `asyncpp::dispatcher`. `push()` places ordinary
callbacks onto the same event loop used for socket events. Platform wakeups
are IOCP completion packets, Linux `eventfd` notifications, or kqueue user
events.

On Windows, `CSocketAsync` submits overlapped Winsock operations and resumes
their promises from IOCP completions. On readiness-based systems, asynchronous
methods first attempt the non-blocking BSD operation, register one-shot
read/write interest on `EAGAIN`, and retry after epoll or kqueue reports the
socket ready.

Only one pending readiness wait of each direction is allowed per socket on
readiness-based backends. Starting a second read or a second write operation
before the first completes raises `std::logic_error`; one read and one write
may be pending concurrently.

## Error contract

Socket and reactor failures throw `std::system_error` with the platform's
native socket error. A TCP receive result of zero for a nonempty buffer
reports EOF; a UDP receive result of zero is a valid empty datagram.
Asynchronous failures are stored in the asyncpp promise and rethrown
when its task is resumed.
