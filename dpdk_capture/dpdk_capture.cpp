// dpdk_capture_fixed.cpp
// DPDK capture with proper TSC conversion and packet parsing

//#define QUEUE_TEST

#ifdef QUEUE_TEST
#include "shared_events.h"
#endif

#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_pdump.h>
#include <rte_tcp.h>
#include <rte_ether.h>
#include <rte_dev.h>


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

/* Max size of a single packet */
#define MAX_PACKET_SZ           2048

#define ETHER_ADDR_LEN 6
#define ETHER_MAX_LEN 1518
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

#define ETHER_PKT_TYPE_IPv4 0x0800
#define ETHER_PKT_TYPE_IPv6 0x86DD
#define L3_PROTOCOL_TCP 0x06

struct rte_mempool* mbuf_pool = nullptr;

// static SharedQueue* g_shared_queue = nullptr;
static volatile bool g_running = true;

struct rte_eth_conf port_conf;

uint64_t tsc_hz;
uint16_t virtio_port_id = RTE_MAX_ETHPORTS; // virtio-user port ID

// signal function
void signal_handler(int signo) {
    if (signo == SIGINT || signo == SIGTERM) {
        g_running = false;
    }
}

// Parse Ethernet frame with proper header checks
bool is_ws_packet(struct rte_mbuf* pkt, uint16_t* src_port, uint16_t* dst_port,
                  uint32_t* tcp_seq) {

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
        ofs += 2;
        struct rte_ipv4_hdr* ipv4_hp =
            rte_pktmbuf_mtod_offset(pkt, struct rte_ipv4_hdr*, ofs);
        if (ipv4_hp->next_proto_id != L3_PROTOCOL_TCP)
            return false;

        ofs += ((ipv4_hp->version_ihl & 0xf) << 2);
        struct rte_tcp_hdr* tcp_hp =
            rte_pktmbuf_mtod_offset(pkt, struct rte_tcp_hdr*, ofs);
        *src_port = rte_cpu_to_be_16(tcp_hp->src_port);
        *dst_port = rte_cpu_to_be_16(tcp_hp->dst_port);
        *tcp_seq = rte_cpu_to_be_32(tcp_hp->sent_seq);

        /* if packet from ws,  do ... */

    } else if (tpid != rte_cpu_to_be_16(ETHER_PKT_TYPE_IPv6)) {
        // ipv6 tcp need check?
        return false;
    } else {
        return false;
    }

    return true;
}

// Helper function to free mbufs
static void
burst_free_mbufs(struct rte_mbuf **pkts, unsigned num)
{
	unsigned i;

	if (pkts == NULL)
		return;

	for (i = 0; i < num; i++) {
		rte_pktmbuf_free(pkts[i]);
		pkts[i] = NULL;
	}
}
// worker thread loop
int lcore_loop(__attribute__((unused)) void* dummy) {
    uint64_t last_report_tsc = rte_get_tsc_cycles();
    uint64_t total_packets = 0, last_total = 0;
    uint64_t total_tcp = 0;
    uint32_t lcore = rte_lcore_id();
    uint16_t port, nb_rx, nb_kernel, num;
    bool is_ws;

    struct rte_mbuf* eth_rx_bufs[MAX_PKT_BURST];
    struct rte_mbuf* kernel_bufs[MAX_PKT_BURST];

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
            nb_rx =  rte_eth_rx_burst(port, 0, eth_rx_bufs, MAX_PKT_BURST);

            if (unlikely(nb_rx == 0))
                continue;

            total_packets += nb_rx;
		nb_kernel = 0;
            /* deal with packets from eth */
            for (uint16_t i = 0; i < nb_rx; i++) {
                // Use CLOCK_MONOTONIC to align with kernel ktime_get_ns()
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                uint64_t ts_ns =
                    (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;

                // FIXED: Proper packet parsing with VLAN and IP options support
                uint16_t src_port = 0;
                uint16_t dst_port = 0;
                uint32_t tcp_seq = 0;

                is_ws = is_ws_packet(eth_rx_bufs[i], &src_port, &dst_port, &tcp_seq);
                if (is_ws) {
                    total_tcp++;

#ifdef QUEUE_TEST
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
#endif
			rte_pktmbuf_free(eth_rx_bufs[i]);
                } else {
			// Forward to kernel via virtio-user
			kernel_bufs[nb_kernel++] = eth_rx_bufs[i];
		}
            }

		/* Burst tx to virtio-user (kernel) */
		if (nb_kernel && virtio_port_id != RTE_MAX_ETHPORTS) {
			num = rte_eth_tx_burst(virtio_port_id, 0, kernel_bufs, nb_kernel);
			if (unlikely(num < nb_kernel)) {
				/* Free mbufs not sent to kernel */
				burst_free_mbufs(&kernel_bufs[num], nb_kernel - num);
			}
		}
        }

	// Deal with packets from kernel (via virtio-user)
	if (virtio_port_id != RTE_MAX_ETHPORTS) {
		nb_kernel = rte_eth_rx_burst(virtio_port_id, 0, kernel_bufs, MAX_PKT_BURST);
		if (nb_kernel > 0) {
			// Forward back to physical port 0
			num = rte_eth_tx_burst(0, 0, kernel_bufs, nb_kernel);
			if (unlikely(num < nb_kernel)) {
				/* Free mbufs not sent */
				burst_free_mbufs(&kernel_bufs[num], nb_kernel - num);
			}
		}
	}

        // Report statistics every second
        uint64_t now_tsc = rte_get_tsc_cycles();
        if (total_packets > last_total &&
            (now_tsc - last_report_tsc) > tsc_hz) {
            std::cout << "[dpdk] lcore " << lcore
                      << ", packets=" << total_packets << " tcp=" << total_tcp
                      << " rate=" << (total_packets - last_total) << " pps\n";
            last_report_tsc = now_tsc;
            last_total = total_packets;
        }
    }
    std::cout << "\n[dpdk] lcore " << lcore << ", shutting down...\n";
    std::cout << "[dpdk] lcore " << lcore
              << ", total packets: " << total_packets << ", TCP: " << total_tcp
              << "\n";
}

// port init func
static inline int port_init(uint16_t port, struct rte_mempool* mbuf_pool) {
    // struct rte_eth_conf port_conf;
    const uint16_t rx_rings = 1, tx_rings = 1;
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

    /* Allocate and set up 1 RX/TX queue per Ethernet port. */
    for (q = 0; q < rx_rings; q++) {
        retval = rte_eth_rx_queue_setup(
            port, q, nb_rxd, rte_eth_dev_socket_id(port), NULL, mbuf_pool);
        if (retval < 0)
            return retval;
    }
    for (q = 0; q < tx_rings; q++) {
        retval = rte_eth_tx_queue_setup(
            port, q, nb_txd, rte_eth_dev_socket_id(port), NULL);
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

/* Create virtio-user port for kernel communication */
static int create_virtio_user_port(uint16_t physical_port) {
	struct rte_ether_addr mac_addr;
	char portname[32];
	char portargs[512];
	uint16_t port_id;

	// Get MAC address of physical port
	rte_eth_macaddr_get(physical_port, &mac_addr);

	// Set the name and arguments for virtio-user device
	snprintf(portname, sizeof(portname), "virtio_user%u", physical_port);
	snprintf(portargs, sizeof(portargs),
		"path=/dev/vhost-net,queues=1,queue_size=%u,iface=%s,mac=%02x:%02x:%02x:%02x:%02x:%02x",
		RX_RING_SIZE, portname,
		mac_addr.addr_bytes[0], mac_addr.addr_bytes[1],
		mac_addr.addr_bytes[2], mac_addr.addr_bytes[3],
		mac_addr.addr_bytes[4], mac_addr.addr_bytes[5]);

	std::cout << "[dpdk] Creating virtio-user port: " << portname << std::endl;
	std::cout << "[dpdk] Args: " << portargs << std::endl;

	// Add the vdev for virtio_user
	if (rte_eal_hotplug_add("vdev", portname, portargs) < 0) {
		std::cerr << "[dpdk] Failed to create virtio-user port for port "
			<< physical_port << std::endl;
		return -1;
	}

	// Find the newly created virtio-user port ID
	RTE_ETH_FOREACH_DEV(port_id) {
		struct rte_eth_dev_info dev_info;
		rte_eth_dev_info_get(port_id, &dev_info);
		
		// Check if this is our virtio-user device
		if (strstr(dev_info.driver_name, "virtio") != NULL) {
			char dev_name[RTE_ETH_NAME_MAX_LEN];
			rte_eth_dev_get_name_by_port(port_id, dev_name);
			if (strcmp(dev_name, portname) == 0) {
				virtio_port_id = port_id;
				std::cout << "[dpdk] virtio-user port created with ID: " 
					<< port_id << std::endl;
				
				// Initialize the virtio-user port
				struct rte_eth_conf virt_conf = {};
				if (rte_eth_dev_configure(port_id, 1, 1, &virt_conf) < 0) {
					std::cerr << "[dpdk] Failed to configure virtio-user port" << std::endl;
					return -1;
				}
				
				if (rte_eth_rx_queue_setup(port_id, 0, RX_RING_SIZE,
						rte_eth_dev_socket_id(port_id), NULL, mbuf_pool) < 0) {
					std::cerr << "[dpdk] Failed to setup RX queue for virtio-user" << std::endl;
					return -1;
				}
				
				if (rte_eth_tx_queue_setup(port_id, 0, TX_RING_SIZE,
						rte_eth_dev_socket_id(port_id), NULL) < 0) {
					std::cerr << "[dpdk] Failed to setup TX queue for virtio-user" << std::endl;
					return -1;
				}
				
				if (rte_eth_dev_start(port_id) < 0) {
					std::cerr << "[dpdk] Failed to start virtio-user port" << std::endl;
					return -1;
				}
				
				return 0;
			}
		}
	}

	std::cerr << "[dpdk] Could not find created virtio-user port" << std::endl;
	return -1;
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

    // initialize packet capture framework 
    rte_pdump_init();

    // Create virtio-user port for kernel communication
    if (create_virtio_user_port(0) != 0) {
        std::cerr << "[dpdk] Warning: Failed to create virtio-user port. "
                  << "Kernel forwarding will be disabled.\n";
        std::cerr << "[dpdk] Make sure /dev/vhost-net exists and you have proper permissions.\n";
        // Continue without virtio-user - packets will just be dropped instead of forwarded
    }

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

