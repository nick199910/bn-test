// dpdk_capture_fixed.cpp
// DPDK capture with proper TSC conversion and packet parsing
#define QUEUE_TEST
#ifdef QUEUE_TEST
#include "shared_events.h"
#endif

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_byteorder.h> 
#include <rte_lcore.h>
#include <rte_pdump.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024
#define NB_MBUF 8192
#define MEMPOOL_CACHE_SIZE 256
#define MAX_PKT_BURST 32

#define ETHER_MAX_LEN   1518
#define ETHER_MAC_LEN 12

#define ETHER_PKT_TYPE_VLAN 0x8100
#define ETHER_PKT_TYPE_QINQ 0x88A8 /**< IEEE 802.1ad QinQ tagging. */
#define ETHER_PKT_TYPE_VLAN1 0x8200
#define ETHER_PKT_TYPE_VLAN2 0x9100

/* check whether it is vlan type or not */
#define ETHER_VLAN_TYPE_CHECK(ptype)                                           \
    ((ptype == rte_cpu_to_be_16(ETHER_PKT_TYPE_VLAN)) ||                       \
     (ptype == rte_cpu_to_be_16(ETHER_PKT_TYPE_QINQ)) ||                       \
     (ptype == rte_cpu_to_be_16(ETHER_PKT_TYPE_VLAN1)) ||                      \
     (ptype == rte_cpu_to_be_16(ETHER_PKT_TYPE_VLAN2)))

#define ETHER_PKT_TYPE_IPv4  0x0800
#define ETHER_PKT_TYPE_IPv6  0x86DD 
#define L3_PROTOCOL_TCP 0x06

struct rte_mempool* mbuf_pool = nullptr;

static SharedQueue* g_shared_queue = nullptr;
static volatile bool g_running = true;

uint64_t tsc_hz;

// signal function
void signal_handler(int signo) {
    if (signo == SIGINT || signo == SIGTERM) {
        g_running = false;
    }
}

// Parse Ethernet frame with proper header checks
bool parse_tcp_packet(struct rte_mbuf* pkt, uint16_t* src_port,
                      uint16_t* dst_port, uint32_t* tcp_seq) {

    // Minimum: Ethernet(14) + IP(20) + TCP(20)
    if (rte_pktmbuf_pkt_len(pkt) < 54) {
        return false;
    }
    uint16_t tpid, ofs;
    /* vlan */
    ofs = ETHER_MAC_LEN;
    tpid = *rte_pktmbuf_mtod_offset(pkt, uint16_t*, ofs);
    while (ETHER_VLAN_TYPE_CHECK(tpid)) {
        ofs += 4;
        tpid = *rte_pktmbuf_mtod_offset(pkt, uint16_t*, ofs);
    }
    if (tpid == rte_cpu_to_be_16(ETHER_PKT_TYPE_IPv4)) {
        struct rte_ipv4_hdr* ipv4_hp = rte_pktmbuf_mtod_offset(pkt, struct rte_ipv4_hdr*, ofs);
        if (ipv4_hp->next_proto_id != L3_PROTOCOL_TCP)
            return false;
        ofs += ((ipv4_hp->version_ihl & 0xf) << 2);
        struct rte_tcp_hdr* tcp_hp = rte_pktmbuf_mtod_offset(pkt, struct rte_tcp_hdr*, ofs);
        *src_port = rte_cpu_to_be_16(tcp_hp->src_port);
        *dst_port = rte_cpu_to_be_16(tcp_hp->dst_port);
        *tcp_seq = rte_cpu_to_be_32(tcp_hp->sent_seq);
        std::cout << "src_port: " << *src_port << " dst_port: " << *dst_port << " tcp_seq: " << *tcp_seq << "\n";
    } else if (tpid != rte_cpu_to_be_16(ETHER_PKT_TYPE_IPv6)) {
        // ipv6 tcp need check?
        return false;
    } else {
        return false;
    }

    return true;
}

// worker thread loop
int lcore_loop(__attribute__((unused)) void* dummy) {
    uint64_t last_report_tsc = rte_get_tsc_cycles();
    uint32_t lcore = rte_lcore_id();
    uint64_t total_packets = 0, last_total = 0;
    uint64_t total_tcp = 0;
    uint16_t port;
    bool is_tcp;

    /*
     * Check that the port is on the same NUMA node as the polling thread
     * for best performance.
     */
    RTE_ETH_FOREACH_DEV(port)
    if (rte_eth_dev_socket_id(port) > 0 &&
        rte_eth_dev_socket_id(port) != (int)rte_socket_id())
        std::cout << "WARNING, port (" << port
                  << ") is on remote NUMA node to polling thread. Performance "
                     "will not be optimal.\n";
    while (1) {
        if (!g_running)
            break;
        RTE_ETH_FOREACH_DEV(port) {

            /* Get burst of RX packets, from first port of pair. */
            struct rte_mbuf* bufs[MAX_PKT_BURST];
            const uint16_t nb_rx = rte_eth_rx_burst(port, 0, bufs, MAX_PKT_BURST);

            if (unlikely(nb_rx == 0))
                continue;
			
            total_packets += nb_rx;
            cout << "total_packets: " << total_packets << "\n";
			
            /* prase packets. */
            for (uint16_t i = 0; i < nb_rx; i++) {
              

                // Use CLOCK_MONOTONIC to align with kernel ktime_get_ns()
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                uint64_t ts_ns = (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;

                // FIXED: Proper packet parsing with VLAN and IP options support
                uint16_t src_port = 0;
                uint16_t dst_port = 0;
                uint32_t tcp_seq = 0;

                is_tcp = parse_tcp_packet(bufs[i], &src_port, &dst_port, &tcp_seq);
                if (is_tcp) {
		    std::cout << src_port << " " << dst_port << " " << tcp_seq << "\n";
                    total_tcp++;

#ifdef QUEUE_TEST
                    UnifiedEvent ev;
                    memset(&ev, 0, sizeof(ev));

                    uint32_t pkt_len = rte_pktmbuf_pkt_len(bufs[i]);
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
#endif
                }

                rte_pktmbuf_free(bufs[i]);
            }
        }
        // Report statistics every second
        uint64_t now_tsc = rte_get_tsc_cycles();
        if (total_packets > last_total && (now_tsc - last_report_tsc) > tsc_hz) {
            std::cout << "[dpdk] lcore "<< lcore << ", packets=" << total_packets
                      << " tcp=" << total_tcp
                      << " rate=" << (total_packets - total_tcp) << " pps\n";
            last_report_tsc = now_tsc;
            last_total= total_packets;		
        }
    }
    std::cout << "\n[dpdk] lcore " << lcore <<", shutting down...\n";
    std::cout << "[dpdk] lcore " << lcore << ", total packets: " << total_packets << ", TCP: " << total_tcp << "\n";
}


// port init func
static inline int port_init(uint16_t port, struct rte_mempool* mbuf_pool) {
    //struct rte_eth_conf port_conf;
    rte_eth_conf port_conf{};
    const uint16_t rx_rings = 1, tx_rings = 0;
    uint16_t nb_rxd = RX_RING_SIZE;
    uint16_t nb_txd = TX_RING_SIZE;
    int retval;
    uint16_t q;
    struct rte_eth_dev_info dev_info;

    if (!rte_eth_dev_is_valid_port(port))
        return -1;

    rte_eth_dev_info_get(port, &dev_info);
    std::cout << "[dpdk] Using port " << port << " (" << dev_info.driver_name
              << ")\n";

    /* Configure the Ethernet device. */
    retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
    if (retval != 0)
        return retval;

    retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
    if (retval != 0)
        return retval;

    /* Allocate and set up 1 RX queue per Ethernet port. */
    for (q = 0; q < rx_rings; q++) {
        retval = rte_eth_rx_queue_setup(
            port, q, nb_rxd, rte_eth_dev_socket_id(port), NULL, mbuf_pool);
        if (retval < 0)
            return retval;
    }

    /* Start the Ethernet port. */
    retval = rte_eth_dev_start(port);
    if (retval < 0)
        return retval;

    /* Enable RX in promiscuous mode for the Ethernet device. */
    rte_eth_promiscuous_enable(port);

    return 0;
}

int main(int argc, char** argv) {
    // signal init
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // init EAL
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        std::cerr << "Failed to init EAL\n";
        return 1;
    }

#ifdef QUEUE_TEST
    // Open shared memory queue
    g_shared_queue = open_shared_queue(SHARED_QUEUE_NAME, 1 << 16);
    if (!g_shared_queue) {
        std::cerr
            << "Failed to open shared queue. Start websocket client first.\n";
        return 1;
    }
#endif

    // creat mempool
    mbuf_pool =
        rte_pktmbuf_pool_create("MBUF_POOL", NB_MBUF, MEMPOOL_CACHE_SIZE, 0,
                                RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (!mbuf_pool) {
        std::cerr << "Failed to create mbuf pool\n";
        return -1;
    }

    // port and queue setup
    uint16_t port = 0;
    RTE_ETH_FOREACH_DEV(port)
    if (port_init(port, mbuf_pool) != 0) {
        std::cerr << "Failed to init port (" << port << ")\n";
        return -1;
    }

    /* initialize packet capture framework */
    rte_pdump_init();
	
    // printf info
    tsc_hz = rte_get_tsc_hz();
    std::cout << "[dpdk] TSC frequency: " << (tsc_hz / 1e9) << " GHz\n";
    std::cout << "[dpdk] Capturing packets... (Ctrl+C to stop)\n";

    // launch worker thread
    rte_eal_mp_remote_launch(lcore_loop, NULL, SKIP_MAIN);

    // maste thread do nothing
    while (1) {
	if (!g_running)
            break;
        usleep(0);
    }
    return 0;
}

