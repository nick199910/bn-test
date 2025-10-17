# Binance WebSocket Latency Analyzer (C++ + DPDK + eBPF)

## 🧩 Overview

This project demonstrates a **high-performance Binance market data receiver** built in modern **C++11** using:
- **WebSocket++** for Binance market data streaming  
- **DPDK** for kernel-bypass packet capture and zero-copy network optimization  
- **eBPF** for kernel-level latency tracing and statistics  
- **rigtorp::MPMCQueue** for lock-free multi-producer/multi-consumer event broadcasting between threads  

The demo measures **WebSocket round-trip latency** in both **user space and kernel space**, showing how DPDK + eBPF instrumentation can be used to analyze network performance in real-time.

---

## 📦 Project Structure

binance-ws_test/
├── CMakeLists.txt # Top-level CMake configuration
├── websocket_client/
│ ├── websocket_client.cpp # Binance WebSocket client (using websocketpp)
│ └── CMakeLists.txt
├── dpdk_capture/
│ ├── dpdk_capture.cpp # DPDK setup, zero-copy RX/TX, timestamping
│ └── CMakeLists.txt
├── ebpf_loader/
│ ├── ebpf_loader.cpp # eBPF loader & latency probe via libbpf
│ └── CMakeLists.txt
├── include/
│ ├── mpmc_queue.h # rigtorp MPMC queue for inter-thread broadcast
│ ├── event.h # Event struct (shared_ptr used for MPMC)
│ └── utils.h # Common utilities and logging
└── tests/
├── latency_test.cpp # Unit test for round-trip latency measurement
└── CMakeLists.txt



---



## Archtecture
A comprehensive latency measurement system for WebSocket connections that correlates timestamps across three layers:

NIC Layer (DPDK): Packet arrival at network card
Kernel Layer (eBPF): Packet processing in kernel TCP stack
User Layer (websocketpp): Message delivery to application

Architecture

┌─────────────┐
│   NIC/DPDK  │  ← Hardware timestamp
└──────┬──────┘
       │ (Network -> Kernel)
┌──────▼──────┐
│  eBPF Probe │  ← Kernel timestamp (tcp_recvmsg)
└──────┬──────┘
       │ (Kernel -> Userspace)
┌──────▼──────┐
│  WebSocket  │  ← Userspace timestamp
│   Client    │
└──────┬──────┘
       │
┌──────▼──────────────┐
│  Shared Memory      │
│  Queue (MPMC)       │  ← All events collected here
└──────┬──────────────┘
       │
┌──────▼──────────┐
│  Correlator     │  ← Matches events, calculates latencies
└─────────────────┘

## Components
1. WebSocket Client (websocket_client_fixed.cpp)

Connects to Binance WebSocket stream
Timestamps message arrival in userspace
Sends periodic pings for RTT measurement
Publishes events to shared memory queue
Exports socket FD for eBPF correlation

2. eBPF Kernel Probe (bpf_program_fixed.c)

Attaches to tcp_recvmsg (kprobe + kretprobe)
Captures kernel timestamp when data enters TCP receive
Records actual bytes copied (from return value)
Correlates via socket pointer
Publishes to ring buffer

3. eBPF Loader (ebpf_loader_fixed.cpp)

Loads and attaches eBPF program
Reads events from eBPF ring buffer
Forwards to shared memory queue
Manages socket FD correlation

4. DPDK Capture (dpdk_capture_fixed.cpp)

Captures packets at NIC level using DPDK
High-resolution hardware timestamps
Parses TCP headers (handles VLAN, IP options)
Publishes NIC-level events
Note: Cannot decrypt TLS, timestamps encrypted packets

5. Event Correlator (event_correlator.cpp)

Reads events from shared queue
Correlates events across layers
Calculates latency breakdowns:

NIC → Kernel
Kernel → Userspace
Total NIC → Userspace


Prints statistics


## 🔧 Build Requirements

### Dependencies

| Component | Purpose | Install Command |
|------------|----------|----------------|
| **CMake ≥ 3.14** | Build system | `sudo apt install cmake` |
| **Clang / GCC** | Compiler (C++11) | `sudo apt install clang` |
| **Boost** | Asio dependency for WebSocket++ | `sudo apt install libboost-system-dev libboost-thread-dev` |
| **WebSocket++** | WebSocket client | `sudo apt install libwebsocketpp-dev` |
| **DPDK** | Kernel-bypass networking | [Install from source](https://github.com/DPDK/dpdk) |
| **libbpf** | eBPF loader and tracing | `sudo apt install libbpf-dev` |
| **Linux Kernel Headers** | For eBPF & DPDK | `sudo apt install linux-headers-$(uname -r)` |

---

```bash 
# Ubuntu/Debian
sudo apt-get install -y \
    build-essential cmake \
    libbpf-dev clang llvm \
    libelf-dev libz-dev \
    libssl-dev libboost-all-dev \
    dpdk dpdk-dev

# Or build from source for latest versions

```


## ⚙️ Build Instructions

### 1. Clone and Initialize
```bash
git clone https://github.com/<yourusername>/binance-ws_test.git
cd binance-ws_test

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### 🚀 Running the Demo
##  Recommended: Automated Script 
```bash
sudo ./run_profiler.sh
```

## 1. Start the WebSocket Client
```bash
./websocket_client wss://fstream.binance.com:443/ws/stream
```
This connects to Binance Futures WebSocket stream and begins receiving market data.

## 2. Start eBPF Latency Probe
```bash
./ebpf_loader
```
This attaches kernel probes to the socket stack and measures latency from packet arrival to user-space message handling.

## 3. Run the DPDK Capture
```bash
./dpdk_capture -l 0-1 -n 4 --
```


## 4. Run the Correlator
```bash
./event_correlator -l 0-1 -n 4 --
```

## DPDK Setup
DPDK requires special setup:
## 1. Bind NIC to DPDK Driver

```bash
# Show available NICs
dpdk-devbind.py --status

# Bind NIC to vfio-pci or uio_pci_generic
sudo modprobe vfio-pci
sudo dpdk-devbind.py --bind=vfio-pci 0000:03:00.0
```
## 2. Configure Hugepages 
```bash
# Allocate 1GB hugepages
echo 512 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# Or add to /etc/sysctl.conf:
vm.nr_hugepages=512
```

## 3. Alternative: Run Without DPDK
If you don't have a DPDK-compatible NIC, comment out the DPDK section in run_profiler.sh. The system will still profile kernel→userspace latency via eBPF.

## Expected Output
Correlator Output
```
[CORR] seq=1234 tcp_seq=987654321 NIC->Kernel=45us Kernel->User=123us Total=168us
[CORR] seq=1235 tcp_seq=987654350 NIC->Kernel=42us Kernel->User=115us Total=157us

=== Correlation Statistics ===
NIC events:       5420
Kernel events:    5418
Userspace events: 5416
Correlated:       5200

=== Average Latencies ===
NIC->Kernel:   43 us
Kernel->User:  118 us
NIC->User:     161 us
```

This shows:

43 μs: Network card → Kernel TCP stack
118 μs: Kernel → Userspace application
161 μs: Total hardware → application latency

WebSocket Ping/Pong RTT
```
[ping] rtt_us=2450 us
```
This measures round-trip time to Binance servers (~2.5ms typical).





