cmake_minimum_required(VERSION 3.23)

include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

add_library(daemonizer
    src/context.cpp
    src/daemon.cpp
    src/launcher.cpp
    src/options.cpp
    src/pipe.cpp
    src/pipe_server.cpp
    src/runtime.cpp
    src/signal.cpp
)
add_library(daemonizer::daemonizer ALIAS daemonizer)
target_compile_features(daemonizer PUBLIC cxx_std_20)
set_target_properties(daemonizer PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(daemonizer PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(daemonizer PUBLIC asyncpp PRIVATE Threads::Threads)
if(BUILD_SHARED_LIBS)
    target_compile_definitions(daemonizer PUBLIC DAEMONIZER_SHARED PRIVATE DAEMONIZER_BUILDING_LIBRARY)
endif()
if(MSVC)
    target_compile_options(daemonizer PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(daemonizer PRIVATE -Wall -Wextra -Wpedantic)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(daemonizer PRIVATE -Wno-mismatched-new-delete)
    endif()
endif()

if(HNUTILS_BUILD_TESTS)
    add_executable(daemonizer_process_helper tests/process_helper.cpp)
    target_link_libraries(daemonizer_process_helper PRIVATE daemonizer::daemonizer)
    add_executable(daemonizer_tests tests/main.cpp tests/daemonizer_tests.cpp)
    target_link_libraries(daemonizer_tests PRIVATE daemonizer::daemonizer doctest::doctest)
    target_compile_definitions(daemonizer_tests PRIVATE
        DAEMONIZER_HELPER_PATH="$<TARGET_FILE:daemonizer_process_helper>"
    )
    add_dependencies(daemonizer_tests daemonizer_process_helper)
    include("${DOCTEST_CMAKE_HELPER}")
    doctest_discover_tests(daemonizer_tests TEST_PREFIX "daemonizer." PROPERTIES TIMEOUT 30)
endif()

install(TARGETS daemonizer EXPORT daemonizer-targets
    ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)
install(DIRECTORY include/ DESTINATION include)
install(EXPORT daemonizer-targets NAMESPACE daemonizer:: DESTINATION lib/cmake/daemonizer)
configure_package_config_file(cmake/daemonizer-config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/daemonizer-config.cmake"
    INSTALL_DESTINATION lib/cmake/daemonizer
)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/daemonizer-config-version.cmake"
    VERSION 0.1.0 COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/daemonizer-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/daemonizer-config-version.cmake"
    DESTINATION lib/cmake/daemonizer
)
