# Commands run concurrently as a pipeline. Hosts never read stdin or write
# stdout: an early exit must not break another host's output pipe (SIGPIPE).
# Validation failures use the shared stderr; all discovery and KV traffic is UDP.
if(NOT DEFINED EXAMPLE)
    message(FATAL_ERROR "Pass -DEXAMPLE=<sdkv_example_broadcast_kv path>")
endif()
execute_process(COMMAND "${EXAMPLE}" --pick-port
    OUTPUT_VARIABLE port OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE result TIMEOUT 3)
if(NOT result STREQUAL "0" OR NOT port MATCHES "^[0-9]+$")
    message(FATAL_ERROR "Could not choose an available shared UDP port: ${result}")
endif()
set(survivor --port "${port}" --duration 12 --silent --expect-keys 4 --expect-peak 5
    --expect-value shared-updated --expect-absent demo/host4)
execute_process(
    COMMAND "${EXAMPLE}" --id host1 ${survivor}
    COMMAND "${EXAMPLE}" --id host2 ${survivor}
    COMMAND "${EXAMPLE}" --id host3 ${survivor}
    COMMAND "${EXAMPLE}" --id host4 --port "${port}" --duration 3 --silent
        --expect-keys 5 --expect-peak 5 --expect-value shared-updated
    RESULTS_VARIABLE results OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 20
)
foreach(result IN LISTS results)
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "Four-process UDP KV scenario failed: ${results}\n${output}\n${errors}")
    endif()
endforeach()
message(STATUS "Four real processes discovered peers, replicated common values, and expired host4's keys.")
