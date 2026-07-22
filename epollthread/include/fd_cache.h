#pragma once
#include <unistd.h>
#include <sys/stat.h>
#include <list>
#include <string>
#include <unordered_map>

class FdCache {
public:
    explicit FdCache(size_t max_entries = 256);
    ~FdCache();

    void clear();

    size_t size() const;

    // 禁止拷贝和移动
    FdCache(const FdCache&) = delete;
    FdCache& operator=(const FdCache&) = delete;

    // 获取缓存的 fd，如果文件未被修改。返回 fd，若未命中或无效则返回 -1。
    int get(const std::string& path, time_t mtime);

    // 插入缓存。如果已满，淘汰最久未使用的 fd 并关闭。
    void put(const std::string& path, int fd, time_t mtime);
private:
    struct CachedFd {
        int fd;
        time_t mtime;
        std::list<std::string>::iterator lru_it;
    };

    size_t max_entries_;
    std::list<std::string> list_;  // LRU 链表，存路径
    std::unordered_map<std::string, CachedFd> map_;
};