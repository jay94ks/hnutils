# doctest reference guidance

- Keep `repo` as an upstream Git submodule pinned to a reviewed release commit.
- Do not edit upstream files directly. Preserve required upstream modifications
  as patches in `patches` and document the application order in `README.md`.
- Keep framework usage and integration notes in `docs`.
- Use the `doctest::doctest` CMake target as a private test-only dependency.
- Keep doctest's own tests, default-main library, and installation disabled.
- Retain a custom runner that sends diagnostics to stdout/stderr and prevents
  debugger breaks and Windows error dialogs, including during test discovery.
