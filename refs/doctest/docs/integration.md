# doctest integration

The root build includes the pinned upstream submodule only when
`HNUTILS_BUILD_TESTS` is enabled. doctest's own tests, default-main library, and
installation are disabled. Test executables link `doctest::doctest` privately.

`libs/socketpp/tests/main.cpp` implements doctest with `DOCTEST_CONFIG_IMPLEMENT`
and a custom `main`. It redirects doctest output to stdout and forces
`no-breaks`, `no-debug-output`, and `no-colors` after command-line parsing.
Windows CRT warnings, errors, and assertions go to stderr; Windows fault
dialogs and abort reporting are disabled before doctest starts. Command-line
test discovery and test filters remain available.

`doctest_discover_tests` registers every `DOCTEST_TEST_CASE` separately with
CTest and applies a 30-second timeout to each case. Assertions in worker
threads use doctest's normal thread-safe `DOCTEST_CHECK` macro. Worker threads
must be joined before the test case finishes; do not use fatal assertions
that unwind a test case while it still owns a joinable thread.
