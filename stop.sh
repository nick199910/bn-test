#!/bin/bash

# 停止所有组件
# 用法: ./stop.sh

# 停止 DPDK
if [ -f pids/dpdk_capture.pid ]; then
    sudo kill $(cat pids/dpdk_capture.pid) 2>/dev/null
    echo "DPDK 捕获已停止"
    rm -f pids/dpdk_capture.pid
fi

# 停止 eBPF
if [ -f pids/ebpf_loader.pid ]; then
    sudo kill $(cat pids/ebpf_loader.pid) 2>/dev/null
    echo "eBPF 加载器已停止"
    rm -f pids/ebpf_loader.pid
fi

# 停止 WebSocket
if [ -f pids/ws_client.pid ]; then
    kill $(cat pids/ws_client.pid) 2>/dev/null
    echo "WebSocket 客户端已停止"
    rm -f pids/ws_client.pid
fi

# 清理共享内存
sudo rm -f /dev/shm/ws_* 2>/dev/null

echo "所有组件已停止"
