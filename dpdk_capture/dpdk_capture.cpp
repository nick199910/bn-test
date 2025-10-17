// dpdk_capture.cpp


#include "mpmc_queue.h"

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_cycles.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

struct DpdkEvent {
  uint64_t tsc;
  uint64_t ts_ns;
  uint32_t pkt_len;
  uint32_t src_port;
  uint32_t dst_port;
  uint32_t tcp_seq;
};

static rigtorp::MPMCQueue<std::shared_ptr<DpdkEvent>>* g_queue = nullptr;

int main(int argc, char** argv) {
  // Expect at least EAL args to be passed in (e.g. -l 0-1 -n 4 --)
  int ret = rte_eal_init(argc, argv);
  if (ret < 0) {
    std::cerr << "Failed to init EAL\n";
    return 1;
  }
  // For simplicity assume port 0 exists and is started externally with default config
  uint16_t port_id = 0;
  uint16_t nb_rx = 1024;
  rigtorp::MPMCQueue<std::shared_ptr<DpdkEvent>> queue(1 << 16);
  g_queue = &queue;

  const uint16_t burst_size = 32;
  rte_mbuf* bufs[burst_size];
  double tsc_hz = rte_get_tsc_hz();
  while (true) {
    uint16_t nb_rxed = rte_eth_rx_burst(port_id, 0, (rte_mbuf**)bufs, burst_size);
    if (nb_rxed == 0) {
      rte_pause();
      continue;
    }
    uint64_t cycles = rte_get_tsc_cycles();
    uint64_t now_ns = (uint64_t)((double)cycles / tsc_hz * 1e9);

    for (uint16_t i = 0; i < nb_rxed; ++i) {
      rte_mbuf* m = bufs[i];
      uint8_t* data = rte_pktmbuf_mtod(m, uint8_t*);
      uint16_t pkt_len = rte_pktmbuf_pkt_len(m);
      DpdkEvent ev;
      ev.tsc = cycles;
      ev.ts_ns = now_ns;
      ev.pkt_len = pkt_len;
      ev.src_port = 0;
      ev.dst_port = 0;
      ev.tcp_seq = 0;
      // Minimal parsing of IPv4 + TCP (assumes no VLAN, contiguous pkt)
      if (pkt_len >= 54) {
        // Ethernet header 14 bytes, IP header begins at offset 14
        uint8_t ip_proto = data[23];
        if (ip_proto == 6) {  // TCP
          uint16_t src_port = (data[34] << 8) | data[35];
          uint16_t dst_port = (data[36] << 8) | data[37];
          ev.src_port = src_port;
          ev.dst_port = dst_port;
          // TCP sequence number bytes 38-41
          ev.tcp_seq = (data[38] << 24) | (data[39] << 16) | (data[40] << 8) | (data[41]);
        }
      }
      auto evt = std::make_shared<DpdkEvent>(ev);

      g_queue->push(evt);
      rte_pktmbuf_free(m);
    }
  }
  return 0;
}
