cmake_minimum_required(VERSION 3.23)

include(CMakePackageConfigHelpers)

add_library(hnconf
    src/common.cpp
    src/error.cpp
    src/node.cpp
    src/config.cpp
    src/parser.cpp
    src/writer.cpp
    src/async_config.cpp
)
add_library(hnconf::hnconf ALIAS hnconf)
target_compile_features(hnconf PUBLIC cxx_std_20)
set_target_properties(hnconf PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(hnconf PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(hnconf PUBLIC asyncpp)
if(BUILD_SHARED_LIBS)
    target_compile_definitions(hnconf PUBLIC HNCONF_SHARED PRIVATE HNCONF_BUILDING_LIBRARY)
endif()
if(MSVC)
    target_compile_options(hnconf PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(hnconf PRIVATE -Wall -Wextra -Wpedantic)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(hnconf PRIVATE -Wno-mismatched-new-delete)
    endif()
endif()

if(HNUTILS_BUILD_TESTS)
    find_package(Threads REQUIRED)
    add_executable(hnconf_tests tests/main.cpp tests/config_tests.cpp)
    target_link_libraries(hnconf_tests PRIVATE hnconf::hnconf doctest::doctest Threads::Threads)
    set_target_properties(hnconf_tests PROPERTIES CXX_EXTENSIONS OFF)
    include("${DOCTEST_CMAKE_HELPER}")
    doctest_discover_tests(hnconf_tests TEST_PREFIX "hnconf." PROPERTIES TIMEOUT 30)
endif()

option(HNCONF_BUILD_EXAMPLES "Build hnconf examples" OFF)
if(HNCONF_BUILD_EXAMPLES)
    find_package(Threads REQUIRED)
    add_executable(hnconf_example examples/config.cpp)
    target_link_libraries(hnconf_example PRIVATE hnconf::hnconf Threads::Threads)
    set_target_properties(hnconf_example PROPERTIES CXX_EXTENSIONS OFF)
    if(HNUTILS_BUILD_TESTS)
        add_test(NAME hnconf.example COMMAND hnconf_example)
        set_tests_properties(hnconf.example PROPERTIES TIMEOUT 30)
    endif()
endif()

install(TARGETS hnconf EXPORT hnconf-targets
    ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)
install(DIRECTORY include/ DESTINATION include)
install(EXPORT hnconf-targets NAMESPACE hnconf:: DESTINATION lib/cmake/hnconf)
configure_package_config_file(cmake/hnconf-config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/hnconf-config.cmake"
    INSTALL_DESTINATION lib/cmake/hnconf
)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/hnconf-config-version.cmake"
    VERSION 0.1.0 COMPATIBILITY SameMajorVersion
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/hnconf-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/hnconf-config-version.cmake"
    DESTINATION lib/cmake/hnconf
)
