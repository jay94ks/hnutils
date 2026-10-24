#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#include "console.hpp"
#include <iostream>

/* Discovery and execution use the same console-only failure policy. */
int runTests(int argc,char** argv) {
    doctest::Context context(argc,argv);
    context.setCout(&std::cout); context.setOption("no-breaks",true);
    context.setOption("no-debug-output",true); context.setOption("no-colors",true); return context.run();
}

/* Report native Windows exceptions to stderr without a debugger or dialog. */
int main(int argc,char** argv) {
    configureFailures();
#if defined(_WIN32)
    __try { return runTests(argc,argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { std::cerr<<"unhandled Windows exception: 0x"<<std::hex<<GetExceptionCode()<<'\n'; return 1; }
#else
    return runTests(argc,argv);
#endif
}
