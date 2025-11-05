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
    libcurl4-openssl-dev \
    dpdk dpdk-dev

# 或者从源码编译最新版本

```

## 1. 启动 WebSocket 客户端

### 基本运行（仅控制台输出）
```bash
./build/ws_client_delay/ws_client_delay
```

### 启用延迟日志文件（用于 P99 分析）
```bash
./build/ws_client_delay/ws_client_delay -wlogs
```

使用 `-wlogs` 参数时，程序会将完整的延迟数据写入日志文件（格式：`latency_<timestamp>.log`），用于后续 P99/P95 等统计分析。日志格式：
```
seq=6524 tcp_seq=4108161045 BN->NIC=147739100ns NIC->Kernel=863241ns Kernel->User=77219ns CPU=6562ns MSK=16422ns Total=148702544ns
```

**注意**：
- 仅写入完整的延迟记录（包含所有层级时间戳）
- 跳过包含 `N/A` 的不完整记录
- 适合用阿里云日志服务、ELK 等工具进行分析

## 2. 启动 eBPF 延迟探针
```bash
./ebpf_loader
```
附加内核探针到 socket 协议栈，测量从数据包到达到用户空间消息处理的延迟。

## 3. 运行 DPDK 数据包捕获

### 方式 1: net_pcap 模式（推荐用于测试/开发）

使用 net_pcap 从现有网卡捕获数据包，无需绑定网卡到 DPDK，不会断网：

```bash
# 确保网卡启用
sudo ip link set eth0 up

# 加载 vhost-net 模块（用于 virtio-user）
sudo modprobe vhost-net
sudo chmod 666 /dev/vhost-net  # 如果需要非 root 运行

# 运行 DPDK 捕获程序
sudo ./build/dpdk_capture/dpdk_capture -l 0-1 -n 2 --vdev=net_pcap0,iface=eth0 --
```

**说明**：
- `--vdev=net_pcap0,iface=eth0`：从 eth0 网卡捕获数据包
- 程序会自动创建 `veth_dpdk` 虚拟接口用于内核通信
- TCP 数据包在 DPDK 中处理，非 TCP 包转发到内核
- 不影响现有网络连接，适合 SSH 远程操作

启动后配置虚拟接口（可选）：
```bash
sudo ip link set veth_dpdk up
sudo ip addr add 192.168.100.1/24 dev veth_dpdk  # 如果需要
```

### 方式 2: 原生 DPDK 模式（生产环境）

直接绑定网卡到 DPDK 驱动，获得最佳性能：

```bash
# 1. 显示可用网卡
dpdk-devbind.py --status

# 2. 启用 vfio noiommu 模式（虚拟机环境需要）
echo 1 | sudo tee /sys/module/vfio/parameters/enable_unsafe_noiommu_mode

# 3. 加载驱动
sudo modprobe vfio-pci

# 4. 解绑网卡
sudo dpdk-devbind.py --unbind 0000:01:00.0

# 5. 绑定到 DPDK
sudo dpdk-devbind.py --bind=vfio-pci 0000:01:00.0

# 6. 验证
dpdk-devbind.py --status
```

⚠️ **警告**：绑定后网卡会从内核消失，SSH 连接会断开！仅在以下情况使用：
- 有多个网卡，其他网卡用于管理
- 通过控制台访问，不依赖网络
- 测试完后会重启恢复

运行命令：
```bash
sudo ./build/dpdk_capture/dpdk_capture -l 0-1 -n 2 --
```

### 配置大页内存

```bash
# 分配 1GB 大页内存
echo 512 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# 或者添加到 /etc/sysctl.conf：
vm.nr_hugepages=512

# 验证
grep HugePages /proc/meminfo
```

### virtio-user vs KNI

本项目已从已废弃的 KNI 迁移到 virtio-user：

| 特性 | KNI (已废弃) | virtio-user (当前) |
|------|-------------|-------------------|
| 内核模块 | 需要编译 rte_kni.ko | 使用上游 vhost-net |
| 维护状态 | DPDK 23.11 已移除 | 官方推荐方案 |
| 功能支持 | 基础功能 | 多队列、TSO、offload |
| 接口名称 | vEth0 | veth_dpdk |
| 稳定性 | 退出时可能崩溃 | 优雅清理 |

## 延迟测量说明

### 延迟指标定义

```
[delay] seq=221 tcp_seq=126 symbol=bnbusdt
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

---

## 延迟日志分析

### 生成延迟日志文件

使用 `-wlogs` 参数运行 WebSocket 客户端：

```bash
./build/ws_client_delay/ws_client_delay -wlogs
```

程序会生成格式如 `latency_1730043123.log` 的日志文件。

### 分析 P99 延迟

使用提供的 Python 分析脚本：

```bash
python3 analyze_latency.py latency_1730043123.log
```

**输出示例**：

```
分析延迟日志文件: latency_1730043123.log
============================================================

BN → NIC（交易所到网卡）:
  样本数量: 12580
  平均值: 144.523 ms
  中位数: 143.892 ms
  最小值: 135.234 ms
  最大值: 148.765 ms
  P50: 143.892 ms
  P95: 147.234 ms
  P99: 147.891 ms
  P999: 148.456 ms

总延迟（端到端）:
  样本数量: 12580
  平均值: 148.623 ms
  中位数: 148.102 ms
  最小值: 139.456 ms
  最大值: 152.891 ms
  P50: 148.102 ms
  P95: 151.234 ms
  P99: 151.891 ms
  P999: 152.456 ms
```

### 使用阿里云日志服务分析

日志格式可直接导入阿里云 SLS 或 ELK：

1. 上传日志文件到阿里云 SLS
2. 配置日志解析规则（正则表达式）
3. 创建统计图表查看 P99 趋势

**正则表达式示例**：
```
seq=(?<seq>\d+) tcp_seq=(?<tcp_seq>\d+) BN->NIC=(?<bn_nic>\d+)ns NIC->Kernel=(?<nic_kernel>\d+)ns Kernel->User=(?<kernel_user>\d+)ns CPU=(?<cpu>\d+)ns MSK=(?<msk>\d+)ns Total=(?<total>\d+)ns
```




