add_library(sdkv_broadcast_kv_scenario STATIC
    broadcast_kv/protocol.cpp
    broadcast_kv/scenario.cpp
)
target_include_directories(sdkv_broadcast_kv_scenario PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/broadcast_kv")
target_link_libraries(sdkv_broadcast_kv_scenario PUBLIC sdkv::sdkv socketpp::socketpp)
target_compile_features(sdkv_broadcast_kv_scenario PUBLIC cxx_std_20)
set_target_properties(sdkv_broadcast_kv_scenario PROPERTIES CXX_EXTENSIONS OFF)
add_executable(sdkv_example_broadcast_kv broadcast_kv/main.cpp)
target_link_libraries(sdkv_example_broadcast_kv PRIVATE sdkv_broadcast_kv_scenario)

if(HNUTILS_BUILD_TESTS)
    add_executable(sdkv_broadcast_kv_tests ../tests/main.cpp broadcast_kv/tests.cpp)
    target_link_libraries(sdkv_broadcast_kv_tests PRIVATE sdkv_broadcast_kv_scenario doctest::doctest)
    include("${DOCTEST_CMAKE_HELPER}")
    doctest_discover_tests(sdkv_broadcast_kv_tests TEST_PREFIX "sdkv.example." PROPERTIES TIMEOUT 30)
    add_test(NAME sdkv.example.broadcast_kv.four_processes
        COMMAND "${CMAKE_COMMAND}" "-DEXAMPLE=$<TARGET_FILE:sdkv_example_broadcast_kv>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/broadcast_kv/four_hosts.cmake"
    )
    set_tests_properties(sdkv.example.broadcast_kv.four_processes PROPERTIES TIMEOUT 25 LABELS network)
endif()
