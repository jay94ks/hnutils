# socketpp class examples

Each source file is an independent executable with its own `main()`.
All examples create their own peers on loopback with OS-assigned ports, so no
second terminal or external server is required.

| Class | Example |
| --- | --- |
| `CSocketAddress` | [address.cpp](address.cpp): parse, resolve, and format IPv4/IPv6 endpoints |
| `CSocket` | [socket.cpp](socket.cpp): synchronous BSD-style TCP operations |
| `CSocketWatcher` | [watcher.cpp](watcher.cpp): queue callbacks, run, and stop |
| `CSocketAsync` | [async_socket.cpp](async_socket.cpp): reactor-backed UDP exchange |
| `CTcpListener` | [tcp_listener.cpp](tcp_listener.cpp): accept a typed client and obtain its peer address |
| `CTcpClient` | [tcp_client.cpp](tcp_client.cpp): typed options, request/reply, and half-close |
| `CTcpListenerAsync` | [tcp_listener_async.cpp](tcp_listener_async.cpp): asynchronous accept |
| `CTcpClientAsync` | [tcp_client_async.cpp](tcp_client_async.cpp): connect and stream I/O with coroutine tasks |
| `CUdpSocket` | [udp_socket.cpp](udp_socket.cpp): addressed and connected datagrams |
| `CUdpSocketAsync` | [udp_socket_async.cpp](udp_socket_async.cpp): asynchronous datagrams and peer selection |
| `CUdpBroadcast` | [udp_broadcast.cpp](udp_broadcast.cpp): broadcasting enabled, discovery/reply flow |
| `CUdpBroadcastAsync` | [udp_broadcast_async.cpp](udp_broadcast_async.cpp): asynchronous discovery/reply flow |

The broadcast examples deliberately use loopback unicast for their runnable
portion; they do not send discovery traffic onto external networks.
Commented calls show limited and directed broadcast transmission using a
separate, non-loopback-bound socket. Choose a permitted interface, destination
port, and subnet address before enabling those calls.

## Build and run

From the repository root:

```sh
cmake -S . -B build -DSOCKETPP_BUILD_EXAMPLES=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug -R socketpp.example --output-on-failure
```

Example executable names are `socketpp_example_<source-name>`.
With Ninja, run one directly as:

```sh
./build/libs/socketpp/examples/socketpp_example_tcp_client
```

On Windows, append `.exe`; multi-configuration generators place executables
in the corresponding configuration subdirectory, such as `examples/Debug/`.
Examples default to OFF and are not installed with the library.
When `HNUTILS_BUILD_TESTS=ON`, enabling examples also registers twelve
bounded CTest smoke tests. Tests and examples report failures via stdout/stderr.

## Shared helpers

[support.hpp](support.hpp) contains only example utilities, not library API.
`example::run()` starts a coroutine and drives `runOne()` on the calling
thread until its future settles. `example::receiveText()` and its asynchronous
counterpart loop over partial TCP reads. `example::report()` catches exceptions
and reports them to stderr without assertions or debugger breaks.

The buffer storage, output addresses, and socket objects remain alive until
their operations complete. The watcher is declared before its sockets, so it
is destroyed last. These examples intentionally omit service-level concerns
such as application framing, cancellation, deadlines, and retry policies.
