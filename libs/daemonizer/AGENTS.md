# daemonizer guidance

## Public API

- Keep public declarations in `daemonz`, with the `C`/`I`/`S`/`E` type prefixes.
- Stateful public classes own private implementations. Never expose native
  process/pipe/event handles, reactor queues, or mutable lifecycle fields.
- `SDaemonOptions` is caller-owned configuration. Validate and copy it before
  creating processes or threads. `SSignal` exposes read-only native metadata.
- Follow Javadoc comments, ASCII C++ source, header guards, 4-space indentation,
  and namespace-close comments from `refs/libcertpp/repo/docs/RULES.md`.

## Processes and async operations

- Windows emulates fork by re-executing `args[0]` with reserved internal flags.
  Preserve argument quoting and use a strict inherited-handle allowlist.
- Unix process creation precedes all runtime threads. Close unused inherited
  endpoints without shutting down endpoints still used by another process.
- Count the launching thread as reactor zero. Create only the remaining
  reactors and the configured workers within the per-process thread budget.
- Keep application work and signal delivery independently awaitable. Join the
  application body before running cleanup or releasing lifecycle state.
- Kernel handlers must not invoke user hooks, allocate application objects,
  or access daemon internals. Forward signals through the normal runtime.
- Retain native overlapped records until kernel cancellation completes.
- Reap every owned child. Limit forced termination to children of this launch
  after their configured cooperative shutdown deadline.

## Validation

- Use doctest with a custom stdout/stderr-only runner and Windows dialogs off.
- Test real process creation in `daemonizer_process_helper`, not by forking
  the multithreaded doctest test process after pipe reactors have started.
- Keep integration tests bounded and make helper daemons terminate themselves.
- Build and run CTest from the repository root on Windows and Linux.
