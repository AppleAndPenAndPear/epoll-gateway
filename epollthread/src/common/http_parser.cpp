#include "http_parser.h"
#include <algorithm>

HttpParser::HttpParser(): state_(State::METHOD), content_length_(0), body_read_(0), body_too_large_(false) {}

bool HttpParser::parse(const char* data, size_t len, HttpRequest& request,size_t& consumed) {
    consumed = 0;
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
                } 
                else if (c == '?') {
                    // 遇到查询字符串起始符
                    request.path = buffer_;      // 前面的就是纯路径
                    buffer_.clear();
                    state_ = State::QUERY;       // 进入查询字符串解析状态
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
            case State::QUERY:
                if (c == ' ') {
                    request.query = buffer_;
                    buffer_.clear();
                    state_ = State::VERSION; // 查询字符串结束，继续解析版本
                } else {
                    buffer_ += c;
                }
            break;
            case State::HEADER:
                if (c == '\r') {
                    // skip
                } else if (c == '\n') {
                    if (buffer_.empty()) {
                        // 头部结束，判断后续 body 类型
                        consumed = i + 1;   // 计入当前换行符

                        // 优先检查 Transfer-Encoding: chunked
                        auto it_te = request.headers.find("transfer-encoding");
                        if (it_te != request.headers.end() && it_te->second.find("chunked") != std::string::npos) {
                            chunk_size_hex_buffer_.clear();
                            chunk_data_remaining_ = 0;
                            chunk_state_ = ChunkState::SIZE;
                            state_ = State::CHUNKED_BODY;
                            // 不返回，继续循环解析 chunked body
                        } else {
                            auto it = request.headers.find("content-length");
                            if (it != request.headers.end()) {
                                content_length_ = std::stoul(it->second);
                                if (content_length_ > MAX_BODY_SIZE) {
                                    body_too_large_ = true;
                                    return false;
                                }
                                if (content_length_ > 0) {
                                    body_read_ = 0;
                                    state_ = State::BODY;
                                    // 继续循环解析 body
                                } else {
                                    state_ = State::DONE;
                                    return true;
                                }
                            } else {
                                state_ = State::DONE;
                                return true;   // 无 body，直接完成
                            }
                        }
                    } 
                    else {
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
                            // 统一转小写，确保大小写不敏感
                            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
                            request.headers[key] = value;
                        }
                        buffer_.clear();
                    }
                } 
                else {
                    buffer_ += c;
                }
            break;
            case State::CHUNKED_BODY: {
                switch (chunk_state_) {
                    case ChunkState::SIZE:
                        if (c == '\r') {
                            // 忽略回车
                        }
                        else if (c == '\n') {
                            if (!chunk_size_hex_buffer_.empty()) {
                                // 解析十六进制大小
                                size_t size = std::stoul(chunk_size_hex_buffer_, nullptr, 16);
                                chunk_size_hex_buffer_.clear();
                                if (size == 0) {
                                    // 最后一个 chunk，准备读取尾部 CRLF
                                    chunk_state_ = ChunkState::TRAILER;
                                    expecting_final_lf_ = false;  // 需要添加这个 bool 成员
                                } else {
                                    chunk_data_remaining_ = size;
                                    chunk_state_ = ChunkState::DATA;
                                }
                            }
                            // 如果 chunk_size_hex_buffer_ 为空，说明是前面 chunk 数据结束后的 CRLF 中的换行，直接忽略
                        }
                        else {
                            chunk_size_hex_buffer_ += c;
                        }
                        break;
                    case ChunkState::DATA:
                        // 读取 chunk 数据字节
                        request.body += c;
                        chunk_data_remaining_--;
                        if (chunk_data_remaining_ == 0) {
                            // 当前 chunk 数据读完，准备处理末尾的 CRLF
                            chunk_state_ = ChunkState::SIZE;   // 返回 SIZE 状态，下一个字符如果是 \r 会被忽略，\n 会被 SIZE 处理
                        }
                        break;
                    case ChunkState::TRAILER:
                        // 最后一个 chunk (size=0) 后的尾部，等待 \r\n
                        if (c == '\r') {
                            expecting_final_lf_ = true;
                        } else if (c == '\n' && expecting_final_lf_) {
                            // 最后的 CRLF 结束，整个 chunked body 完成
                            state_ = State::DONE;
                            consumed = i + 1;
                            return true;
                        } else {
                            // 可能是 trailer 字段，我们简单忽略并重置标志
                            expecting_final_lf_ = false;
                        }
                        break;
                    case ChunkState::DONE:
                        break;  // 理论上不会发生,因为我们在 TRAILER 状态下直接切换到 DONE
                }
                consumed = i + 1;  // chunked 模式下每个字节都已被消费
                break;
            }
            case State::BODY:
                if (body_read_ >= MAX_BODY_SIZE) {
                    body_too_large_ = true;
                    return false; // 拒绝 oversized body
                }
                // 将数据追加到 request.body
                request.body += c;
                body_read_++;
                consumed++;
                if (body_read_ >= content_length_) {
                    state_ = State::DONE;
                    return true; // 请求完全解析
                }
            break;
            case State::DONE:
            break;
        }
        if (state_ == State::DONE) break;
    }
    return false; // 还没解析完
}

void HttpParser::reset(){
    state_ = State::METHOD;
    buffer_.clear();
    body_too_large_ = false;
    content_length_ = 0;
    body_read_ = 0;
    chunk_size_hex_buffer_.clear();
    chunk_data_remaining_ = 0;
    chunk_state_ = ChunkState::SIZE;
    expecting_final_lf_ = false;
}