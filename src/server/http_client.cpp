#include "http_client.h"
#include "connection_pool.h"
#include "mysocket.h"
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

namespace {

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

// Case-insensitive header lookup within the raw header block of a response.
//find value of header
std::string find_header_value(const std::string& headers_part, const std::string& name) {
    size_t line_start = headers_part.find("\r\n") + 2;  // skip the status line which includes method,path and version
    while (line_start < headers_part.size()) {
        const size_t line_end = headers_part.find("\r\n", line_start);
        const size_t count = (line_end == std::string::npos ? headers_part.size() : line_end) - line_start;
        const std::string line = headers_part.substr(line_start, count);
        const size_t colon = line.find(':');
        if (colon != std::string::npos && to_lower(line.substr(0, colon)) == name) {
            std::string value = line.substr(colon + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
            return to_lower(value);
        }
        if (line_end == std::string::npos) break;
        line_start = line_end + 2;
    }
    return "";
}

enum class ChunkScan { Incomplete, Complete, Error };

// Scans a chunked body starting at body_start. On Complete, body_end points
// just past the terminating CRLF of the last-chunk trailer.
ChunkScan scan_chunked_body(const std::string& buf, size_t body_start, size_t& body_end) {
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

SendResult send_all(Socket& sock, const std::string& request, int timeout_ms) {
    size_t sent = 0;
    while (sent < request.size()) {
        const auto wait_result = Poller::wait(sock.getFd(), Poller::Event::Write, timeout_ms);
        if (wait_result == Poller::WaitResult::Timeout) return SendResult::Timeout;
        if (wait_result != Poller::WaitResult::Ready) return SendResult::Failed;
        const ssize_t written = sock.send(request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) continue;
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
                                int timeout_ms) {
    if (timeout_ms <= 0) {
        return make_error_response(504, BackendError::ConnectTimeout, "Gateway Timeout");
    }

    ConnectionPool& pool = global_connection_pool();
    const bool idempotent = method == "GET" || method == "HEAD" || method == "OPTIONS";

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

        if (attempt == 0) pooled = pool.checkout(host, port);
        if (pooled) {
            // Liveness probe (0-timeout): a closed pooled socket shows up as
            // immediately readable with EOF. This catches the stale-connection
            // race up front, before anything is sent — safe for every method.
            const auto ready = Poller::wait(pooled->socket->getFd(), Poller::Event::Read, 0);
            if (ready == Poller::WaitResult::Ready) {
                char peek;
                if (pooled->socket->recv(&peek, 1, MSG_PEEK) <= 0) {
                    pooled.reset();  // backend closed it while idle; use a fresh one
                }
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
            upstream_socket = &*fresh;
        }

        // Build the HTTP request; framing stays keep-alive (HTTP/1.1 default)
        // so the connection can be returned to the pool afterwards.
        std::ostringstream req_stream;
        req_stream << method << " " << path << " HTTP/1.1\r\n";
        req_stream << "Host: " << host << ":" << port << "\r\n";
        for (const auto& [k, v] : req_headers) {
            if (k == "host" || k == "connection") continue;  // hop-by-hop, we manage framing
            req_stream << k << ": " << v << "\r\n";
        }
        if (!req_body.empty()) {
            req_stream << "Content-Length: " << req_body.size() << "\r\n";
        }
        req_stream << "\r\n" << req_body;

        const SendResult send_result = send_all(*upstream_socket, req_stream.str(), timeout_ms);
        if (send_result == SendResult::Timeout) {
            if (used_pooled) {
                pool.invalidate(host, port);
                if (idempotent) continue;  // request already (partially) sent
            }
            return make_error_response(504, BackendError::WriteTimeout, "Gateway Timeout");
        }
        if (send_result == SendResult::Failed) {
            if (used_pooled) {
                pool.invalidate(host, port);  // stale pooled socket
                if (idempotent) continue;
            }
            return resp;
        }

        // Read exactly one response. Termination depends on the response
        // framing: Content-Length, chunked, or EOF (Connection: close).
        constexpr size_t kMaxResponseSize = 64 * 1024 * 1024;
        constexpr size_t kMaxHeaderSize = 256 * 1024;
        std::string response = pooled ? std::move(pooled->leftover) : "";
        if (pooled) pooled->leftover.clear();
        bool peer_closed = false;
        bool framing_error = false;
        bool complete = false;
        bool stale_pooled = false;  // pooled socket died mid-request; retry on a fresh one
        size_t body_end = std::string::npos;  // past the end of the message body
        bool response_uses_keepalive = false;

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
                    size_t end = std::string::npos;
                    const ChunkScan scan = scan_chunked_body(response, header_end + 4, end);
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

            const auto wait_result =
                Poller::wait(upstream_socket->getFd(), Poller::Event::Read, timeout_ms);
            if (wait_result == Poller::WaitResult::Timeout) {
                if (used_pooled) {
                    pool.invalidate(host, port);
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
                const ssize_t n = upstream_socket->recv(buf, sizeof(buf), 0);
                if (n < 0) {
                    if (errno == EINTR) continue;
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
                pool.invalidate(host, port);
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
            resp.body = response.substr(header_end + 4,
                                        body_end == std::string::npos
                                            ? std::string::npos
                                            : body_end - (header_end + 4));
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
                pool.checkin(host, port, std::move(*pooled));
            } else {
                ConnectionPool::PooledConnection conn;
                conn.socket = std::make_shared<Socket>(std::move(*fresh));
                conn.leftover = response.substr(body_end);
                pool.checkin(host, port, std::move(conn));
            }
        }

        resp.error = BackendError::None;
        return resp;
    }

    return make_error_response(502, BackendError::ReadFailed, "Bad Gateway");
}
