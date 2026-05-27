#pragma once
#include <string>
#include <unordered_map>
#include <sstream>

struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    void clear() {
        method.clear();
        path.clear();
        version.clear();
        headers.clear();
        body.clear();
    }
};

struct HttpResponse {
    int status_code = 200;
    std::string status_message = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::string body;

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
    enum class State { METHOD, PATH, VERSION, HEADER, HEADER_END, BODY, DONE };

    HttpParser();

    // 喂入数据，返回 true 表示解析完成（请求头结束）
    bool parse(const char* data, size_t len, HttpRequest& request,size_t& consumed);

    void reset();
private:
    State state_;
    std::string buffer_;
};