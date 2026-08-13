#include "http_handler.h"
#include <sys/epoll.h>
#include <sys/stat.h>       // stat结构体需要
#include "content_type.h"
#include <sys/sendfile.h>       // sendfile 需要
#include <fcntl.h>             // open 需要
#include <unistd.h>            // close 需要
#include <sstream>             // ostringstream 需要
#include <algorithm>           // std::transform 需要
#include <cctype>              // std::tolower 需要
#include <stdexcept>           // std::runtime_error 需要
#include "mylogger.h"
#include <iomanip>      // std::hex 需要
#include <nlohmann/json.hpp>
#include "route_utils.h"
#include "gzip_utils.h"
#include <string>
#include "metrics.h"
#include "http_client.h"
using json = nlohmann::json;

static std::string to_hex(size_t n) {
    std::ostringstream oss;
    oss << std::hex << n;
    return oss.str();
}

HttpHandler::HttpHandler(Epoll& epoll, const Config& config) : epoll_(epoll), www_root_(config.www_root), config_(config), cache_(config.cache_max_entries, config.cache_max_file_size_mb) {
    // 注册示例路由
    addRoute("GET", "/metrics", [](const HttpRequest& req, HttpResponse& resp, const RouteParams&) {
        std::string body = Metrics::instance().to_string();
        resp.status_code = 200;
        resp.status_message = "OK";
        resp.headers["Content-Type"] = "text/plain; version=0.0.4";
        resp.body = body;
        resp.headers["Content-Length"] = std::to_string(body.size());
    });

    addRoute("GET", "/api/hello", [](const HttpRequest& req, HttpResponse& resp,const RouteParams& params) {
        nlohmann::json j;
        j["message"] = "Hello, World!";
        std::string body = j.dump();
        resp.status_code = 200;
        resp.status_message = "OK";
        resp.headers["Content-Type"] = "application/json";
        resp.body = body;
        resp.headers["Content-Length"] = std::to_string(body.size());
    });

    addRoute("POST", "/api/echo", [](const HttpRequest& req, HttpResponse& resp,const RouteParams& params) {
        try {
            auto j = nlohmann::json::parse(req.body);
            nlohmann::json resp_json;
            resp_json["echo"] = j;
            std::string body = resp_json.dump();
            resp.status_code = 200;
            resp.status_message = "OK";
            resp.headers["Content-Type"] = "application/json";
            resp.body = body;
            resp.headers["Content-Length"] = std::to_string(body.size());
        } catch (...) {
            resp.status_code = 400;
            resp.status_message = "Bad Request";
            resp.body = "{\"error\":\"Invalid JSON\"}";
            resp.headers["Content-Type"] = "application/json";
            resp.headers["Content-Length"] = std::to_string(resp.body.size());
        }
    });

    addRoute("PUT", "/api/echo", [](const HttpRequest& req, HttpResponse& resp,const RouteParams& params) {
        resp.status_code = 200;
        resp.status_message = "OK";
        resp.headers["Content-Type"] = "text/plain";
        resp.body = "PUT received: " + req.body;
        resp.headers["Content-Length"] = std::to_string(resp.body.size());
    });

    addRoute("DELETE", "/api/resource", [](const HttpRequest& req, HttpResponse& resp,const RouteParams& params) {
        json j;
        j["status"] = "deleted";
        j["message"] = "Resource deleted successfully";
        resp.body = j.dump();
        resp.status_code = 200;
        resp.status_message = "OK";
        resp.headers["Content-Type"] = "application/json";
        resp.headers["Content-Length"] = std::to_string(resp.body.size());
    });

    addRoute("GET", "/users/{id}", [](const HttpRequest& req, HttpResponse& resp, const RouteParams& params) {
        std::string user_id = params.at("id");
        json j;
        j["id"] = user_id;
        j["name"] = "User_" + user_id;  // 模拟数据
        resp.body = j.dump();
        resp.status_code = 200;
        resp.status_message = "OK";
        resp.headers["Content-Type"] = "application/json";
        resp.headers["Content-Length"] = std::to_string(resp.body.size());
    });

    addRoute("GET", "/chunked", [](const HttpRequest& req, HttpResponse& resp, const RouteParams&) {
        std::string payload = "This is a chunked response.\n";
        payload += "Each line could be generated separately.\n";
        resp.status_code = 200;
        resp.status_message = "OK";
        resp.headers["Content-Type"] = "text/plain";
        resp.chunked = true;      // 关键：设置 chunked 标志
        resp.body = payload;
    });

    for (const auto& route : config.routes) {
        // 简单路径匹配：如果请求路径以 route.path 去掉末尾 '*' 开头，则匹配
        std::string pattern = route.path;
        if (!pattern.empty() && pattern.back() == '*') {
            pattern.pop_back(); // 去除 '*'
        }
        addRoute(route.method, route.path, [this, route](const HttpRequest& req, HttpResponse& resp, const RouteParams&) {
            // 查找 upstream
            auto it = config_.upstreams.find(route.upstream);
            if (it == config_.upstreams.end() || it->second.servers.empty()) {
                resp.status_code = 502;
                resp.body = "Bad Gateway: no upstream server";
                resp.headers["Content-Length"] = std::to_string(resp.body.size());
                return;
            }
            // 暂时取第一个服务器
            const auto& server = it->second.servers[0];

            // 构造转发的路径：将匹配部分替换为后端实际路径（简单处理：直接转发原始路径）
            std::string forward_path = req.path;
            // 转发请求
            BackendResponse be = forward_request(server.host, server.port,
                                                req.method, forward_path,
                                                req.headers, req.body);
            resp.status_code = be.status_code;
            resp.body = be.body;
            // 透传后端响应头
            for (const auto& [k, v] : be.headers) {
                resp.headers[k] = v;
            }
            resp.headers["Content-Length"] = std::to_string(resp.body.size());
        });
    }
}

void HttpHandler::on_connect(Socket* sock){
    read_bufs_[sock];          // 创建空读缓冲
    send_queues_[sock];        // 创建空发送队列
    request_ready_[sock] = false;
    parsers_[sock] = HttpParser();
    keep_alive_[sock] = true;  // 默认 keep-alive
}

void HttpHandler::handle_read(std::shared_ptr<Socket> sock,const std::string& client_ip){
    Socket* sock_ptr = sock.get();
    client_ip_map_[sock_ptr] = client_ip;
    request_start_time_[sock_ptr] = std::chrono::steady_clock::now();
    int fd = sock->getFd();
    try {
        char buf[4096];
        while (true) {
            bool is_ssl = sock_ptr->get_is_ssl_();
            int n = is_ssl ? sock_ptr->sslRead(buf, sizeof(buf)) : sock_ptr->recv(buf, sizeof(buf), 0);
            if (n > 0) {
                auto& read_buf = read_bufs_[sock_ptr];
                read_buf.append(buf, n);

                // 反复解析缓冲区，直到解析不出完整请求
                while (true) {
                    size_t consumed = 0;
                    if (parsers_[sock_ptr].parse(read_buf.data(), read_buf.size(),requests_[sock_ptr], consumed)) {
                        // 成功解析一个完整请求，记录总请求数
                        Metrics::instance().record_total_request();
                        // 从缓冲区移除已消费的数据
                        read_buf.erase(0, consumed);

                        // 获取已解析好的请求引用
                        auto& req = requests_[sock_ptr];

                        // ─── 新增：方法合法性检查 ───
                        if (req.method != "GET" && req.method != "HEAD" && req.method != "POST" && req.method != "PUT" && req.method != "DELETE") {
                            Logger::get()->warn("HTTP method not allowed: {} on fd {}", req.method, fd);
                            send_error_response(sock_ptr, 405, "Method Not Allowed");
                            parsers_[sock_ptr].reset();
                            continue;  // 错误响应已放入发送队列，继续解析下一个请求（如果有）
                        }
                        
                        //记录user-agent（key 已统一小写）
                        auto ua_it = req.headers.find("user-agent");
                        std::string user_agent = (ua_it != req.headers.end()) ? ua_it->second : "-";
                        Logger::get()->info("Request: {} {} {} - UA: {}", req.method, req.path, req.version, user_agent);

                        // 检查 connection 头，决定 keep-alive（key 已统一小写）
                        auto it = req.headers.find("connection");
                        if (it != req.headers.end()) {
                            std::string conn = it->second;
                            std::transform(conn.begin(), conn.end(), conn.begin(), ::tolower);
                            keep_alive_[sock_ptr] = (conn != "close");
                        } else {
                            keep_alive_[sock_ptr] = true; // HTTP/1.1 默认 keep-alive
                        }

                        // 准备响应（使用本次解析出的 request）
                        send_response(sock_ptr, req);  // 你需要调整 send_response

                        // 重置解析器，为下一个请求做准备
                        parsers_[sock_ptr].reset();

                        // ★ 关键：跳出内层循环，等待发送完成再处理下一个请求
                        break;
                    }
                    // ─── 检查 body 是否超出大小限制 ───
                    if (parsers_[sock_ptr].is_body_too_large()) {
                        Logger::get()->warn("Request body too large (>{}) on fd {}",
                            HttpParser::MAX_BODY_SIZE, fd);
                        send_error_response(sock_ptr, 413, "Payload Too Large");
                        // 清除部分已消费的数据
                        if (consumed > 0) read_buf.erase(0, consumed);
                        parsers_[sock_ptr].reset();
                        requests_[sock_ptr].clear();
                        continue;  // 继续检查缓冲区中是否还有后续请求
                    }
                    else {
                        Logger::get()->trace("Parser waiting for more data, buffer size: {}", read_buf.size());
                        break; // 没有完整请求，等待更多数据
                    }
                }
            } else if (n == 0) {
                cleanup(sock);
                return;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                else { throw_system_error("recv"); }
            }
        }
        // 重新激活读事件（如果连接还活跃）
        if (!send_queues_[sock_ptr].empty()) {
            epoll_.mod(fd, EPOLLIN | EPOLLOUT | EPOLLET | EPOLLONESHOT);
        } else {
            epoll_.mod(fd, EPOLLIN | EPOLLET | EPOLLONESHOT);
        }
    } catch (const std::exception& e) {
        Logger::get()->error("Exception in handle_read: {}", e.what());
        cleanup(sock);
    } catch (...) {
        Logger::get()->error("Unknown exception in handle_read");
        cleanup(sock);
    }
}

void HttpHandler::process_request(Socket* sock_ptr){
    HttpRequest& req = requests_[sock_ptr];
    // 检查 connection 头（key 已统一小写）
    auto it = req.headers.find("connection");
    if (it != req.headers.end()) {
        std::string conn = it->second;
        std::transform(conn.begin(), conn.end(), conn.begin(), ::tolower);
        keep_alive_[sock_ptr] = (conn == "keep-alive");
    } else {
        keep_alive_[sock_ptr] = true; // HTTP/1.1 默认 keep-alive
    }
    send_response(sock_ptr, req);          // 准备响应
    request_ready_[sock_ptr] = false; // 允许解析下一个请求
    parsers_[sock_ptr].reset();       // 重置解析器状态
}

void HttpHandler::send_response(Socket* sock, const HttpRequest& req){
    last_requests_[sock] = req;
    HttpResponse resp;
    std::string path = req.path;

    // 默认首页
    if (path.empty() || path == "/") path = "/index.html"; // 默认首页

    bool path_handled = false;

    std::string req_path = req.path;
    std::string req_method = req.method;

    // 遍历已注册的路由，尝试匹配
    for (const auto& route : routes_) {
        if (route.method != req_method) continue;

        RouteParams params;
        if (matchRoute(route.pattern, req_path, params)) {
            route.handler(req, resp, params);
            path_handled = true;
            break;
        }
    }

    bool already_compressed = false;  // ★ 提前声明
    
    if (!path_handled) {
        //防止目录遍历攻击，简单处理：不允许 ".."
        if (path.find("..") != std::string::npos) {
            // 返回 403 Forbidden
            resp.status_code = 403;
            resp.status_message = "Forbidden";
            resp.body = "<h1>403 Forbidden</h1>";       //<h1>:html标题标签，显示为大号字体
            resp.headers["Content-Length"] = std::to_string(resp.body.size());
            resp.headers["Content-Type"] = "text/html";
            resp.chunked = false;
        }
        else{
            Logger::get()->debug("Attempting to serve file: {}", path);

            // 大文件：sendfile 零拷贝 + fd 缓存优化
            int file_fd = -1;

            // 快速路径：TTL 内直接返回，零系统调用
            struct stat st;
            off_t cached_size = 0;
            time_t cached_mtime = 0;
            time_t now = time(nullptr);
            file_fd = fd_cache_.try_get(path, now, &cached_size, &cached_mtime);
            if (file_fd != -1) {
                // 缓存命中！直接使用，无 stat/open/fstat
                Metrics::instance().record_fd_cache_hit();  // 记录缓存命中
                st.st_size = cached_size;
                st.st_mtime = cached_mtime;
                Logger::get()->debug("FdCache fast hit: {}", path);
            }
            else {
                // TTL 过期或未缓存 → 需要 stat() 验证
                std::string file_path = www_root_ + path;
                if (::stat(file_path.c_str(), &st) != 0) {
                    resp.status_code = 404;
                    resp.status_message = "Not Found";
                    resp.body = "<h1>404 Not Found</h1>";
                    resp.headers["Content-Length"] = std::to_string(resp.body.size());
                    resp.headers["Content-Type"] = "text/html";
                    resp.chunked = false;
                    goto after_file;
                }

                // 尝试验证已有缓存条目
                file_fd = fd_cache_.validate(path, st.st_mtime);
                if (file_fd != -1) {
                    Metrics::instance().record_fd_cache_hit();  // 记录缓存命中
                    Logger::get()->debug("FdCache validated: {}", path);
                }
                else {
                    // 缓存中无此条目，执行 open
                    Metrics::instance().record_fd_cache_miss();
                    file_fd = open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
                    if (file_fd >= 0) {
                        fd_cache_.put(path, file_fd, st.st_mtime, st.st_size);
                        Logger::get()->debug("FdCache miss, stored: {}", path);
                    }
                    else {
                        resp.status_code = 500;
                        resp.status_message = "Internal Server Error";
                        resp.body = "<h1>500 Internal Server Error</h1>";
                        resp.headers["Content-Length"] = std::to_string(resp.body.size());
                        resp.headers["Content-Type"] = "text/html";
                        resp.chunked = false;
                        goto after_file;
                    }
                }
            }
            
            if (file_fd >= 0) {
                // st 已由 stat() 填充，无需 fstat
                constexpr size_t MAX_INLINE = 1024 * 1024; // 1MB

                // 判断客户端是否支持 gzip
                bool client_wants_gzip = false;
                auto it = req.headers.find("accept-encoding");
                if (it != req.headers.end() && it->second.find("gzip") != std::string::npos) {
                    client_wants_gzip = true;
                }

                    if (client_wants_gzip && st.st_size <= MAX_INLINE) {
                        // 尝试获取压缩缓存
                        std::string gzip_key = path + "#gzip";
                        const std::string* compressed_cached = cache_.get(gzip_key, st.st_mtime);
                        if (compressed_cached) {
                            // 命中压缩缓存
                            Metrics::instance().record_gzip_cache_hit();   // ★ 使用 gzip 专用计数器
                            resp.body = *compressed_cached;
                            resp.headers["Content-Encoding"] = "gzip";
                            already_compressed = true;
                            Logger::get()->debug("Cache hit (gzip): {}", path);
                        } else {
                            // 未命中，获取原始内容，压缩，并缓存压缩结果
                            const std::string* raw = cache_.get(path, st.st_mtime);
                            if (!raw) {
                                // 原始也没缓存，读文件并存入原始缓存
                                Metrics::instance().record_cache_miss();
                                std::string file_content(st.st_size, '\0');
                                ssize_t n = ::read(file_fd, &file_content[0], st.st_size);
                                if (n == static_cast<ssize_t>(st.st_size)) {
                                    cache_.put(path, file_content, st.st_size, st.st_mtime);
                                    resp.body = std::move(file_content);
                                    raw = &resp.body;  // ★ 修复：指向 resp.body 供后续压缩使用
                                }
                                else{
                                    //close(file_fd);
                                    resp.status_code = 500;
                                    resp.status_message = "Internal Server Error";
                                    resp.body = "<h1>500 Internal Server Error</h1>";
                                    resp.headers["Content-Type"] = "text/html";
                                    resp.headers["Content-Length"] = std::to_string(resp.body.size());

                                    resp.chunked = false;
                                    goto after_file;  // 跳出文件处理
                                }
                            }
                            std::string compressed;
                            if (gzip_compress(*raw, compressed)) {
                                cache_.put(gzip_key, compressed, compressed.size(), st.st_mtime);
                                resp.body = std::move(compressed);
                                resp.headers["Content-Encoding"] = "gzip";
                                already_compressed = true;
                                Metrics::instance().record_gzip_cache_miss();  // ★ 使用 gzip 专用计数器
                            } else if (raw != &resp.body) {
                                // 压缩失败，回退到原始内容（避免 self-copy）
                                resp.body = *raw;
                            }
                            Logger::get()->debug("Gzip cache miss, compressed: {}", already_compressed);
                        }
                        // 无论是否命中，小文件都已读入内存，可以关闭文件
                        // close(file_fd);
                        // file_fd = -1;  // 标记已关闭
                        resp.status_code = 200;
                        resp.status_message = "OK";
                        resp.headers["Content-Type"] = get_content_type(path);
                        resp.headers["Content-Length"] = std::to_string(resp.body.size());
                    }else{
                        // ---------- 不支持 gzip 或大文件：走原有逻辑 ----------
                        if (st.st_size <= MAX_INLINE) {
                            // 小文件但不压缩：直接内存缓存（原来的缓存逻辑）
                            const std::string* cached = cache_.get(path, st.st_mtime);
                            if (cached) {
                                resp.body = *cached;
                                Logger::get()->debug("Cache hit (raw): {}", path);
                                Metrics::instance().record_cache_hit();
                            } else {
                                std::string file_content(st.st_size, '\0');
                                ssize_t n = ::read(file_fd, &file_content[0], st.st_size);
                                if (n == static_cast<ssize_t>(st.st_size)) {
                                    cache_.put(path, file_content, st.st_size, st.st_mtime);
                                    resp.body = std::move(file_content);
                                    Metrics::instance().record_cache_miss();
                                    Logger::get()->debug("Cache miss, stored (raw): {}", path);
                                } else {
                                    // fd 已缓存，由 FdCache 管理生命周期，不关闭
                                    resp.status_code = 500;
                                    resp.status_message = "Internal Server Error";
                                    resp.body = "<h1>500 Internal Server Error</h1>";
                                    resp.headers["Content-Type"] = "text/html";
                                    resp.headers["Content-Length"] = std::to_string(resp.body.size());
                                    resp.chunked = false;
                                    goto after_file;
                                }
                            }
                            // fd 已缓存，由 FdCache 管理生命周期，不关闭
                            resp.status_code = 200;
                            resp.status_message = "OK";
                            resp.headers["Content-Type"] = get_content_type(path);
                            resp.headers["Content-Length"] = std::to_string(resp.body.size());
                        } else {
                            // 大文件：sendfile 零拷贝
                            resp.status_code = 200;
                            resp.status_message = "OK";
                            resp.headers["Content-Type"] = get_content_type(path);
                            resp.headers["Content-Length"] = std::to_string(st.st_size);
                            file_fds_[sock] = file_fd;
                            file_offsets_[sock] = 0;
                            file_sizes_[sock] = st.st_size;
                            resp.body.clear();
                            Logger::get()->debug("Large file via sendfile: {}", path);
                        }
                    }
            }else {
                resp.status_code = 404;
                resp.status_message = "Not Found";
                resp.body = "<h1>404 Not Found</h1>";
                resp.headers["Content-Type"] = "text/html";
                resp.headers["Content-Length"] = std::to_string(resp.body.size());
                resp.chunked = false;
            }
        }
    } // if (!path_handled)
after_file:
    if (keep_alive_[sock]) {
        resp.headers["Connection"] = "keep-alive";
    } 
    else {
        resp.headers["Connection"] = "close";
    }

    // ★ 统一添加 Server 头
    resp.headers["Server"] = "EpollHTTP/0.2";

    if (req.method != "HEAD" && !already_compressed && should_compress(req, resp)) {
        std::string compressed;
        if (gzip_compress(resp.body, compressed)) {
            resp.body = std::move(compressed);
            resp.headers["Content-Encoding"] = "gzip";
            resp.headers["Content-Length"] = std::to_string(resp.body.size());
        }
    }

    if (resp.chunked) {
        resp.headers.erase("Content-Length");               // 不能同时存在
        resp.headers["Transfer-Encoding"] = "chunked";
        // 将原始 body 编码为 chunked 格式
        std::string chunked_body;
        if (!resp.body.empty()) {
            chunked_body += to_hex(resp.body.size()) + "\r\n";
            chunked_body += resp.body + "\r\n";
        }
        chunked_body += "0\r\n\r\n";   // 结束块
        resp.body = chunked_body;
    }

    // 计算总发送字节数
    size_t total_bytes = 0;
    // 将响应头序列化并放入发送队列
    std::string header_str = headers_to_string(resp);
    total_bytes += header_str.size();

    bool is_head = (req.method == "HEAD");
    if (!is_head) {   // 非 HEAD 请求，才可能包含 body 或文件
        if (!resp.body.empty()) {
            total_bytes += resp.body.size();
        } else if (resp.status_code == 200) {
            // 文件响应：头部大小 + 文件大小
            total_bytes += file_sizes_[sock];   // 此时文件大小已存入
        }
    }
    else{
        // HEAD 请求不允许有 body，fd 由 FdCache 管理，仅清理映射
        auto file_it = file_fds_.find(sock);
        if (file_it != file_fds_.end()) {
            file_fds_.erase(sock);
            file_offsets_.erase(sock);
            file_sizes_.erase(sock);
        }
        resp.body.clear();   // 确保内联 body 清空
    }

    // 存储状态码和总大小
    resp_status_[sock] = resp.status_code;
    resp_size_[sock] = total_bytes;

    auto& queue = send_queues_[sock];
    queue.emplace_back(header_str.begin(), header_str.end());

    // 如果有内联 body（错误页面），也放入队列
    if (!resp.body.empty()) {
        queue.emplace_back(resp.body.begin(), resp.body.end());
    }

    // 激活写事件
    int fd = sock->getFd();
    epoll_.mod(fd, EPOLLIN | EPOLLOUT | EPOLLET | EPOLLONESHOT);
}

void HttpHandler::send_error_response(Socket* sock, int code, const std::string& message){
    HttpResponse resp;
    resp.status_code = code;
    resp.status_message = message;
    resp.body = "<h1>" + std::to_string(code) + " " + message + "</h1>";
    resp.headers["Content-Type"] = "text/html";
    resp.headers["Content-Length"] = std::to_string(resp.body.size());
    // 错误响应通常不保持连接
    keep_alive_[sock] = false;
    resp.headers["Connection"] = "close";

    std::string header = headers_to_string(resp);
    auto& queue = send_queues_[sock];
    queue.emplace_back(header.begin(), header.end());
    queue.emplace_back(resp.body.begin(), resp.body.end());

    // ---- 新增：存储状态码和响应大小，以便 handle_write 记录日志 ----
    resp_status_[sock] = code;
    resp_size_[sock] = header.size() + resp.body.size();

    int fd = sock->getFd();
    epoll_.mod(fd, EPOLLOUT | EPOLLET | EPOLLONESHOT);
}

std::string HttpHandler::headers_to_string(const HttpResponse& resp) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << resp.status_code << " " << resp.status_message << "\r\n";
    for (const auto& [key, value] : resp.headers) {
        oss << key << ": " << value << "\r\n";
    }
    oss << "\r\n";   // 空行分隔头部和主体
    return oss.str();
}

void HttpHandler::cleanup(std::shared_ptr<Socket> sock){
    Socket* sock_ptr = sock.get();
    int fd = sock->getFd();
    epoll_.del(fd);               // 显式从 epoll 移除，避免 fd 复用竞态
    sock->closeSSL();             // SSL 优雅关闭（必须在 closefd 之前）
    sock->closefd();
    read_bufs_.erase(sock_ptr);
    send_queues_.erase(sock_ptr);
    request_ready_.erase(sock_ptr);
    parsers_.erase(sock_ptr);
    requests_.erase(sock_ptr);
    keep_alive_.erase(sock_ptr);

    resp_status_.erase(sock_ptr);
    resp_size_.erase(sock_ptr);
    // fd 由 FdCache 统一管理生命周期，这里只清理映射，不关闭
    auto fit = file_fds_.find(sock_ptr);
    if (fit != file_fds_.end()) {
        file_fds_.erase(fit);
    }
    file_offsets_.erase(sock_ptr);
    file_sizes_.erase(sock_ptr);
    client_ip_map_.erase(sock_ptr);
    request_start_time_.erase(sock_ptr);
    last_requests_.erase(sock_ptr);
    Logger::get()->info("HttpHandler: connection closed on fd {}", fd);
}

void HttpHandler::handle_write(std::shared_ptr<Socket> sock){
    int fd = sock->getFd();
    Socket* sock_ptr = sock.get();
    try {
        // 持续处理，直到没有数据可发送且没有新请求可处理
        while (true) {
            auto& queue = send_queues_[sock_ptr];

            // ==================== 阶段1：发送用户态队列中的数据 ====================
            while (!queue.empty()) {
                auto& front = queue.front();
                bool is_ssl = sock_ptr->get_is_ssl_();
                int n = is_ssl ? sock->sslWrite(front.data(), front.size())
                               : sock->send(front.data(), front.size(), 0);
                if (n > 0) {
                    if (n == front.size()) {
                        queue.pop_front();
                    } else {
                        front.erase(front.begin(), front.begin() + n);
                        // 没发完，等下次 EPOLLOUT
                        epoll_.mod(fd, EPOLLOUT | EPOLLET | EPOLLONESHOT);      //直接修改事件，保持 EPOLLOUT 监听，等待下次可写事件继续发送剩余数据
                        break;  // 内核缓冲区满
                    }
                } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    epoll_.mod(fd, EPOLLOUT | EPOLLET | EPOLLONESHOT);
                    break;
                } else if(n == -1 && errno == EPIPE){
                    // 客户端已断开，这是正常现象，清理连接即可
                    Logger::get()->debug("Client disconnected (EPIPE) on fd {}", fd);
                    cleanup(sock);
                    return;
                }else {
                    throw_system_error("send");
                }
            }

            // ==================== 阶段2：发送静态文件（零拷贝） ====================
            // 如果队列已空，判断是否需要关闭连接
            if (queue.empty()) {
                auto file_it = file_fds_.find(sock_ptr);
                if (file_it != file_fds_.end()) {
                    int file_fd = file_it->second;
                    off_t offset = file_offsets_[sock_ptr];
                    off_t remaining = file_sizes_[sock_ptr] - offset;

                    bool is_ssl = sock_ptr->get_is_ssl_();

                    if (is_ssl) {
                        // SSL 连接不能使用 sendfile（sendfile 绕过 OpenSSL 加密层）
                        // 需要将文件内容读入用户态缓冲区，再通过 SSL_write 发送
                        constexpr size_t SSL_SENDFILE_BUF = 65536;  // 64KB 缓冲区
                        std::vector<char> filebuf(std::min(static_cast<off_t>(SSL_SENDFILE_BUF), remaining));
                        ssize_t read_n = ::pread(file_fd, filebuf.data(), filebuf.size(), offset);
                        if (read_n > 0) {
                            int write_n = sock->sslWrite(filebuf.data(), read_n);
                            if (write_n > 0) {
                                offset += write_n;
                                remaining -= write_n;
                                // 如果只写了部分，等待下次 EPOLLOUT
                                if (write_n < read_n) {
                                    file_offsets_[sock_ptr] = offset;
                                    epoll_.mod(fd, EPOLLOUT | EPOLLET | EPOLLONESHOT);
                                    return;
                                }
                            } else if (write_n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                                file_offsets_[sock_ptr] = offset;
                                epoll_.mod(fd, EPOLLOUT | EPOLLET | EPOLLONESHOT);
                                return;
                            } else {
                                Logger::get()->error("SSL_write for file failed on fd {}", fd);
                                file_fds_.erase(sock_ptr);
                                file_offsets_.erase(sock_ptr);
                                file_sizes_.erase(sock_ptr);
                                cleanup(sock);
                                return;
                            }
                        } else if (read_n == -1) {
                            Logger::get()->error("pread for SSL file failed: {}", strerror(errno));
                            file_fds_.erase(sock_ptr);
                            file_offsets_.erase(sock_ptr);
                            file_sizes_.erase(sock_ptr);
                            cleanup(sock);
                            return;
                        }
                        // read_n == 0 means EOF, treat as done
                    } else {
                        // 非 SSL：使用 sendfile 零拷贝
                        while (remaining > 0) {
                            ssize_t n = sendfile(fd, file_fd, &offset, remaining);
                            if (n > 0) {
                                remaining -= n;
                            } else if (n == -1) {
                                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                    file_offsets_[sock_ptr] = offset;
                                    epoll_.mod(fd, EPOLLOUT | EPOLLET | EPOLLONESHOT);
                                    return;
                                } else {
                                    Logger::get()->error("sendfile failed: {}", strerror(errno));
                                    file_fds_.erase(sock_ptr);
                                    file_offsets_.erase(sock_ptr);
                                    file_sizes_.erase(sock_ptr);
                                    cleanup(sock);
                                    return;
                                }
                            }
                        }
                    }

                    // 文件发送完毕，清理文件描述符与映射
                    //close(file_fd);
                    file_fds_.erase(sock_ptr);
                    file_offsets_.erase(sock_ptr);
                    file_sizes_.erase(sock_ptr);
                }

                auto status_it = resp_status_.find(sock_ptr);
                auto start_it = request_start_time_.find(sock_ptr);
                if (status_it != resp_status_.end() && start_it != request_start_time_.end()) {
                    double duration = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - start_it->second).count();
                    Metrics::instance().record_request(status_it->second, duration);
                    // 注意：这里我们不删除 start_time，因为后面 CLF 日志可能还要用，或者我们可以在记录完 CLF 后再删除
                }

                // ★ 新增：记录 CLF 格式的访问日志
                {
                    auto ip_it = client_ip_map_.find(sock_ptr);
                    std::string client_ip = (ip_it != client_ip_map_.end()) ? ip_it->second : "-";

                    // 安全获取请求耗时（防止 map 已被 cleanup 清空导致迭代器失效）
                    int64_t duration_us = 0;
                    auto start_it = request_start_time_.find(sock_ptr);
                    if (start_it != request_start_time_.end()) {
                        duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - start_it->second).count();
                    }

                    std::string method, path, version, user_agent;
                    auto req_it = last_requests_.find(sock_ptr);
                    if (req_it != last_requests_.end()) {
                        method = req_it->second.method;
                        path = req_it->second.path;
                        version = req_it->second.version;
                        auto ua = req_it->second.headers.find("user-agent");
                        user_agent = (ua != req_it->second.headers.end()) ? ua->second : "-";
                        last_requests_.erase(sock_ptr);
                    }

                    char time_buf[64];
                    time_t now = time(nullptr);
                    strftime(time_buf, sizeof(time_buf), "%d/%b/%Y:%H:%M:%S %z", localtime(&now));

                    // 安全获取状态码和响应大小
                    int status_code = 0;
                    size_t response_size = 0;
                    auto status_it = resp_status_.find(sock_ptr);
                    if (status_it != resp_status_.end()) status_code = status_it->second;
                    auto size_it = resp_size_.find(sock_ptr);
                    if (size_it != resp_size_.end()) response_size = size_it->second;

                    Logger::get()->info("{} - - [{}] \"{} {} {}\" {} {} \"-\" \"{}\" {}us",
                        client_ip, time_buf, method, path, version,
                        status_code, response_size,
                        user_agent, duration_us);
                }

                // 清理本次响应的临时记录
                resp_status_.erase(sock_ptr);
                resp_size_.erase(sock_ptr);

                // 如果连接需要关闭，则清理并返回
                if (!keep_alive_[sock_ptr]) {
                    cleanup(sock);
                    return;
                }

                // 检查读缓冲区中是否已有待处理的请求
                if (!read_bufs_[sock_ptr].empty()) {
                    std::string ip = client_ip_map_[sock_ptr]; // 复用 IP
                    handle_read(sock, ip);
                    // 继续循环，尝试发送刚生成的数据
                    continue;
                }

                // 没有待处理的请求了，转为监听读事件，跳出循环
                epoll_.mod(fd, EPOLLIN | EPOLLET | EPOLLONESHOT);
                break;
            } else {
                // 发送队列中仍有数据（EAGAIN 或部分发送），等待下次 EPOLLOUT
                epoll_.mod(fd, EPOLLOUT | EPOLLET | EPOLLONESHOT);
                return;
            }
        }
    }
    catch (const std::exception& e) {
        Logger::get()->error("Exception in handle_write: {}", e.what());
        cleanup(sock);
    } catch (...) {
        Logger::get()->error("Unknown exception in handle_write");
        cleanup(sock);
    }
}

void HttpHandler::close_connection(std::shared_ptr<Socket> sock){
    cleanup(sock);
}

void HttpHandler::addRoute(const std::string& method, const std::string& pattern, RouteHandler handler) {
    routes_.push_back({method, pattern, handler});
}

std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
        if (!token.empty()) tokens.push_back(token);
    }
    return tokens;
}