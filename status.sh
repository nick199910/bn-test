#!/bin/bash

# 检查三个组件是否运行
# 用法: ./status.sh

check_process() {
    local name=$1
    local pid_file="pids/${name}.pid"
    
    if [ -f "$pid_file" ]; then
        local pid=$(cat "$pid_file")
        if kill -0 "$pid" 2>/dev/null; then
            echo "$name: 运行中 (PID: $pid)"
        else
            echo "$name: 已停止"
        fi
    else
        echo "$name: 未启动"
    fi
}

check_process "ws_client"
check_process "ebpf_loader"
check_process "dpdk_capture"
