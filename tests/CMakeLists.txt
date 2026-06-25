include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.5.2
)
FetchContent_MakeAvailable(Catch2)

include(Catch)

find_package(Threads REQUIRED)

foreach(t test_smoke test_types test_clock test_protocol test_order_book test_matching_engine
          test_risk_gateway test_market_data test_event_log test_replay test_session
          test_tcp_gateway test_epoll_gateway test_md_feed test_invariants test_fuzz_protocol
          test_fixture_export test_shrink test_oracle_selftest test_reject_coverage test_spsc_ring
          test_order_pool test_fix_protocol)
  add_executable(${t} unit/${t}.cpp)
  target_link_libraries(${t} PRIVATE qsl_core qsl_warnings Catch2::Catch2WithMain Threads::Threads)
  catch_discover_tests(${t})
endforeach()

# Labelled "concurrency" so the ThreadSanitizer run (`make tsan` / `ctest -L concurrency`) targets
# just the genuinely multithreaded tests.
foreach(t test_spsc_stress test_backpressure test_pipeline)
  add_executable(${t} concurrency/${t}.cpp)
  target_link_libraries(${t} PRIVATE qsl_core qsl_warnings Catch2::Catch2WithMain Threads::Threads)
  catch_discover_tests(${t} PROPERTIES LABELS concurrency)
endforeach()

set(QSL_REPLAY_MISSING_PATH "${CMAKE_CURRENT_BINARY_DIR}/qsl-replay-definitely-missing.log")
file(REMOVE "${QSL_REPLAY_MISSING_PATH}")
add_test(
  NAME qsl_replay_missing_log_fails
  COMMAND ${CMAKE_COMMAND} -D "PROGRAM=$<TARGET_FILE:qsl-replay>"
          -D "ARGS=${QSL_REPLAY_MISSING_PATH}" -D "EXPECT_OUTPUT=log read error"
          -D "REJECT_OUTPUT=records: 0" -P
          "${CMAKE_CURRENT_LIST_DIR}/cmake/expect_command_failure.cmake")

add_test(
  NAME qsl_replay_generate_missing_path_fails
  COMMAND ${CMAKE_COMMAND} -D "PROGRAM=$<TARGET_FILE:qsl-replay>" -D "ARGS=generate"
          -D "EXPECT_OUTPUT=usage:" -D "REJECT_OUTPUT=log read error" -P
          "${CMAKE_CURRENT_LIST_DIR}/cmake/expect_command_failure.cmake")

add_test(
  NAME qsl_replay_generate_extra_arg_fails
  COMMAND ${CMAKE_COMMAND} -D "PROGRAM=$<TARGET_FILE:qsl-replay>"
          -D "ARGS=generate;${CMAKE_CURRENT_BINARY_DIR}/qsl-replay-extra.log;42;extra"
          -D "EXPECT_OUTPUT=usage:" -D "REJECT_OUTPUT=wrote " -P
          "${CMAKE_CURRENT_LIST_DIR}/cmake/expect_command_failure.cmake")

add_test(
  NAME qsl_replay_recover_missing_path_fails
  COMMAND ${CMAKE_COMMAND} -D "PROGRAM=$<TARGET_FILE:qsl-replay>" -D "ARGS=recover"
          -D "EXPECT_OUTPUT=usage:" -D "REJECT_OUTPUT=log read error" -P
          "${CMAKE_CURRENT_LIST_DIR}/cmake/expect_command_failure.cmake")

add_test(
  NAME qsl_replay_append_loop_missing_mode_fails
  COMMAND ${CMAKE_COMMAND} -D "PROGRAM=$<TARGET_FILE:qsl-replay>"
          -D "ARGS=append-loop;${CMAKE_CURRENT_BINARY_DIR}/qsl-replay-append-loop.log"
          -D "EXPECT_OUTPUT=usage:" -D "REJECT_OUTPUT=cannot open" -P
          "${CMAKE_CURRENT_LIST_DIR}/cmake/expect_command_failure.cmake")

add_test(
  NAME qsl_export_stream_missing_mode_seed_fails
  COMMAND ${CMAKE_COMMAND} -D "PROGRAM=$<TARGET_FILE:qsl-export-stream>" -D "ARGS=prop"
          -D "EXPECT_OUTPUT=prop requires <seed>" -P
          "${CMAKE_CURRENT_LIST_DIR}/cmake/expect_command_failure.cmake")

add_test(
  NAME qsl_export_stream_invalid_seed_fails
  COMMAND ${CMAKE_COMMAND} -D "PROGRAM=$<TARGET_FILE:qsl-export-stream>" -D "ARGS=foo"
          -D "EXPECT_OUTPUT=seed must be an unsigned integer" -P
          "${CMAKE_CURRENT_LIST_DIR}/cmake/expect_command_failure.cmake")

add_test(
  NAME qsl_export_stream_invalid_orders_fails
  COMMAND ${CMAKE_COMMAND} -D "PROGRAM=$<TARGET_FILE:qsl-export-stream>" -D "ARGS=1;bad"
          -D "EXPECT_OUTPUT=orders must be an unsigned integer" -P
          "${CMAKE_CURRENT_LIST_DIR}/cmake/expect_command_failure.cmake")

# CLI argument hardening: malformed numeric args must print usage and exit non-zero, not throw out
# of main (std::terminate) or silently truncate an out-of-range port.
add_test(
  NAME qsl_client_invalid_port_fails
  COMMAND ${CMAKE_COMMAND} -D "PROGRAM=$<TARGET_FILE:qsl-client>" -D "ARGS=99xyz" -D
          "EXPECT_OUTPUT=usage:" -P
          "${CMAKE_CURRENT_LIST_DIR}/cmake/expect_command_failure.cmake")

add_test(
  NAME qsl_client_out_of_range_port_fails
  COMMAND ${CMAKE_COMMAND} -D "PROGRAM=$<TARGET_FILE:qsl-client>" -D "ARGS=70000" -D
          "EXPECT_OUTPUT=usage:" -P
          "${CMAKE_CURRENT_LIST_DIR}/cmake/expect_command_failure.cmake")

add_test(
  NAME qsl_mdfeed_invalid_count_fails
  COMMAND
    ${CMAKE_COMMAND} -D "PROGRAM=$<TARGET_FILE:qsl-mdfeed>" -D "ARGS=subscribe;9009;notanumber" -D
    "EXPECT_OUTPUT=usage:" -P "${CMAKE_CURRENT_LIST_DIR}/cmake/expect_command_failure.cmake")

add_test(
  NAME qsl_mdfeed_out_of_range_port_fails
  COMMAND ${CMAKE_COMMAND} -D "PROGRAM=$<TARGET_FILE:qsl-mdfeed>" -D "ARGS=publish;70000" -D
          "EXPECT_OUTPUT=usage:" -P
          "${CMAKE_CURRENT_LIST_DIR}/cmake/expect_command_failure.cmake")

add_test(
  NAME qsl_export_fixture_invalid_seed_fails
  COMMAND ${CMAKE_COMMAND} -D "PROGRAM=$<TARGET_FILE:qsl-export-fixture>" -D "ARGS=abc" -D
          "EXPECT_OUTPUT=usage:" -P
          "${CMAKE_CURRENT_LIST_DIR}/cmake/expect_command_failure.cmake")

# Performance-evidence harness smoke test (PERFORMANCE.md): a tiny run must complete and report.
add_test(NAME qsl_perfeval_smoke COMMAND qsl-perfeval 5000 --latency)
set_tests_properties(qsl_perfeval_smoke PROPERTIES PASS_REGULAR_EXPRESSION
                     "perfeval: latency_ns")

# Malformed order counts must be rejected, not truncated or wrapped.
add_test(NAME qsl_perfeval_invalid_count_fails
         COMMAND ${CMAKE_COMMAND} -D "PROGRAM=$<TARGET_FILE:qsl-perfeval>" -D "ARGS=123abc" -D
                 "EXPECT_OUTPUT=usage:" -P
                 "${CMAKE_CURRENT_LIST_DIR}/cmake/expect_command_failure.cmake")

# Shell unit tests for the shared artifact-publish helper (MAC sanitization +
# trailing-whitespace/blank-line trimming). Portable (sed/awk/mktemp); the script
# locates the repo via BASH_SOURCE, so it runs correctly by absolute path.
add_test(
  NAME qsl_common_publish_artifact
  COMMAND bash "${CMAKE_CURRENT_LIST_DIR}/shell/test_qsl_common.sh")

# Shell unit tests for the dependency-free flamegraph renderer (scripts/flamegraph.py:
# perf-script folding + deterministic SVG rendering) behind `make flamegraph` (#32).
# Portable: needs only python3 (skips cleanly if absent); does not require perf.
add_test(
  NAME qsl_flamegraph_render
  COMMAND bash "${CMAKE_CURRENT_LIST_DIR}/shell/test_flamegraph.sh")

if(EXISTS "/dev/full")
  add_test(
    NAME qsl_replay_generate_append_failure
    COMMAND ${CMAKE_COMMAND} -D "PROGRAM=$<TARGET_FILE:qsl-replay>"
            -D "ARGS=generate;/dev/full;42" -D "EXPECT_OUTPUT=append failed"
            -D "REJECT_OUTPUT=wrote " -P
            "${CMAKE_CURRENT_LIST_DIR}/cmake/expect_command_failure.cmake")
endif()
