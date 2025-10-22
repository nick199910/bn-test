// ws_client.cpp（延迟测量与日志输出）

#include "shared_events.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>
#include <unistd.h>

#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
#include <openssl/ssl.h>
#include <boost/asio/ssl.hpp>
#include <simdjson.h>

typedef websocketpp::config::asio_tls_client tls_client_config;
typedef websocketpp::client<tls_client_config> client_t;

using steady_clock = std::chrono::steady_clock;

// 共享内存队列与全局变量
static SharedQueue* g_shared_queue = nullptr;
static std::atomic<uint64_t> g_seq_no{1};
static int g_websocket_fd = -1;
static volatile bool g_running = true;

void signal_handler(int signo) {
  if (signo == SIGINT || signo == SIGTERM) {
    if (signo == SIGINT) {
      static const char msg[] = "[signal] Caught SIGINT (Ctrl+C), exiting...\n";
      (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
    } else {
      static const char msg[] = "[signal] Caught SIGTERM, exiting...\n";
      (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
    }
    g_running = false;
  }
}

// TLS 初始化回调（可选开启 SNI）
std::shared_ptr<boost::asio::ssl::context> on_tls_init(
    websocketpp::connection_hdl h, client_t& ws_client) {
  namespace asio_ssl = boost::asio::ssl;
  auto ctx = std::make_shared<asio_ssl::context>(asio_ssl::context::sslv23);

  ctx->set_default_verify_paths();
  ctx->set_verify_mode(asio_ssl::verify_peer);

  try {
    client_t::connection_ptr con = ws_client.get_con_from_hdl(h);
    if (con && con->get_uri()) {
      std::string host = con->get_uri()->get_host();
      (void)host; // SNI can be set here if needed.
    }
  } catch (const std::exception& e) {
    std::cerr << "warning: SNI not applied: " << e.what() << "\n";
  }

  return ctx;
}

// 从连接对象提取底层 socket 文件描述符（用于 eBPF 关联）
int get_socket_fd(client_t::connection_ptr con) {
  try {
    auto& socket = con->get_raw_socket();
    if (socket.lowest_layer().is_open()) {
      return socket.lowest_layer().native_handle();
    }
  } catch (const std::exception& e) {
    std::cerr << "Failed to get socket fd: " << e.what() << "\n";
  }
  return -1;
}

// 进程内简化关联器（基于 `event_correlator` 适配），将 NIC/内核/用户态事件进行关联
struct CorrelatedEvent {
  uint64_t ts_nic_ns{0};
  uint64_t ts_kernel_ns{0};
  uint64_t ts_userspace_ns{0};
  uint64_t ts_cpu_deser_end_ns{0};
  uint64_t src_send_ts_ns{0};
  uint64_t seq_no{0};
  uint32_t tcp_seq{0};
  uint64_t sock_ptr{0};
  int32_t sock_fd{-1};

  uint64_t latency_nic_to_kernel() const {
    return (ts_kernel_ns > ts_nic_ns) ? (ts_kernel_ns - ts_nic_ns) : 0;
  }
  uint64_t latency_kernel_to_user() const {
    return (ts_userspace_ns > ts_kernel_ns) ? (ts_userspace_ns - ts_kernel_ns) : 0;
  }
  uint64_t latency_nic_to_user() const {
    return (ts_userspace_ns > ts_nic_ns) ? (ts_userspace_ns - ts_nic_ns) : 0;
  }
  uint64_t latency_cpu_deser() const {
    return (ts_cpu_deser_end_ns > ts_userspace_ns) ? (ts_cpu_deser_end_ns - ts_userspace_ns) : 0;
  }
  uint64_t latency_bn_to_nic() const {
    return (ts_nic_ns > src_send_ts_ns && src_send_ts_ns > 0) ? (ts_nic_ns - src_send_ts_ns) : 0;
  }
  bool is_complete() const {
    return ts_nic_ns > 0 && ts_kernel_ns > 0 && ts_userspace_ns > 0;
  }
};

class EventCorrelator {
public:
  explicit EventCorrelator(std::shared_ptr<spdlog::logger> logger) : logger_(std::move(logger)) {}

  void add_event(const UnifiedEvent& ev) {
    // 保持小窗口；优先基于 tcp_seq / sock_ptr / 时间临近 进行匹配
    switch (ev.type) {
      case EVENT_TYPE_NIC:
        nic_events_.push_back(ev);
        break;
      case EVENT_TYPE_KERNEL:
        kernel_events_.push_back(ev);
        break;
      case EVENT_TYPE_USERSPACE:
        user_events_.push_back(ev);
        break;
      default:
        break;
    }
  }

  void correlate_and_log() {
    // 在最近窗口内尝试进行事件关联
    const size_t MAX_WINDOW = 4096;
    trim_deques(MAX_WINDOW);

    // 针对每个用户态事件，寻找最匹配的内核态与 NIC 事件
    for (size_t ui = 0; ui < user_events_.size(); ++ui) {
      const auto& ue = user_events_[ui];

      // 若可用则优先用 tcp_seq 匹配，否则退化为时间临近匹配
      UnifiedEvent best_k{}; bool has_k = false;
      UnifiedEvent best_n{}; bool has_n = false;

      // 匹配内核事件
      for (const auto& ke : kernel_events_) {
        if (ke.tcp_seq == ue.tcp_seq && ke.tcp_seq != 0) {
          best_k = ke; has_k = true; break;
        }
      }
      if (!has_k) {
        // 退化：若 sock_ptr 可用则优先相同 sock_ptr；否则选择时间上最接近且早于用户态的内核事件
        uint64_t best_dt = UINT64_MAX;
        for (const auto& ke : kernel_events_) {
          if (ke.sock_ptr != 0 && ue.sock_ptr != 0 && ke.sock_ptr != ue.sock_ptr) continue;
          if (ke.ts_kernel_ns > 0 && ke.ts_kernel_ns <= ue.ts_userspace_ns) {
            uint64_t dt = ue.ts_userspace_ns - ke.ts_kernel_ns;
            if (dt < best_dt) { best_dt = dt; best_k = ke; has_k = true; }
          }
        }
      }

      // 匹配 NIC 事件（若存在）
      if (has_k) {
        for (const auto& ne : nic_events_) {
          if (ne.tcp_seq == best_k.tcp_seq && ne.tcp_seq != 0) {
            best_n = ne; has_n = true; break;
          }
        }
        if (!has_n) {
          uint64_t best_dt = UINT64_MAX;
          for (const auto& ne : nic_events_) {
            if (ne.ts_nic_ns > 0 && ne.ts_nic_ns <= best_k.ts_kernel_ns) {
              uint64_t dt = best_k.ts_kernel_ns - ne.ts_nic_ns;
              if (dt < best_dt) { best_dt = dt; best_n = ne; has_n = true; }
            }
          }
        }
      }

      if (has_k) {
        CorrelatedEvent ce{};
        ce.ts_userspace_ns = ue.ts_userspace_ns;
        ce.ts_kernel_ns = best_k.ts_kernel_ns;
        ce.ts_nic_ns = has_n ? best_n.ts_nic_ns : 0;
        ce.ts_cpu_deser_end_ns = ue.ts_cpu_deserialization;
        ce.src_send_ts_ns = ue.src_send_ts_ns;
        ce.seq_no = ue.seq_no;
        ce.tcp_seq = ue.tcp_seq ? ue.tcp_seq : (has_n ? best_n.tcp_seq : best_k.tcp_seq);
        ce.sock_ptr = ue.sock_ptr ? ue.sock_ptr : best_k.sock_ptr;
        ce.sock_fd = ue.sock_fd;

        // 输出四段：BN->NIC、NIC->Kernel、Kernel->User、CPU(反序列化)；
        // Total 定义为 NIC->Kernel + Kernel->User + CPU
        auto nk = has_n ? fmt::format("{}ns", ce.latency_nic_to_kernel()) : std::string("N/A");
        auto ku = fmt::format("{}ns", ce.latency_kernel_to_user());
        auto cpu = fmt::format("{}ns", ce.latency_cpu_deser());
        // per-message 偏移 K（epoch - steady），将 NIC 的 steady 对齐到 Epoch 再与 E 相减
        uint64_t K = (ue.ts_userspace_epoch_ns > ue.ts_userspace_ns) ? (ue.ts_userspace_epoch_ns - ue.ts_userspace_ns) : 0;
        uint64_t bn_nic_ns = (has_n && ue.src_send_ts_ns > 0 && K > 0) ? ((ce.ts_nic_ns + K) - ue.src_send_ts_ns) : 0;
        auto bn_nic = has_n && bn_nic_ns > 0 ? fmt::format("{}ns", bn_nic_ns) : std::string("N/A");
        uint64_t total_with_cpu_ns = ce.latency_nic_to_kernel() + ce.latency_kernel_to_user() + ce.latency_cpu_deser();
        auto total = has_n ? fmt::format("{}ns", total_with_cpu_ns) : std::string("N/A");
        logger_->info(
          "[delay] seq={} tcp_seq={} BN->NIC={} NIC->Kernel={} Kernel->User={} CPU={} Total={}",
          ce.seq_no,
          ce.tcp_seq,
          bn_nic,
          nk,
          ku,
          cpu,
          total);
      }
    }

    // 定期裁剪窗口，避免内存无限增长
    if (user_events_.size() > 4096) user_events_.erase(user_events_.begin(), user_events_.begin() + 2048);
    if (kernel_events_.size() > 8192) kernel_events_.erase(kernel_events_.begin(), kernel_events_.begin() + 4096);
    if (nic_events_.size() > 8192) nic_events_.erase(nic_events_.begin(), nic_events_.begin() + 4096);
  }

private:
  void trim_deques(size_t max_sz) {
    if (nic_events_.size() > max_sz) nic_events_.erase(nic_events_.begin(), nic_events_.end() - max_sz);
    if (kernel_events_.size() > max_sz) kernel_events_.erase(kernel_events_.begin(), kernel_events_.end() - max_sz);
    if (user_events_.size() > max_sz) user_events_.erase(user_events_.begin(), user_events_.end() - max_sz);
  }

  std::vector<UnifiedEvent> nic_events_;
  std::vector<UnifiedEvent> kernel_events_;
  std::vector<UnifiedEvent> user_events_;
  std::shared_ptr<spdlog::logger> logger_;
};

int main(int argc, char** argv) {
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  spdlog::set_level(spdlog::level::info);
  auto logger = spdlog::get("ws_delay");
  if (!logger) {
    logger = spdlog::stdout_color_mt("ws_delay");
  }

  std::string uri = "wss://fstream.binance.com:443/ws/stream";
  if (argc >= 2) uri = argv[1];

  // Initialize shared memory queue (creator)
  g_shared_queue = create_shared_queue(SHARED_QUEUE_NAME, 1 << 16);
  if (!g_shared_queue) {
    logger->error("Failed to create shared queue");
    return 1;
  }

  client_t ws_client;
  ws_client.init_asio();
  ws_client.set_tls_init_handler([&ws_client](websocketpp::connection_hdl h) {
    return on_tls_init(h, ws_client);
  });
  ws_client.clear_access_channels(websocketpp::log::alevel::all);
  ws_client.clear_error_channels(websocketpp::log::elevel::all);

  // 连接建立回调：记录 socket fd 并发送订阅
  ws_client.set_open_handler([&ws_client, logger](websocketpp::connection_hdl h) {
    logger->info("[ws] connected");
    auto con = ws_client.get_con_from_hdl(h);
    g_websocket_fd = get_socket_fd(con);
    if (g_websocket_fd >= 0) {
      logger->info("[ws] socket_fd={}", g_websocket_fd);
      write_socket_fd_to_ebpf(g_websocket_fd);
    }
    std::string sub = R"({"method":"SUBSCRIBE","params":["btcusdt@aggTrade"],"id":1})";
    con->send(sub);
  });

  // 消息回调：写入用户态事件到共享队列（解析 BN 服务器时间与反序列化耗时）
  ws_client.set_message_handler([logger](websocketpp::connection_hdl, client_t::message_ptr msg) {
    // 回调进入时刻（用户态起点）
    uint64_t ts_userspace_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            steady_clock::now().time_since_epoch()).count());
    uint64_t ts_userspace_epoch_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // 解析 BN 发送时间（优先 data.E, 次选 E，再次选 data.T/T），单位通常为毫秒
    uint64_t src_send_ts_ns = 0;
    {
      simdjson::dom::parser parser;
      simdjson::padded_string pj{msg->get_payload()};
      auto doc_res = parser.parse(pj);
      if (doc_res.error() == simdjson::SUCCESS) {
        simdjson::dom::element doc = doc_res.value();
        auto get_uint = [](simdjson::dom::element elem, const char* key, uint64_t& out)->bool{
          auto v = elem[key];
          if (v.error() == simdjson::SUCCESS) {
            uint64_t tmp{}; if (v.get(tmp) == simdjson::SUCCESS) { out = tmp; return true; }
          }
          return false;
        };
        uint64_t e_ms = 0;
        bool found = false;
        // combined stream
        auto data = doc["data"];
        if (!found && data.error() == simdjson::SUCCESS) {
          simdjson::dom::element de = data.value();
          if (get_uint(de, "E", e_ms)) { found = true; }
          else if (get_uint(de, "T", e_ms)) { found = true; }
        }
        // single stream
        if (!found) {
          if (get_uint(doc, "E", e_ms)) { found = true; }
          else if (get_uint(doc, "T", e_ms)) { found = true; }
        }
        if (found) {
          src_send_ts_ns = e_ms * 1000000ULL; // ms -> ns
        }
      } else {
        logger->warn("[parse] simdjson parse error: {}", simdjson::error_message(doc_res.error()));
      }
    }

    // 反序列化完成时刻（这里只在 JSON 解析完成后作为结束时间）
    uint64_t ts_after_deser_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            steady_clock::now().time_since_epoch()).count());

    UnifiedEvent ev{};
    ev.type = EVENT_TYPE_USERSPACE;
    ev.seq_no = g_seq_no.fetch_add(1);
    ev.ts_userspace_ns = ts_userspace_ns;
    ev.ts_userspace_epoch_ns = ts_userspace_epoch_ns;
    ev.ts_cpu_deserialization = ts_after_deser_ns; // 绝对时间，相关器内再做差
    ev.src_send_ts_ns = src_send_ts_ns;            // BN 发送时间（绝对）
    ev.ts_kernel_ns = 0;
    ev.ts_nic_ns = 0;
    ev.sock_fd = g_websocket_fd;
    ev.sock_ptr = 0;
    ev.tcp_seq = 0;
    ev.pid = getpid();
    ev.pkt_len = 0;
    ev.data_len = std::min<uint32_t>(msg->get_payload().size(), MAX_DATA_SIZE - 1);
    memcpy(ev.data, msg->get_payload().c_str(), ev.data_len);
    ev.data[ev.data_len] = '\0';

    if (!shared_queue_push(g_shared_queue, &ev)) {
      logger->warn("[ws] queue full, dropped event");
    }
  });

  // Connect
  websocketpp::lib::error_code ec;
  client_t::connection_ptr con = ws_client.get_connection(uri, ec);
  if (ec) {
    logger->error("Connection error: {}", ec.message());
    return 1;
  }
  ws_client.connect(con);

  // 优雅退出监控线程：等待 g_running=false 后停止事件循环并关闭连接
  std::thread shutdown_thread([&ws_client, con]() mutable {
    while (g_running) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    try {
      websocketpp::lib::error_code ec_close;
      con->close(websocketpp::close::status::going_away, "signal", ec_close);
    } catch (...) { /* 忽略异常，继续停止 */ }
    try {
      ws_client.stop_listening();
    } catch (...) {}
    try {
      ws_client.stop();
    } catch (...) {}
    try {
      ws_client.get_io_service().stop();
    } catch (...) {}
  });

  // 关联线程：从共享队列读取事件并输出各段时延
  std::thread corr_thread([logger]() {
    EventCorrelator corr(logger);
    UnifiedEvent ev{};
    while (g_running) {
      if (shared_queue_pop(g_shared_queue, &ev)) {
        corr.add_event(ev);
        // Correlate every 200 events to amortize cost
        static uint32_t cnt = 0;
        if (++cnt % 200 == 0) {
          corr.correlate_and_log();
        }
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
    // Final pass
    corr.correlate_and_log();
  });

  // 启动 WebSocket 事件循环
  ws_client.run();

  // 优雅退出
  g_running = false;
  if (corr_thread.joinable()) corr_thread.join();
  if (shutdown_thread.joinable()) shutdown_thread.join();

  cleanup_shared_queue(g_shared_queue);
  return 0;
}

