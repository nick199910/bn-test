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
       │ (Receive WebSocket message)
┌──────▼──────────┐
│  JSON Parser    │  ← CPU deserialization (simdjson)
│  (simdjson)     │     ~10-30μs
└──────┬──────────┘
       │ (Parse complete)
┌──────▼──────────┐
│  ZeroMQ Send    │  ← MSK message publish
│  (tcp://5555)   │     ~20-50μs
└──────┬──────────┘
       │ (Message sent)
┌──────▼──────────────┐
│  Shared Memory      │
│  Queue (MPMC)       │  ← All events collected here
└──────┬──────────────┘
       │
┌──────▼──────────┐
│  Correlator     │  ← Matches events, calculates latencies
└─────────────────┘

## 组件说明

### 1. WebSocket 客户端 (ws_client_delay/ws_client.cpp)

- 连接到 Binance WebSocket 数据流
- 记录消息到达用户空间的时间戳
- 使用 simdjson 进行 JSON 反序列化（CPU 延迟 ~10-30μs）
- 通过 ZeroMQ 发送 MSK 消息（MSK 延迟 ~20-50μs）
- 发布事件到共享内存队列
- 导出 socket FD 供 eBPF 关联

### 2. eBPF 内核探针 (ebpf_loader/bpf_program.c)

- 附加到 `tcp_recvmsg` 系统调用（kprobe + kretprobe）
- 捕获数据进入 TCP 接收缓冲区时的内核时间戳
- 记录实际复制的字节数（从返回值获取）
- 通过 socket 指针进行关联
- 发布到 eBPF ring buffer

### 3. eBPF 加载器 (ebpf_loader/ebpf_loader.cpp)

- 加载并附加 eBPF 程序
- 从 eBPF ring buffer 读取事件
- 转发到共享内存队列
- 管理 socket FD 关联

### 4. DPDK 数据包捕获 (dpdk_capture/dpdk_capture.cpp)

- 使用 DPDK 在网卡层面捕获数据包
- 高精度硬件时间戳（NIC 时间戳）
- 解析 TCP 头部（支持 VLAN、IP 选项）
- 发布网卡级事件
- **注意**：无法解密 TLS，只能标记加密数据包的时间戳

### 5. 事件关联器 (event_correlator/event_correlator.cpp)

- 从共享队列读取事件
- 跨层关联事件
- 计算延迟分解：
  - **BN → NIC**：Binance 发送到网卡接收
  - **NIC → Kernel**：网卡到内核
  - **Kernel → User**：内核到用户空间
  - **CPU**：JSON 反序列化
  - **MSK**：ZeroMQ 消息发送
  - **Total**：端到端总延迟
- 打印统计信息


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
| **ZeroMQ** | Message queue for MSK simulation | `sudo apt install libzmq3-dev` |
| **Linux Kernel Headers** | For eBPF & DPDK | `sudo apt install linux-headers-$(uname -r)` |

---

```bash 
# Ubuntu/Debian
sudo apt-get install -y \
    build-essential cmake \
    libbpf-dev clang llvm \
    libelf-dev libz-dev \
    libssl-dev libboost-all-dev \
    libzmq3-dev pkg-config \
    dpdk dpdk-dev

# Or build from source for latest versions

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







