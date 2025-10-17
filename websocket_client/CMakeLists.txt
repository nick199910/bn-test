project(websocket_client)
find_package(Boost REQUIRED COMPONENTS system thread)
# websocketpp is header-only: expect it installed in system include path or vendor it.
# You will likely need to install websocketpp (e.g. apt install libwebsocketpp-dev) or add include path.
add_executable(websocket_client websocket_client.cpp)
target_include_directories(websocket_client PRIVATE ${Boost_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(websocket_client PRIVATE ${Boost_LIBRARIES} pthread ssl crypto)
