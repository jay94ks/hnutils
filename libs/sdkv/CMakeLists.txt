cmake_minimum_required(VERSION 3.23)

include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

add_library(sdkv
    src/change.cpp
    src/common.cpp
    src/listener.cpp
    src/remote_table.cpp
    src/state.cpp
    src/subscription.cpp
    src/table.cpp
)
add_library(sdkv::sdkv ALIAS sdkv)
target_compile_features(sdkv PUBLIC cxx_std_20)
set_target_properties(sdkv PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(sdkv PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(sdkv PUBLIC asyncpp PRIVATE Threads::Threads)
if(BUILD_SHARED_LIBS)
    target_compile_definitions(sdkv PUBLIC SDKV_SHARED PRIVATE SDKV_BUILDING_LIBRARY)
endif()
if(MSVC)
    target_compile_options(sdkv PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(sdkv PRIVATE -Wall -Wextra -Wpedantic)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(sdkv PRIVATE -Wno-mismatched-new-delete)
    endif()
endif()

if(HNUTILS_BUILD_TESTS)
    add_executable(sdkv_tests tests/main.cpp tests/table_tests.cpp)
    target_link_libraries(sdkv_tests PRIVATE sdkv::sdkv doctest::doctest Threads::Threads)
    include("${DOCTEST_CMAKE_HELPER}")
    doctest_discover_tests(sdkv_tests TEST_PREFIX "sdkv." PROPERTIES TIMEOUT 30)
endif()

install(TARGETS sdkv EXPORT sdkv-targets
    ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)
install(DIRECTORY include/ DESTINATION include)
install(EXPORT sdkv-targets NAMESPACE sdkv:: DESTINATION lib/cmake/sdkv)
configure_package_config_file(cmake/sdkv-config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/sdkv-config.cmake"
    INSTALL_DESTINATION lib/cmake/sdkv
)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/sdkv-config-version.cmake"
    VERSION 0.1.0 COMPATIBILITY SameMajorVersion
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/sdkv-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/sdkv-config-version.cmake"
    DESTINATION lib/cmake/sdkv
)
