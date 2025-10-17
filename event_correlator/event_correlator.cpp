// event_correlator.cpp
// Analyzes events from shared queue and correlates NIC->Kernel->Userspace timestamps

#include "shared_events.h"

#include <algorithm>
#include <deque>
#include <iostream>
#include <map>
#include <signal.h>
#include <thread>
#include <vector>

static volatile bool g_running = true;

void signal_handler(int signo) {
  if (signo == SIGINT || signo == SIGTERM) {
    g_running = false;
  }
}

// Correlation window: events within this time delta can be correlated
const uint64_t CORRELATION_WINDOW_NS = 10000000; 

struct CorrelatedEvent {
  uint64_t ts_nic_ns;
  uint64_t ts_kernel_ns;
  uint64_t ts_userspace_ns;
  uint64_t seq_no;
  uint32_t tcp_seq;
  uint64_t sock_ptr;
  int32_t sock_fd;
  
  uint64_t latency_nic_to_kernel() const {
    return (ts_kernel_ns > ts_nic_ns) ? (ts_kernel_ns - ts_nic_ns) : 0;
  }
  
  uint64_t latency_kernel_to_user() const {
    return (ts_userspace_ns > ts_kernel_ns) ? (ts_userspace_ns - ts_kernel_ns) : 0;
  }
  
  uint64_t latency_nic_to_user() const {
    return (ts_userspace_ns > ts_nic_ns) ? (ts_userspace_ns - ts_nic_ns) : 0;
  }
  
  bool is_complete() const {
    return ts_nic_ns > 0 && ts_kernel_ns > 0 && ts_userspace_ns > 0;
  }
};

class EventCorrelator {
private:
  std::deque<UnifiedEvent> nic_events_;
  std::deque<UnifiedEvent> kernel_events_;
  std::deque<UnifiedEvent> user_events_;
  
  std::vector<CorrelatedEvent> completed_;
  
  uint64_t total_nic_ = 0;
  uint64_t total_kernel_ = 0;
  uint64_t total_user_ = 0;
  uint64_t total_correlated_ = 0;
  
public:
  void add_event(const UnifiedEvent& ev) {
    switch (ev.type) {
      case EVENT_TYPE_NIC:
        nic_events_.push_back(ev);
        total_nic_++;
        break;
      case EVENT_TYPE_KERNEL:
        kernel_events_.push_back(ev);
        total_kernel_++;
        break;
      case EVENT_TYPE_USERSPACE:
        user_events_.push_back(ev);
        total_user_++;
        break;
    }
  }
  
  void correlate() {
    // Try to match userspace events with kernel events
    // Match by: close timestamp proximity, socket pointer, or TCP sequence
    
    for (auto& user_ev : user_events_) {
      // Find matching kernel event
      for (auto& kern_ev : kernel_events_) {
        if (should_correlate_kernel_user(kern_ev, user_ev)) {
          // Now try to find matching NIC event
          for (auto& nic_ev : nic_events_) {
            if (should_correlate_nic_kernel(nic_ev, kern_ev)) {
              // Full correlation found!
              CorrelatedEvent corr;
              corr.ts_nic_ns = nic_ev.ts_nic_ns;
              corr.ts_kernel_ns = kern_ev.ts_kernel_ns;
              corr.ts_userspace_ns = user_ev.ts_userspace_ns;
              corr.seq_no = user_ev.seq_no;
              corr.tcp_seq = nic_ev.tcp_seq;
              corr.sock_ptr = kern_ev.sock_ptr;
              corr.sock_fd = user_ev.sock_fd;
              
              completed_.push_back(corr);
              total_correlated_++;
              
              print_correlation(corr);
              break;
            }
          }
          break;
        }
      }
    }
    
    // Clean up old events outside correlation window
    cleanup_old_events();
  }
  
  void print_stats() const {
    std::cout << "\n=== Correlation Statistics ===\n";
    std::cout << "NIC events:       " << total_nic_ << "\n";
    std::cout << "Kernel events:    " << total_kernel_ << "\n";
    std::cout << "Userspace events: " << total_user_ << "\n";
    std::cout << "Correlated:       " << total_correlated_ << "\n";
    std::cout << "Pending NIC:      " << nic_events_.size() << "\n";
    std::cout << "Pending Kernel:   " << kernel_events_.size() << "\n";
    std::cout << "Pending User:     " << user_events_.size() << "\n";
    
    if (!completed_.empty()) {
      uint64_t sum_nic_kernel = 0;
      uint64_t sum_kernel_user = 0;
      uint64_t sum_total = 0;
      
      for (const auto& ev : completed_) {
        sum_nic_kernel += ev.latency_nic_to_kernel();
        sum_kernel_user += ev.latency_kernel_to_user();
        sum_total += ev.latency_nic_to_user();
      }
      
      std::cout << "\n=== Average Latencies ===\n";
      std::cout << "NIC->Kernel:   " << (sum_nic_kernel / completed_.size() / 1000) << " us\n";
      std::cout << "Kernel->User:  " << (sum_kernel_user / completed_.size() / 1000) << " us\n";
      std::cout << "NIC->User:     " << (sum_total / completed_.size() / 1000) << " us\n";
    }
  }
  
private:
  bool should_correlate_kernel_user(const UnifiedEvent& kern, const UnifiedEvent& user) {
    // Match by socket pointer and timestamp proximity
    if (kern.sock_ptr == 0 || user.sock_fd < 0) {
      // Fallback to timestamp proximity
      if (user.ts_userspace_ns < kern.ts_kernel_ns) return false;
      uint64_t delta = user.ts_userspace_ns - kern.ts_kernel_ns;
      return delta < CORRELATION_WINDOW_NS;
    }
    
    // If we have socket information, use it for matching
    // (requires socket_ptr to be populated from sock_fd)
    return true;
  }
  
  bool should_correlate_nic_kernel(const UnifiedEvent& nic, const UnifiedEvent& kern) {
    // Match by TCP sequence number and timestamp proximity
    if (nic.tcp_seq != 0 && kern.tcp_seq != 0 && nic.tcp_seq == kern.tcp_seq) {
      return true;
    }
    
    // Fallback to timestamp proximity
    if (kern.ts_kernel_ns < nic.ts_nic_ns) return false;
    uint64_t delta = kern.ts_kernel_ns - nic.ts_nic_ns;
    return delta < CORRELATION_WINDOW_NS;
  }
  
  void cleanup_old_events() {
    uint64_t now = user_events_.empty() ? 0 : user_events_.back().ts_userspace_ns;
    if (now == 0) return;
    
    // Remove NIC events older than correlation window
    while (!nic_events_.empty() && 
           (now - nic_events_.front().ts_nic_ns) > CORRELATION_WINDOW_NS * 2) {
      nic_events_.pop_front();
    }
    
    // Remove kernel events older than correlation window
    while (!kernel_events_.empty() && 
           (now - kernel_events_.front().ts_kernel_ns) > CORRELATION_WINDOW_NS * 2) {
      kernel_events_.pop_front();
    }
    
    // Remove user events that have been processed
    if (user_events_.size() > 100) {
      user_events_.erase(user_events_.begin(), user_events_.begin() + 50);
    }
  }
  
  void print_correlation(const CorrelatedEvent& ev) {
    std::cout << "[CORR] seq=" << ev.seq_no
              << " tcp_seq=" << ev.tcp_seq
              << " NIC->Kernel=" << (ev.latency_nic_to_kernel() / 1000) << "us"
              << " Kernel->User=" << (ev.latency_kernel_to_user() / 1000) << "us"
              << " Total=" << (ev.latency_nic_to_user() / 1000) << "us\n";
  }
};

int main(int argc, char** argv) {
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
  
  // Open shared memory queue
  SharedQueue* queue = open_shared_queue(SHARED_QUEUE_NAME, 1 << 16);
  if (!queue) {
    std::cerr << "Failed to open shared queue. Start websocket client first.\n";
    return 1;
  }
  
  std::cout << "[correlator] Reading events from shared queue...\n";
  std::cout << "[correlator] Press Ctrl+C to stop and see statistics\n\n";
  
  EventCorrelator correlator;
  
  UnifiedEvent ev;
  uint64_t last_stats_time = 0;
  
  while (g_running) {
    if (shared_queue_pop(queue, &ev)) {
      correlator.add_event(ev);
      
      // Try correlation every 100 events
      static int event_count = 0;
      if (++event_count % 100 == 0) {
        correlator.correlate();
      }
      
      // Print stats every 5 seconds
      uint64_t now = ev.ts_userspace_ns;
      if (now - last_stats_time > 5000000000ULL) {
        correlator.print_stats();
        last_stats_time = now;
      }
    } else {
      // No events, sleep briefly
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  
  // Final correlation pass
  std::cout << "\n[correlator] Performing final correlation...\n";
  correlator.correlate();
  correlator.print_stats();
  
  return 0;
}