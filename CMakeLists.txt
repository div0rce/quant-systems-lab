cmake_minimum_required(VERSION 3.24)
project(quant-systems-lab VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/ProjectOptions.cmake)
include(cmake/CompilerWarnings.cmake)
include(cmake/Sanitizers.cmake)

add_library(qsl_core src/core/types.cpp src/protocol/codec.cpp src/engine/order_book.cpp
                     src/engine/matching_engine.cpp src/engine/risk.cpp
                     src/gateway/order_gateway.cpp)
target_include_directories(qsl_core PUBLIC include)
target_link_libraries(qsl_core PRIVATE qsl_warnings)

enable_testing()
add_subdirectory(tests)
