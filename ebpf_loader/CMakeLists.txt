cmake_minimum_required(VERSION 3.15)
project(ebpf_loader C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBBPF REQUIRED libbpf)

execute_process(COMMAND uname -r OUTPUT_VARIABLE KVER OUTPUT_STRIP_TRAILING_WHITESPACE)

#-----------------------------------------------
# Directories for eBPF (only use UAPI headers)
# ---------------------------------------------
include_directories(
    ${LIBBPF_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/websocket_client
    /usr/include/bpf
    /usr/include
    /usr/include/x86_64-linux-gnu
)
#---------------------------------------------------

link_directories(${LIBBPF_LIBRARY_DIRS})


# ------------------------------------------------------------
# eBPF build (bpf_program.c)
# ------------------------------------------------------------
set(BPF_SRC ${CMAKE_CURRENT_SOURCE_DIR}/bpf_program.c)
set(BPF_OBJ ${CMAKE_CURRENT_BINARY_DIR}/bpf_program.o)


add_custom_command(
  OUTPUT ${BPF_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/ebpf_loader
  COMMAND clang -O2 -g -target bpf
          -D__x86_64__
          -D__TARGET_ARCH_x86
          -D__BPF_TRACING__
          -D_GNU_SOURCE
          -I/usr/include
          -I/usr/include/bpf
          -I/usr/include/x86_64-linux-gnu
          -I/usr/include/linux
          -I/usr/include/asm
          -I/usr/include/asm-generic
          -I/usr/include/uapi
          -c ${BPF_SRC}
          -o ${BPF_OBJ}
  DEPENDS ${BPF_SRC}
  COMMENT "Compiling eBPF program with libc UAPI headers (x86_64 safe)"
)

add_custom_target(bpf_program_build DEPENDS ${BPF_OBJ})

# ------------------------------------------------------------
# eBPF Loader (C++)
# ------------------------------------------------------------
# Build the eBPF loader (userland component)
add_executable(ebpf_loader ${CMAKE_CURRENT_SOURCE_DIR}/ebpf_loader.cpp)
add_dependencies(ebpf_loader bpf_program_build)

target_include_directories(ebpf_loader PRIVATE 
  ${LIBBPF_INCLUDE_DIRS} 
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ebpf_loader PRIVATE 
    ${LIBBPF_LIBRARIES}
    pthread 
    elf 
    z 
    rt
)


target_compile_options(ebpf_loader PRIVATE -Wall -Wextra -O3)

install(TARGETS ebpf_loader RUNTIME DESTINATION bin)
install(FILES  ${BPF_OBJ}
        DESTINATION share/ws-profiler)



