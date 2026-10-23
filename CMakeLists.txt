cmake_minimum_required(VERSION 3.23)

project(hnutils LANGUAGES CXX)

include(CTest)

option(HNUTILS_BUILD_TESTS "Build hnutils tests" ${BUILD_TESTING})

set(ASYNCPP_BUILD_TEST OFF CACHE BOOL "Build asyncpp tests" FORCE)
set(ASYNCPP_BUILD_DOCS OFF CACHE BOOL "Build asyncpp documentation" FORCE)

add_subdirectory(refs/asyncpp/repo)

if(HNUTILS_BUILD_TESTS)
    set(DOCTEST_WITH_TESTS OFF CACHE BOOL "Build doctest tests/examples" FORCE)
    set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF CACHE BOOL "Build doctest's default main" FORCE)
    set(DOCTEST_NO_INSTALL ON CACHE BOOL "Skip installing the test-only dependency" FORCE)
    add_subdirectory(refs/doctest/repo)
endif()

add_subdirectory(libs/socketpp)
