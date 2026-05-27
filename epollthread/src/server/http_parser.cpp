#include "http_parser.h"
#include <algorithm>

HttpParser::HttpParser(): state_(State::METHOD){}

bool HttpParser::parse(const char* data, size_t len, HttpRequest& request,size_t& consumed) {
    for (size_t i = 0; i < len; ++i) {
        char c = data[i];
        switch (state_) {
            case State::METHOD:
                if (c == ' ') {
                    request.method = buffer_;
                    buffer_.clear();
                    state_ = State::PATH;
                } else {
                    buffer_ += c;
                }
            break;
            case State::PATH:
                if (c == ' ') {
                    request.path = buffer_;
                    buffer_.clear();
                    state_ = State::VERSION;
                } else {
                    buffer_ += c;
                }
            break;
            case State::VERSION:
                if (c == '\r') {
                    // skip
                } else if (c == '\n') {
                    request.version = buffer_;
                    buffer_.clear();
                    state_ = State::HEADER;
                } else {
                    buffer_ += c;
                }
            break;
            case State::HEADER:
                if (c == '\r') {
                    // skip
                } else if (c == '\n') {
                    if (buffer_.empty()) {
                        state_ = State::HEADER_END;
                        consumed = i + 1;   //本次调用消费的字节数（到尾部 \n）
                        return true; // 头结束
                    } else {
                        // 解析头部行
                        auto colon = buffer_.find(':');
                        if (colon != std::string::npos) {
                            std::string key = buffer_.substr(0, colon);
                            std::string value = buffer_.substr(colon + 1);
                            // 去除首尾空格
                            key.erase(0, key.find_first_not_of(" \t"));
                            key.erase(key.find_last_not_of(" \t") + 1);
                            value.erase(0, value.find_first_not_of(" \t"));
                            value.erase(value.find_last_not_of(" \t") + 1);
                            request.headers[key] = value;
                        }
                        buffer_.clear();
                    }
                } else {
                    buffer_ += c;
                }
            break;
            case State::HEADER_END:
                // 不应再进入
            break;
            case State::BODY:
            case State::DONE:
            break;
        }
    }
    return false; // 还没解析完
}

void HttpParser::reset(){
    state_ = State::METHOD;
    buffer_.clear();
}