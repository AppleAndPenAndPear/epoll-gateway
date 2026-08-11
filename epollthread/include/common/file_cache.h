#pragma once
#include <string>
#include <unordered_map>
#include <list>
#include <sys/stat.h>

struct CacheEntry {
    std::string content;   // 文件内容
    size_t size;           // 文件大小（字节）
    time_t mtime;          // 文件修改时间（用于缓存失效，可选）
};

class FileCache {
private:
    size_t max_entries_;    // 最大缓存条目数
    size_t max_file_size_;  // 最大文件大小（字节）
    using CacheItem = std::pair<std::string, CacheEntry>;
    std::list<CacheItem> list_;    // 链表存储实际缓存数据，头部为最近使用
    std::unordered_map<std::string, decltype(list_)::iterator> map_;    // 哈希表映射路径到链表迭代器
public:
    explicit FileCache(size_t max_entries = 1024, size_t max_file_size_mb = 1);

    // 查找缓存，返回指向内容的指针（如果存在且未失效）
    const std::string* get(const std::string& path, time_t file_mtime);

    // 插入缓存（如果已存在则更新，并移动到头部）
    void put(const std::string& path, const std::string& content, size_t size, time_t mtime);

    // 清除所有缓存
    void clear();

    // 获取当前缓存条目数
    size_t size() const;

    size_t max_file_size() const;
};
