cmake_minimum_required(VERSION 3.15)
project(normal_ws CXX)

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

# RapidJSON: header-only JSON library
find_path(RAPIDJSON_INCLUDE_DIRS "rapidjson/document.h"
  PATHS /usr/local/include /usr/include
  NO_DEFAULT_PATH
)

if (NOT RAPIDJSON_INCLUDE_DIRS)
  message(STATUS "RapidJSON not found in system. Falling back to FetchContent...")
  set(RAPIDJSON_BUILD_DOC OFF CACHE BOOL "" FORCE)
  set(RAPIDJSON_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(RAPIDJSON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    rapidjson
    GIT_REPOSITORY https://github.com/Tencent/rapidjson.git
    GIT_TAG v1.1.0
  )
  FetchContent_MakeAvailable(rapidjson)
  set(RAPIDJSON_INCLUDE_DIRS ${rapidjson_SOURCE_DIR}/include)
else()
  message(STATUS "RapidJSON found at: ${RAPIDJSON_INCLUDE_DIRS}")
endif()

# rdkafka: Kafka C/C++ client library (optional)
find_package(RdKafka CONFIG QUIET)
if (NOT RdKafka_FOUND)
  message(STATUS "RdKafka not found in system. Building without Kafka support.")
  message(STATUS "  To enable Kafka: sudo apt-get install librdkafka-dev")
  set(HAVE_RDKAFKA FALSE)
else()
  set(HAVE_RDKAFKA TRUE)
endif()

# websocketpp is header-only: expect it installed in system include path
add_executable(binance_market_nologger binance_market_nologger_00.cc)

target_include_directories(binance_market_nologger PRIVATE
  ${Boost_INCLUDE_DIRS}
  ${RAPIDJSON_INCLUDE_DIRS}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(binance_market_nologger PRIVATE
  ${Boost_LIBRARIES}
  Threads::Threads
  OpenSSL::SSL
  OpenSSL::Crypto
  rt
  spdlog::spdlog
)

# Link rdkafka only if found
if(HAVE_RDKAFKA)
  target_link_libraries(binance_market_nologger PRIVATE
    RdKafka::rdkafka
    RdKafka::rdkafka++
  )
  target_compile_definitions(binance_market_nologger PRIVATE HAVE_RDKAFKA)
endif()

target_compile_options(binance_market_nologger PRIVATE 
  -Wall -Wextra -O3
  -Wno-class-memaccess  # Ignore RapidJSON memcpy warnings
)

# 如果定义了 EVN_JAPAN 环境变量，添加编译定义
if(DEFINED ENV{EVN_JAPAN})
  target_compile_definitions(binance_market_nologger PRIVATE EVN_JAPAN)
endif()

install(TARGETS binance_market_nologger RUNTIME DESTINATION bin)
