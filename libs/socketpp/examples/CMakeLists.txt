set(SOCKETPP_EXAMPLES
    address
    socket
    async_socket
    watcher
    tcp_listener
    tcp_client
    tcp_listener_async
    tcp_client_async
    udp_socket
    udp_socket_async
    udp_broadcast
    udp_broadcast_async
)

foreach(example IN LISTS SOCKETPP_EXAMPLES)
    set(example_target "socketpp_example_${example}")
    add_executable("${example_target}" "${example}.cpp")
    target_link_libraries("${example_target}" PRIVATE socketpp::socketpp)
    target_compile_features("${example_target}" PRIVATE cxx_std_20)
    set_target_properties("${example_target}" PROPERTIES CXX_EXTENSIONS OFF)
    if(HNUTILS_BUILD_TESTS)
        add_test(NAME "socketpp.example.${example}" COMMAND "${example_target}")
        set_tests_properties("socketpp.example.${example}" PROPERTIES TIMEOUT 30)
    endif()
endforeach()
