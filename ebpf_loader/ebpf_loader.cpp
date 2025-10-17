// ebpf_loader_fixed.cpp
// Fixed loader with shared queue and socket correlation

#include "shared_events.h"

#include <bpf/libbpf.h>
#include <bpf/bpf.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <signal.h>

static volatile bool g_running = true;
static SharedQueue* g_shared_queue = nullptr;

void signal_handler(int signo) {
  if (signo == SIGINT || signo == SIGTERM) {
    g_running = false;
  }
}

// Event structure from BPF program
struct bpf_event {
  uint32_t type;
  uint64_t seq_no;
  uint64_t ts_nic_ns;
  uint64_t ts_kernel_ns;
  uint64_t ts_userspace_ns;
  int32_t sock_fd;
  uint64_t sock_ptr;
  uint32_t tcp_seq;
  uint32_t pid;
  uint32_t pkt_len;
  uint32_t data_len;
  uint16_t src_port;
  uint16_t dst_port;
  char data[512];
};

static int handle_event(void* ctx, void* data, size_t data_sz) {
  const bpf_event* e = static_cast<const bpf_event*>(data);
  
  std::cout << "[ebpf] pid=" << e->pid
            << " sockptr=0x" << std::hex << e->sock_ptr << std::dec
            << " bytes=" << e->pkt_len
            << " ts_ns=" << e->ts_kernel_ns << "\n";
  
  // Push to shared queue for correlation
  UnifiedEvent unified;
  memcpy(&unified, e, sizeof(UnifiedEvent));
  
  if (g_shared_queue && !shared_queue_push(g_shared_queue, &unified)) {
    std::cerr << "[ebpf] queue full, dropped event\n";
  }
  
  return 0;
}

int main(int argc, char** argv) {
  const char* obj_file = "bpf_program_fixed.o";
  if (argc >= 2) obj_file = argv[1];

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  // Open shared memory queue
  g_shared_queue = open_shared_queue(SHARED_QUEUE_NAME, 1 << 16);
  if (!g_shared_queue) {
    std::cerr << "Failed to open shared queue. Start websocket client first.\n";
    return 1;
  }

  struct bpf_object* obj = bpf_object__open_file(obj_file, nullptr);
  if (!obj) {
    std::cerr << "Failed to open bpf object\n";
    return 1;
  }

  if (bpf_object__load(obj)) {
    std::cerr << "Failed to load bpf object\n";
    return 1;
  }

  // Attach kprobe
  struct bpf_program* kprobe_prog = bpf_object__find_program_by_name(
      obj, "kprobe__tcp_recvmsg");
  if (!kprobe_prog) {
    std::cerr << "Failed to find kprobe program\n";
    return 1;
  }

  struct bpf_link* kprobe_link = bpf_program__attach(kprobe_prog);
  if (!kprobe_link) {
    std::cerr << "Failed to attach kprobe\n";
    return 1;
  }

  // Attach kretprobe
  struct bpf_program* kretprobe_prog = bpf_object__find_program_by_name(
      obj, "kretprobe__tcp_recvmsg");
  if (!kretprobe_prog) {
    std::cerr << "Failed to find kretprobe program\n";
    return 1;
  }

  struct bpf_link* kretprobe_link = bpf_program__attach(kretprobe_prog);
  if (!kretprobe_link) {
    std::cerr << "Failed to attach kretprobe\n";
    return 1;
  }

  // Get ring buffer map
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

  // Read WebSocket socket FD from shared memory
  int ws_fd = read_socket_fd_from_ebpf();
  if (ws_fd >= 0) {
    std::cout << "[ebpf] monitoring socket fd=" << ws_fd << "\n";
    
    // TODO: Get kernel socket pointer for this FD and store in socket_map
    // This requires reading /proc/<pid>/fdinfo/<fd> or using bpf_get_socket_cookie
  }

  std::cout << "[ebpf loader] waiting for events... (Ctrl+C to stop)\n";
  
  while (g_running) {
    int ret = ring_buffer__poll(rb, 100);
    if (ret < 0 && ret != -EINTR) {
      std::cerr << "ring buffer poll error: " << ret << "\n";
      break;
    }
  }

  std::cout << "\n[ebpf] shutting down...\n";

  ring_buffer__free(rb);
  bpf_link__destroy(kprobe_link);
  bpf_link__destroy(kretprobe_link);
  bpf_object__close(obj);
  
  return 0;
}
