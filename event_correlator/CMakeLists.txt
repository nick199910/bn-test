project(event_correlator)

include_directories(
    ${CMAKE_SOURCE_DIR}/websocket_client
)

add_executable(event_correlator event_correlator.cpp)
target_link_libraries(event_correlator
  rt
  Threads::Threads
)


target_compile_options(event_correlator PRIVATE -Wall -Wextra -O3)

install(TARGETS event_correlator RUNTIME DESTINATION bin)


