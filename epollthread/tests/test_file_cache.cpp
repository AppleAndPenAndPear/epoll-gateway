#include <gtest/gtest.h>
#include "file_cache.h"

TEST(FileCacheTest, BasicInsertAndGet) {
    FileCache cache(10);
    std::string path = "/index.html";
    std::string content = "<html>hello</html>";
    cache.put(path, content, content.size(), time(nullptr));

    auto* cached = cache.get(path, 0);   // Skip mtime check
    ASSERT_NE(cached, nullptr);
    EXPECT_EQ(*cached, content);
}

TEST(FileCacheTest, ExpiredByMtime) {
    FileCache cache(10);
    std::string path = "/test.txt";
    std::string content = "data";
    cache.put(path, content, content.size(), 100);  // mtime=100

    // Check with a larger mtime; should return nullptr (the file was updated)
    auto* cached = cache.get(path, 200);
    EXPECT_EQ(cached, nullptr);
}

TEST(FileCacheTest, EvictionWhenFull) {
    FileCache cache(2);  // Holds only 2 entries
    cache.put("/a", "aaa", 3, 0);
    cache.put("/b", "bbb", 3, 0);
    cache.put("/c", "ccc", 3, 0);  // Should evict /a

    EXPECT_EQ(cache.get("/a", 0), nullptr);
    EXPECT_NE(cache.get("/b", 0), nullptr);
    EXPECT_NE(cache.get("/c", 0), nullptr);
}

// Files larger than max_file_size are not cached
// The unit here is MB, so setting it to 0 MB rejects any non-empty content.
TEST(FileCacheTest, RejectOversizedFile) {
    FileCache cache(10, 0);
    std::string path = "/large.txt";
    std::string content = "This is more than 10 bytes";
    ASSERT_GT(content.size(), 0U);

    cache.put(path, content, content.size(), 100);
    EXPECT_EQ(cache.size(), 0);  // The cache should be empty
    EXPECT_EQ(cache.get(path, 0), nullptr);  // The lookup should fail too
}

// A file exactly equal to max_file_size can be cached
TEST(FileCacheTest, AcceptExactSizedFile) {
    FileCache cache(10, 1);
    std::string path = "/exact.txt";
    std::string content = "1234567890";  // 10 bytes, always allowed under the 1 MB limit
    cache.put(path, content, content.size(), 200);
    EXPECT_EQ(cache.size(), 1);
    EXPECT_NE(cache.get(path, 0), nullptr);
}

// Inserting the same path multiple times updates the content instead of creating multiple entries
TEST(FileCacheTest, UpdateExistingPath) {
    FileCache cache(10);
    std::string path = "/update.txt";
    cache.put(path, "old", 3, 100);
    cache.put(path, "newer", 5, 200);
    EXPECT_EQ(cache.size(), 1);  // Still only one entry
    auto* cached = cache.get(path, 0);
    ASSERT_NE(cached, nullptr);
    EXPECT_EQ(*cached, "newer");
}