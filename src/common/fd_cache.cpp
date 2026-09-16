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

    // TTL still valid -> return directly, zero syscalls
    if (now - it->second.validated_at < ttl_sec_) {
        list_.splice(list_.begin(), list_, it->second.lru_it);
        if (file_size) *file_size = it->second.file_size;
        if (mtime) *mtime = it->second.mtime;
        return it->second.fd;
    }
    return -1;  // expired; caller must stat + validate
}

int FdCache::validate(const std::string& path, time_t mtime) {
    auto it = map_.find(path);
    if (it == map_.end()) return -1;

    if (it->second.mtime != mtime) {
        // file was modified; close the old fd and remove the entry
        ::close(it->second.fd);
        list_.erase(it->second.lru_it);
        map_.erase(it);
        return -1;
    }

    // validation passed; refresh the timestamp
    it->second.validated_at = time(nullptr);
    list_.splice(list_.begin(), list_, it->second.lru_it);
    return it->second.fd;
}

void FdCache::put(const std::string& path, int fd, time_t mtime, off_t file_size){
    // if already present, remove the old entry first
    auto it = map_.find(path);
    if (it != map_.end()) {
        ::close(it->second.fd);
        list_.erase(it->second.lru_it);
        map_.erase(it);
    }

    // evict the oldest entries
    while (list_.size() >= max_entries_) {
        auto& last_path = list_.back();
        auto last_it = map_.find(last_path);
        if (last_it != map_.end()) {
            ::close(last_it->second.fd);
            map_.erase(last_it);
        }
        list_.pop_back();
    }

    // insert the new entry
    list_.push_front(path);
    map_[path] = {fd, file_size, mtime, time(nullptr), list_.begin()};
}