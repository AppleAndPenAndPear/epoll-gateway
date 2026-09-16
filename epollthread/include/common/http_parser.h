#pragma once
#include <string>
#include <unordered_map>
#include <sstream>

struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::string query;
    std::string trace_id;       // records the request's trace_id for log correlation
    std::string host;
    std::string tenant;
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    void clear() {
        method.clear();
        path.clear();
        version.clear();
        query.clear();
        trace_id.clear();
        host.clear();
        tenant.clear();
        headers.clear();
        body.clear();
    }
};

struct HttpResponse {
    int status_code = 200;
    std::string status_message = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    bool chunked = false;   // set when chunked transfer encoding is used

    std::string to_string() const {     // serialization only: to_string() is a read-only serializer matching const semantics; keep const, but set Content-Length externally before calling
        std::ostringstream oss;
        oss << "HTTP/1.1 " << status_code << " " << status_message << "\r\n";
        for (const auto& [key, value] : headers) {
            oss << key << ": " << value << "\r\n";
        }
        oss << "\r\n";
        oss << body;
        return oss.str();
    }
};

class HttpParser {
public:
    enum class State { METHOD, PATH, VERSION, QUERY, HEADER, CHUNKED_BODY, BODY, DONE };

    enum class ChunkState { SIZE, DATA, TRAILER, DONE } chunk_state_ = ChunkState::SIZE;

    static constexpr size_t MAX_BODY_SIZE = 1024 * 1024; // 1MB cap

    HttpParser();

    // Feed data; returns true when parsing is complete (end of request headers)
    bool parse(const char* data, size_t len, HttpRequest& request,size_t& consumed);

    // Check whether the body exceeded the size limit
    bool is_body_too_large() const { return body_too_large_; }

    void reset();
private:
    State state_;
    std::string buffer_;
    size_t content_length_;     // body length
    size_t body_read_;          // bytes read so far
    bool body_too_large_ = false;  // set when the body exceeds the limit
    std::string chunk_size_hex_buffer_;
    size_t chunk_data_remaining_ = 0;
    bool expecting_final_lf_ = false;   // used to wait for the final \n in the TRAILER state
};