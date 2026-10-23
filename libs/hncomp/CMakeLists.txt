cmake_minimum_required(VERSION 3.23)
include(CMakePackageConfigHelpers)

add_library(hncomp src/common.cpp src/compressor.cpp src/lz.cpp src/deflate.cpp
    src/lzma.cpp src/bzip2.cpp src/zstd.cpp src/brotli.cpp src/chunk_stream.cpp
    src/archive.cpp src/archive_io.cpp src/archive_xz.cpp src/archive_crypto.cpp)
add_library(hncomp::hncomp ALIAS hncomp)
target_compile_features(hncomp PUBLIC cxx_std_20)
set_target_properties(hncomp PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(hncomp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_link_libraries(hncomp PUBLIC asyncpp PRIVATE certpp::certpp)
if(WIN32 AND CERTPP_BUILD_SHARED)
    add_custom_command(TARGET hncomp POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:certpp> $<TARGET_FILE_DIR:hncomp>)
endif()
if(BUILD_SHARED_LIBS)
    target_compile_definitions(hncomp PUBLIC HNCOMP_SHARED PRIVATE HNCOMP_BUILDING_LIBRARY)
endif()
if(MSVC)
    target_compile_options(hncomp PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(hncomp PRIVATE -Wall -Wextra -Wpedantic)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(hncomp PRIVATE -Wno-mismatched-new-delete)
    endif()
endif()

if(HNUTILS_BUILD_TESTS)
    add_executable(hncomp_tests tests/main.cpp tests/compression_tests.cpp tests/archive_tests.cpp tests/archive_crypto_tests.cpp)
    target_link_libraries(hncomp_tests PRIVATE hncomp::hncomp doctest::doctest certpp::certpp)
    include("${DOCTEST_CMAKE_HELPER}")
    doctest_discover_tests(hncomp_tests TEST_PREFIX "hncomp." PROPERTIES TIMEOUT 120)
    add_executable(hncomp_oracle tests/oracle.cpp)
    target_link_libraries(hncomp_oracle PRIVATE hncomp::hncomp)
    set_target_properties(hncomp_tests hncomp_oracle PROPERTIES CXX_EXTENSIONS OFF)
    option(HNCOMP_REFERENCE_TESTS "Run optional Python/system-codec interoperability tests" OFF)
    if(HNCOMP_REFERENCE_TESTS)
        find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
        add_test(NAME hncomp.reference COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/reference_check.py" "$<TARGET_FILE:hncomp_oracle>")
        set_tests_properties(hncomp.reference PROPERTIES TIMEOUT 180 LABELS reference
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
        add_test(NAME hncomp.archive.reference COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/archive_reference.py" "$<TARGET_FILE:hncomp_oracle>")
        set_tests_properties(hncomp.archive.reference PROPERTIES TIMEOUT 180 LABELS reference
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
        find_program(HNCOMP_OPENSSL_TEST openssl)
        find_program(HNCOMP_7Z_TEST 7z)
        find_program(HNCOMP_XZ_TEST xz)
        if(HNCOMP_OPENSSL_TEST AND HNCOMP_7Z_TEST AND HNCOMP_XZ_TEST)
            add_test(NAME hncomp.archive.crypto.reference COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/archive_crypto_reference.py" "$<TARGET_FILE:hncomp_oracle>")
            set_tests_properties(hncomp.archive.crypto.reference PROPERTIES TIMEOUT 180 LABELS reference
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
        endif()
    endif()
endif()

option(HNCOMP_BUILD_EXAMPLES "Build hncomp usage examples" OFF)
if(HNCOMP_BUILD_EXAMPLES)
    add_executable(hncomp_example examples/compression.cpp)
    target_link_libraries(hncomp_example PRIVATE hncomp::hncomp)
    add_executable(hncomp_archive_example examples/archive.cpp)
    target_link_libraries(hncomp_archive_example PRIVATE hncomp::hncomp)
    if(HNUTILS_BUILD_TESTS)
        add_test(NAME hncomp.example COMMAND hncomp_example)
        set_tests_properties(hncomp.example PROPERTIES TIMEOUT 30)
        add_test(NAME hncomp.archive.example COMMAND hncomp_archive_example)
        set_tests_properties(hncomp.archive.example PROPERTIES TIMEOUT 30)
    endif()
endif()

install(TARGETS hncomp EXPORT hncomp-targets
    ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin INCLUDES DESTINATION include)
install(DIRECTORY include/ DESTINATION include)
install(FILES NOTICE DESTINATION share/licenses/hncomp)
install(EXPORT hncomp-targets NAMESPACE hncomp:: DESTINATION lib/cmake/hncomp)
configure_package_config_file(cmake/hncomp-config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/hncomp-config.cmake" INSTALL_DESTINATION lib/cmake/hncomp)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/hncomp-config-version.cmake"
    VERSION 0.1.0 COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/hncomp-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/hncomp-config-version.cmake" DESTINATION lib/cmake/hncomp)
