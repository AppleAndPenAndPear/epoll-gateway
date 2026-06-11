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