include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.5.2
)
FetchContent_MakeAvailable(Catch2)

include(Catch)

foreach(t test_smoke test_types test_clock test_protocol test_order_book test_matching_engine
          test_risk_gateway test_market_data test_event_log test_replay test_session
          test_tcp_gateway test_md_feed test_invariants test_fuzz_protocol)
  add_executable(${t} unit/${t}.cpp)
  target_link_libraries(${t} PRIVATE qsl_core qsl_warnings Catch2::Catch2WithMain)
  catch_discover_tests(${t})
endforeach()

set(QSL_REPLAY_MISSING_PATH "${CMAKE_CURRENT_BINARY_DIR}/qsl-replay-definitely-missing.log")
file(REMOVE "${QSL_REPLAY_MISSING_PATH}")
add_test(
  NAME qsl_replay_missing_log_fails
  COMMAND ${CMAKE_COMMAND} -D "PROGRAM=$<TARGET_FILE:qsl-replay>"
          -D "ARGS=${QSL_REPLAY_MISSING_PATH}" -D "EXPECT_OUTPUT=log read error"
          -D "REJECT_OUTPUT=records: 0" -P
          "${CMAKE_CURRENT_LIST_DIR}/cmake/expect_command_failure.cmake")

if(EXISTS "/dev/full")
  add_test(
    NAME qsl_replay_generate_append_failure
    COMMAND ${CMAKE_COMMAND} -D "PROGRAM=$<TARGET_FILE:qsl-replay>"
            -D "ARGS=generate;/dev/full;42" -D "EXPECT_OUTPUT=append failed"
            -D "REJECT_OUTPUT=wrote " -P
            "${CMAKE_CURRENT_LIST_DIR}/cmake/expect_command_failure.cmake")
endif()
