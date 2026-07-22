#pragma once
#include <unistd.h>
#include <sys/stat.h>
#include <list>
#include <string>
#include <unordered_map>

// 文件描述符缓存，通过 TTL 机制避免每次请求都 stat()。
// 典型用法：
//   1. try_get() — 快速路径，TLT 内直接返回 fd+size+mtime，零系统调用
//   2. validate() — TTL 过期后，调用方 stat() 确认 mtime 来验证缓存
//   3. put() — 首次插入
class FdCache {
public:
    // ttl_sec: 跳过 stat() 验证的最大时间（秒）
    explicit FdCache(size_t max_entries = 256, int ttl_sec = 10);
    ~FdCache();

    void clear();
    size_t size() const;

    // 禁止拷贝和移动
    FdCache(const FdCache&) = delete;
    FdCache& operator=(const FdCache&) = delete;

    // 快速获取：缓存命中且在 TTL 内 → 返回 fd 并填充 size/mtime
    // 返回 -1 表示未命中或已过期（调用方需走 stat+validate 路径）
    int try_get(const std::string& path, time_t now, off_t* file_size, time_t* mtime);

    // 验证已有条目：调用方已 stat() 拿到真实 mtime，验证缓存是否依然有效。
    // 有效则更新 validated_at 并返回 fd；无效则淘汰并返回 -1。
    int validate(const std::string& path, time_t mtime);

    // 插入新条目。如果已满，淘汰最久未使用的 fd 并关闭。
    void put(const std::string& path, int fd, time_t mtime, off_t file_size);
private:
    struct CachedFd {
        int fd;
        off_t file_size;
        time_t mtime;         // 文件修改时间
        time_t validated_at;  // 上次 stat() 验证时间
        std::list<std::string>::iterator lru_it;
    };

    size_t max_entries_;
    int ttl_sec_;
    std::list<std::string> list_;  // LRU 链表，存路径
    std::unordered_map<std::string, CachedFd> map_;
};