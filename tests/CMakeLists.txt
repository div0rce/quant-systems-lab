include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.5.2
)
FetchContent_MakeAvailable(Catch2)

include(Catch)

foreach(t test_smoke test_types test_clock test_protocol test_order_book test_matching_engine
          test_risk_gateway test_market_data test_event_log)
  add_executable(${t} unit/${t}.cpp)
  target_link_libraries(${t} PRIVATE qsl_core qsl_warnings Catch2::Catch2WithMain)
  catch_discover_tests(${t})
endforeach()
