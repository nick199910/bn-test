#include <iostream>
#include <string>
#include <atomic>
#include <csignal>
#include <x86intrin.h>

#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>

#include <boost/asio/ip/tcp.hpp>

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"

// #include "util/time.h"  // Not needed - using standard chrono instead

// Define GET_CURRENT_TIME macro for timestamp
#define GET_CURRENT_TIME() (std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count())

#ifdef HAVE_RDKAFKA
// Kafka producer would be included here if available
// #include "util/kafka_producer.h"
#warning "Kafka support enabled but kafka_producer.h not available"
#endif

using websocketpp::connection_hdl;
using steady_clock = std::chrono::steady_clock;

// 延迟测试配置
std::atomic<int> message_count(0);
std::atomic<bool> should_stop(false);  // 停止标志

// 前向声明
typedef websocketpp::client<websocketpp::config::asio_tls_client> client;
client* g_client_ptr = nullptr;

// 信号处理函数
void signal_handler(int signal) {
  if (signal == SIGINT) {
    std::cout << "\n[signal] Caught SIGINT (Ctrl+C), exiting..." << std::endl;
    should_stop.store(true);
    // 主动停止 WebSocket 客户端
    if (g_client_ptr) {
      g_client_ptr->stop();
    }
  }
}

// 自动检测CPU频率（GHz）
double get_cpu_freq_ghz() {
    uint64_t start = __rdtsc();
    auto t1 = std::chrono::high_resolution_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto t2 = std::chrono::high_resolution_clock::now();
    uint64_t end = __rdtsc();
    
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
    uint64_t cycles = end - start;
    double freq_ghz = (cycles * 1000000000.0) / (duration * 1000000000.0);
    return freq_ghz;
}

double CPU_FREQ_GHZ = get_cpu_freq_ghz();

// TLS context 类型
typedef websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context> context_ptr;

std::string fmtSymbol(std::string s) {
  s.insert(s.size()-4, "-");
  return s;
}

class BinanceWebSocket {
public:
  BinanceWebSocket() {
    // 设置全局指针，供信号处理使用
    g_client_ptr = &m_client;
    
    m_client.set_access_channels(websocketpp::log::alevel::all);
    m_client.clear_access_channels(websocketpp::log::alevel::frame_payload);

    m_client.set_reuse_addr(true);
    m_client.init_asio();

    // 设置消息处理器
    m_client.set_message_handler(bind(&BinanceWebSocket::on_message, this, std::placeholders::_1, std::placeholders::_2));

    // 设置连接打开和关闭处理器
    m_client.set_open_handler(bind(&BinanceWebSocket::on_open, this, std::placeholders::_1));
    m_client.set_close_handler(bind(&BinanceWebSocket::on_close, this, std::placeholders::_1));
    m_client.set_tls_init_handler([](websocketpp::connection_hdl hdl) {
      std::shared_ptr<websocketpp::lib::asio::ssl::context> ctx =
        std::make_shared<websocketpp::lib::asio::ssl::context>(websocketpp::lib::asio::ssl::context::sslv23_client);
            
      return ctx;
    });
    // websocketpp 默认会自动处理 ping 和 pong 消息，所以通常不需要显式地处理 ping 消息，除非你想要自定义处理方式。
    m_client.set_ping_handler(bind(&BinanceWebSocket::on_ping, this, std::placeholders::_1, std::placeholders::_2));

    // 延迟测试模式：不需要文件日志
    m_logger = spdlog::default_logger();  // 使用默认logger
    spdlog::set_pattern("%v");  

    std::string brokers = "b-1.marketprice.ha3uq3.c5.kafka.ap-southeast-1.amazonaws.com:9092,b-2.marketprice.ha3uq3.c5.kafka.ap-southeast-1.amazonaws.com:9092,b-3.marketprice.ha3uq3.c5.kafka.ap-southeast-1.amazonaws.com:9092";
    std::string swapTopic = "swapBbo";
#ifdef HAVE_RDKAFKA
    m_swapProducer = std::make_shared<CKafkaProducer>(brokers, swapTopic);
    if (!m_swapProducer.get()) {
      m_logger->error("[Kafka] Failed to create producer");
      return;
    }

    if (0 != m_swapProducer->Create()) {
      m_logger->error("[Kafka] Failed to initialize producer");
    }
#else
    m_logger->info("[Kafka] Support disabled at compile time");
    (void)brokers; (void)swapTopic;  // Suppress unused warnings
#endif
    return;
  }

  context_ptr on_tls_init(const char * hostname, websocketpp::connection_hdl) {
    context_ptr ctx = websocketpp::lib::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::sslv23);
    return ctx;
  }

  // 连接到 Binance WebSocket 服务
  void connect(const std::string& uri) {
    websocketpp::lib::error_code ec;
    client::connection_ptr con = m_client.get_connection(uri, ec);

    if (ec) {
      std::cout << "conn fail: " << ec.message() << std::endl;
      return;
    }

    m_client.connect(con);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 运行 WebSocket 客户端
    m_client.run();
  }

  // 连接成功的处理
  void on_open(connection_hdl hdl) {
    std::cout << "conn success!! send subscribe request..." << std::endl;
    
    // 在连接建立后设置 Socket 选项
    try {
      client::connection_ptr con = m_client.get_con_from_hdl(hdl);
      if (con) {
        auto& ssl_socket = con->get_socket();
        auto& socket = ssl_socket.lowest_layer();
        
        boost::system::error_code ec;
        
        // TCP_NODELAY
        socket.set_option(boost::asio::ip::tcp::no_delay(true), ec);
        if (!ec) spdlog::info("TCP_NODELAY 已启用");
        
        // SO_RCVBUF (512KB)
        ec.clear();
        socket.set_option(boost::asio::socket_base::receive_buffer_size(524288), ec);
        if (!ec) spdlog::info("SO_RCVBUF 设置为 512KB");
        
        // SO_SNDBUF (512KB)
        ec.clear();
        socket.set_option(boost::asio::socket_base::send_buffer_size(524288), ec);
        if (!ec) spdlog::info("SO_SNDBUF 设置为 512KB");
        
        // SO_KEEPALIVE
        ec.clear();
        socket.set_option(boost::asio::socket_base::keep_alive(true), ec);
        if (!ec) spdlog::info("SO_KEEPALIVE 已启用");
      }
    } catch (const std::exception& e) {
      spdlog::error("Socket 优化设置异常: {}", e.what());
    }

    // 发送订阅消息（订阅 bookticker 数据）
    std::string subscribe_message = R"({
        "method": "SUBSCRIBE",
        "params": [
            "btcusdt@bookTicker"
        ],
        "id": 1
    })";

    m_client.send(hdl, subscribe_message, websocketpp::frame::opcode::text);
  }

  // 连接关闭的处理
  void on_close(connection_hdl hdl) {
    std::cout << "conn closed" << std::endl;
    m_client.get_io_service().post([&]() {
      m_client.run();
    });
  }

  bool on_ping(websocketpp::connection_hdl hdl, const std::string& payload) {
    m_client.pong(hdl, payload);
    return true;
  }

  // 处理消息
  void on_message(connection_hdl hdl, client::message_ptr msg) {
    // 如果收到停止信号，直接返回
    if (should_stop.load()) {
      return;
    }
    
    // 📍 开始计时 - 接收到数据（使用 steady_clock，与 ws_client_delay 一致）
    uint64_t t_start = __rdtsc();
    uint64_t ts_userspace_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            steady_clock::now().time_since_epoch()).count());
    uint64_t ts_userspace_epoch_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    try {
      char* json = (char*)msg->get_payload().c_str();
      rapidjson::Document doc;
      doc.Parse(json);

      if (doc.HasMember("e") && doc["e"].IsString() && doc["e"].GetString() == std::string("bookTicker")) {
        std::string symbol = fmtSymbol(doc["s"].GetString());
        double askPrice = stod(std::string(doc["a"].GetString()));
        double askVolume = stod(std::string(doc["A"].GetString()));
        double bidPrice = stod(std::string(doc["b"].GetString()));
        double bidVolume = stod(std::string(doc["B"].GetString()));
        long updateID = doc["u"].GetInt64();
        uint64_t eTime_ns = doc["E"].GetInt64() * 1000000ULL;  // 毫秒转纳秒
        uint64_t tTime_ns = doc["T"].GetInt64() * 1000000ULL;  // 毫秒转纳秒

        // 生成日志上报
        rapidjson::Document frame;
        frame.SetObject();
        rapidjson::Document::AllocatorType& allocator = frame.GetAllocator();
        frame.AddMember("channel", rapidjson::Value("binance_ws_cc", allocator), allocator);
        frame.AddMember("cost", rapidjson::Value("-1", allocator), allocator);
        frame.AddMember("device_id", rapidjson::Value("-1", allocator), allocator);
#ifdef EVN_JAPAN
        frame.AddMember("event", rapidjson::Value("jpBinanceBBOCCReport", allocator), allocator);
#else
        frame.AddMember("event", rapidjson::Value("binanceBBOCCReport", allocator), allocator);
#endif 
        frame.AddMember("lang", rapidjson::Value("-1", allocator), allocator);
        frame.AddMember("net", rapidjson::Value("-1", allocator), allocator);
        {
          rapidjson::Value reportData(rapidjson::kObjectType);
          reportData.AddMember("symbol", rapidjson::Value(symbol.c_str(), allocator), allocator);
          reportData.AddMember("contractType", rapidjson::Value("swap", allocator), allocator);
          reportData.AddMember("exchangeTime", tTime_ns, allocator);
          reportData.AddMember("bid1", bidPrice, allocator);
          reportData.AddMember("bid1Volume", rapidjson::Value(std::to_string(bidVolume).c_str(), allocator), allocator);
          reportData.AddMember("ask1", askPrice, allocator);
          reportData.AddMember("ask1Volume", rapidjson::Value(std::to_string(askVolume).c_str(), allocator), allocator);
          reportData.AddMember("eventTime", rapidjson::Value(std::to_string(tTime_ns).c_str(), allocator), allocator);
          reportData.AddMember("now", ts_userspace_ns, allocator);
          frame.AddMember("pbpropv0", reportData, allocator);
        }
        frame.AddMember("server", rapidjson::Value("rapidjsonback", allocator), allocator);
        frame.AddMember("time", rapidjson::Value(std::to_string(ts_userspace_ns/1000000).c_str(), allocator), allocator);
        frame.AddMember("type", rapidjson::Value("binance_ws_cc", allocator), allocator);
        frame.AddMember("uid", rapidjson::Value("-1", allocator), allocator);
        frame.AddMember("version", rapidjson::Value("-1", allocator), allocator);

        // 序列化为 JSON 字符串
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        frame.Accept(writer);
        // m_logger->info(buffer.GetString());  // 延迟测试时注释掉避免干扰

        // report msk
        rapidjson::Document kafkaFrame;
        kafkaFrame.SetObject();
        rapidjson::Document::AllocatorType& kallocator = kafkaFrame.GetAllocator();
        kafkaFrame.AddMember("bornTimestamp", rapidjson::Value(std::to_string(ts_userspace_ns/1000000).c_str(), kallocator), kallocator); 
        {
          rapidjson::Value bboKafkaMsg(rapidjson::kObjectType);
          bboKafkaMsg.AddMember("symbol", rapidjson::Value(symbol.c_str(), kallocator), kallocator);
  #ifdef EVN_JAPAN
          bboKafkaMsg.AddMember("exchange", rapidjson::Value("JPWSCC-BN", kallocator), kallocator);
  #else
          bboKafkaMsg.AddMember("exchange", rapidjson::Value("WSCC-BN", kallocator), kallocator);
  #endif
          bboKafkaMsg.AddMember("ts", tTime_ns, kallocator);
          bboKafkaMsg.AddMember("ask1Price", askPrice, kallocator);
          bboKafkaMsg.AddMember("ask1Volume", askVolume, kallocator);
          bboKafkaMsg.AddMember("bid1Price", bidPrice, kallocator);
          bboKafkaMsg.AddMember("bid1Volume", bidVolume, kallocator);

          // 序列化为 JSON 字符串
          rapidjson::StringBuffer bbobuffer;
          rapidjson::Writer<rapidjson::StringBuffer> bbowriter(bbobuffer);
          bboKafkaMsg.Accept(bbowriter);

          kafkaFrame.AddMember("data", rapidjson::Value(bbobuffer.GetString(), allocator), allocator);
        }

        // 序列化为 JSON 字符串
        rapidjson::StringBuffer kbuffer;
        rapidjson::Writer<rapidjson::StringBuffer> kwriter(kbuffer);
        kafkaFrame.Accept(kwriter);

        std::string bboKafkaRawKey = std::string(symbol) + std::string(":WSCC-BN:") + std::to_string(ts_userspace_ns/1000000);
        std::string bboKafkaRawMsg = std::string(kbuffer.GetString());
#ifdef HAVE_RDKAFKA
        m_swapProducer->PushMessage(bboKafkaRawMsg, bboKafkaRawKey);
#else
        (void)bboKafkaRawMsg; (void)bboKafkaRawKey;  // Suppress unused warnings
#endif
        
        // 📍 结束计时 - 数据已发送
        uint64_t t_end = __rdtsc();
        uint64_t cycles = t_end - t_start;
        uint64_t local_latency_ns = static_cast<uint64_t>(cycles / CPU_FREQ_GHZ);  // 本地处理延迟（纳秒）
        
        // 计算延迟（所有单位：纳秒）
        // 注意：需要使用 epoch 时间（system_clock）与 BN 的 epoch 时间比较
        uint64_t bn_to_local_ns = ts_userspace_epoch_ns > eTime_ns ? (ts_userspace_epoch_ns - eTime_ns) : 0;
        uint64_t total_with_cpu_ns = bn_to_local_ns + local_latency_ns;
        
        // 增加计数器
        int count = ++message_count;
        
        // 使用spdlog打印延迟信息（纳秒单位，与 ws_client_delay 一致）
        spdlog::info("{{\"seq\":{},\"symbol\":\"{}\",\"BN->Local\":{}ns,\"CPU\":{}ns,\"Total\":{}ns,\"exchange_ts\":{},\"receive_ts\":{}}}", 
                     count, symbol, bn_to_local_ns, local_latency_ns, total_with_cpu_ns, eTime_ns, ts_userspace_epoch_ns);
      }
    } catch (const std::exception& e) {
      std::cerr << "process payload fail: " << e.what() << ", payload: " << msg->get_payload() << std::endl;
    }
  }

private:
  client m_client;
  std::shared_ptr<spdlog::logger> m_logger;
#ifdef HAVE_RDKAFKA
  std::shared_ptr<CKafkaProducer> m_swapProducer;
#endif
};

int main() {
  // 注册信号处理函数
  signal(SIGINT, signal_handler);
  
  // 配置spdlog：只输出到控制台
  auto console_logger = spdlog::stdout_color_mt("console");
  spdlog::set_default_logger(console_logger);
  spdlog::set_pattern("[%H:%M:%S.%e] %v");
  
  // 打印CPU频率和优化信息
  spdlog::info("=== 延迟测试启动 ===");
  spdlog::info("检测到的CPU频率: {:.2f} GHz", CPU_FREQ_GHZ);
  spdlog::info("Press Ctrl+C to stop");
  spdlog::info("Socket优化: TCP_NODELAY + 大缓冲区 + KEEPALIVE");
  spdlog::info("时间单位: 纳秒 (ns)");
  spdlog::info("");
  
  BinanceWebSocket ws;
  std::string uri = "wss://fstream.binance.com:443/ws/stream";
  ws.connect(uri);
  
  spdlog::info("=== 程序结束 ===");
  return 0;
}

