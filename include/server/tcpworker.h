#pragma once
#include <iostream>
#include "mysocket.h"
#include "pool.h"
#include "myepoll.h"
#include "http_handler.h"
#include "upstream_manager.h"

extern std::atomic<bool> stop_server_flag;
extern std::atomic<uint64_t> config_reload_generation;

// Builds a hardened server SSL_CTX: minimum TLS 1.2, an AEAD-only cipher
// whitelist with forward secrecy, session resumption (cache + tickets).
// Returns nullptr and fills *error on any failure.
SSL_CTX* build_hardened_ssl_ctx(const std::string& cert_path, const std::string& key_path,
                                std::string* error);

class TcpWorker {
private:
  Socket listen_sock_;
  DynamicThreadPool* pool_;
  std::atomic<bool> closed_;
  Epoll epoll_;

  enum class SSLState { HANDSHAKING, READY };
  struct ConnInfo {
    std::shared_ptr<Socket> sock;
    SSLState ssl_state;
  };
  std::unordered_map<int, ConnInfo> conns_;

  UpstreamManager upstream_manager_;
  ApiKeyManager api_key_manager_;
  std::shared_ptr<RateLimiterManager> rate_limiter_manager_;   // Shared across workers

  HttpHandler handler_;
  std::unordered_map<int, time_t> last_active_; // Last active time per connection (second granularity)
  int keepalive_timeout_; // Keep-alive timeout (seconds)
  time_t last_timeout_check_ = 0; // Time of the last timeout check
  time_t last_limiter_cleanup_ = 0; // Time of the last limiter cleanup
  std::unordered_map<int, std::string> client_ips_; // Client IP per connection for logging
  std::string config_path_;
  uint64_t applied_reload_generation_ = 0;

  void update_active(int fd);
  void check_timeout();

  SSL_CTX* ssl_ctx_ = nullptr;
public:
  TcpWorker(Socket&& listen_sock, DynamicThreadPool* pool, const Config& config, std::shared_ptr<RateLimiterManager> rate_limiter_manager, const std::string& config_path);

  void run();
  void handle_accept();
  void handle_client(int fd, uint32_t events);
  void close();
};