#include "file_cache.h"

FileCache::FileCache(size_t max_entries, size_t max_file_size_mb) : max_entries_(max_entries), max_file_size_(max_file_size_mb * 1024 * 1024) {
}

const std::string* FileCache::get(const std::string& path, time_t file_mtime) {
    auto it = map_.find(path);
    if (it != map_.end()) {
        // cache hit; check whether it is stale
        auto& entry = *(it->second);
        if (file_mtime == 0 || entry.second.mtime == file_mtime) {
            // cache valid (file_mtime==0 skips the check); move to the front of the list and return the content
            list_.splice(list_.begin(), list_, it->second);
            return &entry.second.content;
        } else {
            // cache stale; remove the old entry
            list_.erase(it->second);
            map_.erase(it);
        }
    }
    // cache miss or stale
    return nullptr;
}


void FileCache::put(const std::string& path, const std::string& content, size_t size, time_t mtime){
    // reject files larger than the size limit
    if (size > max_file_size_) {
        return;
    }
    auto it = map_.find(path);
    if (it != map_.end()) {
        // already present; update the content and move to the front
        it->second->second.content = content;
        it->second->second.size = size;
        it->second->second.mtime = mtime;
        list_.splice(list_.begin(), list_, it->second);
        return;
    }
    // if the cache is full, evict the least recently used entry (list back)
    if (list_.size() >= max_entries_) {
        auto last = list_.back();
        map_.erase(last.first);  // would need CacheEntry to store the path, or erase via the iterator
        list_.pop_back();
    }
    // insert the new entry at the front of the list
    list_.emplace_front(path,CacheEntry{content, size, mtime});
    map_[path] = list_.begin();
}

void FileCache::clear() {
    list_.clear();
    map_.clear();
}

size_t FileCache::size() const {
    return list_.size();
}

size_t FileCache::max_file_size() const {
    return max_file_size_;
}