#include <gtest/gtest.h>
#include "http_parser.h"  // 包含 HttpResponse（可能需调整路径）

// 如果你的 HttpResponse 定义在单独头文件，请包含相应头文件
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