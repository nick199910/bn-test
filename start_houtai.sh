#!/bin/bash

# 后台启动三个组件
# 用法: ./start_houtai.sh

mkdir -p logs pids

# 1. 启动 WebSocket 客户端
nohup ./build/ws_client_delay/ws_client_delay -wlogs > logs/ws_client.log 2>&1 &
echo $! > pids/ws_client.pid
echo "WebSocket 客户端已启动, PID: $(cat pids/ws_client.pid)"
sleep 2

# 2. 启动 eBPF 加载器
sudo nohup ./build/ebpf_loader/ebpf_loader ./build/ebpf_loader/bpf_program.o > logs/ebpf_loader.log 2>&1 &
echo $! > pids/ebpf_loader.pid
echo "eBPF 加载器已启动, PID: $(cat pids/ebpf_loader.pid)"
sleep 1

# 3. 启动 DPDK 捕获
sudo nohup ./build/dpdk_capture/dpdk_capture -l 0-1 -n 2 --vdev=net_pcap0,iface=eth0 -- > logs/dpdk_capture.log 2>&1 &
echo $! > pids/dpdk_capture.pid
echo "DPDK 捕获已启动, PID: $(cat pids/dpdk_capture.pid)"

echo "所有组件已启动"