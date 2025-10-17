cmake_minimum_required(VERSION 3.16)
project(binance_ws_latency_demo LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost REQUIRED COMPONENTS system thread)
include_directories(${Boost_INCLUDE_DIRS})


add_subdirectory(websocket_client)
add_subdirectory(ebpf_loader)
add_subdirectory(dpdk_capture)
add_subdirectory(tests)
