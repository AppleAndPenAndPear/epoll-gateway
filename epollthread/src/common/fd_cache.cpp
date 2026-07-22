#include "fd_cache.h"
#include <time.h>

FdCache::FdCache(size_t max_entries, int ttl_sec)
    : max_entries_(max_entries), ttl_sec_(ttl_sec) {}

FdCache::~FdCache(){
    clear();
}

void FdCache::clear(){
    for (auto& [path, entry] : map_) {
        ::close(entry.fd);
    }
    map_.clear();
    list_.clear();
}

size_t FdCache::size() const { 
    return list_.size(); 
}

int FdCache::try_get(const std::string& path, time_t now, off_t* file_size, time_t* mtime) {
    auto it = map_.find(path);
    if (it == map_.end()) return -1;

    // TTL 未过期 → 直接返回，零系统调用
    if (now - it->second.validated_at < ttl_sec_) {
        list_.splice(list_.begin(), list_, it->second.lru_it);
        if (file_size) *file_size = it->second.file_size;
        if (mtime) *mtime = it->second.mtime;
        return it->second.fd;
    }
    return -1;  // 已过期，调用方需 stat + validate
}

int FdCache::validate(const std::string& path, time_t mtime) {
    auto it = map_.find(path);
    if (it == map_.end()) return -1;

    if (it->second.mtime != mtime) {
        // 文件已被修改，关闭旧 fd 并移除
        ::close(it->second.fd);
        list_.erase(it->second.lru_it);
        map_.erase(it);
        return -1;
    }

    // 验证通过，更新时间戳
    it->second.validated_at = time(nullptr);
    list_.splice(list_.begin(), list_, it->second.lru_it);
    return it->second.fd;
}

void FdCache::put(const std::string& path, int fd, time_t mtime, off_t file_size){
    // 如果已存在，先移除旧的
    auto it = map_.find(path);
    if (it != map_.end()) {
        ::close(it->second.fd);
        list_.erase(it->second.lru_it);
        map_.erase(it);
    }

    // 淘汰最旧条目
    while (list_.size() >= max_entries_) {
        auto& last_path = list_.back();
        auto last_it = map_.find(last_path);
        if (last_it != map_.end()) {
            ::close(last_it->second.fd);
            map_.erase(last_it);
        }
        list_.pop_back();
    }

    // 插入新条目
    list_.push_front(path);
    map_[path] = {fd, file_size, mtime, time(nullptr), list_.begin()};
}