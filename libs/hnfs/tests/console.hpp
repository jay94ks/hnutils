#ifndef HNFS_TEST_CONSOLE_HPP
#define HNFS_TEST_CONSOLE_HPP

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <crtdbg.h>
#include <cstdlib>
#include <initializer_list>
#endif

/* Never display CRT assertions or Windows fault dialogs during tests or examples. */
inline void configureFailures() {
#if defined(_WIN32)
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    for (const auto kind:{_CRT_WARN,_CRT_ERROR,_CRT_ASSERT}) {
        _CrtSetReportMode(kind,_CRTDBG_MODE_FILE); _CrtSetReportFile(kind,_CRTDBG_FILE_STDERR);
    }
    _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
#endif
}
#endif
