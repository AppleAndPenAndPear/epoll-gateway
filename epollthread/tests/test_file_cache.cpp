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

// 测试超过 max_file_size 的文件不会被缓存
// 这里的单位是 MB，因此设置为 0 MB 会拒绝任何非空内容。
TEST(FileCacheTest, RejectOversizedFile) {
    FileCache cache(10, 0);
    std::string path = "/large.txt";
    std::string content = "This is more than 10 bytes";
    ASSERT_GT(content.size(), 0U);

    cache.put(path, content, content.size(), 100);
    EXPECT_EQ(cache.size(), 0);  // 缓存应该为空
    EXPECT_EQ(cache.get(path, 0), nullptr);  // 获取也应该失败
}

// 测试正好等于 max_file_size 的文件可以被缓存
TEST(FileCacheTest, AcceptExactSizedFile) {
    FileCache cache(10, 1);
    std::string path = "/exact.txt";
    std::string content = "1234567890";  // 10 bytes，1 MB 上限下必然允许
    cache.put(path, content, content.size(), 200);
    EXPECT_EQ(cache.size(), 1);
    EXPECT_NE(cache.get(path, 0), nullptr);
}

// 测试多次插入同一个 path 会更新内容，而不是创建多个条目
TEST(FileCacheTest, UpdateExistingPath) {
    FileCache cache(10);
    std::string path = "/update.txt";
    cache.put(path, "old", 3, 100);
    cache.put(path, "newer", 5, 200);
    EXPECT_EQ(cache.size(), 1);  // 仍然只有一个条目
    auto* cached = cache.get(path, 0);
    ASSERT_NE(cached, nullptr);
    EXPECT_EQ(*cached, "newer");
}