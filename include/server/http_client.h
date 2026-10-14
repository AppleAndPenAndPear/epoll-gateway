#pragma once
#include <string>
#include <map>
#include <unordered_map>
#include <memory>
#include <cstdint>
#include <chrono>

class Socket;

struct BackendResponse {
    int status_code = 502;
    std::map<std::string, std::string> headers;
    std::string body;
    enum class Error {
        None,
        ConnectFailed,
        ConnectTimeout,
        WriteFailed,
        WriteTimeout,
        ReadFailed,
        ReadTimeout,
        InvalidResponse
    } error = Error::None;
};

using BackendError = BackendResponse::Error;

// TLS policy for one upstream connection. enable=false keeps the historical
// plaintext hop; the fields mirror UpstreamServer's tls_* config entries.
struct UpstreamTlsOptions {
    bool enable = false;
    std::string ca_file;       // CA bundle; empty = system default trust store;verification during connection setup TLS
    std::string server_name;   // SNI + hostname check; empty = chain-only verification
    bool skip_verify = false;  // disable verification (insecure, lab use only)
};

bool should_retry_backend_request(const std::string& method,
                                 BackendError error,
                                 int attempt_count);

BackendResponse forward_request(const std::string& host, int port,
                                const std::string& method,
                                const std::string& path,
                                const std::unordered_map<std::string, std::string>& req_headers,
                                const std::string& req_body,
                                int timeout_ms,
                                size_t max_response_bytes,
                                const UpstreamTlsOptions& tls = {});

// Non-blocking, epoll-driven single-upstream round trip.
//
// forward_request() performs the same exchange on the calling thread and parks
// that thread on a poller for every wait, which forces one worker thread per
// in-flight backend call. This class instead exposes an advance() step so the
// gateway's own event loop drives the upstream socket: a backend call consumes
// no thread while it waits, so the number of concurrent upstream calls is no
// longer bounded by a thread-pool size. The request framing, response framing,
// pool identity/retry rules and error mapping are ported one-to-one from
// forward_request() so both paths behave identically.
//
// Usage: construct with the snapshotted Params, call advance(0) once right
// after adding fd() to epoll, then advance(event_mask) for every event epoll
// reports for fd(). Map the returned Want to EPOLLIN/EPOLLOUT and re-arm;
// Want::Done means the exchange finished (see succeeded()/result()).
class UpstreamExchange {
public:
    // Everything the exchange needs, copied by value: the caller's config and
    // route tables are reassigned in place on a hot reload, so nothing may be
    // referenced across the (event-loop) turn.
    struct Params {
        std::string host;
        int port = 0;
        std::string method;
        std::string path;
        std::unordered_map<std::string, std::string> headers;
        std::string body;
        int timeout_ms = 5000;
        size_t max_response_bytes = 0;
        UpstreamTlsOptions tls;
    };

    enum class Want { Read, Write, Done };

    // `idempotent` mirrors should_retry_backend_request(): only an idempotent
    // method may be replayed on a fresh connection after a pooled one failed.
    UpstreamExchange(Params params, bool idempotent);
    ~UpstreamExchange();

    // Drives the state machine one step. `events` is 0 for the first call (just
    // after construction) and the epoll mask afterwards; the mask is only
    // consulted to detect a failed non-blocking connect. Returns the direction
    // the caller must wait on next, or Done.
    Want advance(uint32_t events);

    // Upstream socket fd, or -1 before it exists / after Done.
    int fd() const;

    const Params& params() const { return params_; }
    bool succeeded() const { return succeeded_; }
    const BackendResponse& result() const { return result_; }
    // Idle deadline: refreshed on every successful I/O step, so a backend that
    // keeps making progress is never cut off mid-response.
    std::chrono::steady_clock::time_point deadline() const { return deadline_; }

    // Returns a reusable keep-alive upstream socket to the process-wide pool.
    // Only valid after advance() returned Done with succeeded() true. The
    // caller MUST have already removed fd() from its epoll set, otherwise the
    // pooled socket would still be registered while another request uses it.
    void reuse_to_pool();

private:
    enum class Phase { Connect, Connecting, TlsHandshake, Send, Read, Done };
    enum class FrameResult { Incomplete, Complete, Error };

    Want run(uint32_t events);
    bool open_connection();
    bool open_fresh();
    void build_request();
    FrameResult try_frame();
    void finish_ok();
    void refresh_deadline();
    bool expired() const;
    // Terminates on failure, first replaying on a fresh connection when that is
    // safe (idempotent method, pooled connection, first attempt). Returns true
    // when a retry was started and the caller must keep driving.
    bool fail(BackendError error, int http_status, const std::string& body);

    Params params_;
    bool idempotent_ = false;
    Phase phase_ = Phase::Connect;
    std::string pool_scheme_;
    std::shared_ptr<Socket> sock_;
    bool used_pooled_ = false;
    bool retried_ = false;       // internal pooled->fresh replay already done
    bool force_fresh_ = false;   // next attempt must not consult the pool
    std::string request_bytes_;
    size_t send_off_ = 0;
    std::string response_bytes_;
    size_t body_end_ = std::string::npos;
    bool response_keepalive_ = false;
    bool response_is_chunked_ = false;
    bool peer_closed_ = false;
    std::string decoded_body_;
    bool succeeded_ = false;
    BackendResponse result_;
    std::chrono::steady_clock::time_point deadline_;
};