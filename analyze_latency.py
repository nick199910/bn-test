#!/usr/bin/env python3
"""
延迟日志分析工具 - 计算 P50/P95/P99/P999 等统计信息

用法：
  python3 analyze_latency.py latency_1730043123.log
"""

import sys
import re
from statistics import median, mean
import numpy as np

def parse_latency_log(filename):
    """解析延迟日志文件"""
    data = {
        'BN_NIC': [],
        'NIC_Kernel': [],
        'Kernel_User': [],
        'CPU': [],
        'MSK': [],
        'Total': []
    }
    
    with open(filename, 'r') as f:
        for line in f:
            # 解析格式: seq=6524 tcp_seq=4108161045 BN->NIC=147739100ns ...
            match = re.search(r'BN->NIC=(\d+)ns', line)
            if match:
                data['BN_NIC'].append(int(match.group(1)))
            
            match = re.search(r'NIC->Kernel=(\d+)ns', line)
            if match:
                data['NIC_Kernel'].append(int(match.group(1)))
            
            match = re.search(r'Kernel->User=(\d+)ns', line)
            if match:
                data['Kernel_User'].append(int(match.group(1)))
            
            match = re.search(r'CPU=(\d+)ns', line)
            if match:
                data['CPU'].append(int(match.group(1)))
            
            match = re.search(r'MSK=(\d+)ns', line)
            if match:
                data['MSK'].append(int(match.group(1)))
            
            match = re.search(r'Total=(\d+)ns', line)
            if match:
                data['Total'].append(int(match.group(1)))
    
    return data

def calculate_percentiles(values, name):
    """计算各项百分位统计"""
    if not values:
        print(f"\n{name}: 无数据")
        return
    
    values_ms = [v / 1_000_000 for v in values]  # 转换为毫秒
    
    print(f"\n{name}:")
    print(f"  样本数量: {len(values)}")
    print(f"  平均值: {mean(values_ms):.3f} ms")
    print(f"  中位数: {median(values_ms):.3f} ms")
    print(f"  最小值: {min(values_ms):.3f} ms")
    print(f"  最大值: {max(values_ms):.3f} ms")
    print(f"  P50: {np.percentile(values_ms, 50):.3f} ms")
    print(f"  P95: {np.percentile(values_ms, 95):.3f} ms")
    print(f"  P99: {np.percentile(values_ms, 99):.3f} ms")
    print(f"  P999: {np.percentile(values_ms, 99.9):.3f} ms")

def main():
    if len(sys.argv) < 2:
        print("用法: python3 analyze_latency.py <latency_log_file>")
        sys.exit(1)
    
    filename = sys.argv[1]
    print(f"分析延迟日志文件: {filename}")
    print("=" * 60)
    
    data = parse_latency_log(filename)
    
    # 输出各层级统计
    calculate_percentiles(data['BN_NIC'], "BN → NIC（交易所到网卡）")
    calculate_percentiles(data['NIC_Kernel'], "NIC → Kernel（网卡到内核）")
    calculate_percentiles(data['Kernel_User'], "Kernel → User（内核到用户态）")
    calculate_percentiles(data['CPU'], "CPU（JSON 反序列化）")
    calculate_percentiles(data['MSK'], "MSK（ZeroMQ 发送）")
    calculate_percentiles(data['Total'], "总延迟（端到端）")
    
    print("\n" + "=" * 60)
    print("分析完成！")

if __name__ == '__main__':
    main()
