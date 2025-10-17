project(tests)

include_directories(
    ${CMAKE_SOURCE_DIR}/websocket_client
)

add_executable(rtt_test rtt_test.cpp)

