# Repository guidance

## Scope

This repository is a monorepo for reusable libraries and command-line tools.

## Layout

- Put reusable libraries and shared concepts in `libs/<name>`.
- Put each CLI application in its own `tools/<name>` directory.
- Put repository-wide plans, designs, and usage notes in `docs`.
- Put external Git repositories in `refs/<repository>/repo` as submodules.
- Keep local patches for an external repository in
  `refs/<repository>/patches` and integration notes in
  `refs/<repository>/docs`.

## Working rules

- Memory-mapped I/O in every library must use bounded, sliding windows. Never map an
  entire file merely because a 64-bit address space is available. Honor OS mapping
  alignment, 64-bit offsets, explicit flush errors and window-boundary reads/writes.
- Expose mapped-window sizing through validated option structures and runtime setters;
  keep mapped pointers private and serialize window-policy changes with I/O.

- Check for a more specific `AGENTS.md` before changing a subtree.
- Keep libraries independent of individual tools unless the dependency is
  explicitly part of the library's purpose.
- Give every library and tool its own README with purpose, build, test, and
  usage instructions.
- Do not commit generated build output or vendored dependency caches.
- Write C++ test cases with doctest. Keep it as a private test-only dependency
  from `refs/doctest/repo` and report failures through stdout/stderr without
  debugger breaks or interactive Windows dialogs.
- Treat files inside `refs/*/repo` as upstream-owned. Preserve required
  changes as reproducible patch files instead of committing changes inside a
  submodule.
- Keep commits and changes scoped to one library, tool, document, or external
  reference whenever practical.
