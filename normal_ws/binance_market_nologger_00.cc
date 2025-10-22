#include <iostream>
#include <string>
#include <atomic>
#include <x86intrin.h>

#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>

#include <boost/asio/ip/tcp.hpp>

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include "util/kafka_producer.h"
#include "util/time.h"

using websocketpp::connection_hdl;

// 延迟测试配置
const int MAX_MESSAGES = 20;  // 只接收20条数据
std::atomic<int> message_count(0);
std::atomic<bool> should_stop(false);  // 停止标志

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

// WebSocket 客户端类型
typedef websocketpp::client<websocketpp::config::asio_tls_client> client;
typedef websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context> context_ptr;

std::string fmtSymbol(std::string s) {
  s.insert(s.size()-4, "-");
  return s;
}

class BinanceWebSocket {
public:
  BinanceWebSocket() {
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
    m_swapProducer = std::make_shared<CKafkaProducer>(brokers, swapTopic);
	  if (!m_swapProducer.get()) {
      printf("kafka auto share fail");
      return;
    }

    if (0 != m_swapProducer->Create()) {
      printf("kafka create fail");
	    return;
	  }
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
            "btcusdt@bookTicker",
            "ethusdt@bookTicker"
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
    // 如果已经达到限制，直接返回不处理
    if (should_stop.load()) {
      return;
    }
    
    // 提前检查，避免超过限制
    int current_count = message_count.load();
    if (current_count >= MAX_MESSAGES) {
      return;
    }
    
    // 📍 开始计时 - 接收到数据
    uint64_t t_start = __rdtsc();
    
    // payload: {"e":"bookTicker","u":7016077745944,"s":"ETHUSDT","b":"1896.65","B":"99.579","a":"1896.66","A":"190.561","T":1741867464776,"E":1741867464776}
    long nowns = GET_CURRENT_TIME();

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
        long eTime = doc["E"].GetInt64() * 1000000;
        long tTime = doc["T"].GetInt64() * 1000000;

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
          reportData.AddMember("exchangeTime", tTime, allocator);
          reportData.AddMember("bid1", bidPrice, allocator);
          reportData.AddMember("bid1Volume", bidVolume, allocator);
          reportData.AddMember("ask1", askPrice, allocator);
          reportData.AddMember("ask1Volume", askVolume, allocator);
          reportData.AddMember("now", nowns, allocator);
          frame.AddMember("pbpropv0", reportData, allocator);
        }
        frame.AddMember("server", rapidjson::Value("rapidjsonback", allocator), allocator);
        frame.AddMember("time", rapidjson::Value(std::to_string(nowns/1000000).c_str(), allocator), allocator);
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
        kafkaFrame.AddMember("bornTimestamp", rapidjson::Value(std::to_string(nowns/1000000).c_str(), kallocator), kallocator); 
        {
          rapidjson::Value bboKafkaMsg(rapidjson::kObjectType);
          bboKafkaMsg.AddMember("symbol", rapidjson::Value(symbol.c_str(), kallocator), kallocator);
  #ifdef EVN_JAPAN
          bboKafkaMsg.AddMember("exchange", rapidjson::Value("JPWSCC-BN", kallocator), kallocator);
  #else
          bboKafkaMsg.AddMember("exchange", rapidjson::Value("WSCC-BN", kallocator), kallocator);
  #endif
          bboKafkaMsg.AddMember("ts", tTime, kallocator);
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

        std::string bboKafkaRawKey = std::string(symbol) + std::string(":WSCC-BN:") + std::to_string(nowns/1000000);
        std::string bboKafkaRawMsg = std::string(kbuffer.GetString());
        m_swapProducer->PushMessage(bboKafkaRawMsg, bboKafkaRawKey);
        
        // 📍 结束计时 - 数据已发送
        uint64_t t_end = __rdtsc();
        uint64_t cycles = t_end - t_start;
        double local_latency_us = cycles / (CPU_FREQ_GHZ * 1000.0);  // 本地处理延迟（微秒）
        
        // 计算端到端延迟（网络+处理）
        double total_latency_ms = (nowns - tTime) / 1000000.0;  // 转换为毫秒
        double network_latency_ms = total_latency_ms - (local_latency_us / 1000.0);  // 网络延迟（毫秒）
        
        // 增加计数器
        int count = ++message_count;
        
        // 使用spdlog打印延迟信息（JSON格式，便于后续分析）
        spdlog::info("{{\"seq\":{},\"symbol\":\"{}\",\"local_us\":{:.2f},\"network_ms\":{:.2f},\"total_ms\":{:.2f},\"exchange_ts\":{},\"receive_ts\":{}}}", 
                     count, symbol, local_latency_us, network_latency_ms, total_latency_ms, tTime/1000000, nowns/1000000);
        
        // 达到20条后停止
        if (count >= MAX_MESSAGES) {
          should_stop.store(true);  // 设置停止标志
          spdlog::info("=== 测试完成，接收了 {} 条数据 ===", MAX_MESSAGES);
          m_client.stop();
        }
      }
    } catch (const std::exception& e) {
      std::cerr << "process payload fail: " << e.what() << ", payload: " << msg->get_payload() << std::endl;
    }
  }

private:
  client m_client;
  std::shared_ptr<spdlog::logger> m_logger;
  std::shared_ptr<CKafkaProducer> m_swapProducer;
};

int main() {
  // 配置spdlog：只输出到控制台
  auto console_logger = spdlog::stdout_color_mt("console");
  spdlog::set_default_logger(console_logger);
  spdlog::set_pattern("[%H:%M:%S.%e] %v");
  
  // 打印CPU频率和优化信息
  spdlog::info("=== 延迟测试启动 ===");
  spdlog::info("检测到的CPU频率: {:.2f} GHz", CPU_FREQ_GHZ);
  spdlog::info("将接收 {} 条数据后自动退出", MAX_MESSAGES);
  spdlog::info("Socket优化: TCP_NODELAY + 大缓冲区 + KEEPALIVE");
  spdlog::info("");
  
  BinanceWebSocket ws;
  std::string uri = "wss://fstream.binance.com:443/ws/stream";
  ws.connect(uri);
  
  spdlog::info("=== 程序结束 ===");
  return 0;
}

