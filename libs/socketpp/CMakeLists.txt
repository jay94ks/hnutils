cmake_minimum_required(VERSION 3.23)

include(CMakePackageConfigHelpers)

add_library(socketpp
    src/address.cpp
    src/async_socket.cpp
    src/options.cpp
    src/socket.cpp
    src/tcp_client.cpp
    src/tcp_client_async.cpp
    src/tcp_listener.cpp
    src/tcp_listener_async.cpp
    src/udp_broadcast.cpp
    src/udp_broadcast_async.cpp
    src/udp_socket.cpp
    src/udp_socket_async.cpp
    src/watcher.cpp
)
add_library(socketpp::socketpp ALIAS socketpp)

option(SOCKETPP_BUILD_EXAMPLES "Build the socketpp per-class examples" OFF)

target_compile_features(socketpp PUBLIC cxx_std_20)
set_target_properties(socketpp PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(socketpp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(socketpp PUBLIC asyncpp)

if(WIN32)
    target_link_libraries(socketpp PRIVATE ws2_32 mswsock)
endif()

if(BUILD_SHARED_LIBS)
    target_compile_definitions(socketpp
        PUBLIC SOCKETPP_SHARED
        PRIVATE SOCKETPP_BUILDING_LIBRARY
    )
endif()

if(MSVC)
    target_compile_options(socketpp PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(socketpp PRIVATE -Wall -Wextra -Wpedantic)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(socketpp PRIVATE -Wno-mismatched-new-delete)
    endif()
endif()

if(HNUTILS_BUILD_TESTS)
    add_executable(socketpp_tests tests/main.cpp tests/socket_tests.cpp tests/transport_tests.cpp)
    target_link_libraries(socketpp_tests PRIVATE socketpp::socketpp doctest::doctest)
    target_compile_features(socketpp_tests PRIVATE cxx_std_20)
    set_target_properties(socketpp_tests PROPERTIES CXX_EXTENSIONS OFF)
    include("${DOCTEST_CMAKE_HELPER}")
    doctest_discover_tests(socketpp_tests
        TEST_PREFIX "socketpp."
        PROPERTIES TIMEOUT 30
    )
endif()

if(SOCKETPP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

install(TARGETS socketpp
    EXPORT socketpp-targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)
install(DIRECTORY include/ DESTINATION include)
install(EXPORT socketpp-targets
    NAMESPACE socketpp::
    DESTINATION lib/cmake/socketpp
)

configure_package_config_file(
    cmake/socketpp-config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/socketpp-config.cmake"
    INSTALL_DESTINATION lib/cmake/socketpp
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/socketpp-config-version.cmake"
    VERSION 0.1.0
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/socketpp-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/socketpp-config-version.cmake"
    DESTINATION lib/cmake/socketpp
)
