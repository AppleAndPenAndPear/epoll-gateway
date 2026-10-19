#include "http_client.h"
#include "connection_pool.h"
#include "client_tls.h"
#include "mysocket.h"
#include "mylogger.h"
#include "poller.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace {

// Per-upstream SSL_CTX cache, keyed by the verification policy (not by
// host): building a context loads the CA material and pays ECDH setup, so
// it must not happen per request. Contexts live until process exit, like
// global_connection_pool().
SSL_CTX* get_upstream_ssl_ctx(const UpstreamTlsOptions& tls) {
    static std::mutex mu;
    static std::unordered_map<std::string, SSL_CTX*> cache;
    const std::string key =
        std::string(tls.skip_verify ? "1|" : "0|") + tls.ca_file + "|" + tls.server_name;
    std::lock_guard<std::mutex> lock(mu);
    const auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    ClientTlsConfig cfg;
    cfg.enable_tls = true;
    cfg.insecure = tls.skip_verify;
    cfg.ca_path = tls.ca_file;      // empty → system default trust store
    cfg.verify_host = tls.server_name;  // empty → chain-only verification
    std::string error;
    SSL_CTX* ctx = build_client_ssl_ctx(cfg, &error);
    if (!ctx) {
        Logger::get()->error("upstream TLS: {}", error);
        return nullptr;
    }
    cache[key] = ctx;
    return ctx;
}

BackendResponse make_error_response(int status_code, BackendError error, const std::string& body) {
    BackendResponse response;
    response.status_code = status_code;
    response.body = body;
    response.error = error;
    return response;
}

bool is_transient_backend_error(BackendError error) {
    switch (error) {
        case BackendError::ConnectFailed:
        case BackendError::ConnectTimeout:
        case BackendError::WriteFailed:
        case BackendError::WriteTimeout:
        case BackendError::ReadFailed:
        case BackendError::ReadTimeout:
            return true;
        default:
            return false;
    }
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// RFC 7230 §6.1 hop-by-hop headers must never be forwarded on the upstream
// hop. This also covers the framing headers the gateway re-emits itself
// (Host, Content-Length) and Proxy-* credentials, which must not leak to a
// backend. `lower_name` is expected to be lowercased already.
bool is_hop_by_hop_header(const std::string& lower_name) {
    static const std::unordered_set<std::string> kHopByHop = {
        "connection", "keep-alive", "proxy-authenticate", "proxy-authorization",
        "proxy-connection", "te", "trailer", "transfer-encoding", "upgrade",
        "host", "content-length"};
    return kHopByHop.count(lower_name) != 0;
}

// Tokens named in the client's Connection header are themselves hop-by-hop
// (RFC 7230 §6.1) and must be dropped alongside it.
std::unordered_set<std::string> connection_tokens(
        const std::unordered_map<std::string, std::string>& headers) {
    std::unordered_set<std::string> tokens;
    const auto it = headers.find("connection");
    if (it == headers.end()) return tokens;
    // Split on commas by hand, not via istringstream+getline: the stream
    // machinery (locale/istream construction + per-token getline) profiled hot
    // on the proxy request path. Mirrors response_connection_tokens() in
    // http_handler.cpp so the two sides cannot drift.
    const std::string& value = it->second;
    size_t pos = 0;
    while (pos <= value.size()) {
        size_t comma = value.find(',', pos);
        if (comma == std::string::npos) comma = value.size();
        size_t begin = pos;
        size_t end = comma;
        while (begin < end && (value[begin] == ' ' || value[begin] == '\t')) ++begin;
        while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t')) --end;
        if (begin < end) {
            std::string token = value.substr(begin, end - begin);
            for (char& c : token) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            tokens.insert(std::move(token));
        }
        pos = comma + 1;
    }
    return tokens;
}

// Case-insensitive header lookup within the raw header block of a response.
//find value of header
std::string find_header_value(const std::string& headers_part, const std::string& name) {
    const size_t nlen = name.size();
    size_t line_start = headers_part.find("\r\n") + 2;  // skip the status line which includes method,path and version
    while (line_start < headers_part.size()) {
        const size_t line_end = headers_part.find("\r\n", line_start);
        const size_t field_end = (line_end == std::string::npos ? headers_part.size() : line_end);
        // Field name is compared in place, case-insensitively, without
        // materializing a lowercase substring of every header: the per-header
        // substring + to_lower allocation profiled hot on the proxy path.
        const size_t colon = headers_part.find(':', line_start);
        if (colon != std::string::npos && colon < field_end && colon - line_start == nlen) {
            bool match = true;
            for (size_t i = 0; i < nlen; ++i) {
                const char c = static_cast<char>(std::tolower(
                    static_cast<unsigned char>(headers_part[line_start + i])));
                if (c != name[i]) { match = false; break; }
            }
            if (match) {
                std::string value = headers_part.substr(colon + 1, field_end - colon - 1);
                const size_t begin = value.find_first_not_of(" \t");
                if (begin == std::string::npos) return "";
                const size_t end = value.find_last_not_of(" \t");
                value.erase(end + 1);
                value.erase(0, begin);
                for (char& c : value) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                return value;
            }
        }
        if (line_end == std::string::npos) break;
        line_start = line_end + 2;
    }
    return "";
}

enum class ChunkScan { Incomplete, Complete, Error };

// Scans a chunked body starting at body_start. On Complete, body_end points
// just past the terminating CRLF of the last-chunk trailer. When `decoded` is
// given, the real (de-chunked) payload bytes are appended to it as chunks are
// recognized; trailer fields are dropped.
ChunkScan scan_chunked_body(const std::string& buf, size_t body_start, size_t& body_end,
                            std::string* decoded = nullptr) {
    size_t pos = body_start;
    for (;;) {
        const size_t line_end = buf.find("\r\n", pos);
        if (line_end == std::string::npos) return ChunkScan::Incomplete;
        std::string hex = buf.substr(pos, line_end - pos);
        const size_t semi = hex.find(';');
        if (semi != std::string::npos) hex.erase(semi);
        if (hex.empty()) return ChunkScan::Error;
        size_t size = 0;
        for (char c : hex) {
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return ChunkScan::Error;
            if (size > (SIZE_MAX >> 4)) return ChunkScan::Error;
            size = size * 16 + static_cast<size_t>(d);
        }
        pos = line_end + 2;
        if (size == 0) {
            // Trailer section: consume lines until the empty line (possibly none).
            size_t t = pos;
            for (;;) {
                const size_t e = buf.find("\r\n", t);
                if (e == std::string::npos) return ChunkScan::Incomplete;
                if (e == t) {           // empty line ends the trailer section
                    body_end = e + 2;
                    return ChunkScan::Complete;
                }
                t = e + 2;
            }
        }
        if (buf.size() < pos + size + 2) return ChunkScan::Incomplete;
        if (buf.compare(pos + size, 2, "\r\n") != 0) return ChunkScan::Error;
        if (decoded) decoded->append(buf, pos, size);
        pos += size + 2;
    }
}

}  // namespace

bool should_retry_backend_request(const std::string& method,
                                 BackendError error,
                                 int attempt_count) {
    if (attempt_count >= 2) {
        return false;
    }

    const bool idempotent = method == "GET" || method == "HEAD" || method == "OPTIONS";
    return idempotent && is_transient_backend_error(error);
}

// Sends one request over the given socket with the timeout, exactly as before.
namespace {
enum class SendResult { Ok, Timeout, Failed };

SendResult send_all(Socket& sock, bool is_ssl, const std::string& request, int timeout_ms) {
    size_t sent = 0;
    while (sent < request.size()) {
        // TLS can ask for a read mid-write (protocol housekeeping), so the
        // poll direction follows last_ssl_want(); plaintext always wants write.
        const Poller::Event event = (is_ssl && sock.last_ssl_want() == SSLWant::READ)
                                        ? Poller::Event::Read : Poller::Event::Write;
        const auto wait_result = Poller::wait(sock.getFd(), event, timeout_ms);
        if (wait_result == Poller::WaitResult::Timeout) return SendResult::Timeout;
        if (wait_result != Poller::WaitResult::Ready) return SendResult::Failed;
        const ssize_t written = is_ssl
            ? sock.sslWrite(const_cast<char*>(request.data() + sent), request.size() - sent)
            : sock.send(request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) continue;  // WANT_READ/WANT_WRITE; re-poll with the right direction
            return SendResult::Failed;
        }
        if (written == 0) return SendResult::Failed;
        sent += static_cast<size_t>(written);
    }
    return SendResult::Ok;
}
}  // namespace

BackendResponse forward_request(const std::string& host, int port,
                                const std::string& method,
                                const std::string& path,
                                const std::unordered_map<std::string, std::string>& req_headers,
                                const std::string& req_body,
                                int timeout_ms,
                                size_t max_response_bytes,
                                const UpstreamTlsOptions& tls) {
    if (timeout_ms <= 0) {
        return make_error_response(504, BackendError::ConnectTimeout, "Gateway Timeout");
    }

    ConnectionPool& pool = global_connection_pool();
    const bool idempotent = method == "GET" || method == "HEAD" || method == "OPTIONS";
    // Pool identity includes the verification policy: a session verified for
    // one server name is only reusable by upstreams with the same policy.
    // The CA file is part of the identity too (matching the SSL_CTX cache key in
    // get_upstream_ssl_ctx): two upstreams sharing host:port and server name but
    // trusting different CAs must not hand each other a session verified against
    // the other's trust store.
    const std::string pool_scheme = !tls.enable ? ""
        : tls.skip_verify ? "tls/insecure|" : "tls/" + tls.ca_file + "|" + tls.server_name + "|";

    // Two rounds: first try a pooled keep-alive connection (it may have been
    // closed by the backend while idle), then a brand-new connection. Failures
    // that happen after the request was sent are retried only for idempotent
    // methods — for POST/PUT/DELETE the backend may already have acted on the
    // first attempt, and blindly resending could duplicate the side effect.
    for (int attempt = 0; attempt < 2; ++attempt) {
        BackendResponse resp = make_error_response(502, BackendError::ConnectFailed, "Bad Gateway");
        std::optional<ConnectionPool::PooledConnection> pooled;
        std::optional<Socket> fresh;
        Socket* upstream_socket = nullptr;
        bool used_pooled = false;

        if (attempt == 0) pooled = pool.checkout(host, port, pool_scheme);
        if (pooled) {
            // Liveness probe (0-timeout): a closed pooled socket shows up as
            // immediately readable with EOF. This catches the stale-connection
            // race up front, before anything is sent — safe for every method.
            // TLS sessions are probed with SSL_peek: a raw recv MSG_PEEK could
            // not tell a close_notify alert record apart from app data.
            const auto ready = Poller::wait(pooled->socket->getFd(), Poller::Event::Read, 0);
            if (ready == Poller::WaitResult::Ready) {
                bool alive;
                if (pooled->socket->get_is_ssl_()) {
                    alive = pooled->socket->sslAlive();
                } else {
                    char peek;
                    alive = pooled->socket->recv(&peek, 1, MSG_PEEK) > 0;
                }
                if (!alive) pooled.reset();  // backend closed it while idle; use a fresh one
            }
        }
        if (pooled) {
            upstream_socket = pooled->socket.get();
            used_pooled = true;
        } else {
            try {
                fresh = Socket(AF_INET, SOCK_STREAM, 0);
                fresh->setnonblocking();
                fresh->setnodelay();  // avoid delayed-ACK stalls on proxied responses
            } catch (const std::exception&) {
                return resp;
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
                return resp;
            }

            bool connected = fresh->connect(reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
            if (!connected) {
                const auto wait_result = Poller::wait(fresh->getFd(), Poller::Event::Write, timeout_ms);
                if (wait_result == Poller::WaitResult::Timeout) {
                    return make_error_response(504, BackendError::ConnectTimeout, "Gateway Timeout");
                }
                if (wait_result != Poller::WaitResult::Ready || fresh->socketError() != 0) {
                    return resp;
                }
            }
            // TLS handshake on the fresh connection: SSL_connect drives the
            // state machine step by step, each incomplete step re-arms the
            // poller in the direction the SSL layer asked for (WANT_READ or
            // WANT_WRITE). A verification failure lands in FAILED and 502s.
            if (tls.enable) {
                SSL_CTX* ctx = get_upstream_ssl_ctx(tls);
                if (!ctx) return resp;
                if (!fresh->initSSLClient(ctx, tls.server_name)) return resp;
                bool handshake_ok = false;
                for (;;) {
                    const auto status = fresh->sslConnect();
                    if (status == SSLHandshakeStatus::COMPLETE) { handshake_ok = true; break; }
                    if (status == SSLHandshakeStatus::FAILED) return resp;
                    const Poller::Event event = status == SSLHandshakeStatus::WANT_READ
                                                    ? Poller::Event::Read : Poller::Event::Write;
                    const auto wait_result = Poller::wait(fresh->getFd(), event, timeout_ms);
                    if (wait_result == Poller::WaitResult::Timeout) {
                        return make_error_response(504, BackendError::ConnectTimeout, "Gateway Timeout");
                    }
                    if (wait_result != Poller::WaitResult::Ready) return resp;
                }
                if (!handshake_ok) return resp;
            }
            upstream_socket = &*fresh;
        }

        // Build the HTTP request; framing stays keep-alive (HTTP/1.1 default)
        // so the connection can be returned to the pool afterwards. Framing
        // and hop-by-hop headers are never forwarded: the body was parsed
        // (chunked de-chunked, CL-checked) and is re-framed by Content-Length.
        std::ostringstream req_stream;
        req_stream << method << " " << path << " HTTP/1.1\r\n";
        req_stream << "Host: " << host << ":" << port << "\r\n";
        const std::unordered_set<std::string> conn_tokens = connection_tokens(req_headers);
        for (const auto& [k, v] : req_headers) {
            const std::string lower_key = to_lower(k);
            if (is_hop_by_hop_header(lower_key) || conn_tokens.count(lower_key) != 0) continue;
            req_stream << k << ": " << v << "\r\n";
        }
        // Send framing whenever a body is possible: non-empty body, or the
        // client explicitly framed an empty body (Content-Length: 0) — without
        // a CL, a keep-alive backend would wait for a body that never comes.
        const bool needs_content_length = !req_body.empty() ||
            req_headers.count("content-length") || req_headers.count("transfer-encoding");
        if (needs_content_length) {
            req_stream << "Content-Length: " << req_body.size() << "\r\n";
        }
        req_stream << "\r\n" << req_body;

        const SendResult send_result =
            send_all(*upstream_socket, upstream_socket->get_is_ssl_(), req_stream.str(), timeout_ms);
        if (send_result == SendResult::Timeout) {
            if (used_pooled) {
                pool.invalidate(host, port, pool_scheme);
                if (idempotent) continue;  // request already (partially) sent
            }
            return make_error_response(504, BackendError::WriteTimeout, "Gateway Timeout");
        }
        if (send_result == SendResult::Failed) {
            if (used_pooled) {
                pool.invalidate(host, port, pool_scheme);  // stale pooled socket
                if (idempotent) continue;
            }
            return resp;
        }

        // Read exactly one response. Termination depends on the response
        // framing: Content-Length, chunked, or EOF (Connection: close). The body
        // cap comes from config (Config::max_response_bytes): a bounded buffer is
        // what keeps a hostile or broken backend from exhausting gateway memory.
        const size_t kMaxResponseSize = max_response_bytes;
        constexpr size_t kMaxHeaderSize = 256 * 1024;
        std::string response = pooled ? std::move(pooled->leftover) : "";
        if (pooled) pooled->leftover.clear();
        bool peer_closed = false;
        bool framing_error = false;
        bool complete = false;
        bool stale_pooled = false;  // pooled socket died mid-request; retry on a fresh one
        size_t body_end = std::string::npos;  // past the end of the message body
        bool response_uses_keepalive = false;
        bool response_is_chunked = false;
        std::string decoded_body;  // de-chunked payload when the backend sent chunked

        while (!complete && response.size() <= kMaxResponseSize) {
            const size_t header_end = response.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                if (response.size() > kMaxHeaderSize) { framing_error = true; break; }
            } else {
                const std::string headers_part = response.substr(0, header_end);
                const std::string conn = find_header_value(headers_part, "connection");
                const std::string status_line = headers_part.substr(0, headers_part.find("\r\n"));
                const bool http11 = status_line.find("HTTP/1.1") == 0;
                response_uses_keepalive = http11 ? (conn != "close") : (conn == "keep-alive");

                const std::string te = find_header_value(headers_part, "transfer-encoding");
                if (te.find("chunked") != std::string::npos) {
                    response_is_chunked = true;
                    size_t end = std::string::npos;
                    decoded_body.clear();  // the scan restarts from scratch as data arrives
                    const ChunkScan scan = scan_chunked_body(response, header_end + 4, end, &decoded_body);
                    if (scan == ChunkScan::Complete) {
                        body_end = end;
                        complete = true;
                        break;
                    }
                    if (scan == ChunkScan::Error) { framing_error = true; break; }
                } else if (!te.empty()) {
                    framing_error = true;  // unsupported transfer-encoding
                    break;
                } else {
                    const std::string cl = find_header_value(headers_part, "content-length");
                    if (!cl.empty() && cl.find_first_not_of("0123456789") == std::string::npos) {
                        size_t content_length = 0;
                        try {
                            content_length = static_cast<size_t>(std::stoull(cl));
                        } catch (const std::exception&) {
                            framing_error = true;
                            break;
                        }
                        if (content_length > kMaxResponseSize) { framing_error = true; break; }
                        if (response.size() >= header_end + 4 + content_length) {
                            body_end = header_end + 4 + content_length;
                            complete = true;
                            break;
                        }
                    } else if (!response_uses_keepalive) {
                        // No framing info and the peer will close: read to EOF.
                        complete = false;
                    } else {
                        framing_error = true;  // keep-alive without framing is unusable
                        break;
                    }
                }
            }

            const bool is_ssl = upstream_socket->get_is_ssl_();
            // TLS renegotiation can request a write mid-read, so the poll
            // direction follows last_ssl_want() like the send path does.
            const Poller::Event event = (is_ssl && upstream_socket->last_ssl_want() == SSLWant::WRITE)
                                            ? Poller::Event::Write : Poller::Event::Read;
            const auto wait_result =
                Poller::wait(upstream_socket->getFd(), event, timeout_ms);
            if (wait_result == Poller::WaitResult::Timeout) {
                if (used_pooled) {
                    pool.invalidate(host, port, pool_scheme);
                    if (idempotent) {
                        stale_pooled = true;
                        break;
                    }
                }
                return make_error_response(504, BackendError::ReadTimeout, "Gateway Timeout");
            }
            if (wait_result != Poller::WaitResult::Ready) {
                break;  // read failed
            }
            {
                char buf[65536];
                const ssize_t n = is_ssl ? upstream_socket->sslRead(buf, sizeof(buf))
                                         : upstream_socket->recv(buf, sizeof(buf), 0);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN) continue;  // WANT_*; re-poll in the right direction
                    break;  // read failed
                }
                if (n == 0) {
                    peer_closed = true;
                    // With close-delimited framing, EOF completes the response.
                    if (header_end != std::string::npos && !response_uses_keepalive) {
                        body_end = response.size();
                        complete = true;
                    }
                    break;
                }
                response.append(buf, static_cast<size_t>(n));
            }
        }

        if (stale_pooled) {
            continue;  // retry on a brand-new connection
        }

        if (!complete || framing_error) {
            if (used_pooled) {
                pool.invalidate(host, port, pool_scheme);
                if (attempt == 0 && idempotent) {
                    continue;  // stale/invalid pooled socket; retry on a fresh one
                }
            }
            resp.error = framing_error ? BackendError::InvalidResponse : BackendError::ReadFailed;
            return resp;
        }

        // Parse the status line
        {
            const size_t pos = response.find(' ');
            const size_t pos2 = pos == std::string::npos ? std::string::npos
                                                         : response.find(' ', pos + 1);
            if (pos == std::string::npos || pos2 == std::string::npos) {
                resp.error = BackendError::InvalidResponse;
                return resp;
            }
            try {
                resp.status_code = std::stoi(response.substr(pos + 1, pos2 - pos - 1));
            } catch (const std::exception&) {
                resp.error = BackendError::InvalidResponse;
                return resp;
            }
        }

        // Parse headers and body
        {
            const size_t header_end = response.find("\r\n\r\n");
            const std::string headers_part = response.substr(0, header_end);
            if (response_is_chunked) {
                resp.body = std::move(decoded_body);  // real payload; the gateway re-frames for the client
            } else {
                resp.body = response.substr(header_end + 4,
                                            body_end == std::string::npos
                                                ? std::string::npos
                                                : body_end - (header_end + 4));
            }
            size_t line_start = headers_part.find("\r\n") + 2;  // skip the status line
            while (line_start < headers_part.size()) {
                const size_t line_end = headers_part.find("\r\n", line_start);
                const std::string line = headers_part.substr(line_start, line_end - line_start);
                const size_t colon = line.find(':');
                if (colon != std::string::npos) {
                    std::string key = line.substr(0, colon);
                    std::string value = line.substr(colon + 1);
                    value.erase(0, value.find_first_not_of(" \t"));
                    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                        value.pop_back();
                    }
                    resp.headers[key] = value;
                }
                if (line_end == std::string::npos) break;
                line_start = line_end + 2;
            }
        }

        // Return the connection to the pool when the backend allows reuse.
        if (response_uses_keepalive && !peer_closed) {
            if (used_pooled) {
                pooled->leftover = response.substr(body_end);
                pool.checkin(host, port, pool_scheme, std::move(*pooled));
            } else {
                ConnectionPool::PooledConnection conn;
                conn.socket = std::make_shared<Socket>(std::move(*fresh));
                conn.leftover = response.substr(body_end);
                pool.checkin(host, port, pool_scheme, std::move(conn));
            }
        }

        resp.error = BackendError::None;
        return resp;
    }

    return make_error_response(502, BackendError::ReadFailed, "Bad Gateway");
}

// ────────────────────────────── UpstreamExchange ──────────────────────────────
// See the class comment in http_client.h. Every framing/retry/timeout detail
// below is deliberately identical to forward_request() above: the two paths
// must not diverge, so the blocking version stays the reference implementation
// used by composite dependencies.

namespace {
constexpr size_t kExchangeMaxHeaderSize = 256 * 1024;
constexpr size_t kExchangeReadChunk = 65536;
}  // namespace

UpstreamExchange::UpstreamExchange(Params params, bool idempotent)
    : params_(std::move(params)), idempotent_(idempotent) {
    result_ = make_error_response(502, BackendError::ConnectFailed, "Bad Gateway");
    // Pool identity includes the verification policy: a session verified for
    // one server name is only reusable by upstreams with the same policy.
    // The CA file is part of the identity too (matching the SSL_CTX cache key in
    // get_upstream_ssl_ctx); see forward_request for the rationale.
    pool_scheme_ = !params_.tls.enable ? ""
        : params_.tls.skip_verify ? "tls/insecure|" : "tls/" + params_.tls.ca_file + "|" + params_.tls.server_name + "|";
    refresh_deadline();
    if (params_.timeout_ms <= 0) {
        phase_ = Phase::Done;
        result_ = make_error_response(504, BackendError::ConnectTimeout, "Gateway Timeout");
    }
}

UpstreamExchange::~UpstreamExchange() = default;

int UpstreamExchange::fd() const {
    return sock_ ? sock_->getFd() : -1;
}

void UpstreamExchange::refresh_deadline() {
    deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(params_.timeout_ms);
}

bool UpstreamExchange::expired() const {
    return std::chrono::steady_clock::now() >= deadline_;
}

bool UpstreamExchange::open_fresh() {
    try {
        sock_ = std::make_shared<Socket>(AF_INET, SOCK_STREAM, 0);
        sock_->setnonblocking();
        sock_->setnodelay();  // avoid delayed-ACK stalls on proxied responses
    } catch (const std::exception&) {
        sock_.reset();
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(params_.port);
    if (inet_pton(AF_INET, params_.host.c_str(), &addr.sin_addr) != 1) {
        sock_.reset();
        return false;
    }

    bool connected = false;
    try {
        connected = sock_->connect(reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    } catch (const std::exception&) {
        sock_.reset();
        return false;
    }

    if (!connected) {
        // Non-blocking connect in progress: Connecting waits for writability,
        // then confirms success through SO_ERROR.
        phase_ = Phase::Connecting;
        refresh_deadline();
        return true;
    }

    if (!params_.tls.enable) {
        phase_ = Phase::Send;
        refresh_deadline();
        return true;
    }

    // TLS handshake on a fresh connection: a verification failure lands in
    // FAILED and 502s (see forward_request()).
    SSL_CTX* ctx = get_upstream_ssl_ctx(params_.tls);
    if (!ctx || !sock_->initSSLClient(ctx, params_.tls.server_name)) {
        sock_.reset();
        return false;
    }
    phase_ = Phase::TlsHandshake;
    refresh_deadline();
    return true;
}

bool UpstreamExchange::open_connection() {
    used_pooled_ = false;
    if (!force_fresh_) {
        ConnectionPool& pool = global_connection_pool();
        std::optional<ConnectionPool::PooledConnection> pooled =
            pool.checkout(params_.host, params_.port, pool_scheme_);
        if (pooled) {
            // Liveness probe (0-timeout): a closed pooled socket shows up as
            // immediately readable with EOF. This catches the stale-connection
            // race up front, before anything is sent — safe for every method.
            // TLS sessions are probed with SSL_peek: a raw recv MSG_PEEK could
            // not tell a close_notify alert record apart from app data.
            const auto ready = Poller::wait(pooled->socket->getFd(), Poller::Event::Read, 0);
            bool alive = true;
            if (ready == Poller::WaitResult::Ready) {
                if (pooled->socket->get_is_ssl_()) {
                    alive = pooled->socket->sslAlive();
                } else {
                    char peek;
                    alive = pooled->socket->recv(&peek, 1, MSG_PEEK) > 0;
                }
            }
            if (alive) {
                // A pooled TLS session is already handshaked, so it goes
                // straight to Send. Leftover bytes belong to the previous
                // response and are prepended to the read buffer.
                sock_ = pooled->socket;
                used_pooled_ = true;
                response_bytes_ = std::move(pooled->leftover);
                phase_ = Phase::Send;
                refresh_deadline();
                return true;
            }
            // Backend closed it while idle: fall through to a fresh connection.
        }
    }
    return open_fresh();
}

void UpstreamExchange::build_request() {
    // Build the HTTP request; framing stays keep-alive (HTTP/1.1 default) so
    // the connection can be returned to the pool afterwards. Framing and
    // hop-by-hop headers are never forwarded: the body was parsed (chunked
    // de-chunked, CL-checked) and is re-framed by Content-Length.
    std::ostringstream req_stream;
    req_stream << params_.method << " " << params_.path << " HTTP/1.1\r\n";
    req_stream << "Host: " << params_.host << ":" << params_.port << "\r\n";
    const std::unordered_set<std::string> conn_tokens = connection_tokens(params_.headers);
    for (const auto& [k, v] : params_.headers) {
        const std::string lower_key = to_lower(k);
        if (is_hop_by_hop_header(lower_key) || conn_tokens.count(lower_key) != 0) continue;
        req_stream << k << ": " << v << "\r\n";
    }
    // Send framing whenever a body is possible: non-empty body, or the client
    // explicitly framed an empty body (Content-Length: 0) — without a CL, a
    // keep-alive backend would wait for a body that never comes.
    const bool needs_content_length = !params_.body.empty() ||
        params_.headers.count("content-length") || params_.headers.count("transfer-encoding");
    if (needs_content_length) {
        req_stream << "Content-Length: " << params_.body.size() << "\r\n";
    }
    req_stream << "\r\n" << params_.body;
    request_bytes_ = req_stream.str();
}

UpstreamExchange::FrameResult UpstreamExchange::try_frame() {
    const size_t header_end = response_bytes_.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return response_bytes_.size() > kExchangeMaxHeaderSize ? FrameResult::Error
                                                              : FrameResult::Incomplete;
    }

    const std::string headers_part = response_bytes_.substr(0, header_end);
    const std::string conn = find_header_value(headers_part, "connection");
    const std::string status_line = headers_part.substr(0, headers_part.find("\r\n"));
    const bool http11 = status_line.find("HTTP/1.1") == 0;
    response_keepalive_ = http11 ? (conn != "close") : (conn == "keep-alive");

    const std::string te = find_header_value(headers_part, "transfer-encoding");
    if (te.find("chunked") != std::string::npos) {
        response_is_chunked_ = true;
        size_t end = std::string::npos;
        decoded_body_.clear();  // the scan restarts from scratch as data arrives
        const ChunkScan scan = scan_chunked_body(response_bytes_, header_end + 4, end, &decoded_body_);
        if (scan == ChunkScan::Complete) {
            body_end_ = end;
            return FrameResult::Complete;
        }
        return scan == ChunkScan::Error ? FrameResult::Error : FrameResult::Incomplete;
    }
    if (!te.empty()) return FrameResult::Error;  // unsupported transfer-encoding

    const std::string cl = find_header_value(headers_part, "content-length");
    if (!cl.empty() && cl.find_first_not_of("0123456789") == std::string::npos) {
        size_t content_length = 0;
        try {
            content_length = static_cast<size_t>(std::stoull(cl));
        } catch (const std::exception&) {
            return FrameResult::Error;
        }
        if (content_length > params_.max_response_bytes) return FrameResult::Error;
        if (response_bytes_.size() >= header_end + 4 + content_length) {
            body_end_ = header_end + 4 + content_length;
            return FrameResult::Complete;
        }
        return FrameResult::Incomplete;
    }
    // No usable framing header: the message is only bounded by the peer
    // closing the connection, so it completes at EOF.
    if (!response_keepalive_) {
        if (peer_closed_) {
            body_end_ = response_bytes_.size();
            return FrameResult::Complete;
        }
        return FrameResult::Incomplete;
    }
    return FrameResult::Error;  // keep-alive without framing is unusable
}

void UpstreamExchange::finish_ok() {
    // Parse the status line
    const size_t pos = response_bytes_.find(' ');
    const size_t pos2 = pos == std::string::npos ? std::string::npos
                                                 : response_bytes_.find(' ', pos + 1);
    if (pos == std::string::npos || pos2 == std::string::npos) {
        phase_ = Phase::Done;
        result_ = make_error_response(502, BackendError::InvalidResponse, "Bad Gateway");
        return;
    }
    try {
        result_.status_code = std::stoi(response_bytes_.substr(pos + 1, pos2 - pos - 1));
    } catch (const std::exception&) {
        phase_ = Phase::Done;
        result_ = make_error_response(502, BackendError::InvalidResponse, "Bad Gateway");
        return;
    }

    // Parse headers and body
    const size_t header_end = response_bytes_.find("\r\n\r\n");
    const std::string headers_part = response_bytes_.substr(0, header_end);
    if (response_is_chunked_) {
        result_.body = std::move(decoded_body_);  // real payload; the gateway re-frames for the client
    } else {
        result_.body = response_bytes_.substr(header_end + 4,
                                              body_end_ == std::string::npos
                                                  ? std::string::npos
                                                  : body_end_ - (header_end + 4));
    }
    size_t line_start = headers_part.find("\r\n") + 2;  // skip the status line
    while (line_start < headers_part.size()) {
        const size_t line_end = headers_part.find("\r\n", line_start);
        const std::string line = headers_part.substr(line_start, line_end - line_start);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                value.pop_back();
            }
            result_.headers[key] = value;
        }
        if (line_end == std::string::npos) break;
        line_start = line_end + 2;
    }

    result_.error = BackendError::None;
    succeeded_ = true;
    phase_ = Phase::Done;
}

bool UpstreamExchange::fail(BackendError error, int http_status, const std::string& body) {
    if (used_pooled_) {
        // The pooled socket is suspected broken (stale keep-alive); drop it
        // before deciding whether the request may be replayed.
        global_connection_pool().invalidate(params_.host, params_.port, pool_scheme_);
        if (idempotent_ && !retried_) {
            // The request may already have reached the backend on the pooled
            // socket, so replaying is only safe for idempotent methods; a
            // non-idempotent one (POST/PUT/DELETE) must surface the failure.
            retried_ = true;
            force_fresh_ = true;
            used_pooled_ = false;
            sock_.reset();  // the caller unregistered the fd before driving us here
            response_bytes_.clear();
            decoded_body_.clear();
            request_bytes_.clear();
            send_off_ = 0;
            body_end_ = std::string::npos;
            response_keepalive_ = false;
            response_is_chunked_ = false;
            peer_closed_ = false;
            phase_ = Phase::Connect;
            refresh_deadline();
            return true;
        }
    }
    result_ = make_error_response(http_status, error, body);
    succeeded_ = false;
    phase_ = Phase::Done;
    return false;
}

UpstreamExchange::Want UpstreamExchange::advance(uint32_t events) {
    try {
        return run(events);
    } catch (const std::exception& e) {
        // Socket helpers throw on unrecoverable errors; map them to the same
        // failure codes the blocking path would have produced.
        Logger::get()->warn("Upstream exchange failed: {}", e.what());
        BackendError error = BackendError::ConnectFailed;
        if (phase_ == Phase::Send) error = BackendError::WriteFailed;
        else if (phase_ == Phase::Read) error = BackendError::ReadFailed;
        if (fail(error, 502, "Bad Gateway")) {
            // fail() started a fresh-connection replay; keep driving it. Re-entry
            // cannot loop forever: retried_ makes the next failure terminal.
            return run(0);
        }
        return Want::Done;
    }
}

UpstreamExchange::Want UpstreamExchange::run(uint32_t events) {
    uint32_t ev = events;
    for (;;) {
        if (phase_ == Phase::Done) return Want::Done;

        if (expired()) {
            // The idle deadline elapsed without progress in the current phase.
            switch (phase_) {
                case Phase::Connect:
                case Phase::Connecting:
                case Phase::TlsHandshake:
                    if (fail(BackendError::ConnectTimeout, 504, "Gateway Timeout")) continue;
                    return Want::Done;
                case Phase::Send:
                    if (fail(BackendError::WriteTimeout, 504, "Gateway Timeout")) continue;
                    return Want::Done;
                case Phase::Read:
                    if (fail(BackendError::ReadTimeout, 504, "Gateway Timeout")) continue;
                    return Want::Done;
                default:
                    return Want::Done;
            }
        }

        switch (phase_) {
            case Phase::Connect: {
                ev = 0;
                if (!open_connection()) {
                    if (fail(BackendError::ConnectFailed, 502, "Bad Gateway")) continue;
                    return Want::Done;
                }
                break;
            }

            case Phase::Connecting: {
                if (ev == 0) {
                    // Connect was just initiated: wait for the socket to become
                    // writable without inspecting SO_ERROR yet (it still reads
                    // as "in progress" at this point).
                    return Want::Write;
                }
                ev = 0;
                if (sock_->socketError() != 0) {
                    if (fail(BackendError::ConnectFailed, 502, "Bad Gateway")) continue;
                    return Want::Done;
                }
                if (!params_.tls.enable) {
                    phase_ = Phase::Send;
                    refresh_deadline();
                    break;
                }
                SSL_CTX* ctx = get_upstream_ssl_ctx(params_.tls);
                if (!ctx || !sock_->initSSLClient(ctx, params_.tls.server_name)) {
                    if (fail(BackendError::ConnectFailed, 502, "Bad Gateway")) continue;
                    return Want::Done;
                }
                phase_ = Phase::TlsHandshake;
                refresh_deadline();
                break;
            }

            case Phase::TlsHandshake: {
                ev = 0;
                const auto status = sock_->sslConnect();
                if (status == SSLHandshakeStatus::COMPLETE) {
                    phase_ = Phase::Send;
                    refresh_deadline();
                    break;
                }
                if (status == SSLHandshakeStatus::FAILED) {
                    if (fail(BackendError::ConnectFailed, 502, "Bad Gateway")) continue;
                    return Want::Done;
                }
                refresh_deadline();
                return status == SSLHandshakeStatus::WANT_READ ? Want::Read : Want::Write;
            }

            case Phase::Send: {
                ev = 0;
                if (request_bytes_.empty()) build_request();
                if (send_off_ < request_bytes_.size()) {
                    // One send attempt per event: returning EAGAIN re-arms the
                    // event loop instead of spinning here.
                    const bool is_ssl = sock_->get_is_ssl_();
                    const ssize_t n = is_ssl
                        ? sock_->sslWrite(const_cast<char*>(request_bytes_.data() + send_off_),
                                          request_bytes_.size() - send_off_)
                        : sock_->send(request_bytes_.data() + send_off_,
                                      request_bytes_.size() - send_off_, MSG_NOSIGNAL);
                    if (n > 0) {
                        send_off_ += static_cast<size_t>(n);
                        refresh_deadline();
                        break;  // re-enter the Send phase for the remaining bytes
                    }
                    // n < 0: EAGAIN/EWOULDBLOCK (would block) or a real failure.
                    if (errno == EINTR) break;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        refresh_deadline();
                        if (is_ssl && sock_->last_ssl_want() == SSLWant::READ) return Want::Read;
                        return Want::Write;
                    }
                    if (fail(BackendError::WriteFailed, 502, "Bad Gateway")) continue;
                    return Want::Done;
                }
                phase_ = Phase::Read;
                refresh_deadline();
                break;
            }

            case Phase::Read: {
                ev = 0;
                const FrameResult framed = try_frame();
                if (framed == FrameResult::Complete) {
                    finish_ok();
                    return Want::Done;
                }
                if (framed == FrameResult::Error) {
                    if (fail(BackendError::InvalidResponse, 502, "Bad Gateway")) continue;
                    return Want::Done;
                }
                // Bounded buffer: a hostile or broken backend must not be able
                // to grow the response past the configured cap.
                if (response_bytes_.size() > params_.max_response_bytes) {
                    if (fail(BackendError::ReadFailed, 502, "Bad Gateway")) continue;
                    return Want::Done;
                }

                const bool is_ssl = sock_->get_is_ssl_();
                char buf[kExchangeReadChunk];
                const ssize_t n = is_ssl ? sock_->sslRead(buf, sizeof(buf))
                                         : sock_->recv(buf, sizeof(buf), 0);
                if (n > 0) {
                    response_bytes_.append(buf, static_cast<size_t>(n));
                    refresh_deadline();
                    continue;  // re-frame with the new bytes
                }
                if (n == 0) {
                    peer_closed_ = true;
                    // With close-delimited framing, EOF completes the response.
                    if (response_bytes_.find("\r\n\r\n") != std::string::npos && !response_keepalive_) {
                        const FrameResult eof_frame = try_frame();
                        if (eof_frame == FrameResult::Complete) {
                            finish_ok();
                            return Want::Done;
                        }
                    }
                    if (fail(BackendError::ReadFailed, 502, "Bad Gateway")) continue;
                    return Want::Done;
                }
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    refresh_deadline();
                    if (is_ssl && sock_->last_ssl_want() == SSLWant::WRITE) return Want::Write;
                    return Want::Read;
                }
                if (fail(BackendError::ReadFailed, 502, "Bad Gateway")) continue;
                return Want::Done;
            }

            case Phase::Done:
                return Want::Done;
        }
    }
}

void UpstreamExchange::reuse_to_pool() {
    if (!succeeded_ || !sock_ || !response_keepalive_ || peer_closed_) return;
    ConnectionPool::PooledConnection conn;
    conn.socket = sock_;                       // keep this request's connection alive
    conn.leftover = body_end_ == std::string::npos || body_end_ >= response_bytes_.size()
                        ? std::string()
                        : response_bytes_.substr(body_end_);
    conn.last_used = std::chrono::steady_clock::now();
    global_connection_pool().checkin(params_.host, params_.port, pool_scheme_, std::move(conn));
    sock_.reset();
}
