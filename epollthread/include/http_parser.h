#pragma once
#include <string>
#include <unordered_map>
#include <sstream>

struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::string query;   // 新增
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    void clear() {
        method.clear();
        path.clear();
        version.clear();
        query.clear();
        headers.clear();
        body.clear();
    }
};

struct HttpResponse {
    int status_code = 200;
    std::string status_message = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    bool chunked = false;   // 新增,标记是否使用分块传输编码

    std::string to_string() const {     //职责分离，to_string() 变成只读的序列化函数，符合 const 语义;保持 const，但调用前在外部先设置好 Content-Length
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

    static constexpr size_t MAX_BODY_SIZE = 1024 * 1024; // 1MB 上限

    HttpParser();

    // 喂入数据，返回 true 表示解析完成（请求头结束）
    bool parse(const char* data, size_t len, HttpRequest& request,size_t& consumed);

    // 检查 body 是否超出大小限制
    bool is_body_too_large() const { return body_too_large_; }

    void reset();
private:
    State state_;
    std::string buffer_;
    size_t content_length_;     //body 长度
    size_t body_read_;          //已读取的字节数
    bool body_too_large_ = false;  //body 超出限制标记
    std::string chunk_size_hex_buffer_;
    size_t chunk_data_remaining_ = 0;
    bool expecting_final_lf_ = false;   // 用于 TRAILER 状态等待最后的 \n
};