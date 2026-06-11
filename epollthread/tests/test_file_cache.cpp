#include <gtest/gtest.h>
#include "file_cache.h"

TEST(FileCacheTest, BasicInsertAndGet) {
    FileCache cache(10);
    std::string path = "/index.html";
    std::string content = "<html>hello</html>";
    cache.put(path, content, content.size(), time(nullptr));

    auto* cached = cache.get(path, 0);   // 不检查 mtime
    ASSERT_NE(cached, nullptr);
    EXPECT_EQ(*cached, content);
}

TEST(FileCacheTest, ExpiredByMtime) {
    FileCache cache(10);
    std::string path = "/test.txt";
    std::string content = "data";
    cache.put(path, content, content.size(), 100);  // mtime=100

    // 用更大的 mtime 检查，应返回 nullptr（文件已更新）
    auto* cached = cache.get(path, 200);
    EXPECT_EQ(cached, nullptr);
}

TEST(FileCacheTest, EvictionWhenFull) {
    FileCache cache(2);  // 只能存 2 个条目
    cache.put("/a", "aaa", 3, 0);
    cache.put("/b", "bbb", 3, 0);
    cache.put("/c", "ccc", 3, 0);  // 应淘汰 /a

    EXPECT_EQ(cache.get("/a", 0), nullptr);
    EXPECT_NE(cache.get("/b", 0), nullptr);
    EXPECT_NE(cache.get("/c", 0), nullptr);
}