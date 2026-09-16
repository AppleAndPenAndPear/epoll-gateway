#include <gtest/gtest.h>
#include "http_parser.h"  // Includes HttpResponse (path may need adjusting)

// If HttpResponse is defined in a separate header, include that header here
TEST(HttpResponseTest, Serialization) {
    HttpResponse resp;
    resp.status_code = 200;
    resp.status_message = "OK";
    resp.headers["Content-Type"] = "text/plain";
    resp.body = "Hello";
    resp.headers["Content-Length"] = std::to_string(resp.body.size());

    std::string raw = resp.to_string();
    EXPECT_TRUE(raw.find("HTTP/1.1 200 OK") != std::string::npos);
    EXPECT_TRUE(raw.find("Content-Type: text/plain") != std::string::npos);
    EXPECT_TRUE(raw.find("Content-Length: 5") != std::string::npos);
    EXPECT_TRUE(raw.find("Hello") != std::string::npos);
}