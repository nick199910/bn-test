cmake_minimum_required(VERSION 3.15)
project(ws_client_delay CXX)

find_package(Boost REQUIRED COMPONENTS system thread)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# 使用 spdlog 作为日志库：优先本地已安装，其次自动拉取
include(FetchContent)
set(SPDLOG_FALLBACK OFF)
find_package(spdlog CONFIG QUIET)
if (NOT spdlog_FOUND)
  set(SPDLOG_FALLBACK ON)
  message(STATUS "spdlog not found in system. Falling back to FetchContent...")
  FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.13.0
  )
  FetchContent_MakeAvailable(spdlog)
endif()

# simdjson: header-only with small static lib. Try config, fallback to FetchContent.
find_package(simdjson CONFIG QUIET)
if (NOT simdjson_FOUND)
  message(STATUS "simdjson not found in system. Falling back to FetchContent...")
  FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG v3.9.1
  )
  FetchContent_MakeAvailable(simdjson)
endif()

# ZeroMQ: message queue for MSK simulation
find_package(cppzmq CONFIG QUIET)
if (NOT cppzmq_FOUND)
  message(STATUS "cppzmq not found in system. Attempting to find libzmq...")
  find_package(PkgConfig)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(ZMQ libzmq)
    if(ZMQ_FOUND)
      message(STATUS "libzmq found via pkg-config")
    else()
      message(WARNING "ZeroMQ not found. Install with: sudo apt-get install libzmq3-dev")
    endif()
  endif()
endif()

# websocketpp is header-only: expect it installed in system include path
add_executable(ws_client_delay ws_client.cpp)

target_include_directories(ws_client_delay PRIVATE
  ${Boost_INCLUDE_DIRS}
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ws_client_delay PRIVATE
  ${Boost_LIBRARIES}
  Threads::Threads
  OpenSSL::SSL
  OpenSSL::Crypto
  rt
)
target_link_libraries(ws_client_delay PRIVATE spdlog::spdlog)
target_link_libraries(ws_client_delay PRIVATE simdjson::simdjson)

# Link ZeroMQ if available
if(cppzmq_FOUND)
  target_link_libraries(ws_client_delay PRIVATE cppzmq)
  target_compile_definitions(ws_client_delay PRIVATE HAVE_ZMQ)
elseif(ZMQ_FOUND)
  target_include_directories(ws_client_delay PRIVATE ${ZMQ_INCLUDE_DIRS})
  target_link_libraries(ws_client_delay PRIVATE ${ZMQ_LIBRARIES})
  target_compile_definitions(ws_client_delay PRIVATE HAVE_ZMQ)
endif()

target_compile_options(ws_client_delay PRIVATE -Wall -Wextra -O3)

install(TARGETS ws_client_delay RUNTIME DESTINATION bin)