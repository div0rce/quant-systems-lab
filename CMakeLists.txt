cmake_minimum_required(VERSION 3.24)
project(quant-systems-lab VERSION 0.2.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/ProjectOptions.cmake)
include(cmake/CompilerWarnings.cmake)
include(cmake/Sanitizers.cmake)

add_library(qsl_core src/core/types.cpp src/protocol/codec.cpp src/protocol/fix.cpp
                     src/engine/order_book.cpp
                     src/engine/matching_engine.cpp src/engine/risk.cpp
                     src/gateway/order_gateway.cpp src/feed/market_data.cpp
                     src/feed/publisher.cpp src/replay/event_log.cpp src/replay/command.cpp
                     src/replay/recovery.cpp src/gateway/session.cpp src/gateway/tcp_server.cpp
                     src/gateway/epoll_server.cpp src/feed/udp_feed.cpp src/replay/fixture.cpp
                     src/replay/shrink.cpp)
target_include_directories(qsl_core PUBLIC include)
target_link_libraries(qsl_core PRIVATE qsl_warnings)

add_executable(qsl-loginspect apps/qsl-loginspect/main.cpp)
target_link_libraries(qsl-loginspect PRIVATE qsl_core qsl_warnings)

add_executable(qsl-replay apps/qsl-replay/main.cpp)
target_link_libraries(qsl-replay PRIVATE qsl_core qsl_warnings)

add_executable(qsl-gateway apps/qsl-gateway/main.cpp)
target_link_libraries(qsl-gateway PRIVATE qsl_core qsl_warnings)

add_executable(qsl-client apps/qsl-client/main.cpp)
target_link_libraries(qsl-client PRIVATE qsl_core qsl_warnings)

add_executable(qsl-mdfeed apps/qsl-mdfeed/main.cpp)
target_link_libraries(qsl-mdfeed PRIVATE qsl_core qsl_warnings)

add_executable(qsl-export-fixture apps/qsl-export-fixture/main.cpp)
target_link_libraries(qsl-export-fixture PRIVATE qsl_core qsl_warnings)

add_executable(qsl-export-stream apps/qsl-export-stream/main.cpp)
target_link_libraries(qsl-export-stream PRIVATE qsl_core qsl_warnings)

# Performance-evidence harness (PERFORMANCE.md). Separate binary from qsl-bench so it cannot perturb
# the qsl-bench numbers. The default target measures pure performance (throughput, latency, cycles).
add_executable(qsl-perfeval apps/qsl-perfeval/main.cpp)
target_link_libraries(qsl-perfeval PRIVATE qsl_core qsl_warnings)

# Same source, allocation-counting build: it overrides every global operator new to count ALL
# allocations (plain and over-aligned). Kept a separate target because those overrides add a little
# work per aligned allocation and would perturb cycle/throughput numbers, so performance is measured
# with qsl-perfeval and allocations/order with qsl-perfeval-allocs. See PERFORMANCE.md methodology.
add_executable(qsl-perfeval-allocs apps/qsl-perfeval/main.cpp)
target_link_libraries(qsl-perfeval-allocs PRIVATE qsl_core qsl_warnings)
target_compile_definitions(qsl-perfeval-allocs PRIVATE QSL_PERFEVAL_COUNT_ALLOCS)

if(QSL_BUILD_BENCHMARKS)
  find_package(Threads REQUIRED)
  add_executable(qsl-bench apps/qsl-bench/main.cpp benchmarks/bench_order_pool.cpp
                           benchmarks/bench_storage.cpp benchmarks/bench_false_sharing.cpp
                           benchmarks/bench_recovery.cpp)
  target_link_libraries(qsl-bench PRIVATE qsl_core qsl_warnings Threads::Threads)
endif()

if(QSL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
