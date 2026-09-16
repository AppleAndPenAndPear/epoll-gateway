#include "gzip_utils.h"
#include <zlib.h>
#include <stdexcept>

bool gzip_compress(const std::string& input, std::string& output) {
    if (input.empty()) return false;

    z_stream stream{};
    stream.zalloc = Z_NULL;
    stream.zfree = Z_NULL;
    stream.opaque = Z_NULL;

    // Initialize deflate with gzip format (15 + 16)
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return false;
    }

    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = input.size();

    const size_t CHUNK = 16384;
    unsigned char out[CHUNK];
    do {
        stream.next_out = out;
        stream.avail_out = CHUNK;
        int ret = deflate(&stream, Z_FINISH);
        if (ret == Z_STREAM_ERROR) {
            deflateEnd(&stream);
            return false;
        }
        int have = CHUNK - stream.avail_out;
        output.append(reinterpret_cast<char*>(out), have);
    } while (stream.avail_out == 0);

    deflateEnd(&stream);
    return true;
}

bool should_compress(const HttpRequest& req, const HttpResponse& resp) {
    // Check whether the client accepts gzip
    auto it = req.headers.find("accept-encoding");
    if (it == req.headers.end()) return false;
    if (it->second.find("gzip") == std::string::npos) return false;

    // Only compress text-based types
    auto ct = resp.headers.find("Content-Type");
    if (ct == resp.headers.end()) return false;
    std::string type = ct->second;
    if (type.find("text/") == 0 || type.find("application/json") == 0 ||
        type.find("application/javascript") == 0 || type.find("application/xml") == 0 ||
        type.find("image/svg+xml") == 0) {
        return !resp.body.empty(); // Only compress when there is a body
    }
    return false;
}