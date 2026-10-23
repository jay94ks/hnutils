cmake_minimum_required(VERSION 3.23)

project(hnutils LANGUAGES CXX)

include(CTest)

option(HNUTILS_BUILD_TESTS "Build hnutils tests" ${BUILD_TESTING})

set(ASYNCPP_BUILD_TEST OFF CACHE BOOL "Build asyncpp tests" FORCE)
set(ASYNCPP_BUILD_DOCS OFF CACHE BOOL "Build asyncpp documentation" FORCE)

add_subdirectory(refs/asyncpp/repo)

# Cryptographic primitives are supplied by the pinned libcertpp reference.
set(CERTPP_BUILD_TESTS OFF CACHE BOOL "Build certpp tests" FORCE)
set(CERTPP_BUILD_EXAMPLES OFF CACHE BOOL "Build certpp examples" FORCE)
option(CERTPP_BUILD_SHARED "Build certpp as a shared library" ${BUILD_SHARED_LIBS})
# Upstream's BMI2 immediate-operand helper requires inlining unavailable at -O0.
# Use its supported portable SIMD switch by default for GNU/Clang Debug builds;
# SHA/AES hardware acceleration remains enabled through independent switches.
set(HNUTILS_CERTPP_PORTABLE_DEBUG OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND (CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_CONFIGURATION_TYPES))
    set(HNUTILS_CERTPP_PORTABLE_DEBUG ON)
endif()
option(CERTPP_DISABLE_HWACCEL_SIMD "Use portable libcertpp SIMD paths" ${HNUTILS_CERTPP_PORTABLE_DEBUG})
add_subdirectory(refs/libcertpp/repo)
install(FILES refs/libcertpp/repo/LICENSE DESTINATION share/licenses/certpp)

if(HNUTILS_BUILD_TESTS)
    set(DOCTEST_WITH_TESTS OFF CACHE BOOL "Build doctest tests/examples" FORCE)
    set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF CACHE BOOL "Build doctest's default main" FORCE)
    set(DOCTEST_NO_INSTALL ON CACHE BOOL "Skip installing the test-only dependency" FORCE)
    add_subdirectory(refs/doctest/repo)
endif()

add_subdirectory(libs/socketpp)
add_subdirectory(libs/daemonizer)
add_subdirectory(libs/sdkv)
add_subdirectory(libs/hnconf)
add_subdirectory(libs/hncomp)
