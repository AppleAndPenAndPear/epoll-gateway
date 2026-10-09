#include <gtest/gtest.h>
#include "http_parser.h"

TEST(HttpParserTest, ParseSimpleGetRequest) {
    HttpParser parser;
    HttpRequest request;
    size_t consumed = 0;
    std::string raw = "GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n";

    bool done = parser.parse(raw.data(), raw.size(), request, consumed);
    EXPECT_TRUE(done);
    EXPECT_EQ(request.method, "GET");
    EXPECT_EQ(request.path, "/index.html");
    EXPECT_EQ(request.version, "HTTP/1.1");
    EXPECT_EQ(request.headers.size(), 1);
    EXPECT_EQ(request.headers["host"], "localhost");
}

TEST(HttpParserTest, ParsePostWithBody) {
    HttpParser parser;
    HttpRequest request;
    size_t consumed = 0;
    std::string raw = "POST /api HTTP/1.1\r\nContent-Length: 5\r\n\r\nHello";

    bool done = parser.parse(raw.data(), raw.size(), request, consumed);
    EXPECT_TRUE(done);
    EXPECT_EQ(request.method, "POST");
    EXPECT_EQ(request.body, "Hello");
}

TEST(HttpParserTest, ParseQueryString) {
    HttpParser parser;
    HttpRequest request;
    size_t consumed = 0;
    std::string raw = "GET /search?q=test HTTP/1.1\r\n\r\n";

    bool done = parser.parse(raw.data(), raw.size(), request, consumed);
    EXPECT_TRUE(done);
    EXPECT_EQ(request.path, "/search");
    EXPECT_EQ(request.query, "q=test");
}

    TEST(HttpParserTest, ParsesFragmentedRequestWhenConsumedBytesAreRemoved) {
        HttpParser parser;
        HttpRequest request;
        std::string buffered;
        const std::string raw =
            "POST /api/fragmented?token=value HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
            "hello";
        bool done = false;

        for (size_t offset = 0; offset < raw.size(); offset += 3) {
            buffered.append(raw, offset, 3);
            size_t consumed = 0;
            done = parser.parse(buffered.data(), buffered.size(), request, consumed);
            ASSERT_LE(consumed, buffered.size());
            buffered.erase(0, consumed);
            if (done) break;
        }

        ASSERT_TRUE(done);
        EXPECT_TRUE(buffered.empty());
        EXPECT_EQ(request.method, "POST");
        EXPECT_EQ(request.path, "/api/fragmented");
        EXPECT_EQ(request.query, "token=value");
        EXPECT_EQ(request.body, "hello");
    }

// ─── Request smuggling prevention ───

namespace {
// Feeds a raw request and returns the parse outcome + error category.
struct ParseOutcome {
    bool done;
    HttpParser::ParseError error;
};

ParseOutcome parse_raw(const std::string& raw) {
    HttpParser parser;
    HttpRequest request;
    size_t consumed = 0;
    const bool done = parser.parse(raw.data(), raw.size(), request, consumed);
    return {done, parser.error()};
}
}  // namespace

TEST(HttpParserSmugglingTest, AcceptsSingleContentLength) {
    auto out = parse_raw("POST /api HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nHello");
    EXPECT_TRUE(out.done);
    EXPECT_EQ(out.error, HttpParser::ParseError::NONE);
}

TEST(HttpParserSmugglingTest, RejectsDuplicateContentLength) {
    // Conflicting values are the classic smuggling vector; identical duplicates
    // are refused too, per the P2 hardening decision.
    auto out = parse_raw("POST /api HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 10\r\n\r\nHelloXXXXX");
    EXPECT_FALSE(out.done);
    EXPECT_EQ(out.error, HttpParser::ParseError::DUPLICATE_CONTENT_LENGTH);
}

TEST(HttpParserSmugglingTest, RejectsIdenticalDuplicateContentLength) {
    auto out = parse_raw("POST /api HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nHello");
    EXPECT_FALSE(out.done);
    EXPECT_EQ(out.error, HttpParser::ParseError::DUPLICATE_CONTENT_LENGTH);
}

TEST(HttpParserSmugglingTest, RejectsContentLengthWithTransferEncoding) {
    auto out = parse_raw("POST /api HTTP/1.1\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    EXPECT_FALSE(out.done);
    EXPECT_EQ(out.error, HttpParser::ParseError::CL_TE_CONFLICT);
}

TEST(HttpParserSmugglingTest, RejectsTransferEncodingWithContentLength) {
    // TE arrives first; CL must still be refused
    auto out = parse_raw("POST /api HTTP/1.1\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n0\r\n\r\n");
    EXPECT_FALSE(out.done);
    EXPECT_EQ(out.error, HttpParser::ParseError::CL_TE_CONFLICT);
}

TEST(HttpParserSmugglingTest, RejectsNonChunkedTransferEncoding) {
    auto out = parse_raw("POST /api HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n");
    EXPECT_FALSE(out.done);
    EXPECT_EQ(out.error, HttpParser::ParseError::INVALID_TRANSFER_ENCODING);
}

TEST(HttpParserSmugglingTest, RejectsTransferEncodingContainingChunkedToken) {
    auto out = parse_raw("POST /api HTTP/1.1\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n");
    EXPECT_FALSE(out.done);
    EXPECT_EQ(out.error, HttpParser::ParseError::INVALID_TRANSFER_ENCODING);
}

TEST(HttpParserSmugglingTest, AcceptsCaseInsensitiveChunkedTransferEncoding) {
    auto out = parse_raw("POST /api HTTP/1.1\r\nTransfer-Encoding: Chunked\r\n\r\n0\r\n\r\n");
    EXPECT_TRUE(out.done);
    EXPECT_EQ(out.error, HttpParser::ParseError::NONE);
}

TEST(HttpParserSmugglingTest, RejectsDuplicateTransferEncoding) {
    auto out = parse_raw("POST /api HTTP/1.1\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    EXPECT_FALSE(out.done);
    EXPECT_EQ(out.error, HttpParser::ParseError::INVALID_TRANSFER_ENCODING);
}

TEST(HttpParserSmugglingTest, RejectsOversizedHeaderBlock) {
    const std::string header = "X: a\r\n";
    std::string header_block;
    while (header_block.size() <= HttpParser::MAX_HEADERS_SIZE) header_block += header;
    const std::string raw = "GET / HTTP/1.1\r\n" + header_block + "\r\n";
    auto out = parse_raw(raw);
    EXPECT_FALSE(out.done);
    EXPECT_EQ(out.error, HttpParser::ParseError::HEADERS_TOO_LARGE);
}

TEST(HttpParserSmugglingTest, RejectsNonNumericContentLength) {
    auto out = parse_raw("POST /api HTTP/1.1\r\nContent-Length: 5x\r\n\r\nHello");
    EXPECT_FALSE(out.done);
    EXPECT_EQ(out.error, HttpParser::ParseError::INVALID_CONTENT_LENGTH);
}

TEST(HttpParserSmugglingTest, RejectsHeaderValueWithControlChar) {
    std::string raw = "GET / HTTP/1.1\r\nX-Evil: bad\x01value\r\n\r\n";
    auto out = parse_raw(raw);
    EXPECT_FALSE(out.done);
    EXPECT_EQ(out.error, HttpParser::ParseError::INVALID_HEADER);
}

TEST(HttpParserSmugglingTest, RejectsInvalidHeaderNameChar) {
    std::string raw = "GET / HTTP/1.1\r\nX Ev(l: value\r\n\r\n";
    auto out = parse_raw(raw);
    EXPECT_FALSE(out.done);
    EXPECT_EQ(out.error, HttpParser::ParseError::INVALID_HEADER);
}

TEST(HttpParserSmugglingTest, RejectsHeaderLineWithoutColon) {
    auto out = parse_raw("GET / HTTP/1.1\r\nBrokenHeaderLine\r\n\r\n");
    EXPECT_FALSE(out.done);
    EXPECT_EQ(out.error, HttpParser::ParseError::INVALID_HEADER);
}

TEST(HttpParserSmugglingTest, RejectsControlCharInRequestLine) {
    std::string raw = std::string("GET /\x01 HTTP/1.1\r\n\r\n");
    auto out = parse_raw(raw);
    EXPECT_FALSE(out.done);
    EXPECT_EQ(out.error, HttpParser::ParseError::INVALID_HEADER);
}

TEST(HttpParserSmugglingTest, RejectsOverlongRequestLine) {
    std::string raw = "GET /" + std::string(HttpParser::MAX_REQUEST_LINE_SIZE + 16, 'a') + " HTTP/1.1\r\n\r\n";
    auto out = parse_raw(raw);
    EXPECT_FALSE(out.done);
    EXPECT_EQ(out.error, HttpParser::ParseError::REQUEST_LINE_TOO_LONG);
}

TEST(HttpParserSmugglingTest, RejectsOverlongHeaderLine) {
    std::string raw = "GET / HTTP/1.1\r\nX-Big: " + std::string(HttpParser::MAX_HEADER_LINE_SIZE + 16, 'a') + "\r\n\r\n";
    auto out = parse_raw(raw);
    EXPECT_FALSE(out.done);
    EXPECT_EQ(out.error, HttpParser::ParseError::HEADER_TOO_LONG);
}

TEST(HttpParserSmugglingTest, RejectsNonHexChunkSize) {
    auto out = parse_raw("POST /api HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nDATA\r\n0\r\n\r\n");
    EXPECT_FALSE(out.done);
    EXPECT_EQ(out.error, HttpParser::ParseError::INVALID_CHUNK_SIZE);
}

TEST(HttpParserSmugglingTest, StillAcceptsValidChunkedRequest) {
    auto out = parse_raw("POST /api HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nHello\r\n0\r\n\r\n");
    EXPECT_TRUE(out.done);
    EXPECT_EQ(out.error, HttpParser::ParseError::NONE);
}

TEST(HttpParserSmugglingTest, AcceptsChunkedBodyAtMaximumSize) {
    const std::string body(HttpParser::MAX_BODY_SIZE, 'x');
    const std::string raw = std::string("POST /api HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n") +
                            "100000\r\n" + body + "\r\n0\r\n\r\n";
    HttpParser parser;
    HttpRequest request;
    size_t consumed = 0;
    EXPECT_TRUE(parser.parse(raw.data(), raw.size(), request, consumed));
    EXPECT_EQ(parser.error(), HttpParser::ParseError::NONE);
    EXPECT_EQ(request.body.size(), HttpParser::MAX_BODY_SIZE);
}

TEST(HttpParserSmugglingTest, RejectsChunkedBodyOneByteOverMaximum) {
    const std::string body(HttpParser::MAX_BODY_SIZE + 1, 'x');
    const std::string raw = std::string("POST /api HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n") +
                            "100001\r\n" + body + "\r\n0\r\n\r\n";
    auto out = parse_raw(raw);
    EXPECT_FALSE(out.done);
    EXPECT_EQ(out.error, HttpParser::ParseError::BODY_TOO_LARGE);
}

TEST(HttpParserSmugglingTest, ResetClearsErrorState) {
    HttpParser parser;
    HttpRequest request;
    size_t consumed = 0;
    std::string bad = "POST / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n";
    parser.parse(bad.data(), bad.size(), request, consumed);
    EXPECT_NE(parser.error(), HttpParser::ParseError::NONE);
    parser.reset();
    request.clear();  // the handler clears the request between keep-alive requests
    EXPECT_EQ(parser.error(), HttpParser::ParseError::NONE);

    std::string good = "GET / HTTP/1.1\r\n\r\n";
    EXPECT_TRUE(parser.parse(good.data(), good.size(), request, consumed));
}