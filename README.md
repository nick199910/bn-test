## 系统架构
一个全面的 WebSocket 连接延迟测量系统，跨三层关联时间戳：

- **网卡层 (DPDK)**：数据包到达网卡
- **内核层 (eBPF)**：内核 TCP 协议栈处理数据包
- **用户层 (websocketpp)**：消息传递到应用程序

### 架构流程图

┌─────────────┐
│   NIC/DPDK  │  ← 硬件时间戳
└──────┬──────┘
       │ (网络 -> 内核)
┌──────▼──────┐
│  eBPF 探针  │  ← 内核时间戳 (tcp_recvmsg)
└──────┬──────┘
       │ (内核 -> 用户空间)
┌──────▼──────┐
│  WebSocket  │  ← 用户空间时间戳
│   Client    │
└──────┬──────┘
       │ (接收 WebSocket 消息)
┌──────▼──────────┐
│  JSON 解析器    │  ← CPU 反序列化 (simdjson)
│  (simdjson)     │     ~10-30μs
└──────┬──────────┘
       │ (解析完成)
┌──────▼──────────┐
│  ZeroMQ 发送    │  ← MSK 消息发布
│  (tcp://5555)   │     ~20-50μs
└──────┬──────────┘
       │ (消息已发送)
┌──────▼──────────────┐
│  共享内存队列        │
│  (MPMC)            │  ← 所有事件在此收集
└──────┬──────────────┘
       │
┌──────▼──────────┐
│  关联器         │  ← 匹配事件，计算延迟
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

#### 如何捕获内核处理数据包的时刻？

**核心思路**：通过 eBPF **kprobe/kretprobe** 钩住 `tcp_recvmsg` 系统调用，在数据从内核 TCP 接收缓冲区复制到用户空间的**入口时刻**打时间戳，在**返回时刻**获取实际复制的字节数。

**实现流程**（伪代码）：

```c
// 1. kprobe 在 tcp_recvmsg 函数入口触发（数据进入内核 TCP 栈）
SEC("kprobe/tcp_recvmsg")
int kprobe__tcp_recvmsg(struct pt_regs *ctx) {
    __u64 thread_id = bpf_get_current_pid_tgid();
    
    // 立即获取内核时间戳（关键！）
    __u64 kernel_timestamp = bpf_ktime_get_ns();
    // ↑ 这是数据在内核 TCP 栈中被处理的时刻
    
    // 获取 socket 指针用于关联
    __u64 sock_ptr = (unsigned long)PT_REGS_PARM1(ctx);
    
    // 保存上下文到临时 map，等待 kretprobe
    struct recv_context ctx = {
        .ts_ns = kernel_timestamp,
        .sock_ptr = sock_ptr,
        .pid = pid
    };
    bpf_map_update_elem(&recv_ctx_map, &thread_id, &ctx, BPF_ANY);
    
    return 0;
}

// 2. kretprobe 在 tcp_recvmsg 函数返回时触发
SEC("kretprobe/tcp_recvmsg")
int kretprobe__tcp_recvmsg(struct pt_regs *ctx) {
    // 获取返回值（实际复制的字节数）
    long bytes_copied = PT_REGS_RC(ctx);
    if (bytes_copied <= 0) return 0;  // 错误或无数据
    
    // 取回 kprobe 保存的上下文
    struct recv_context *rctx = bpf_map_lookup_elem(&recv_ctx_map, &thread_id);
    
    // 3. 构建内核事件
    struct event ev = {
        .type = EVENT_TYPE_KERNEL,
        .ts_kernel_ns = rctx->ts_ns,    // 内核时间戳
        .sock_ptr = rctx->sock_ptr,      // Socket 指针（用于关联）
        .pkt_len = bytes_copied,         // 实际字节数
        .pid = rctx->pid
    };
    
    // 4. 发送到 eBPF ring buffer
    bpf_ringbuf_submit(&rb, &ev, 0);
    
    return 0;
}
```

### 4. DPDK 数据包捕获 (dpdk_capture/dpdk_capture.cpp)

- 使用 DPDK 在网卡层面捕获数据包
- 高精度硬件时间戳（NIC 时间戳）
- 解析 TCP 头部（支持 VLAN、IP 选项）
- 发布网卡级事件
- **注意**：无法解密 TLS，只能标记加密数据包的时间戳

#### 如何捕获网卡接收数据包最后一个字节的时刻？

**核心思路**：DPDK 的 `rte_eth_rx_burst()` 是阻塞等待，只有当 **完整数据包** 到达网卡后才会返回。因此，在 `rx_burst` 返回后立即获取时间戳，即为网卡接收完整数据包（包括最后一个字节）的时刻。

**实现流程**（伪代码）：

```cpp
// 1. DPDK 接收数据包（阻塞等待完整帧）
uint16_t nb_packets = rte_eth_rx_burst(port_id, queue_id, mbufs, burst_size);
// ↑ 此函数返回时，网卡已接收完整数据包到 DMA 缓冲区

// 2. 立即获取时间戳
clock_gettime(CLOCK_MONOTONIC, &ts);
uint64_t nic_timestamp = ts.tv_sec * 1e9 + ts.tv_nsec;
// ↑ 这就是网卡接收完最后一个字节的时间

// 3. 解析数据包，提取 TCP 序列号
for (each packet in mbufs) {
    parse_ethernet_header();    // 处理 VLAN 标签
    parse_ip_header();          // 处理 IP 选项
    parse_tcp_header();         // 提取 tcp_seq, src_port, dst_port
    
    // 4. 构建 NIC 事件
    UnifiedEvent event = {
        .type = EVENT_TYPE_NIC,
        .ts_nic_ns = nic_timestamp,  // 网卡时间戳
        .tcp_seq = tcp_seq,           // TCP 序列号（用于关联）
        .src_port = src_port,
        .dst_port = dst_port,
        .pkt_len = packet_length
    };
    
    // 5. 发送到共享内存队列
    shared_queue_push(queue, &event);
}
```

**为什么这个时间戳准确？**

1. **硬件保证**：`rx_burst` 只有在 DMA 传输完整个数据包后才返回
2. **零拷贝**：DPDK 直接访问网卡 DMA 内存，无需内核拷贝
3. **最小延迟**：紧跟 `rx_burst` 返回后立即打时间戳（代码第 184 行）
4. **时钟对齐**：使用 `CLOCK_MONOTONIC` 与内核时钟一致



### 5. 延迟分析（集成在 ws_client_delay 中）

ws_client_delay 内置了完整的事件关联和延迟分析功能：
- 实时关联 NIC、Kernel、Userspace 事件
- 自动计算各层延迟分解：
  - **BN → NIC**：Binance 发送到网卡接收
  - **NIC → Kernel**：网卡到内核
  - **Kernel → User**：内核到用户空间
  - **CPU**：JSON 反序列化
  - **MSK**：ZeroMQ 消息发送
  - **Total**：端到端总延迟
- 实时打印延迟统计信息


```bash 
# Ubuntu/Debian 系统依赖安装
sudo apt-get install -y \
    build-essential cmake \
    libbpf-dev clang llvm \
    libelf-dev libz-dev \
    libssl-dev libboost-all-dev \
    libzmq3-dev pkg-config \
    dpdk dpdk-dev

# 或者从源码编译最新版本

```

## 1. 启动 WebSocket 客户端
```bash
./websocket_client wss://fstream.binance.com:443/ws/stream
```
连接到 Binance Futures WebSocket 数据流并开始接收市场数据。

## 2. 启动 eBPF 延迟探针
```bash
./ebpf_loader
```
附加内核探针到 socket 协议栈，测量从数据包到达到用户空间消息处理的延迟。

## 3. 运行 DPDK 数据包捕获
```bash
./dpdk_capture -l 0-1 -n 4 --
```

## DPDK 设置
DPDK 需要特殊配置：
### 1. 绑定网卡到 DPDK 驱动

```bash
# 显示可用网卡
dpdk-devbind.py --status

# 绑定网卡到 vfio-pci 或 uio_pci_generic
sudo modprobe vfio-pci
sudo dpdk-devbind.py --bind=vfio-pci 0000:01:00.0 // 注: 这里要根据真实的网卡做实时的网卡号替换， ox环境的aws为 0000:01:00.0 ， 但是aws为虚拟网卡，其可能产生
较大的延迟  
```
### 2. 配置大页内存 
```bash
# 分配 1GB 大页内存
echo 512 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# 或者添加到 /etc/sysctl.conf：
vm.nr_hugepages=512
```

## 延迟测量说明

### 延迟指标定义

```
[delay] seq=221 tcp_seq=126 
BN->NIC=?ns      // Binance 发送到网卡接收 (物理网络延迟)
NIC->Kernel=?ns    // 网卡到内核 TCP 栈 (中断 + 协议栈处理)
Kernel->User=?ns    // 内核到用户空间 (系统调用 + 上下文切换)
CPU=?ns                // JSON 反序列化 (simdjson)
MSK=?ns               // ZeroMQ 消息发送
Total=?ns        // 端到端总延迟 (约 135.6ms)
```

### 1. 如何识别同一个数据包跨三层？

通过 **TCP 序列号 (tcp_seq)** 进行关联：

```
时间轴: ────────────────────────────────────────────────▶

T1: NIC 层捕获
    DPDK: tcp_seq=126, ts_nic=?
    ↓ (4.2ms 内核处理)

T2: Kernel 层捕获  
    eBPF: tcp_seq=126, ts_kernel=?, sock_ptr=?
    ↓ (0.16ms 系统调用)

T3: User 层捕获
    ws_client: tcp_seq=126, ts_user=?
    ↓ (0.9μs JSON 解析)

T4: CPU 处理完成
    ↓ (2μs ZMQ 发送)

T5: MSK 发送完成
```

**关联逻辑**：
```cpp
// 优先级 1: 通过 TCP 序列号精确匹配
if (nic_event.tcp_seq == kernel_event.tcp_seq 
    && kernel_event.tcp_seq == user_event.tcp_seq 
    && tcp_seq != 0) {
    // 找到同一个数据包！
    correlate(nic_event, kernel_event, user_event);
}

// 优先级 2: 通过 Socket 指针匹配
if (kernel_event.sock_ptr == user_event.sock_ptr 
    && sock_ptr != 0) {
    correlate(kernel_event, user_event);
}

// 优先级 3: 时间窗口匹配 (fallback)
if (abs(t2 - t1) < 10ms) {
    correlate(event1, event2);
}
```

### 2. DPDK 可以优化的延迟

#### 可优化部分 (约 4.4ms，占 3.2%)

```
当前架构（标准 Socket）:
┌─────────┐
│   NIC   │  ← 数据包到达
└────┬────┘
     │ 中断通知
┌────▼────────┐
│ 内核协议栈   │  ← 4.2ms (NIC->Kernel)
│ TCP/IP 处理  │
└────┬────────┘
     │ copy_to_user
┌────▼────────┐
│ 用户空间     │  ← 0.16ms (Kernel->User)
└─────────────┘

DPDK 架构（绕过内核）:
┌─────────┐
│   NIC   │  ← 数据包到达
└────┬────┘
     │ DMA 直接传输（零拷贝）
┌────▼────────┐
│ 用户空间     │  ← 省略 4.4ms！
│ DPDK 应用   │
└─────────────┘
```

**优化效果**：
- **NIC→Kernel (4.2ms)** → 0ms（绕过内核）
- **Kernel→User (0.16ms)** → 0ms（零拷贝）
- **总优化：~4.4ms**

#### 无法优化部分 (约 131.2ms，占 96.8%)

- **BN→NIC (131.2ms)**：物理网络延迟，只能通过机房位置优化
- **CPU (0.9μs)**：JSON 解析必须，已经很快
- **MSK (2μs)**：消息发送必须，已经很快

#### 实际建议

**当前瓶颈**：网络延迟 (131ms) 占 96.8%

**优化优先级**：
1. **迁移到新加坡机房** → 节省 ~120ms (89%)
2. **使用 AWS Direct Connect** → 节省 ~10-20ms (7-15%)
3. **使用 DPDK 绕过内核** → 节省 ~4.4ms (3.2%)

**结论**：除非已经在新加坡机房且使用专线，否则 DPDK 的 3.2% 优化不如优化网络路径。  







