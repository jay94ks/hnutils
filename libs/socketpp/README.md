# socketpp

`socketpp` is a C++20 socket library in the `sockpp` namespace. It combines a
synchronous BSD-style socket wrapper with coroutine-based asynchronous I/O
powered by [asyncpp](../../refs/asyncpp/README.md).

## Public types

- `CSocketAddress` owns a resolved IPv4 or IPv6 socket address.
- `CSocket` owns a native socket and exposes synchronous bind, listen, accept,
  connect, send, receive, socket-option, and address operations.
- `CSocketWatcher` runs the platform event backend and implements
  `asyncpp::dispatcher`.
- `CSocketAsync` owns a non-blocking `CSocket` and provides coroutine versions
  of connect, accept, send, receive, send-to, and receive-from.
- `sockpp::opts` contains typed socket options such as `SNoDelay`,
  `SReuseAddress`, `SKeepAlive`, `SBroadcast`, `SSendBuffer`, and
  `SReceiveBuffer`. Each option can be applied to `CSocket` or `CSocketAsync`
  through `socket.setOption(option)` or `option.apply(socket)`.
  Platform-specific options use a no-op template branch with an
  instantiation-time compiler warning when unavailable; `SReusePort` and
  `SExclusiveAddressUse` demonstrate the Unix and Windows cases.

See [typed socket options](docs/options.md) for the support matrix and fallback
contract.

All stateful public types use private implementation objects. Native handles,
OS address storage, completion records, and reactor registrations are not part
of the public API.

## Platform backends

| Platform | Watcher backend |
| --- | --- |
| Windows | IOCP |
| Linux | epoll + eventfd |
| macOS and BSD | kqueue + `EVFILT_USER` |

The watcher must outlive every `CSocketAsync` attached to it. Run `run()` on
one event-loop thread, or repeatedly call `runOne()` from an application-owned
loop. Calling `stop()` wakes a blocked event loop.

## Build and test

From the repository root:

```sh
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Tests use [doctest](../../refs/doctest/README.md). CTest discovers and runs each
test case separately. The custom test runner sends doctest diagnostics to
standard output and disables debugger breaks. On Windows it also redirects
CRT diagnostics to standard error and disables operating system fault dialogs.

doctest is only required when `HNUTILS_BUILD_TESTS` is enabled and is not linked
into the socketpp library or installed with it. Test cases can also be listed
or filtered directly using doctest's `--list-test-cases` and `--test-case`
command-line options.

## Example

```cpp
#include <socketpp.hpp>

#include <asyncpp/sync_wait.h>
#include <thread>

sockpp::CSocketWatcher watcher;
auto socket = sockpp::CSocketAsync::open(
    watcher,
    sockpp::EAF_IPV4,
    sockpp::ESOCK_STREAM,
    sockpp::EPROTO_TCP);

auto endpoint = sockpp::CSocketAddress::loopbackIpv4(8080);
std::jthread eventLoop([&watcher] { watcher.run(); });

auto connected = asyncpp::as_promise(socket.connect(endpoint));
connected.get();

watcher.stop();
```

`run()` must be active while the coroutine is waiting; applications normally
start the watcher before awaiting network work. Production code should also
ensure that `stop()` is called on exceptional exit before joining the event
loop thread.
