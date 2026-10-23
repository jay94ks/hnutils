# Protocol-specific sockets

All types are in `sockpp` and available through `<socketpp.hpp>`. Each class
is move-only and owns a private implementation; no underlying socket or
mutable native handle is exposed.

| Synchronous class | Coroutine class | Operations |
| --- | --- | --- |
| `CTcpListener` | `CTcpListenerAsync` | `bind`, `listen`, `accept` |
| `CTcpClient` | `CTcpClientAsync` | `bind`, `connect`, `send`, `sendAll`, `receive`, `shutdown` |
| `CUdpSocket` | `CUdpSocketAsync` | `bind`, `connect`, `send`, `receive`, `sendTo`, `receiveFrom`, `shutdown` |
| `CUdpBroadcast` | `CUdpBroadcastAsync` | `bind`, `send(buffer, port)`, `sendTo`, `receiveFrom` |

Default construction creates a closed object. Use `Type::open(family)` for
synchronous TCP/UDP, and `TypeAsync::open(watcher, family)` for asynchronous
TCP/UDP. The default family is `EAF_INET`; `EAF_INET6` is also supported.
Other families are rejected with `std::invalid_argument`.
Broadcast sockets are IPv4-only: `CUdpBroadcast::open()` and
`CUdpBroadcastAsync::open(watcher)`.

All classes provide `isOpen()`, idempotent `close()`, `localAddress()`,
native `setOption()/getOption()`, and inline typed `setOption(option)`.
Clients and ordinary UDP sockets also expose `peerAddress()`.
Moved-from objects are closed and may be assigned another object.
Using a closed protocol wrapper reports `std::system_error`.
Native I/O failures preserve their native error code.

## TCP

Call `bind()` and then `listen()` before accepting. Synchronous listeners
return `CTcpClient`; asynchronous listeners return `task<CTcpClientAsync>`.
The latter automatically retains the listener's watcher.

TCP is a byte stream, not a message protocol. `send()` and `receive()` may
return a partial buffer. `sendAll()` retries until the complete buffer has
been transmitted or an error occurs; a zero-progress write is an error.
`receive()` returning zero for a nonempty buffer indicates orderly EOF.
Use framing or a receive loop in application protocols.

## UDP

`sendTo()` sends one datagram, and `receiveFrom()` returns its sender
address. `connect()` selects a default destination and filters incoming
peers; it does not establish a TCP-style connection. After that, use
`send()` and `receive()` without an explicit destination.

Zero-length datagrams are valid and are not EOF. No method retries a partial
datagram by sending another packet. Oversized datagrams fail with the native
socket error. Supply enough receive space for the application's datagrams:
truncation behavior and errors are platform-dependent.

On Windows, UDP peer selection uses nonblocking Winsock `connect()`, while
TCP uses IOCP and `ConnectEx`. Microsoft documents that
[`ConnectEx` only supports connection-oriented sockets](https://learn.microsoft.com/en-us/windows/win32/api/mswsock/nc-mswsock-lpfn_connectex).

## IPv4 broadcast

Broadcast creation enables `opts::SBroadcast{true}` automatically; failure
to enable it fails creation. Other socket options remain caller-controlled.

- `send(buffer, port)` targets the limited broadcast address `255.255.255.255`.
- `sendTo(buffer, endpoint)` also supports a subnet-directed broadcast address.
  The caller supplies that address; the class does not enumerate interfaces.
- `receiveFrom()` receives both broadcast traffic and unicast replies.

Bind receivers to `CSocketAddress::anyIpv4(port)` to receive across local
IPv4 interfaces. Binding a sender to a specific local IPv4 address can select
an interface. The OS routing table and network/firewall configuration still
control delivery; successful transmission is not a delivery guarantee.
IPv6 uses multicast rather than this IPv4 broadcast API.

## Coroutine lifetime

Coroutine methods have the same names as synchronous methods, but return
`asyncpp::task<T>`. Drive the watcher while waiting for those tasks.
Creating a task alone does not start network I/O; await it or use
`asyncpp::as_promise()`.

The watcher must outlive its attached sockets and operations. Keep the socket
object, buffer storage, and peer/source output parameters alive until all
operations have completed. Do not move, close, or destroy the object while
operations are pending. Address inputs and span descriptors are captured by
value; the underlying buffer memory is not copied.

See [examples](../examples/README.md) for a single-threaded caller-owned
watcher loop and separate examples for every public class.
