cmake_minimum_required(VERSION 3.15)
project(dpdk_capture CXX)

# Use C++11 (DPDK is fine with this)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# DPDK locations
set(RTE_INCLUDE /usr/local/include)
set(RTE_LIB /usr/local/lib)

# DPDK pkg-config support (if available)
find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
    pkg_check_modules(DPDK libdpdk)
endif()

include_directories(
    ${CMAKE_SOURCE_DIR}/websocket_client
    ${RTE_INCLUDE}
)

if (DPDK_FOUND)
    include_directories(${DPDK_INCLUDE_DIRS})
    link_directories(${DPDK_LIBRARY_DIRS})
endif()

# Build target
add_executable(dpdk_capture dpdk_capture.cpp)

# Do NOT include kernel headers here — they cause typedef conflicts
target_include_directories(dpdk_capture PRIVATE
    ${RTE_INCLUDE}
    ${CMAKE_SOURCE_DIR}/websocket_client
)

# Define macros and link DPDK libraries
target_compile_definitions(dpdk_capture PRIVATE _GNU_SOURCE)

# Enable SSSE3 intrinsics required by DPDK memcpy implementations.
# Use -mssse3; if you prefer automatic tuning on the build host use -march=native instead.
target_compile_options(dpdk_capture PRIVATE -mssse3)

# (Optional) also enable other common x86 vector extensions if your DPDK install expects them:
# target_compile_options(dpdk_capture PRIVATE -msse4.1)

if (DPDK_FOUND)
    target_link_libraries(dpdk_capture PRIVATE ${DPDK_LIBRARIES})
else()
    target_link_libraries(dpdk_capture PRIVATE
        pthread
        dl
        rte_eal
        rte_mbuf
        rte_ethdev
    )
endif()
