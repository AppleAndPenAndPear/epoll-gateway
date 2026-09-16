#pragma once
#include <unistd.h>
#include <sys/stat.h>
#include <list>
#include <string>
#include <unordered_map>

// File descriptor cache that avoids stat() on every request via a TTL mechanism.
// Typical usage:
//   1. try_get() - fast path: within the TTL, returns fd+size+mtime directly, zero syscalls
//   2. validate() - after TTL expiry, the caller stat()s and confirms mtime to revalidate
//   3. put() - first insertion
class FdCache {
public:
    // ttl_sec: max time (in seconds) to skip stat() validation
    explicit FdCache(size_t max_entries = 256, int ttl_sec = 10);
    ~FdCache();

    void clear();
    size_t size() const;

    // Non-copyable and non-movable
    FdCache(const FdCache&) = delete;
    FdCache& operator=(const FdCache&) = delete;

    // Quick lookup: on a cache hit within the TTL, returns the fd and fills in size/mtime
    // Returns -1 on miss or expiry (caller must take the stat+validate path)
    int try_get(const std::string& path, time_t now, off_t* file_size, time_t* mtime);

    // Validate an existing entry: the caller has already stat()ed and holds the real mtime,
    // used to check whether the cache is still valid.
    // If valid, updates validated_at and returns the fd; otherwise evicts and returns -1.
    int validate(const std::string& path, time_t mtime);

    // Insert a new entry. If full, evicts and closes the least recently used fd.
    void put(const std::string& path, int fd, time_t mtime, off_t file_size);
private:
    struct CachedFd {
        int fd;
        off_t file_size;
        time_t mtime;         // file modification time
        time_t validated_at;  // time of the last stat() validation
        std::list<std::string>::iterator lru_it;
    };

    size_t max_entries_;
    int ttl_sec_;
    std::list<std::string> list_;  // LRU list of paths
    std::unordered_map<std::string, CachedFd> map_;
};