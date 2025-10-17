// ebpf_loader.cpp
// Build with libbpf and -lelf -lz
#include <bpf/libbpf.h>
#include <bpf/bpf.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <thread>

using namespace std;

struct event {
  uint64_t ts_ns;
  uint64_t sockptr;
  uint32_t copied;
  uint32_t pid;
};

static int handle_event(void* ctx, void* data, size_t data_sz) {
  const event* e = static_cast<const event*>(data);
  std::cout << "[ebpf] pid=" << e->pid
            << " sockptr=0x" << std::hex << e->sockptr << std::dec
            << " copied=" << e->copied
            << " ts_ns=" << e->ts_ns << "\n";
  return 0;
}

int main(int argc, char** argv) {
  const char* obj_file = "bpf_program.o";
  if (argc >= 2) obj_file = argv[1];

  struct bpf_object* obj = nullptr;
  int err = 0;

  obj = bpf_object__open_file(obj_file, nullptr);
  if (!obj) {
    std::cerr << "Failed to open bpf object\n";
    return 1;
  }

  if (bpf_object__load(obj)) {
    std::cerr << "Failed to load bpf object\n";
    return 1;
  }

  // get the map fd for ringbuf
  int map_fd = bpf_object__find_map_fd_by_name(obj, "rb");
  if (map_fd < 0) {
    std::cerr << "Failed to find map 'rb'\n";
    return 1;
  }

  struct ring_buffer* rb = ring_buffer__new(map_fd, handle_event, nullptr, nullptr);
  if (!rb) {
    std::cerr << "Failed to create ring buffer\n";
    return 1;
  }

  std::cout << "[ebpf loader] waiting for events...\n";
  while (true) {
    int ret = ring_buffer__poll(rb, 100 /*ms*/);
    if (ret < 0) {
      std::cerr << "ring buffer poll error\n";
      break;
    }
  }

  ring_buffer__free(rb);
  bpf_object__close(obj);
  return 0;
}
