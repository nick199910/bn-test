// dpdk_capture_fixed.cpp
// DPDK capture with proper TSC conversion and packet parsing

#include "shared_events.h"

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_cycles.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <signal.h>
#include <time.h>

static SharedQueue* g_shared_queue = nullptr;
static volatile bool g_running = true;

void signal_handler(int signo) {
  if (signo == SIGINT || signo == SIGTERM) {
    g_running = false;
  }
}

// Parse Ethernet frame with proper header checks
bool parse_tcp_packet(uint8_t* data, uint16_t pkt_len,
                      uint16_t* src_port, uint16_t* dst_port, uint32_t* tcp_seq) {
  *src_port = 0;
  *dst_port = 0;
  *tcp_seq = 0;
  
  // Minimum: Ethernet(14) + IP(20) + TCP(20)
  if (pkt_len < 54) {
    return false;
  }
  
  uint16_t offset = 0;
  
  // Check for VLAN tag (EtherType 0x8100)
  uint16_t ethertype = (data[12] << 8) | data[13];
  if (ethertype == 0x8100) {
    // VLAN tag present, skip 4 bytes
    offset = 18;
    ethertype = (data[16] << 8) | data[17];
  } else {
    offset = 14;
  }
  
  // Check for IPv4 (EtherType 0x0800)
  if (ethertype != 0x0800) {
    return false;
  }
  
  if (pkt_len < offset + 20) {
    return false;
  }
  
  // Get IP header length (IHL field in nibbles, convert to bytes)
  uint8_t ihl = (data[offset] & 0x0f) * 4;
  if (ihl < 20) {
    return false;
  }
  
  // Check if it's TCP (protocol 6)
  uint8_t ip_proto = data[offset + 9];
  if (ip_proto != 6) {
    return false;
  }
  
  // Check if packet has full IP header + TCP header
  if (pkt_len < offset + ihl + 20) {
    return false;
  }
  
  // TCP header starts after IP header
  uint16_t tcp_offset = offset + ihl;
  
  *src_port = (data[tcp_offset] << 8) | data[tcp_offset + 1];
  *dst_port = (data[tcp_offset + 2] << 8) | data[tcp_offset + 3];
  *tcp_seq = (data[tcp_offset + 4] << 24) | (data[tcp_offset + 5] << 16) |
             (data[tcp_offset + 6] << 8) | data[tcp_offset + 7];
  
  return true;
}

int main(int argc, char** argv) {
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
  
  // Initialize EAL
  int ret = rte_eal_init(argc, argv);
  if (ret < 0) {
    std::cerr << "Failed to init EAL\n";
    return 1;
  }
  
  // Open shared memory queue
  g_shared_queue = open_shared_queue(SHARED_QUEUE_NAME, 1 << 16);
  if (!g_shared_queue) {
    std::cerr << "Failed to open shared queue. Start websocket client first.\n";
    return 1;
  }
  
  uint16_t port_id = 0;
  
  // Check if port exists
  if (!rte_eth_dev_is_valid_port(port_id)) {
    std::cerr << "Port " << port_id << " is not valid\n";
    return 1;
  }
  
  // Get port info
  struct rte_eth_dev_info dev_info;
  ret = rte_eth_dev_info_get(port_id, &dev_info);
  if (ret != 0) {
    std::cerr << "Failed to get device info\n";
    return 1;
  }
  
  std::cout << "[dpdk] Using port " << port_id << " (" << dev_info.driver_name << ")\n";
 
  // 创建 MBUF 内存池
  const unsigned nb_mbufs = 8192;
  const unsigned mbuf_cache = 256;
  rte_mempool* mbuf_pool = rte_pktmbuf_pool_create(
      "MBUF_POOL", nb_mbufs, mbuf_cache, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
  if (!mbuf_pool) {
    std::cerr << "Failed to create mbuf pool\n";
    return 1;
  }

  // 配置端口（1 个 RX 队列、0 个 TX 队列）
  rte_eth_conf port_conf{};
  port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
  ret = rte_eth_dev_configure(port_id, 1 /*rx*/, 0 /*tx*/, &port_conf);
  if (ret < 0) {
    std::cerr << "Failed to configure port\n";
    return 1;
  }

  // 设置 RX 队列
  const uint16_t rx_desc = 512;
  ret = rte_eth_rx_queue_setup(port_id, 0, rx_desc, rte_eth_dev_socket_id(port_id), nullptr, mbuf_pool);
  if (ret < 0) {
    std::cerr << "Failed to setup rx queue\n";
    return 1;
  }

  // 启动端口
  ret = rte_eth_dev_start(port_id);
  if (ret < 0) {
    std::cerr << "Failed to start device\n";
    return 1;
  }
  rte_eth_promiscuous_enable(port_id);

  const uint16_t burst_size = 32;
  rte_mbuf* bufs[burst_size];
  double tsc_hz = rte_get_tsc_hz();
 
  std::cout << "[dpdk] TSC frequency: " << (tsc_hz / 1e9) << " GHz\n";
  std::cout << "[dpdk] Capturing packets... (Ctrl+C to stop)\n";
  
  uint64_t total_packets = 0;
  uint64_t total_tcp = 0;
  uint64_t last_report_tsc = rte_get_tsc_cycles();
  
  while (g_running) {
    uint16_t nb_rxed = rte_eth_rx_burst(port_id, 0, bufs, burst_size);
    
    if (nb_rxed == 0) {
      rte_pause();
      continue;
    }
    
    total_packets += nb_rxed;
    
    for (uint16_t i = 0; i < nb_rxed; ++i) {
      rte_mbuf* m = bufs[i];
      
      // Use CLOCK_MONOTONIC to align with kernel ktime_get_ns()
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      uint64_t ts_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
      
      uint8_t* data = rte_pktmbuf_mtod(m, uint8_t*);
      uint16_t pkt_len = rte_pktmbuf_pkt_len(m);
      
      // FIXED: Proper packet parsing with VLAN and IP options support
      uint16_t src_port = 0;
      uint16_t dst_port = 0;
      uint32_t tcp_seq = 0;
      
      bool is_tcp = parse_tcp_packet(data, pkt_len, &src_port, &dst_port, &tcp_seq);
      
      if (is_tcp) {
        total_tcp++;
        
        UnifiedEvent ev;
        memset(&ev, 0, sizeof(ev));
        
        ev.type = EVENT_TYPE_NIC;
        ev.seq_no = 0;
        ev.ts_nic_ns = ts_ns;
        ev.ts_kernel_ns = 0;
        ev.ts_userspace_ns = 0;
        ev.sock_fd = -1;
        ev.sock_ptr = 0;
        ev.tcp_seq = tcp_seq;
        ev.pid = 0;
        ev.pkt_len = pkt_len;
        ev.data_len = 0;
        ev.src_port = src_port;
        ev.dst_port = dst_port;
        
        if (!shared_queue_push(g_shared_queue, &ev)) {
          // Queue full, drop silently
        }
      }
      
      rte_pktmbuf_free(m);
    }
    
    // Report statistics every second
    uint64_t now_tsc = rte_get_tsc_cycles();
    if ((now_tsc - last_report_tsc) > tsc_hz) {
      std::cout << "[dpdk] packets=" << total_packets 
                << " tcp=" << total_tcp 
                << " rate=" << (total_packets - total_tcp) << " pps\n";
      last_report_tsc = now_tsc;
    }
  }
  
  std::cout << "\n[dpdk] shutting down...\n";
  std::cout << "[dpdk] total packets: " << total_packets << ", TCP: " << total_tcp << "\n";
  rte_eth_dev_stop(port_id);
  rte_eth_dev_close(port_id);
  rte_eal_cleanup();
  return 0;
}