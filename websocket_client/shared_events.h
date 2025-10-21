// shared_events.h
// Unified event structure and shared memory queue for correlation

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#define SHARED_QUEUE_NAME "/ws_latency_queue"
#define SHARED_SOCKFD_NAME "/ws_sockfd_map"
#define MAX_DATA_SIZE 512

// Event types for correlation
enum EventType {
  EVENT_TYPE_NIC = 1,        // From DPDK
  EVENT_TYPE_KERNEL = 2,     // From eBPF
  EVENT_TYPE_USERSPACE = 3   // From WebSocket client
};

// Unified event structure with all timestamp types
struct UnifiedEvent {
  uint32_t type;              // EventType
  uint64_t seq_no;            // Sequential ID from userspace
  
  // Timestamps at different layers
  uint64_t ts_nic_ns;         // NIC RX timestamp (DPDK)
  uint64_t ts_kernel_ns;      // Kernel receive timestamp (eBPF)
  uint64_t ts_userspace_ns;   // User-space receive timestamp
  uint64_t ts_userspace_epoch_ns; // User-space receive timestamp in epoch ns (system_clock)
  uint64_t ts_cpu_deserialization; // Userspace deserialization end timestamp (absolute)
  uint64_t src_send_ts_ns;    // Exchange send timestamp from WS payload (absolute, ns)
  
  // Correlation fields
  int32_t sock_fd;            // Socket file descriptor
  uint64_t sock_ptr;          // Kernel socket pointer (from eBPF)
  uint32_t tcp_seq;           // TCP sequence number
  uint32_t pid;               // Process ID
  
  // Packet/message info
  uint32_t pkt_len;           // Packet length (for NIC/kernel events)
  uint32_t data_len;          // Payload data length
  uint16_t src_port;          // Source port
  uint16_t dst_port;          // Destination port
  
  char data[MAX_DATA_SIZE];   // Payload snippet or metadata
} __attribute__((aligned(64)));

// Lock-free shared memory queue structure
struct SharedQueue {
  std::atomic<uint64_t> head;
  std::atomic<uint64_t> tail;
  uint64_t capacity;
  uint64_t mask;
  UnifiedEvent events[0];  // Flexible array member
} __attribute__((aligned(64)));

// Create or open shared memory queue
static inline SharedQueue* create_shared_queue(const char* name, size_t capacity) {
  // Capacity must be power of 2
  if ((capacity & (capacity - 1)) != 0) {
    return nullptr;
  }
  
  size_t total_size = sizeof(SharedQueue) + capacity * sizeof(UnifiedEvent);
  
  int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
  if (fd < 0) {
    return nullptr;
  }
  
  if (ftruncate(fd, total_size) < 0) {
    close(fd);
    return nullptr;
  }
  
  void* ptr = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  
  if (ptr == MAP_FAILED) {
    return nullptr;
  }
  
  SharedQueue* q = static_cast<SharedQueue*>(ptr);
  q->head.store(0, std::memory_order_relaxed);
  q->tail.store(0, std::memory_order_relaxed);
  q->capacity = capacity;
  q->mask = capacity - 1;
  
  return q;
}

// Open existing shared queue
static inline SharedQueue* open_shared_queue(const char* name, size_t capacity) {
  size_t total_size = sizeof(SharedQueue) + capacity * sizeof(UnifiedEvent);
  
  int fd = shm_open(name, O_RDWR, 0666);
  if (fd < 0) {
    return nullptr;
  }
  
  void* ptr = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  
  if (ptr == MAP_FAILED) {
    return nullptr;
  }
  
  return static_cast<SharedQueue*>(ptr);
}

// Push event to queue (returns false if full)
static inline bool shared_queue_push(SharedQueue* q, const UnifiedEvent* ev) {
  uint64_t head = q->head.load(std::memory_order_acquire);
  uint64_t tail = q->tail.load(std::memory_order_acquire);
  
  if (head - tail >= q->capacity) {
    return false;  // Queue full
  }
  
  uint64_t idx = head & q->mask;
  memcpy(&q->events[idx], ev, sizeof(UnifiedEvent));
  
  q->head.store(head + 1, std::memory_order_release);
  return true;
}

// Pop event from queue (returns false if empty)
static inline bool shared_queue_pop(SharedQueue* q, UnifiedEvent* ev) {
  uint64_t head = q->head.load(std::memory_order_acquire);
  uint64_t tail = q->tail.load(std::memory_order_acquire);
  
  if (tail >= head) {
    return false;  // Queue empty
  }
  
  uint64_t idx = tail & q->mask;
  memcpy(ev, &q->events[idx], sizeof(UnifiedEvent));
  
  q->tail.store(tail + 1, std::memory_order_release);
  return true;
}

// Cleanup shared queue
static inline void cleanup_shared_queue(SharedQueue* q) {
  if (q) {
    size_t total_size = sizeof(SharedQueue) + q->capacity * sizeof(UnifiedEvent);
    munmap(q, total_size);
  }
  shm_unlink(SHARED_QUEUE_NAME);
}

// Socket FD sharing for eBPF correlation
struct SocketFdMap {
  std::atomic<int32_t> websocket_fd;
  std::atomic<uint64_t> sock_ptr;  // Filled by eBPF
} __attribute__((aligned(64)));

static inline void write_socket_fd_to_ebpf(int fd) {
  int shm_fd = shm_open(SHARED_SOCKFD_NAME, O_CREAT | O_RDWR, 0666);
  if (shm_fd < 0) return;
  
  ftruncate(shm_fd, sizeof(SocketFdMap));
  
  SocketFdMap* map = static_cast<SocketFdMap*>(
      mmap(nullptr, sizeof(SocketFdMap), PROT_READ | PROT_WRITE, 
           MAP_SHARED, shm_fd, 0));
  close(shm_fd);
  
  if (map != MAP_FAILED) {
    map->websocket_fd.store(fd, std::memory_order_release);
    munmap(map, sizeof(SocketFdMap));
  }
}

static inline int read_socket_fd_from_ebpf() {
  int shm_fd = shm_open(SHARED_SOCKFD_NAME, O_RDONLY, 0666);
  if (shm_fd < 0) return -1;
  
  SocketFdMap* map = static_cast<SocketFdMap*>(
      mmap(nullptr, sizeof(SocketFdMap), PROT_READ, MAP_SHARED, shm_fd, 0));
  close(shm_fd);
  
  if (map == MAP_FAILED) return -1;
  
  int fd = map->websocket_fd.load(std::memory_order_acquire);
  munmap(map, sizeof(SocketFdMap));
  
  return fd;
}