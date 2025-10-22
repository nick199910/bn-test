// test_bn_latency.cpp - 直接测试到币安服务器的真实延迟
// 编译: g++ -O2 -o test_bn_latency test_bn_latency.cpp -pthread

#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <fcntl.h>

using namespace std::chrono;

// 测试 TCP 连接延迟（更准确，因为 ICMP 可能被阻止）
double test_tcp_latency(const std::string& host, int port, int timeout_ms = 2000) {
    // 解析域名
    struct hostent* he = gethostbyname(host.c_str());
    if (!he) {
        return -1.0;
    }
    
    // 创建 socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1.0;
    }
    
    // 设置非阻塞
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    
    // 设置服务器地址
    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    memcpy(&server.sin_addr, he->h_addr_list[0], he->h_length);
    
    // 记录开始时间
    auto start = high_resolution_clock::now();
    
    // 尝试连接
    connect(sock, (struct sockaddr*)&server, sizeof(server));
    
    // 使用 select 等待连接完成
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);
    
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    
    int ret = select(sock + 1, NULL, &wfds, NULL, &tv);
    
    auto end = high_resolution_clock::now();
    close(sock);
    
    if (ret > 0) {
        // 连接成功，计算延迟
        auto duration = duration_cast<microseconds>(end - start).count();
        return duration / 1000.0; // 返回毫秒
    }
    
    return -1.0;
}

// 计算统计信息
struct Stats {
    double min;
    double max;
    double avg;
    double median;
    int success_count;
    int total_count;
};

Stats calculate_stats(const std::vector<double>& latencies) {
    Stats stats;
    stats.total_count = latencies.size();
    
    std::vector<double> valid;
    for (double lat : latencies) {
        if (lat > 0) {
            valid.push_back(lat);
        }
    }
    
    stats.success_count = valid.size();
    
    if (valid.empty()) {
        stats.min = stats.max = stats.avg = stats.median = 0;
        return stats;
    }
    
    std::sort(valid.begin(), valid.end());
    
    stats.min = valid.front();
    stats.max = valid.back();
    
    double sum = 0;
    for (double lat : valid) {
        sum += lat;
    }
    stats.avg = sum / valid.size();
    
    stats.median = valid[valid.size() / 2];
    
    return stats;
}

int main() {
    std::cout << "=== 币安服务器延迟测试 ===" << std::endl;
    std::cout << std::endl;
    
    const std::string host = "fstream.binance.com";
    const int port = 443;
    const int test_count = 10;
    
    std::cout << "目标: " << host << ":" << port << std::endl;
    std::cout << "测试次数: " << test_count << std::endl;
    std::cout << std::endl;
    
    // 获取服务器 IP
    struct hostent* he = gethostbyname(host.c_str());
    if (he) {
        std::cout << "服务器 IP: ";
        for (int i = 0; he->h_addr_list[i] != NULL; i++) {
            struct in_addr addr;
            memcpy(&addr, he->h_addr_list[i], sizeof(struct in_addr));
            std::cout << inet_ntoa(addr);
            if (he->h_addr_list[i + 1] != NULL) {
                std::cout << ", ";
            }
        }
        std::cout << std::endl;
        std::cout << std::endl;
    }
    
    std::cout << "开始测试..." << std::endl;
    std::cout << "----------------------------------------" << std::endl;
    
    std::vector<double> latencies;
    
    for (int i = 0; i < test_count; i++) {
        double latency = test_tcp_latency(host, port);
        latencies.push_back(latency);
        
        if (latency > 0) {
            std::cout << "测试 " << (i + 1) << ": " 
                      << std::fixed << std::setprecision(2) 
                      << latency << " ms" << std::endl;
        } else {
            std::cout << "测试 " << (i + 1) << ": 失败" << std::endl;
        }
        
        // 避免过快连接
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    
    std::cout << "----------------------------------------" << std::endl;
    std::cout << std::endl;
    
    // 计算统计
    Stats stats = calculate_stats(latencies);
    
    std::cout << "=== 统计结果 ===" << std::endl;
    std::cout << "成功: " << stats.success_count << "/" << stats.total_count << std::endl;
    
    if (stats.success_count > 0) {
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "最小: " << stats.min << " ms" << std::endl;
        std::cout << "最大: " << stats.max << " ms" << std::endl;
        std::cout << "平均: " << stats.avg << " ms" << std::endl;
        std::cout << "中位: " << stats.median << " ms" << std::endl;
    }
    std::cout << std::endl;
    
    // 和测量的 BN->NIC 对比
    std::cout << "=== 延迟对比分析 ===" << std::endl;
    std::cout << std::endl;
    
    double measured_bn_nic_ms = 123.369; // 从日志中提取的值
    
    std::cout << "测量的 BN->NIC 延迟: " << measured_bn_nic_ms << " ms" << std::endl;
    
    if (stats.success_count > 0) {
        std::cout << "TCP 连接延迟:        " << stats.avg << " ms" << std::endl;
        std::cout << std::endl;
        
        // TCP 连接是往返，单程约为一半
        double estimated_one_way = stats.avg / 2.0;
        std::cout << "估算单程网络延迟:    " << std::fixed << std::setprecision(2) 
                  << estimated_one_way << " ms" << std::endl;
        
        // BN 服务器处理时间
        double estimated_bn_processing = 10.0; // 假设 5-15ms
        
        double expected_bn_nic = estimated_one_way + estimated_bn_processing;
        std::cout << "预期 BN->NIC 延迟:   " << expected_bn_nic << " ms" << std::endl;
        std::cout << "  (单程网络 " << estimated_one_way << " ms + BN处理 " 
                  << estimated_bn_processing << " ms)" << std::endl;
        std::cout << std::endl;
        
        // 计算差异
        double difference = measured_bn_nic_ms - expected_bn_nic;
        
        std::cout << "差异: " << std::abs(difference) << " ms" << std::endl;
        std::cout << std::endl;
        
        // 分析
        std::cout << "=== 分析 ===" << std::endl;
        
        if (std::abs(difference) < 20) {
            std::cout << "✓ 延迟测量基本一致，时钟同步正常" << std::endl;
        } else if (difference > 20) {
            std::cout << "✗ 测量的 BN->NIC 比预期高 " << difference << " ms" << std::endl;
            std::cout << "  可能原因:" << std::endl;
            std::cout << "  1. 本地时钟比币安服务器慢（时钟偏差）" << std::endl;
            std::cout << "  2. E 字段是事件时间，不是发送时间" << std::endl;
            std::cout << "  3. 服务器处理时间比预估的长" << std::endl;
        } else {
            std::cout << "✗ 测量的 BN->NIC 比预期低 " << std::abs(difference) << " ms" << std::endl;
            std::cout << "  可能原因:" << std::endl;
            std::cout << "  1. 本地时钟比币安服务器快（时钟偏差约 " 
                      << std::abs(difference) << " ms）" << std::endl;
            std::cout << "  2. WebSocket 连接建立后，后续数据使用了更快的路径" << std::endl;
            std::cout << "  3. TCP 连接测试包含了握手延迟" << std::endl;
        }
        
        std::cout << std::endl;
        std::cout << "建议:" << std::endl;
        std::cout << "1. 检查 NTP 时钟同步: timedatectl status" << std::endl;
        std::cout << "2. 验证币安 E 字段的实际含义" << std::endl;
        std::cout << "3. 如果时钟偏差 > 50ms，考虑强制同步时间" << std::endl;
    }
    
    return 0;
}
