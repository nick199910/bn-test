// websocket_client.cpp

#include "shared_events.h"

#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <memory>
#include <sys/mman.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
#include <openssl/ssl.h>
#include <boost/asio/ssl.hpp>

typedef websocketpp::config::asio_tls_client tls_client_config;
typedef websocketpp::client<tls_client_config> client_t;

using steady_clock = std::chrono::steady_clock;

// Shared memory queue for correlation
static SharedQueue* g_shared_queue = nullptr;
static std::atomic<uint64_t> g_seq_no(1);

// Map to store socket fd for correlation with eBPF
static int g_websocket_fd = -1;

// TLS init callback with SNI support
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
      if (!host.empty()) {
        // // Set SNI hostname
        // SSL* ssl = ctx->native_handle();
        // // SSL* ssl = static_cast<SSL*>(ctx->native_handle());
        // SSL_set_tlsext_host_name(ssl, host.c_str());
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "warning: SNI not applied: " << e.what() << "\n";
  }

  return ctx;
}

// Extract socket file descriptor from connection
int get_socket_fd(client_t::connection_ptr con) {
  try {
    // Access underlying socket through Boost.Asio
    auto& socket = con->get_raw_socket();
    if (socket.lowest_layer().is_open()) {
      return socket.lowest_layer().native_handle();
    }
  } catch (const std::exception& e) {
    std::cerr << "Failed to get socket fd: " << e.what() << "\n";
  }
  return -1;
}

int main(int argc, char** argv) {
  std::string uri = "wss://fstream.binance.com:443/ws/stream";
  if (argc >= 2) uri = argv[1];

  // Initialize shared memory queue
  g_shared_queue = create_shared_queue(SHARED_QUEUE_NAME, 1 << 16);
  if (!g_shared_queue) {
    std::cerr << "Failed to create shared queue\n";
    return 1;
  }

  client_t ws_client;

  ws_client.init_asio();
  
  // Set TLS handler with capture
  ws_client.set_tls_init_handler(
      [&ws_client](websocketpp::connection_hdl h) {
        return on_tls_init(h, ws_client);
      });

  ws_client.clear_access_channels(websocketpp::log::alevel::all);
  ws_client.clear_error_channels(websocketpp::log::elevel::all);

  ws_client.set_open_handler([&ws_client](websocketpp::connection_hdl h) {
    std::cout << "[ws] connected\n";
    
    auto con = ws_client.get_con_from_hdl(h);
    
    // Extract and store socket fd for eBPF correlation
    g_websocket_fd = get_socket_fd(con);
    if (g_websocket_fd >= 0) {
      std::cout << "[ws] socket_fd=" << g_websocket_fd << "\n";
      // Write socket FD to shared memory for eBPF to read
      write_socket_fd_to_ebpf(g_websocket_fd);
    }

    // Subscribe to Binance stream
    std::string sub = R"({"method":"SUBSCRIBE","params":["btcusdt@aggTrade"],"id":1})";
    con->send(sub);
  });

  ws_client.set_message_handler(
      [](websocketpp::connection_hdl h, client_t::message_ptr msg) {
        uint64_t user_ts = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                steady_clock::now().time_since_epoch()).count());

        UnifiedEvent ev;
        ev.type = EVENT_TYPE_USERSPACE;
        ev.seq_no = g_seq_no.fetch_add(1);
        ev.ts_userspace_ns = user_ts;
        ev.ts_kernel_ns = 0;
        ev.ts_nic_ns = 0;
        ev.sock_fd = g_websocket_fd;
        ev.sock_ptr = 0;
        ev.tcp_seq = 0;
        ev.pid = getpid();
        ev.data_len = std::min(msg->get_payload().size(), 
                               sizeof(ev.data) - 1);
        memcpy(ev.data, msg->get_payload().c_str(), ev.data_len);
        ev.data[ev.data_len] = '\0';

        if (!shared_queue_push(g_shared_queue, &ev)) {
          // Queue full
          std::cerr << "[ws] queue full, dropped event\n";
        }
      });

  // FIXED: Use proper ping frame instead of send()
  ws_client.set_pong_handler(
      [](websocketpp::connection_hdl h, std::string payload) {
        uint64_t now_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                steady_clock::now().time_since_epoch()).count());
        
        uint64_t sent_ns = 0;
        try {
          sent_ns = std::stoull(payload);
        } catch (...) {
          sent_ns = 0;
        }
        
        uint64_t rtt_ns = (sent_ns == 0) ? 0 : (now_ns - sent_ns);
        std::cout << "[ping] rtt_us=" << (rtt_ns / 1000) << " us\n";
      });

  // Connect
  websocketpp::lib::error_code ec;
  client_t::connection_ptr con = ws_client.get_connection(uri, ec);
  if (ec) {
    std::cerr << "Connection error: " << ec.message() << "\n";
    return 1;
  }

  ws_client.connect(con);

  // FIXED: Send proper WebSocket ping frames
  std::thread ping_thread([&ws_client, con]() {
    // Wait for connection to establish
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    while (true) {
      uint64_t ts_ns = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              steady_clock::now().time_since_epoch()).count());
      
      std::string payload = std::to_string(ts_ns);
      
      try {
        // Use ping() instead of send() to trigger pong handler
        websocketpp::lib::error_code ec;
        con->ping(payload, ec);
        if (ec) {
          std::cerr << "[ping] error: " << ec.message() << "\n";
        }
      } catch (const std::exception& e) {
        std::cerr << "[ping] exception: " << e.what() << "\n";
      }
      
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  });

  ws_client.run();
  ping_thread.join();

  cleanup_shared_queue(g_shared_queue);
  return 0;
}