#include "tcpworker.h"
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <fstream>


TcpWorker::TcpWorker(Socket&& listen_sock, DynamicThreadPool* pool, const Config& config,
                     std::shared_ptr<UpstreamManager> upstream_manager,
                     std::shared_ptr<RateLimiterManager> rate_limiter_manager, const std::string& config_path):
  listen_sock_(std::move(listen_sock)),
  pool_(pool),
  closed_(false),
  epoll_(),
  upstream_manager_(std::move(upstream_manager)),
  api_key_manager_(config.api_keys),
  rate_limiter_manager_(std::move(rate_limiter_manager)),
  handler_(epoll_, config, *upstream_manager_, api_key_manager_, rate_limiter_manager_),
  keepalive_timeout_(config.keepalive_timeout),
  shutdown_drain_timeout_(config.shutdown_drain_timeout),
  config_path_(config_path),
  applied_reload_generation_(config_reload_generation.load(std::memory_order_relaxed)) {
  epoll_.add(listen_sock_.getFd(), EPOLLIN);

  SSL_library_init();
  OpenSSL_add_all_algorithms();
  std::string ssl_error;
  ssl_ctx_ = build_hardened_ssl_ctx(config.tls.cert_path, config.tls.key_path, &ssl_error);
  if (!ssl_ctx_) {
    // Fail fast: a gateway that cannot serve TLS should not come up at all.
    throw std::runtime_error("TLS setup failed (" + config.tls.cert_path + ", " +
                             config.tls.key_path + "): " + ssl_error);
  }

  Logger::get()->info("TcpWorker created with listen fd {} by move", listen_sock_.getFd());
}

void TcpWorker::update_active(int fd) {
    last_active_[fd] = time(nullptr);
}

void TcpWorker::check_timeout() {
  time_t now = time(nullptr);
  const uint64_t reload_generation = config_reload_generation.load(std::memory_order_relaxed);
  if (reload_generation != applied_reload_generation_) {
    std::vector<std::string> reload_errors;
    Config reloaded;
    if (Config::is_valid_file(config_path_)) {
      reloaded = Config::from_file(config_path_, &reload_errors);
    } else {
      reload_errors.push_back("file missing or not valid JSON");
    }
    for (const std::string& e : Config::validate(reloaded)) {
      reload_errors.push_back(e);
    }
    if (!reload_errors.empty()) {
      std::string joined;
      for (const std::string& e : reload_errors) {
        if (!joined.empty()) joined += "; ";
        joined += e;
      }
      // AUDIT: rejected reload, keep serving with the old config
      Logger::get()->warn("AUDIT config_reload_rejected path={} errors=[{}]", config_path_, joined);
    } else {
      handler_.reload_config(reloaded);
      keepalive_timeout_ = reloaded.keepalive_timeout;

      // Hot-reload the certificate/private key from the (possibly new) paths.
      // SSL objects hold their own reference to the old context, so freeing it
      // here only drops our reference; in-flight connections keep it alive.
      std::string tls_error;
      SSL_CTX* new_ctx = build_hardened_ssl_ctx(reloaded.tls.cert_path, reloaded.tls.key_path, &tls_error);
      if (new_ctx) {
        SSL_CTX* old_ctx = ssl_ctx_;
        ssl_ctx_ = new_ctx;
        SSL_CTX_free(old_ctx);
        Logger::get()->info("AUDIT tls_reload_applied cert={} key={}",
                            reloaded.tls.cert_path, reloaded.tls.key_path);
      } else {
        Logger::get()->warn("AUDIT tls_reload_rejected cert={} key={} error={}",
                            reloaded.tls.cert_path, reloaded.tls.key_path, tls_error);
      }

      applied_reload_generation_ = reload_generation;
      Logger::get()->info("AUDIT config_reload_applied path={}", config_path_);
    }
  }
    for (auto it = last_active_.begin(); it != last_active_.end(); ) {
        int fd = it->first;
        // A live connection always has entries in both conns_ and last_active_
        // (accept inserts both, every cleanup path erases both). Membership in
        // conns_ is the source of truth; probing with fcntl(fd) would be unsafe
        // because the fd number may already have been reused by a new connection.
        if (conns_.find(fd) == conns_.end()) {
            Logger::get()->debug("check_timeout: remove stale last_active_ entry fd {}", fd);
            it = last_active_.erase(it);
            continue;
        }
        if (now - it->second > keepalive_timeout_) {
            // Timed out, close the connection
            Logger::get()->info("Idle timeout on fd {} (last active {}s ago), closing ...",fd, now - it->second);
            auto conn_it = conns_.find(fd);
            if (conn_it != conns_.end()) {
                handler_.cleanup(conn_it->second.sock);
                conns_.erase(conn_it);
            }
            it = last_active_.erase(it);   // Remove the timer record
        } else {
            ++it;
        }
    }
    
    upstream_manager_->check_health();  // Active health check every second

    // Periodically clean up long-unused rate limiters to prevent unbounded memory growth (shared across workers, protected by a mutex)
    if (now - last_limiter_cleanup_ >= 60) {
        rate_limiter_manager_->cleanup();
        last_limiter_cleanup_ = now;
    }
}

void TcpWorker::run(){
  const int MAX_EVENTS = 1024;
  epoll_event evs[MAX_EVENTS];

  while (!closed_ && !stop_server_flag.load()) {
    auto wait_result = epoll_.wait(evs, MAX_EVENTS, 1000);  // 1s timeout, configurable
    if (!wait_result) {
      // No ready events (timeout or interrupted)
      if (wait_result.interrupted) {
        continue;
      }
      if (wait_result.timeout) {
          if (stop_server_flag.load()) break;
          check_timeout();   // Periodic cleanup of expired connections
          continue;
      }
      continue;
    }
    for (int i = 0; i < wait_result.event_count; ++i) {
      int fd = evs[i].data.fd;
      if (fd == listen_sock_.getFd()) {
        handle_accept();
      } else {
        // update_active is already called in handle_client(); avoid duplicating it here
        handle_client(fd, evs[i].events);
      }
    }
    // Even when events keep arriving, check timeouts periodically (under high load epoll_wait may never time out)
    time_t now = time(nullptr);
    if (now - last_timeout_check_ >= 1) {
      check_timeout();
      last_timeout_check_ = now;
    }
  }

  // ── Graceful shutdown (P4): the stop flag is set, so stop accepting new
  // connections first, then keep serving in-flight requests until they finish
  // or the drain budget expires. Idle keep-alive connections are closed
  // immediately so clients reconnect to another instance. ──
  drain_connections();

  Logger::get()->info("TcpWorker on fd {} exiting", listen_sock_.getFd());
}

void TcpWorker::drain_connections() {
  // Phase 1: stop accepting. Removing the listen fd from epoll prevents any
  // further accept events; already-accepted sockets stay in the kernel
  // backlog and will be picked up by another instance (SO_REUSEPORT).
  epoll_.del(listen_sock_.getFd());
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(shutdown_drain_timeout_);
  Logger::get()->info("Draining {} connection(s), budget {}s", conns_.size(),
                      shutdown_drain_timeout_);

  const int MAX_EVENTS = 64;
  epoll_event evs[MAX_EVENTS];
  while (!conns_.empty()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      Logger::get()->warn("Drain budget of {}s expired with {} connection(s) still open; closing them",
                          shutdown_drain_timeout_, conns_.size());
      break;
    }
    // Keep processing events so in-flight requests can complete.
    auto wait_result = epoll_.wait(evs, MAX_EVENTS, 100);
    if (wait_result) {
      for (int i = 0; i < wait_result.event_count; ++i) {
        const int fd = evs[i].data.fd;
        if (fd != listen_sock_.getFd()) {
          handle_client(fd, evs[i].events);
        }
      }
    }
    // Phase 2: close connections that are between requests (idle keep-alive)
    // or stuck in handshake; keep those with an in-flight request/response.
    for (auto it = conns_.begin(); it != conns_.end();) {
      const int fd = it->first;
      const bool handshaking = it->second.ssl_state == SSLState::HANDSHAKING;
      if (handshaking || !handler_.has_inflight_work(it->second.sock.get())) {
        handler_.cleanup(it->second.sock);
        it = conns_.erase(it);
        last_active_.erase(fd);
        client_ips_.erase(fd);
      } else {
        ++it;
      }
    }
  }

  // Force-close whatever is left (drain expired or the last events finished).
  for (auto& [fd, conn] : conns_) {
    handler_.cleanup(conn.sock);
  }
  conns_.clear();
  last_active_.clear();
}

void TcpWorker::handle_accept() {
  while (true) {
    sockaddr_in client;
    socklen_t len = sizeof(client);
    auto client_opt = listen_sock_.accept((sockaddr*)&client,&len);
    if (!client_opt) {
      break;
    }
    int clientsock = *client_opt; // Safe dereference

    // Get the client IP
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client.sin_addr, ip_str, sizeof(ip_str));
    client_ips_[clientsock] = ip_str;   // Store it

    last_active_[clientsock] = time(nullptr);  // Record the activity time
    auto client_sock = make_shared<Socket>(clientsock);
    client_sock->setnonblocking();   // Set non-blocking
    client_sock->setcloexec();
    // Disable Nagle: TLS responses leave the socket in several small writes
    // (handshake records, header/body records), and with Nagle enabled each
    // stall on an unacked segment costs the peer's 40ms delayed-ACK timer.
    client_sock->setnodelay();

    // Set up SSL
    if (!client_sock->initSSL(ssl_ctx_)) {
      Logger::get()->error("SSL init failed for fd {}", clientsock);
      continue;
    }

    epoll_.add(clientsock, EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET | EPOLLONESHOT);
    conns_[clientsock] = {client_sock, SSLState::HANDSHAKING};
    handler_.on_connect(client_sock.get());   // Initialize the queues
    Logger::get()->info("Worker accepted client fd {}", clientsock);
  }
}

void TcpWorker::handle_client(int fd, uint32_t events) {
  auto it = conns_.find(fd);
  if (it == conns_.end()) return;
  auto& conn = it->second;

  if (conn.ssl_state == SSLState::HANDSHAKING) {
    if (conn.sock->sslAccept()) {
      conn.ssl_state = SSLState::READY;
      epoll_.mod(fd, EPOLLIN | EPOLLET | EPOLLONESHOT);  // Only watch reads
      Logger::get()->info("SSL handshake done on fd {}", fd);
    } else {
      // Handshake not finished, re-register the events (include EPOLLOUT, since the SSL handshake may need to write)
      // EPOLLONESHOT requires re-registration after every event
      epoll_.mod(fd, EPOLLIN | EPOLLOUT | EPOLLET | EPOLLONESHOT);
    }
    return;
  }

  // 1. Refresh the activity time whenever an event arrives (before handling it)
  update_active(fd);

  if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
    // Error or hangup event, clean up directly
    Logger::get()->info("Worker: EPOLLERR/EPOLLHUP on fd {}", fd);
    handler_.cleanup(conn.sock);   // Clean up the EchoHandler internal state and epoll
    conns_.erase(fd);         // Remove it from our own connection table
    // The sock shared_ptr refcount drops, and the Socket is finally destructed automatically
    last_active_.erase(fd);          // ★ Explicitly erase
    client_ips_.erase(fd); // Remove the client IP record
    return;
  }

  if (events & EPOLLIN) {
    handler_.handle_read(conn.sock, client_ips_[fd]);  // Pass in the client IP
  } else if (events & EPOLLOUT) {
    handler_.handle_write(conn.sock);
  } else {
    Logger::get()->warn("Unexpected event on fd {}: {}", fd, events);
    handler_.cleanup(conn.sock);
    conns_.erase(fd);
    last_active_.erase(fd);          // ★ Explicitly erase
    client_ips_.erase(fd); // Remove the client IP record
    return;
  }

  // 3. handle_read/handle_write may internally call cleanup due to errors or a peer close;
  //    conns_ no longer holds the fd then, so the timer record must be removed as well.
  if (conns_.find(fd) == conns_.end()) {
    last_active_.erase(fd);          // ★ Erase the stale record
    Logger::get()->debug("Connection on fd {} closed during event handling", fd);
  }
}

void TcpWorker::close() {
  closed_ = true;
  // Optionally close listen_fd_ to wake up epoll_wait (otherwise it may block forever)
  ::shutdown(listen_sock_.getFd(), SHUT_RD);
}