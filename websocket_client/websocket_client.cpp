// websocket_client.cpp
// C++11, google-ish style. Uses websocketpp (Boost.Asio + OpenSSL).
// Build: see CMakeLists below.

#include "mpmc_queue.h"

// #include ""

#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

// websocketpp
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
#include <openssl/ssl.h>   // for SSL_CTX_set_tlsext_host_name
#include <boost/asio/ssl.hpp>

typedef websocketpp::config::asio_tls_client tls_client_config;
typedef websocketpp::client<tls_client_config> client_t;

using steady_clock = std::chrono::steady_clock;
using time_point = steady_clock::time_point;

struct Event {
  uint64_t seq_no;            // sequential message id
  uint64_t user_ts_ns;        // user space receive timestamp (ns)
  uint64_t sent_ts_ns;        // when we sent ping/payload (ns) if applicable
  uintptr_t sock_ptr;         // socket pointer hint for correlation
  uint64_t tcp_seq;           // tcp seq number hint (if available)
  std::string payload;        // raw payload or short snippet
};

static rigtorp::MPMCQueue<std::shared_ptr<Event>>* g_queue = nullptr;


static std::atomic<uint64_t> g_seq_no(1);

// TLS init callback for websocketpp
std::shared_ptr<boost::asio::ssl::context> on_tls_init(
    const char* /*hostname*/, websocketpp::connection_hdl) {
  auto ctx = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::sslv23);
  ctx->set_default_verify_paths();
  ctx->set_verify_mode(boost::asio::ssl::verify_peer);
  return ctx;
}

int main(int argc, char** argv) {
  // usage: websocket_client <binance_stream> <queue_capacity>
  // default endpoint if not provided:
  std::string uri = "wss://fstream.binance.com:443/ws/stream";
  size_t queue_cap = 1 << 16;
  if (argc >= 2) uri = argv[1];
  if (argc >= 3) queue_cap = std::stoul(argv[2]);

  rigtorp::MPMCQueue<std::shared_ptr<Event>> queue(queue_cap);
  g_queue = &queue;

  using client_t = websocketpp::client<websocketpp::config::asio_tls_client>;

  client_t ws_client; // make accessible to the lambda (or capture reference)

  auto tls_handler_with_sni = [&ws_client](websocketpp::connection_hdl h)
      -> std::shared_ptr<boost::asio::ssl::context> {
      namespace asio_ssl = boost::asio::ssl;
      auto ctx = std::make_shared<asio_ssl::context>(asio_ssl::context::sslv23);

      ctx->set_default_verify_paths();
      ctx->set_verify_mode(asio_ssl::verify_peer);

      // Try to set SNI hostname on the SSL_CTX
      try {
          client_t::connection_ptr con = ws_client.get_con_from_hdl(h);
          if (con && con->get_uri()) {
              std::string host = con->get_uri()->get_host();
              if (!host.empty()) {
                  // native_handle() returns SSL_CTX*
                  SSL_CTX_set_tlsext_servername_callback(ctx->native_handle(), host.c_str());
                  // SSL_CTX_set_tlsext_host_name(ctx->native_handle(), host.c_str());
              }
          }
      } catch (const std::exception& e) {
          // ignore: best-effort SNI only
          std::cerr << "warning: SNI not applied: " << e.what() << "\n";
      }

      return ctx;
  };


  ws_client.init_asio();
  ws_client.set_tls_init_handler(tls_handler_with_sni);
  ws_client.clear_access_channels(websocketpp::log::alevel::all);
  ws_client.clear_error_channels(websocketpp::log::elevel::all);

  ws_client.set_open_handler([&ws_client](websocketpp::connection_hdl h) {
    std::cout << "[ws] connected\n";
    // send a subscription (example: BTCUSDT@aggTrade) -- adapt as needed:
    auto con = ws_client.get_con_from_hdl(h);
    std::string sub = R"({"method":"SUBSCRIBE","params":["btcusdt@aggTrade"],"id":1})";
    con->send(sub);
    // schedule periodic pings to measure RTT
  });

  ws_client.set_message_handler([&ws_client](websocketpp::connection_hdl h, client_t::message_ptr msg) {
    uint64_t user_ts = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(steady_clock::now().time_since_epoch()).count());

    Event ev;
    ev.seq_no = g_seq_no.fetch_add(1);
    ev.user_ts_ns = user_ts;
    ev.sent_ts_ns = 0;
    ev.sock_ptr = 0;  // will be filled by kernel-ebpf via socket pointer correlation if available
    ev.tcp_seq = 0;
    ev.payload = msg->get_payload().substr(0, 512);
    auto evt = std::make_shared<Event>(ev);


    if (!g_queue->try_push(evt)) {
      // queue full, drop
    }
  });


  // ping/pong handler to measure RTT in user space
  ws_client.set_pong_handler([&ws_client](websocketpp::connection_hdl h, std::string payload) {
    uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(steady_clock::now().time_since_epoch()).count());
    // payload carries send timestamp in ascii
    uint64_t sent_ns = 0;
    try {
      sent_ns = std::stoull(payload);
    } catch (...) {
      sent_ns = 0;
    }
    uint64_t rtt_ns = (sent_ns == 0) ? 0 : (now_ns - sent_ns);
    std::cout << "[ping] rtt_us=" << (rtt_ns / 1000) << " us\n";
  });

  // connect
  websocketpp::lib::error_code ec;
  client_t::connection_ptr con = ws_client.get_connection(uri, ec);
  if (ec) {
    std::cerr << "Connection error: " << ec.message() << std::endl;
    return 1;
  }

  ws_client.connect(con);

  // Start a background thread that sends pings every second (payload = timestamp)
  std::thread ping_thread([&ws_client, con]() {
    while (true) {
      uint64_t ts_ns = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(steady_clock::now().time_since_epoch()).count());
      std::string payload = std::to_string(ts_ns);
      try {
        con->send(payload);
      } catch (const std::exception& e) {
        // ignore
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  });

  ws_client.run();
  ping_thread.join();
  return 0;
}
