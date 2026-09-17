#include "http_parser.h"
#include <algorithm>

HttpParser::HttpParser(): state_(State::METHOD), content_length_(0), body_read_(0) {}

namespace {
// Header names are RFC 7230 tokens: tchar set only.
bool is_tchar(char c) {
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
        case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}

// Field values may contain visible chars, space and horizontal tab only.
bool is_valid_field_value_char(char c) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (uc == '\t') return true;
    return uc >= 0x20 && uc != 0x7F;
}

// Request-line characters must be printable ASCII (space is handled as a delimiter).
bool is_valid_request_line_char(char c) {
    const unsigned char uc = static_cast<unsigned char>(c);
    return uc > 0x20 && uc != 0x7F;
}
}  // namespace

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
                } else if (!is_valid_request_line_char(c)) {
                    fail(ParseError::INVALID_HEADER);
                    return false;
                } else {
                    buffer_ += c;
                    if (buffer_.size() > MAX_REQUEST_LINE_SIZE) {
                        fail(ParseError::REQUEST_LINE_TOO_LONG);
                        return false;
                    }
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
                } else if (!is_valid_request_line_char(c)) {
                    fail(ParseError::INVALID_HEADER);
                    return false;
                } else {
                    buffer_ += c;
                    if (buffer_.size() > MAX_REQUEST_LINE_SIZE) {
                        fail(ParseError::REQUEST_LINE_TOO_LONG);
                        return false;
                    }
                }
            break;
            case State::VERSION:
                if (c == '\r') {
                    // skip
                } else if (c == '\n') {
                    request.version = buffer_;
                    buffer_.clear();
                    state_ = State::HEADER;
                } else if (!is_valid_request_line_char(c)) {
                    fail(ParseError::INVALID_HEADER);
                    return false;
                } else {
                    buffer_ += c;
                    if (buffer_.size() > MAX_REQUEST_LINE_SIZE) {
                        fail(ParseError::REQUEST_LINE_TOO_LONG);
                        return false;
                    }
                }
            break;
            case State::QUERY:
                if (c == ' ') {
                    request.query = buffer_;
                    buffer_.clear();
                    state_ = State::VERSION; // query string ends; continue with the version
                } else if (!is_valid_request_line_char(c)) {
                    fail(ParseError::INVALID_HEADER);
                    return false;
                } else {
                    buffer_ += c;
                    if (buffer_.size() > MAX_REQUEST_LINE_SIZE) {
                        fail(ParseError::REQUEST_LINE_TOO_LONG);
                        return false;
                    }
                }
            break;
            case State::HEADER:
                if (c == '\r') {
                    // skip
                } else if (c == '\n') {
                    if (buffer_.empty()) {
                        // end of headers; determine the body type that follows
                        consumed = i + 1;   // count the current newline

                        // A Transfer-Encoding header takes over the body framing, but only
                        // chunked is supported; anything else is refused (smuggling vector).
                        auto it_te = request.headers.find("transfer-encoding");
                        if (it_te != request.headers.end()) {
                            if (it_te->second.find("chunked") == std::string::npos) {
                                fail(ParseError::INVALID_TRANSFER_ENCODING);
                                return false;
                            }
                            chunk_size_hex_buffer_.clear();
                            chunk_data_remaining_ = 0;
                            chunk_state_ = ChunkState::SIZE;
                            state_ = State::CHUNKED_BODY;
                            // do not return; keep parsing the chunked body in the loop
                        } else {
                            auto it = request.headers.find("content-length");
                            if (it != request.headers.end()) {
                                // Content-Length must be a plain digit string; parse manually
                                // to avoid exceptions and silent truncation.
                                const std::string& cl = it->second;
                                bool digits_ok = !cl.empty();
                                size_t cl_value = 0;
                                for (char ch : cl) {
                                    if (ch < '0' || ch > '9') { digits_ok = false; break; }
                                    if (cl_value > MAX_BODY_SIZE) { cl_value = MAX_BODY_SIZE + 1; break; }
                                    cl_value = cl_value * 10 + static_cast<size_t>(ch - '0');
                                }
                                if (!digits_ok) {
                                    fail(ParseError::INVALID_CONTENT_LENGTH);
                                    return false;
                                }
                                content_length_ = cl_value;
                                if (content_length_ > MAX_BODY_SIZE) {
                                    fail(ParseError::BODY_TOO_LARGE);
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
                        if (colon == std::string::npos) {
                            // malformed header line (no colon)
                            fail(ParseError::INVALID_HEADER);
                            return false;
                        }
                        std::string key = buffer_.substr(0, colon);
                        std::string value = buffer_.substr(colon + 1);
                        // trim leading/trailing whitespace
                        key.erase(0, key.find_first_not_of(" \t"));
                        key.erase(key.find_last_not_of(" \t") + 1);
                        value.erase(0, value.find_first_not_of(" \t"));
                        value.erase(value.find_last_not_of(" \t") + 1);
                        // lowercase the key to make lookups case-insensitive
                        std::transform(key.begin(), key.end(), key.begin(), ::tolower);

                        // Reject illegal characters in the header line
                        if (key.empty()) {
                            fail(ParseError::INVALID_HEADER);
                            return false;
                        }
                        for (char ch : key) {
                            if (!is_tchar(ch)) { fail(ParseError::INVALID_HEADER); return false; }
                        }
                        for (char ch : value) {
                            if (!is_valid_field_value_char(ch)) { fail(ParseError::INVALID_HEADER); return false; }
                        }

                        // Body framing headers are only allowed once and must not mix:
                        // duplicate Content-Length or CL together with TE is the classic
                        // request-smuggling vector.
                        if (key == "content-length") {
                            if (request.headers.count("content-length")) {
                                fail(ParseError::DUPLICATE_CONTENT_LENGTH);
                                return false;
                            }
                            if (request.headers.count("transfer-encoding")) {
                                fail(ParseError::CL_TE_CONFLICT);
                                return false;
                            }
                        } else if (key == "transfer-encoding") {
                            if (request.headers.count("content-length")) {
                                fail(ParseError::CL_TE_CONFLICT);
                                return false;
                            }
                        }

                        request.headers[key] = value;
                        buffer_.clear();
                    }
                }
                else {
                    buffer_ += c;
                    if (buffer_.size() > MAX_HEADER_LINE_SIZE) {
                        fail(ParseError::HEADER_TOO_LONG);
                        return false;
                    }
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
                                // parse the hex chunk size; only hex digits (plus an optional
                                // chunk extension after ';') are accepted
                                std::string hex = chunk_size_hex_buffer_;
                                const size_t semi = hex.find(';');
                                if (semi != std::string::npos) hex.erase(semi);
                                size_t size = 0;
                                bool hex_ok = !hex.empty();
                                for (char ch : hex) {
                                    int d;
                                    if (ch >= '0' && ch <= '9') d = ch - '0';
                                    else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
                                    else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
                                    else { hex_ok = false; break; }
                                    size = size * 16 + static_cast<size_t>(d);
                                    if (size > MAX_BODY_SIZE) break;
                                }
                                if (!hex_ok) {
                                    fail(ParseError::INVALID_CHUNK_SIZE);
                                    return false;
                                }
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
                            if (chunk_size_hex_buffer_.size() > 16) {
                                fail(ParseError::INVALID_CHUNK_SIZE);
                                return false;
                            }
                        }
                        break;
                    case ChunkState::DATA:
                        // consume a chunk data byte
                        request.body += c;
                        chunk_data_remaining_--;
                        if (request.body.size() >= MAX_BODY_SIZE) {
                            fail(ParseError::BODY_TOO_LARGE);
                            return false;
                        }
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
                    fail(ParseError::BODY_TOO_LARGE);
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
    parse_error_ = ParseError::NONE;
    content_length_ = 0;
    body_read_ = 0;
    chunk_size_hex_buffer_.clear();
    chunk_data_remaining_ = 0;
    chunk_state_ = ChunkState::SIZE;
    expecting_final_lf_ = false;
}
