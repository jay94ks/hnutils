# socketpp guidance

## Public API

- Keep all public declarations in the `sockpp` namespace.
- Public stateful classes use a private PIMPL. Do not expose implementation
  structs, OS address storage, watcher registrations, or mutable native
  handles.
- Public classes use the `C` prefix, enums use `E`, member functions use
  camelCase, and private fields begin with `_`.
- Every public declaration must have an accurate Javadoc block. Use the
  comment and formatting conventions in `refs/libcertpp/repo/docs/RULES.md`.
- `include/socketpp.hpp` is the public umbrella header and must include every
  public module header.

## Networking

- `CSocket` owns exactly one native socket and remains a synchronous BSD-style
  wrapper.
- `CSocketAsync` owns its `CSocket`; it must not expose a mutable socket or
  native handle.
- `CSocketWatcher` is the reactor and `asyncpp::dispatcher`: IOCP on Windows,
  epoll on Linux, and kqueue on supported BSD-family systems.
- Keep OS headers and backend-specific types out of public headers.
- Report socket failures with `std::system_error` and the native socket error
  code. Never silently convert an I/O failure into EOF.

## Validation

Write test cases with doctest (`DOCTEST_TEST_CASE`, `DOCTEST_CHECK`, and related
macros). Keep the custom runner in `tests/main.cpp`: it routes diagnostics to
stdout/stderr and disables debugger breaks and Windows failure dialogs.
Register test cases individually with `doctest_discover_tests` and retain a
bounded CTest timeout for networking tests.

Configure, build, and test from the repository root:

```sh
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```
