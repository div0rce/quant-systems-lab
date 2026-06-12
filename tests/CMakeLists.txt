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
          test_order_pool)
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

if(EXISTS "/dev/full")
  add_test(
    NAME qsl_replay_generate_append_failure
    COMMAND ${CMAKE_COMMAND} -D "PROGRAM=$<TARGET_FILE:qsl-replay>"
            -D "ARGS=generate;/dev/full;42" -D "EXPECT_OUTPUT=append failed"
            -D "REJECT_OUTPUT=wrote " -P
            "${CMAKE_CURRENT_LIST_DIR}/cmake/expect_command_failure.cmake")
endif()
