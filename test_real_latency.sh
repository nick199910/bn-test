#!/bin/bash
# 测试真实的网络延迟

echo "=== 真实网络延迟测试 ==="
echo

BN_IPS=(
    "13.113.15.142"
    "52.196.84.185"
    "54.249.83.76"
)

echo "1. ICMP Ping 测试（如果可用）:"
for ip in "${BN_IPS[@]}"; do
    echo -n "  $ip: "
    ping -c 3 -W 1 $ip 2>/dev/null | tail -n 1 | grep -oP 'min/avg/max[^=]*= \K[^/]*' || echo "ICMP 被阻止"
done
echo

echo "2. TCP SYN 延迟测试（更准确）:"
echo "使用 hping3（如果可用）或 nc"
echo

for ip in "${BN_IPS[@]}"; do
    echo "  测试 $ip:443"
    
    # 使用 time + nc 测试
    for i in {1..3}; do
        start=$(date +%s%N)
        timeout 2 nc -z -w 1 $ip 443 2>/dev/null
        if [ $? -eq 0 ]; then
            end=$(date +%s%N)
            latency_ns=$((end - start))
            latency_ms=$(echo "scale=2; $latency_ns / 1000000" | bc)
            echo "    尝试 $i: ${latency_ms} ms"
        else
            echo "    尝试 $i: 超时或失败"
        fi
        sleep 0.1
    done
    echo
done

echo "3. 使用 mtr 跟踪路由（如果可用）:"
if command -v mtr &> /dev/null; then
    mtr -r -c 10 -n ${BN_IPS[0]} | head -n 15
else
    echo "  未安装 mtr，运行: sudo apt-get install mtr"
fi
echo

echo "=== 分析 ==="
echo
echo "正常延迟参考（从中国到日本东京）:"
echo "  • 北京/上海 → 东京: 30-50ms"
echo "  • 深圳/广州 → 东京: 40-60ms"
echo "  • 香港 → 东京: 50-70ms"
echo
echo "如果延迟 > 100ms，可能原因："
echo "  1. 虚拟机网络性能差"
echo "  2. 跨越多个网络提供商"
echo "  3. 带宽限制/QoS"
echo "  4. 路由不优"
echo
echo "建议："
echo "  1. 检查虚拟机宿主机的网络"
echo "  2. 使用物理机测试对比"
echo "  3. 考虑更换网络提供商或数据中心"
