#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <iostream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <crtdbg.h>
#include <cstdlib>
#endif

namespace {

/* Route Windows/CRT failures to the console without interactive dialogs. */
void configureFailures() {
#if defined(_WIN32)
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    for (const auto kind : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(kind, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(kind, _CRTDBG_FILE_STDERR);
    }
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
}

/* Enforce console-only doctest reports even if a debugger is attached. */
int runTests(int argc, char** argv) {
    doctest::Context context(argc, argv);
    context.setCout(&std::cout);
    context.setOption("no-breaks", true);
    context.setOption("no-debug-output", true);
    context.setOption("no-colors", true);
    return context.run();
}

} // namespace

/* Configure reporting before discovery or test execution. */
int main(int argc, char** argv) {
    configureFailures();
#if defined(_WIN32)
    __try {
        return runTests(argc, argv);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::cerr << "unhandled Windows exception: 0x" << std::hex << GetExceptionCode() << '\n';
        return 1;
    }
#else
    return runTests(argc, argv);
#endif
}
