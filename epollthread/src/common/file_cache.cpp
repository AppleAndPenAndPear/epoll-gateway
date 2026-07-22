#include "file_cache.h"

FileCache::FileCache(size_t max_entries, size_t max_file_size_mb) : max_entries_(max_entries), max_file_size_(max_file_size_mb * 1024 * 1024) {
}

const std::string* FileCache::get(const std::string& path, time_t file_mtime) {
    auto it = map_.find(path);
    if (it != map_.end()) {
        // 找到缓存项，检查是否过期
        auto& entry = *(it->second);
        if (file_mtime == 0 || entry.second.mtime == file_mtime) {
            // 缓存有效（file_mtime==0 表示跳过检查），移动到链表头部并返回内容
            list_.splice(list_.begin(), list_, it->second);
            return &entry.second.content;
        } else {
            // 缓存过期，移除旧项
            list_.erase(it->second);
            map_.erase(it);
        }
    }
    // 缓存未命中或已过期
    return nullptr;
}


void FileCache::put(const std::string& path, const std::string& content, size_t size, time_t mtime){
    // 过滤超过最大限制的文件
    if (size > max_file_size_) {
        return;
    }
    auto it = map_.find(path);
    if (it != map_.end()) {
        // 已存在，更新内容并移动到头部
        it->second->second.content = content;
        it->second->second.size = size;
        it->second->second.mtime = mtime;
        list_.splice(list_.begin(), list_, it->second);
        return;
    }
    // 如果缓存已满，淘汰最久未使用的（链表尾部）
    if (list_.size() >= max_entries_) {
        auto last = list_.back();
        map_.erase(last.first);  // 需要 CacheEntry 存储 path，或通过迭代器删除
        list_.pop_back();
    }
    // 插入新条目到链表头部
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