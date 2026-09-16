#pragma once
#include <string>
#include <unordered_map>
#include <list>
#include <sys/stat.h>

struct CacheEntry {
    std::string content;   // file content
    size_t size;           // file size in bytes
    time_t mtime;          // file modification time (optional, used for cache invalidation)
};

class FileCache {
private:
    size_t max_entries_;    // max number of cache entries
    size_t max_file_size_;  // max file size in bytes
    using CacheItem = std::pair<std::string, CacheEntry>;
    std::list<CacheItem> list_;    // list holding the actual cache data, most recently used at the front
    std::unordered_map<std::string, decltype(list_)::iterator> map_;    // hash map from path to list iterator
public:
    explicit FileCache(size_t max_entries = 1024, size_t max_file_size_mb = 1);

    // Look up the cache; returns a pointer to the content if present and still valid
    const std::string* get(const std::string& path, time_t file_mtime);

    // Insert into the cache (updates and moves to the front if already present)
    void put(const std::string& path, const std::string& content, size_t size, time_t mtime);

    // Clear all cached entries
    void clear();

    // Get the current number of cached entries
    size_t size() const;

    size_t max_file_size() const;
};
