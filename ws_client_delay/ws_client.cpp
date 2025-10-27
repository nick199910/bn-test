// ws_client.cpp（延迟测量与日志输出）

#include "shared_events.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unistd.h>

#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
#include <openssl/ssl.h>
#include <boost/asio/ssl.hpp>
#include <simdjson.h>
#include <sstream>
#include <vector>

#ifdef HAVE_CURL
#include <curl/curl.h>
#endif

#ifdef HAVE_ZMQ
#include <zmq.hpp>
#endif

typedef websocketpp::config::asio_tls_client tls_client_config;
typedef websocketpp::client<tls_client_config> client_t;

using steady_clock = std::chrono::steady_clock;

// 共享内存队列与全局变量
static SharedQueue* g_shared_queue = nullptr;
static std::atomic<uint64_t> g_seq_no{1};
static int g_websocket_fd = -1;
static volatile bool g_running = true;

#ifdef HAVE_ZMQ
// Simple ZeroMQ Producer for MSK message simulation
class ZMQProducer {
public:
  ZMQProducer(const std::string& endpoint) 
    : context_(1), socket_(context_, zmq::socket_type::pub) {
    try {
      socket_.bind(endpoint);
    } catch (const zmq::error_t& e) {
      throw std::runtime_error(std::string("ZMQ bind failed: ") + e.what());
    }
  }
  
  void send_message(const std::string& topic, const std::string& msg) {
    // Send topic as first frame
    zmq::message_t topic_msg(topic.data(), topic.size());
    socket_.send(topic_msg, zmq::send_flags::sndmore);
    
    // Send message body as second frame
    zmq::message_t body_msg(msg.data(), msg.size());
    socket_.send(body_msg, zmq::send_flags::none);
  }
  
private:
  zmq::context_t context_;
  zmq::socket_t socket_;
};

static std::shared_ptr<ZMQProducer> g_zmq_producer;
#endif

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

#ifdef HAVE_CURL
// CURL 回调函数，用于接收 REST API 响应
static size_t write_callback_impl(void* contents, size_t size, size_t nmemb, void* userp) {
  ((std::string*)userp)->append((char*)contents, size * nmemb);
  return size * nmemb;
}

// 获取所有 USDT 交易对（可修改过滤条件）
std::vector<std::string> fetch_all_symbols(const std::string& api_url, std::shared_ptr<spdlog::logger> logger) {
  std::vector<std::string> symbols;
  CURL* curl = curl_easy_init();
  if (!curl) {
    logger->error("[REST] Failed to initialize curl");
    return symbols;
  }

  std::string response_buffer;
  curl_easy_setopt(curl, CURLOPT_URL, api_url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback_impl);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buffer);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

  CURLcode res = curl_easy_perform(curl);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    logger->error("[REST] curl_easy_perform() failed: {}", curl_easy_strerror(res));
    return symbols;
  }

  // 使用 simdjson 解析响应
  try {
    simdjson::dom::parser parser;
    simdjson::padded_string pj{response_buffer};
    auto doc = parser.parse(pj);
    if (doc.error() != simdjson::SUCCESS) {
      logger->error("[REST] Failed to parse JSON");
      return symbols;
    }

    // 期货市场: doc["symbols"] 数组
    auto symbols_array = doc["symbols"];
    if (symbols_array.error() == simdjson::SUCCESS) {
      for (auto symbol_obj : symbols_array) {
        std::string_view symbol_name;
        std::string_view status;
        if (symbol_obj["symbol"].get(symbol_name) == simdjson::SUCCESS &&
            symbol_obj["status"].get(status) == simdjson::SUCCESS) {
          // 只订阅 TRADING 状态且为 USDT 结尾的交易对
          if (status == "TRADING" && symbol_name.ends_with("USDT")) {
            std::string s(symbol_name);
            // 转换为小写
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
            symbols.push_back(s);
          }
        }
      }
    }
  } catch (const std::exception& e) {
    logger->error("[REST] Exception while parsing: {}", e.what());
  }

  logger->info("[REST] Fetched {} trading symbols", symbols.size());
  return symbols;
}
#endif

// 构建订阅消息
std::string build_subscribe_message(const std::vector<std::string>& symbols, const std::string& stream_type = "aggTrade") {
  std::ostringstream oss;
  oss << R"({"method":"SUBSCRIBE","params":[)";
  
  for (size_t i = 0; i < symbols.size(); ++i) {
    if (i > 0) oss << ",";
    oss << "\"" << symbols[i] << "@" << stream_type << "\"";
  }
  
  oss << R"(],"id":1})";
  return oss.str();
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

// 异步日志写入器
class AsyncLogWriter {
public:
  explicit AsyncLogWriter(const std::string& filename) 
    : running_(true), filename_(filename) {
    log_file_.open(filename, std::ios::out | std::ios::app);
    if (!log_file_.is_open()) {
      throw std::runtime_error("Failed to open log file: " + filename);
    }
    
    // 启动写入线程
    writer_thread_ = std::thread([this]() {
      while (running_) {
        std::string line;
        {
          std::unique_lock<std::mutex> lock(mutex_);
          if (queue_.empty()) {
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
          }
          line = std::move(queue_.front());
          queue_.pop();
        }
        
        // 写入文件（在锁外执行 I/O）
        log_file_ << line;
        
        // 每 500 条刷新一次，平衡性能和数据安全
        if (++write_count_ % 500 == 0) {
          log_file_.flush();
        }
      }
      
      // 退出前写入所有剩余数据
      std::unique_lock<std::mutex> lock(mutex_);
      while (!queue_.empty()) {
        log_file_ << queue_.front();
        queue_.pop();
      }
      log_file_.flush();
    });
  }
  
  ~AsyncLogWriter() {
    running_ = false;
    if (writer_thread_.joinable()) {
      writer_thread_.join();
    }
    if (log_file_.is_open()) {
      log_file_.close();
    }
  }
  
  // 非阻塞写入
  void write(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(line);
    
    // 队列过大时发出警告（但不阻塞）
    if (queue_.size() > 10000) {
      static uint64_t last_warn = 0;
      uint64_t now = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
      if (now - last_warn > 60) {  // 每分钟最多警告一次
        std::cerr << "[AsyncLogWriter] Warning: queue size = " << queue_.size() << std::endl;
        last_warn = now;
      }
    }
  }
  
  size_t queue_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

private:
  std::atomic<bool> running_;
  std::ofstream log_file_;
  std::string filename_;
  std::queue<std::string> queue_;
  mutable std::mutex mutex_;
  std::thread writer_thread_;
  uint64_t write_count_{0};
};

// 进程内简化关联器（基于 `event_correlator` 适配），将 NIC/内核/用户态事件进行关联
struct CorrelatedEvent {
  uint64_t ts_nic_ns{0};
  uint64_t ts_kernel_ns{0};
  uint64_t ts_userspace_ns{0};
  uint64_t ts_cpu_deser_end_ns{0};
  uint64_t src_send_ts_ns{0};
  uint64_t msk_send_ns{0};
  uint64_t seq_no{0};
  uint32_t tcp_seq{0};
  uint64_t sock_ptr{0};
  int32_t sock_fd{-1};
  std::string symbol;  // Trading pair symbol

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
  uint64_t latency_msk_send() const {
    return (msk_send_ns > ts_cpu_deser_end_ns) ? (msk_send_ns - ts_cpu_deser_end_ns) : 0;
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
  explicit EventCorrelator(std::shared_ptr<spdlog::logger> logger, AsyncLogWriter* async_writer = nullptr) 
    : logger_(std::move(logger)), async_writer_(async_writer) {}

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
        ce.msk_send_ns = ue.ts_msk_send_ns;
        ce.seq_no = ue.seq_no;
        ce.tcp_seq = ue.tcp_seq ? ue.tcp_seq : (has_n ? best_n.tcp_seq : best_k.tcp_seq);
        ce.sock_ptr = ue.sock_ptr ? ue.sock_ptr : best_k.sock_ptr;
        ce.sock_fd = ue.sock_fd;
        
        // Extract symbol from JSON payload
        ce.symbol = "UNKNOWN";
        try {
          simdjson::dom::parser parser;
          simdjson::padded_string pj{ue.data, ue.data_len};
          auto doc_res = parser.parse(pj);
          if (doc_res.error() == simdjson::SUCCESS) {
            simdjson::dom::element doc = doc_res.value();
            std::string_view symbol_view;
            // Try combined stream: data.s
            auto data = doc["data"];
            if (data.error() == simdjson::SUCCESS) {
              if (data.value()["s"].get(symbol_view) == simdjson::SUCCESS) {
                ce.symbol = std::string(symbol_view);
              }
            } else {
              // Try direct format: s
              if (doc["s"].get(symbol_view) == simdjson::SUCCESS) {
                ce.symbol = std::string(symbol_view);
              }
            }
          }
        } catch (...) {
          // Keep default "UNKNOWN" on parse error
        }

        // 输出五段：BN->NIC、NIC->Kernel、Kernel->User、CPU(反序列化)、MSK_Send
        // Total = BN->NIC + NIC->Kernel + Kernel->User + CPU + MSK_Send
        auto nk = has_n ? fmt::format("{}ns", ce.latency_nic_to_kernel()) : std::string("N/A");
        auto ku = fmt::format("{}ns", ce.latency_kernel_to_user());
        auto cpu = fmt::format("{}ns", ce.latency_cpu_deser());
        auto msk = fmt::format("{}ns", ce.latency_msk_send());
        // per-message 偏移 K（epoch - steady），将 NIC 的 steady 对齐到 Epoch 再与 E 相减
        uint64_t K = (ue.ts_userspace_epoch_ns > ue.ts_userspace_ns) ? (ue.ts_userspace_epoch_ns - ue.ts_userspace_ns) : 0;
        uint64_t bn_nic_ns = (has_n && ue.src_send_ts_ns > 0 && K > 0) ? ((ce.ts_nic_ns + K) - ue.src_send_ts_ns) : 0;
        auto bn_nic = has_n && bn_nic_ns > 0 ? fmt::format("{}ns", bn_nic_ns) : std::string("N/A");
        // Total = BN->NIC + NIC->Kernel + Kernel->User + CPU + MSK_Send
        uint64_t total_ns = bn_nic_ns + ce.latency_nic_to_kernel() + ce.latency_kernel_to_user() + ce.latency_cpu_deser() + ce.latency_msk_send();
        auto total = has_n ? fmt::format("{}ns", total_ns) : std::string("N/A");
        logger_->info(
          "[delay] seq={} tcp_seq={} symbol={} BN->NIC={} NIC->Kernel={} Kernel->User={} CPU={} MSK={} Total={}",
          ce.seq_no,
          ce.tcp_seq,
          ce.symbol,
          bn_nic,
          nk,
          ku,
          cpu,
          msk,
          total);
        
        // 异步写入日志文件（仅写入完整的记录，跳过包含 N/A 的）
        if (async_writer_ && has_n && bn_nic_ns > 0) {
          std::ostringstream log_line;
          log_line << "seq=" << ce.seq_no
                   << " tcp_seq=" << ce.tcp_seq
                   << " symbol=" << ce.symbol
                   << " BN->NIC=" << bn_nic_ns << "ns"
                   << " NIC->Kernel=" << ce.latency_nic_to_kernel() << "ns"
                   << " Kernel->User=" << ce.latency_kernel_to_user() << "ns"
                   << " CPU=" << ce.latency_cpu_deser() << "ns"
                   << " MSK=" << ce.latency_msk_send() << "ns"
                   << " Total=" << total_ns << "ns\n";
          async_writer_->write(log_line.str());  // 非阻塞写入
        }
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
  AsyncLogWriter* async_writer_;
};

int main(int argc, char** argv) {
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  spdlog::set_level(spdlog::level::info);
  auto logger = spdlog::get("ws_delay");
  if (!logger) {
    logger = spdlog::stdout_color_mt("ws_delay");
  }

  // 解析命令行参数
  bool enable_log_file = false;
  std::string uri = "wss://fstream.binance.com:443/ws/stream";
  
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-wlogs") {
      enable_log_file = true;
    } else if (arg.find("wss://") == 0 || arg.find("ws://") == 0) {
      uri = arg;
    }
  }
  
  // 创建异步日志写入器（如果启用）
  std::unique_ptr<AsyncLogWriter> async_writer;
  if (enable_log_file) {
    std::string log_filename = "latency_" + std::to_string(std::time(nullptr)) + ".log";
    try {
      async_writer.reset(new AsyncLogWriter(log_filename));
      logger->info("[wlogs] Async latency log writer enabled: {}", log_filename);
    } catch (const std::exception& e) {
      logger->error("[wlogs] Failed to create async writer: {}", e.what());
      enable_log_file = false;
    }
  }

  // 获取所有交易对（Binance 期货市场）
  std::vector<std::string> symbols;
#ifdef HAVE_CURL
  std::string api_url = "https://fapi.binance.com/fapi/v1/exchangeInfo";
  logger->info("[REST] Fetching all trading symbols from {}", api_url);
  symbols = fetch_all_symbols(api_url, logger);
  
  if (symbols.empty()) {
    logger->warn("[REST] No symbols fetched, falling back to default subscription");
    symbols.push_back("btcusdt");
  }
#else
  logger->warn("[CURL] Not available. Using default subscription. Install libcurl4-openssl-dev to enable dynamic symbol fetching.");
  symbols.push_back("btcusdt");
  symbols.push_back("ethusdt");
  symbols.push_back("bnbusdt");
#endif
  
  // 限制最多订阅交易对数量
  // Binance 单连接限制：通过二分法测试得出最大值为 205 个流
  const size_t MAX_SUBSCRIPTIONS = 205;
  if (symbols.size() > MAX_SUBSCRIPTIONS) {
    logger->warn("[WS] Symbol count {} exceeds limit {}, truncating to first {} symbols", 
                 symbols.size(), MAX_SUBSCRIPTIONS, MAX_SUBSCRIPTIONS);
    symbols.resize(MAX_SUBSCRIPTIONS);
  }
  
  // 构建订阅消息
  std::string subscribe_msg = build_subscribe_message(symbols, "aggTrade");
  logger->info("[WS] Subscribe message length: {} bytes, symbols count: {}", subscribe_msg.size(), symbols.size());

  // Initialize shared memory queue (creator)
  // 使用 1M 容量以支持长时间运行（约 640 MB 内存，可缓冲 ~15 分钟高峰流量）
  g_shared_queue = create_shared_queue(SHARED_QUEUE_NAME, 1 << 20);  // 1M events
  if (!g_shared_queue) {
    logger->error("Failed to create shared queue");
    return 1;
  }

#ifdef HAVE_ZMQ
  // Initialize ZeroMQ producer for MSK message sending
  try {
    std::string zmq_endpoint = "tcp://*:5555";
    if (argc >= 3) zmq_endpoint = argv[2];  // Optional: custom ZMQ endpoint
    g_zmq_producer = std::make_shared<ZMQProducer>(zmq_endpoint);
    logger->info("[ZMQ] Producer initialized at {}", zmq_endpoint);
  } catch (const std::exception& e) {
    logger->error("[ZMQ] Failed to initialize producer: {}", e.what());
    return 1;
  }
#else
  logger->warn("[ZMQ] ZeroMQ support not available. Install libzmq3-dev and rebuild.");
#endif

  client_t ws_client;
  ws_client.init_asio();
  ws_client.set_tls_init_handler([&ws_client](websocketpp::connection_hdl h) {
    return on_tls_init(h, ws_client);
  });
  ws_client.clear_access_channels(websocketpp::log::alevel::all);
  ws_client.clear_error_channels(websocketpp::log::elevel::all);

  // 连接建立回调：记录 socket fd 并发送订阅
  ws_client.set_open_handler([&ws_client, logger, subscribe_msg](websocketpp::connection_hdl h) {
    logger->info("[ws] connected");
    auto con = ws_client.get_con_from_hdl(h);
    g_websocket_fd = get_socket_fd(con);
    if (g_websocket_fd >= 0) {
      logger->info("[ws] socket_fd={}", g_websocket_fd);
      write_socket_fd_to_ebpf(g_websocket_fd);
    }
    // 发送订阅消息（订阅所有交易对）
    logger->info("[ws] Sending subscription for all symbols...");
    con->send(subscribe_msg);
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

    // 解析 BN 发送时间（确定性使用 E 字段 - Event time，与 normal_ws 一致），单位为毫秒
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
        // combined stream: data.E
        auto data = doc["data"];
        if (data.error() == simdjson::SUCCESS) {
          simdjson::dom::element de = data.value();
          if (get_uint(de, "E", e_ms)) {
            src_send_ts_ns = e_ms * 1000000ULL; // ms -> ns
          }
        } else {
          // single stream: E
          if (get_uint(doc, "E", e_ms)) {
            src_send_ts_ns = e_ms * 1000000ULL; // ms -> ns
          }
        }
      } else {
        logger->warn("[parse] simdjson parse error: {}", simdjson::error_message(doc_res.error()));
      }
    }

    // 反序列化完成时刻（这里只在 JSON 解析完成后作为结束时间）
    uint64_t ts_after_deser_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            steady_clock::now().time_since_epoch()).count());

    // 真实构建和发送 MSK 消息（使用 ZeroMQ）
    uint64_t ts_msk_send_ns = 0;
    {
#ifdef HAVE_ZMQ
      if (g_zmq_producer) {
        simdjson::dom::parser parser;
        simdjson::padded_string pj{msg->get_payload()};
        auto doc_res = parser.parse(pj);
        if (doc_res.error() == simdjson::SUCCESS) {
          simdjson::dom::element doc = doc_res.value();
          
          // 构建类似 Kafka 的消息格式
          std::ostringstream msk_msg;
          msk_msg << "{\"bornTimestamp\":" << (ts_userspace_epoch_ns / 1000000)
                  << ",\"data\":{";
          
          // 提取事件类型和基本字段
          std::string_view event_type;
          std::string_view symbol;
          
          // 检查是否是 combined stream
          auto data = doc["data"];
          if (data.error() == simdjson::SUCCESS) {
            simdjson::dom::element de = data.value();
            if (de["e"].get(event_type) == simdjson::SUCCESS) {
              msk_msg << "\"event\":\"" << event_type << "\"";
            }
            if (de["s"].get(symbol) == simdjson::SUCCESS) {
              msk_msg << ",\"symbol\":\"" << symbol << "\"";
            }
          } else {
            if (doc["e"].get(event_type) == simdjson::SUCCESS) {
              msk_msg << "\"event\":\"" << event_type << "\"";
            }
            if (doc["s"].get(symbol) == simdjson::SUCCESS) {
              msk_msg << ",\"symbol\":\"" << symbol << "\"";
            }
          }
          
          msk_msg << ",\"ts\":" << src_send_ts_ns;
          msk_msg << ",\"exchange\":\"WSCC-BN\"";
          msk_msg << "}}";
          
          std::string msk_payload = msk_msg.str();
          std::string topic = std::string(symbol.empty() ? "unknown" : symbol) + ":WSCC-BN";
          
          // 真实发送到 ZeroMQ
          try {
            g_zmq_producer->send_message(topic, msk_payload);
          } catch (const std::exception& e) {
            logger->warn("[ZMQ] Send failed: {}", e.what());
          }
        }
      }
#else
      // 如果没有 ZMQ，只做简单的消息构建模拟
      std::ostringstream msk_msg;
      msk_msg << "{\"bornTimestamp\":" << (ts_userspace_epoch_ns / 1000000)
              << ",\"data\":{\"ts\":" << src_send_ts_ns << "}}";
      std::string msk_payload = msk_msg.str();
      (void)msk_payload; // 防止未使用警告
#endif
      
      // 记录 MSK 发送完成时间
      ts_msk_send_ns = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              steady_clock::now().time_since_epoch()).count());
    }

    UnifiedEvent ev{};
    ev.type = EVENT_TYPE_USERSPACE;
    ev.seq_no = g_seq_no.fetch_add(1);
    ev.ts_userspace_ns = ts_userspace_ns;
    ev.ts_userspace_epoch_ns = ts_userspace_epoch_ns;
    ev.ts_cpu_deserialization = ts_after_deser_ns; // 绝对时间，相关器内再做差
    ev.src_send_ts_ns = src_send_ts_ns;            // BN 发送时间（绝对）
    ev.ts_msk_send_ns = ts_msk_send_ns;            // MSK 发送完成时间（绝对）
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
  std::thread corr_thread([logger, &async_writer]() {
    EventCorrelator corr(logger, async_writer.get());
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

  // 关闭异步日志写入器（析构函数会自动完成剩余写入）
  if (async_writer) {
    logger->info("[wlogs] Closing async log writer, queue size: {}", async_writer->queue_size());
    async_writer.reset();  // 触发析构，等待所有数据写入完成
    logger->info("[wlogs] Async log writer closed");
  }

  cleanup_shared_queue(g_shared_queue);
  return 0;
}

