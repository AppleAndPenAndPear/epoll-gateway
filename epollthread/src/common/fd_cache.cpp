#include "fd_cache.h"

FdCache::FdCache(size_t max_entries): max_entries_(max_entries) {

}

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

int FdCache::get(const std::string& path, time_t mtime) {
    auto it = map_.find(path);
    if (it == map_.end()) return -1;

    if (it->second.mtime != mtime) {
        // 文件已被修改，关闭旧 fd 并移除
        ::close(it->second.fd);
        list_.erase(it->second.lru_it);
        map_.erase(it);
        return -1;
    }

    // 移动到 LRU 头部
    list_.splice(list_.begin(), list_, it->second.lru_it);
    return it->second.fd;
}

void FdCache::put(const std::string& path, int fd, time_t mtime){
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
    map_[path] = {fd, mtime, list_.begin()};    
}