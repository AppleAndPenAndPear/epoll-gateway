#include "admin_server.h"
#include "metrics.h"
#include "mylogger.h"
#include "gateway_version.h"

#include <arpa/inet.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

using json = nlohmann::json;

// Parsed request head; at global scope to match the forward declaration in
// admin_server.h (authorize takes a reference to it).
struct ParsedAdminRequest {
    std::string method;
    std::string path;
    std::unordered_map<std::string, std::string> headers;  // lowercased names
    size_t body_consumed = 0;        // body bytes already read while hunting for the head terminator
    long long content_length = -1;   // -1 when absent or not a valid non-negative integer
    bool chunked = false;            // Transfer-Encoding: chunked (size unknown up front)
    bool ok = false;
};

namespace {

constexpr size_t MAX_REQUEST_BYTES = 8192;
constexpr int RECV_TIMEOUT_SECONDS = 5;
constexpr size_t MAX_DRAIN_BYTES = 65536;

// Discard the rest of the request body before the socket is closed. We
// deliberately never parse a body (no admin endpoint takes one), but closing a
// socket that still has unread received data makes the kernel send RST instead
// of FIN — and an RST can destroy a response the client has not read yet. So
// drain what the caller's Content-Length promises, bounded, before returning.
void drain_request_body(int fd, const ParsedAdminRequest& req) {
    size_t remaining = 0;
    if (req.content_length > 0) {
        const size_t total = static_cast<size_t>(req.content_length);
        remaining = total > req.body_consumed ? total - req.body_consumed : 0;
    } else if (req.chunked) {
        remaining = MAX_DRAIN_BYTES;   // Unknown length: drain what has arrived
    }
    if (remaining == 0) return;
    if (remaining > MAX_DRAIN_BYTES) remaining = MAX_DRAIN_BYTES;

    // Shorten the timeout for this phase: the accept loop is serial, so a
    // client that stalls mid-body must not hold the admin listener for 5s.
    timeval tv{};
    tv.tv_sec = 1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[1024];
    size_t drained = 0;
    while (drained < remaining) {
        const size_t want = std::min(sizeof(buf), remaining - drained);
        const ssize_t n = ::recv(fd, buf, want, 0);
        if (n <= 0) break;   // EOF, error or timeout
        drained += static_cast<size_t>(n);
    }
}

// Read one blocking-with-timeout HTTP request (headers only; admin endpoints
// carry no body). Returns false on IO failure or an oversized/malformed head.
bool read_request_head(int fd, ParsedAdminRequest& out) {
    timeval tv{};
    tv.tv_sec = RECV_TIMEOUT_SECONDS;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    std::string head;
    char buf[1024];
    while (head.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        head.append(buf, static_cast<size_t>(n));
        if (head.size() > MAX_REQUEST_BYTES) return false;
    }

    // Request line: METHOD SP PATH SP HTTP/1.x
    const size_t line_end = head.find("\r\n");
    const std::string request_line = head.substr(0, line_end);
    const size_t sp1 = request_line.find(' ');
    const size_t sp2 = request_line.rfind(' ');
    if (sp1 == std::string::npos || sp2 == std::string::npos || sp2 <= sp1) return false;
    out.method = request_line.substr(0, sp1);
    out.path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    if (out.path.empty() || out.path[0] != '/') return false;

    // Headers (name lowercased; header values trimmed like the main parser)
    size_t pos = line_end + 2;
    while (pos < head.size()) {
        const size_t eol = head.find("\r\n", pos);
        if (eol == std::string::npos || eol == pos) break;
        const std::string line = head.substr(pos, eol - pos);
        pos = eol + 2;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        const auto trim = [](std::string& s) {
            const char* ws = " \t";
            size_t b = s.find_first_not_of(ws);
            size_t e = s.find_last_not_of(ws);
            s = b == std::string::npos ? "" : s.substr(b, e - b + 1);
        };
        for (auto& c : name) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        trim(value);
        out.headers[name] = value;
    }

    // Record how much of a body we already swallowed while scanning for the
    // head terminator, plus how much more the caller promised — drain_request_body
    // needs both to know what is still unread in the kernel buffer.
    const size_t head_end = head.find("\r\n\r\n") + 4;
    if (head.size() > head_end) out.body_consumed = head.size() - head_end;
    if (const auto cl = out.headers.find("content-length"); cl != out.headers.end()) {
        try {
            const long long n = std::stoll(cl->second);
            if (n >= 0) out.content_length = n;
        } catch (...) {
            // Malformed Content-Length: no exact drain target; best effort only
        }
    }
    if (const auto te = out.headers.find("transfer-encoding"); te != out.headers.end() &&
        te->second.find("chunked") != std::string::npos) {
        out.chunked = true;
    }
    out.ok = true;
    return true;
}

// Length-independent comparison so response timing does not leak key content.
bool constant_time_equals(const std::string& a, const std::string& b) {
    volatile unsigned char diff = static_cast<unsigned char>(a.size() ^ b.size());
    const size_t len = std::max(a.size(), b.size());
    for (size_t i = 0; i < len; ++i) {
        const unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<unsigned char>(ca ^ cb);
    }
    return diff == 0;
}

bool send_all(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

void send_response(int fd, int status, const std::string& status_text,
                   const std::string& body, const char* content_type = "application/json") {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << " " << status_text << "\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    send_all(fd, oss.str());
}

std::string peer_to_string(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

} // namespace

AdminServer::AdminServer(const Config& config, std::shared_ptr<UpstreamManager> upstream_manager,
                         const std::string& config_path)
    : config_(config), upstream_manager_(std::move(upstream_manager)), config_path_(config_path) {}

AdminServer::~AdminServer() {
    if (thread_.joinable()) thread_.join();
    // listen_sock_ closes its fd via RAII
}

// Note: I/O here deliberately uses ::recv/::send on raw fds instead of the
// Socket wrappers. Socket::recv/send/accept throw on ECONNRESET, ECONNABORTED,
// EMFILE and friends, but in this blocking-with-timeout model those errnos are
// routine per-connection noise that must never escape run() (an uncaught
// exception in a thread calls std::terminate and would kill the whole
// process). Socket is used purely as the RAII fd owner.
void AdminServer::start() {
    listen_sock_ = Socket(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (listen_sock_.getFd() < 0) throw std::runtime_error("admin: socket() failed");
    int one = 1;
    setsockopt(listen_sock_.getFd(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.admin.port);
    if (inet_pton(AF_INET, config_.admin.bind.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("admin: invalid bind address '" + config_.admin.bind + "'");
    }
    if (::bind(listen_sock_.getFd(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        throw std::runtime_error("admin: bind failed on " + config_.admin.bind + ":" +
                                 std::to_string(config_.admin.port));
    }
    if (::listen(listen_sock_.getFd(), 64) != 0) {
        throw std::runtime_error("admin: listen failed");
    }

    thread_ = std::thread(&AdminServer::run, this);
    Logger::get()->info("Admin API listening on {}:{} (endpoints: /admin/stats, /admin/upstreams, /admin/reload)",
                        config_.admin.bind, config_.admin.port);
}

void AdminServer::wait() {
    if (thread_.joinable()) thread_.join();
}

void AdminServer::run() {
    while (!stop_server_flag.load(std::memory_order_relaxed)) {
        // Nothing may escape this loop: an uncaught exception in a thread calls
        // std::terminate and would take the whole gateway down with it. The
        // per-request code is written to not throw (raw ::recv/::send below),
        // but the refresh path reads and parses a file, and JSON/string growth
        // can still fail on allocation — so the guarantee is enforced here.
        try {
            maybe_refresh_keys();

            pollfd pfd{listen_sock_.getFd(), POLLIN, 0};
            if (::poll(&pfd, 1, 1000) <= 0) continue;   // Timeout or EINTR: re-check the stop flag

            sockaddr_in peer{};
            socklen_t peer_len = sizeof(peer);
            const int fd = ::accept(listen_sock_.getFd(), reinterpret_cast<sockaddr*>(&peer), &peer_len);
            if (fd < 0) continue;

            const Socket client(fd);   // RAII: closed on any exit path from this scope
            const std::string peer_str = peer_to_string(peer);
            handle_connection(client.getFd(), peer_str);
        } catch (const std::exception& e) {
            Logger::get()->error("AUDIT admin_request_failed error={}", e.what());
        } catch (...) {
            Logger::get()->error("AUDIT admin_request_failed error=unknown");
        }
    }
}

bool AdminServer::authorize(const ParsedAdminRequest& req, std::string* err) {
    std::string key;
    if (const auto it = req.headers.find("x-api-key"); it != req.headers.end()) {
        key = it->second;
    } else if (const auto auth = req.headers.find("authorization"); auth != req.headers.end() &&
               auth->second.rfind("Bearer ", 0) == 0) {
        key = auth->second.substr(7);
    }
    if (key.empty()) {
        *err = "missing api key";
        return false;
    }
    for (const std::string& allowed : config_.admin.api_keys) {
        if (constant_time_equals(key, allowed)) return true;
    }
    *err = "invalid api key";
    return false;
}

void AdminServer::handle_connection(int fd, const std::string& peer) {
    ParsedAdminRequest req;
    int status = 400;
    const char* status_text = "Bad Request";
    std::string body = R"({"error":"malformed request"})";

    if (read_request_head(fd, req) && req.ok) {
        std::string auth_err;
        if (!authorize(req, &auth_err)) {
            Logger::get()->warn("AUDIT admin_auth_failed peer={} method={} path={} reason={}",
                                peer, req.method, req.path, auth_err);
            status = 401;
            status_text = "Unauthorized";
            body = R"({"error":"unauthorized"})";
        } else if (req.path == "/admin/stats" && req.method == "GET") {
            status = 200;
            status_text = "OK";
            body = handle_stats();
        } else if (req.path == "/admin/upstreams" && req.method == "GET") {
            status = 200;
            status_text = "OK";
            body = handle_upstreams();
        } else if (req.path == "/admin/reload" && req.method == "POST") {
            auto [reload_status, reload_body] = handle_reload();
            status = reload_status;
            status_text = reload_status == 200 ? "OK" : "Bad Request";
            body = std::move(reload_body);
        } else {
            status = 404;
            status_text = "Not Found";
            body = R"({"error":"unknown admin endpoint"})";
        }
    }

    send_response(fd, status, status_text, body);
    // Must happen after the response, before the socket closes (see the note
    // on drain_request_body): a client that sent a body would otherwise get an
    // RST that can discard the response we just wrote.
    drain_request_body(fd, req);
}

std::string AdminServer::handle_stats() {
    const auto snap = Metrics::instance().snapshot();
    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - start_time_).count();
    json j;
    j["version"] = GATEWAY_VERSION;
    j["uptime_seconds"] = uptime;
    j["requests"] = {
        {"total", snap.total_requests},
        {"2xx", snap.requests_2xx},
        {"3xx", snap.requests_3xx},
        {"4xx", snap.requests_4xx},
        {"5xx", snap.requests_5xx},
    };
    j["latency"] = {
        {"sum_seconds", snap.duration_sum},
        {"count", snap.duration_count},
    };
    return j.dump() + "\n";
}

std::string AdminServer::handle_upstreams() {
    json upstreams = json::array();
    for (const auto& up : upstream_manager_->status_snapshot()) {
        json backends = json::array();
        for (const auto& b : up.backends) {
            backends.push_back({
                {"host", b.host},
                {"port", b.port},
                {"healthy", b.healthy},
                {"consecutive_failures", b.consecutive_failures},
                {"circuit_open", b.circuit_open},
            });
        }
        upstreams.push_back({{"name", up.name}, {"backends", backends}});
    }
    json j;
    j["upstreams"] = upstreams;
    return j.dump() + "\n";
}

std::pair<int, std::string> AdminServer::handle_reload() {
    std::vector<std::string> errors;
    Config reloaded;
    if (!Config::is_valid_file(config_path_)) {
        errors.push_back("config file missing or not valid JSON");
    } else {
        reloaded = Config::from_file(config_path_, &errors);
        for (const std::string& e : Config::validate(reloaded)) {
            errors.push_back(e);
        }
    }
    if (!errors.empty()) {
        std::string joined;
        for (const std::string& e : errors) {
            if (!joined.empty()) joined += "; ";
            joined += e;
        }
        Logger::get()->warn("AUDIT admin_reload_rejected errors=[{}]", joined);
        json j;
        j["status"] = "rejected";
        j["errors"] = errors;
        return {400, j.dump() + "\n"};
    }
    // Hand the validated admin section to the refresh step so it adopts exactly
    // the revision that was validated, instead of re-reading (and possibly
    // picking up a newer revision of) config.json.
    pending_admin_keys_ = PendingAdminKeys{reloaded.admin.enabled, reloaded.admin.api_keys};
    // Valid config: bump the reload generation; workers validate-and-apply it
    // at their next safe checkpoint (same path as SIGHUP, within ~1s).
    config_reload_generation.fetch_add(1, std::memory_order_relaxed);
    Logger::get()->info("AUDIT admin_reload_triggered via admin api");
    json j;
    j["status"] = "reload_triggered";
    return {200, j.dump() + "\n"};
}

// Adopt a validated admin key set. `enabled` distinguishes the two reasons the
// list can be empty: admin switched off (a legitimate new state — keep serving
// with the previous keys, since the listener cannot be stopped at runtime) from
// an enabled-but-empty list (a config error we never adopt).
void AdminServer::apply_admin_keys(const std::vector<std::string>& keys, bool enabled,
                                   bool from_admin_api) {
    const char* source = from_admin_api ? "admin api" : "sighup";
    if (!enabled) {
        Logger::get()->info("AUDIT admin_keys_unchanged reason=admin_disabled source={} "
                            "(listener keeps its current keys until restart)", source);
        return;
    }
    if (keys.empty()) {
        // validate() rejects this combination, so it should be unreachable —
        // guarding anyway because adopting it would lock the listener out.
        Logger::get()->warn("AUDIT admin_keys_refresh_failed reason=empty_key_list source={} "
                            "errors=[keeping previous keys]", source);
        return;
    }
    config_.admin.api_keys = keys;
    Logger::get()->info("AUDIT admin_keys_rotated count={} source={}", keys.size(), source);
}

// Hot-reload admin.api_keys: whenever the reload generation changes (SIGHUP or
// POST /admin/reload), adopt the new key list so rotation needs no restart.
// Listener topology (enabled/port/bind) intentionally stays fixed until
// restart — only the keys are runtime-refreshable. A failed refresh always
// keeps the previous keys, so the listener can never lock itself out.
// No locking needed: this runs on the accept-loop thread, same as authorize().
void AdminServer::maybe_refresh_keys() {
    const uint64_t gen = config_reload_generation.load(std::memory_order_relaxed);
    if (gen == applied_generation_) return;
    applied_generation_ = gen;   // Mark seen regardless of outcome; failures are logged once

    // Preferred path: the admin API already parsed and validated this config.
    if (pending_admin_keys_) {
        PendingAdminKeys pending = std::move(*pending_admin_keys_);
        pending_admin_keys_.reset();
        apply_admin_keys(pending.keys, pending.enabled, /*from_admin_api=*/true);
        return;
    }

    // SIGHUP path: no parsed config in hand, so read the file ourselves.
    std::vector<std::string> errors;
    if (!Config::is_valid_file(config_path_)) {
        errors.push_back("config file missing or not valid JSON");
    } else {
        Config reloaded = Config::from_file(config_path_, &errors);
        for (const std::string& e : Config::validate(reloaded)) {
            errors.push_back(e);
        }
        if (errors.empty()) {
            apply_admin_keys(reloaded.admin.api_keys, reloaded.admin.enabled, /*from_admin_api=*/false);
            return;
        }
    }
    std::string joined;
    for (const std::string& e : errors) {
        if (!joined.empty()) joined += "; ";
        joined += e;
    }
    Logger::get()->warn("AUDIT admin_keys_refresh_failed source=sighup errors=[{}]", joined);
}
