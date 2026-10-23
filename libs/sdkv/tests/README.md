# sdkv tests

Tests use doctest and a custom stdout/stderr-only runner with Windows error
dialogs and debugger breaks disabled. CTest registers each case separately
with a 30-second timeout.

Run from the repository root:

```sh
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug -R sdkv --no-tests=error --output-on-failure
```

Coverage includes opaque binary ownership, empty values, 4096-byte limits,
per-key revisions including remove/recreate, before/after images, no-op
suppression, compare-and-set, snapshot/live handoff, cycles and diamonds,
subscription ownership and reentrancy, callback exceptions, multithreaded
CAS, executor scheduling, coroutine ownership, and an application-defined
remote cache adapter without networking.
