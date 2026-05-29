include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.5.2
)
FetchContent_MakeAvailable(Catch2)

add_executable(test_smoke unit/test_smoke.cpp)
target_link_libraries(test_smoke PRIVATE qsl_core Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(test_smoke)
