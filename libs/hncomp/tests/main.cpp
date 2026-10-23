#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#include <iostream>
#include "console.hpp"

/* Configure Windows and doctest for console-only failures, including discovery. */
int main(int argc, char** argv) {
    configureTestConsole();
    doctest::Context context(argc, argv);
    context.setCout(&std::cout); context.setOption("no-breaks", true);
    context.setOption("no-debug-output", true); context.setOption("no-colors", true);
    return context.run();
}
