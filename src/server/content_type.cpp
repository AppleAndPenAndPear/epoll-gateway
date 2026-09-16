#include "content_type.h"
#include <unordered_map>
#include <string_view>

std::string get_content_type(const std::string& path) {
    // Use string_view to avoid the memory allocation from substr
    static const std::unordered_map<std::string_view, std::string_view> mime_map = {
        {".html", "text/html"},
        {".htm", "text/html"},
        {".css", "text/css"},
        {".js", "application/javascript"},
        {".json", "application/json"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif", "image/gif"},
        {".svg", "image/svg+xml"},
        {".ico", "image/x-icon"},
        {".txt", "text/plain"},
        {".xml", "application/xml"},
        {".pdf", "application/pdf"},
        {".zip", "application/zip"},
        {".mp3", "audio/mpeg"},
        {".mp4", "video/mp4"},
    };

    auto ext_pos = path.rfind('.');
    if (ext_pos != std::string::npos) {
        std::string_view ext(path.data() + ext_pos, path.size() - ext_pos);
        auto it = mime_map.find(ext);
        if (it != mime_map.end()) return std::string(it->second);
    }
    return "application/octet-stream"; // Default binary stream
}