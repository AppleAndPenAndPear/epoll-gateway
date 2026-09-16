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
                    // hit the query string start delimiter
                    request.path = buffer_;      // everything before '?' is the pure path
                    buffer_.clear();
                    state_ = State::QUERY;       // switch to query string parsing
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
                    state_ = State::VERSION; // query string ends; continue with the version
                } else {
                    buffer_ += c;
                }
            break;
            case State::HEADER:
                if (c == '\r') {
                    // skip
                } else if (c == '\n') {
                    if (buffer_.empty()) {
                        // end of headers; determine the body type that follows
                        consumed = i + 1;   // count the current newline

                        // check Transfer-Encoding: chunked first
                        auto it_te = request.headers.find("transfer-encoding");
                        if (it_te != request.headers.end() && it_te->second.find("chunked") != std::string::npos) {
                            chunk_size_hex_buffer_.clear();
                            chunk_data_remaining_ = 0;
                            chunk_state_ = ChunkState::SIZE;
                            state_ = State::CHUNKED_BODY;
                            // do not return; keep parsing the chunked body in the loop
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
                                    // keep parsing the body in the loop
                                } else {
                                    state_ = State::DONE;
                                    return true;
                                }
                            } else {
                                state_ = State::DONE;
                                return true;   // no body; done immediately
                            }
                        }
                    } 
                    else {
                        // parse a header line
                        auto colon = buffer_.find(':');
                        if (colon != std::string::npos) {
                            std::string key = buffer_.substr(0, colon);
                            std::string value = buffer_.substr(colon + 1);
                            // trim leading/trailing whitespace
                            key.erase(0, key.find_first_not_of(" \t"));
                            key.erase(key.find_last_not_of(" \t") + 1);
                            value.erase(0, value.find_first_not_of(" \t"));
                            value.erase(value.find_last_not_of(" \t") + 1);
                            // lowercase the key to make lookups case-insensitive
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
                            // ignore the carriage return
                        }
                        else if (c == '\n') {
                            if (!chunk_size_hex_buffer_.empty()) {
                                // parse the hex chunk size
                                size_t size = std::stoul(chunk_size_hex_buffer_, nullptr, 16);
                                chunk_size_hex_buffer_.clear();
                                if (size == 0) {
                                    // last chunk; prepare to read the trailing CRLF
                                    chunk_state_ = ChunkState::TRAILER;
                                    expecting_final_lf_ = false;  // handled via the expecting_final_lf_ member
                                } else {
                                    chunk_data_remaining_ = size;
                                    chunk_state_ = ChunkState::DATA;
                                }
                            }
                            // if chunk_size_hex_buffer_ is empty, this is the LF of the CRLF that followed
                            // the previous chunk's data; just ignore it
                        }
                        else {
                            chunk_size_hex_buffer_ += c;
                        }
                        break;
                    case ChunkState::DATA:
                        // consume a chunk data byte
                        request.body += c;
                        chunk_data_remaining_--;
                        if (chunk_data_remaining_ == 0) {
                            // current chunk finished; handle its trailing CRLF next
                            chunk_state_ = ChunkState::SIZE;   // back to SIZE state: a following \r is ignored, \n is handled by SIZE
                        }
                        break;
                    case ChunkState::TRAILER:
                        // trailer after the last chunk (size=0); wait for \r\n
                        if (c == '\r') {
                            expecting_final_lf_ = true;
                        } else if (c == '\n' && expecting_final_lf_) {
                            // final CRLF seen; the whole chunked body is complete
                            state_ = State::DONE;
                            consumed = i + 1;
                            return true;
                        } else {
                            // possibly a trailer field; simply ignore it and reset the flag
                            expecting_final_lf_ = false;
                        }
                        break;
                    case ChunkState::DONE:
                        break;  // unreachable in practice: we switch from TRAILER straight to DONE
                }
                consumed = i + 1;  // in chunked mode every byte is consumed
                break;
            }
            case State::BODY:
                if (body_read_ >= MAX_BODY_SIZE) {
                    body_too_large_ = true;
                    return false; // reject an oversized body
                }
                // append the byte to request.body
                request.body += c;
                body_read_++;
                consumed++;
                if (body_read_ >= content_length_) {
                    state_ = State::DONE;
                    return true; // request fully parsed
                }
            break;
            case State::DONE:
            break;
        }
        if (state_ == State::DONE) break;
    }
    return false; // not yet fully parsed
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