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

HttpHandler::HttpHandler(Epoll& epoll) : epoll_(epoll) {
}

void HttpHandler::on_connect(Socket* sock){
    read_bufs_[sock];          // 创建空读缓冲
    send_queues_[sock];        // 创建空发送队列
    request_ready_[sock] = false;
    parsers_[sock] = HttpParser();
    keep_alive_[sock] = true;  // 默认 keep-alive
}

void HttpHandler::handle_read(std::shared_ptr<Socket> sock){
    int fd = sock->getFd();
    Socket* sock_ptr = sock.get();
    try {
        char buf[4096];
        while (true) {
            int n = sock->recv(buf, sizeof(buf), 0);
            if (n > 0) {
                auto& read_buf = read_bufs_[sock_ptr];
                read_buf.append(buf, n);

                // 反复解析缓冲区，直到解析不出完整请求
                while (true) {
                    size_t consumed = 0;
                    if (parsers_[sock_ptr].parse(read_buf.data(), read_buf.size(),requests_[sock_ptr], consumed)) {
                        // 成功解析一个请求，从缓冲区移除已消费的数据
                        read_buf.erase(0, consumed);

                        // 获取已解析好的请求引用
                        auto& req = requests_[sock_ptr];

                        // ─── 新增：方法合法性检查 ───
                        if (req.method != "GET" && req.method != "HEAD") {
                            Logger::get()->warn("HTTP method not allowed: {} on fd {}", req.method, fd);
                            send_error_response(sock_ptr, 405, "Method Not Allowed");
                            parsers_[sock_ptr].reset();
                            continue;  // 错误响应已放入发送队列，继续解析下一个请求（如果有）
                        }
                        
                        //记录User-Agent
                        auto ua_it = req.headers.find("User-Agent");
                        std::string user_agent = (ua_it != req.headers.end()) ? ua_it->second : "-";
                        Logger::get()->info("Request: {} {} {} - UA: {}", req.method, req.path, req.version, user_agent);

                        // 检查 Connection 头，决定 keep-alive
                        auto it = req.headers.find("Connection");
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
    // 检查 Connection 头
    auto it = req.headers.find("Connection");
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
    HttpResponse resp;
    std::string path = req.path;

    // 默认首页
    if (path.empty() || path == "/") path = "/index.html"; // 默认首页
    
    //防止目录遍历攻击，简单处理：不允许 ".."
    if (path.find("..") != std::string::npos) {
        // 返回 403 Forbidden
        resp.status_code = 403;
        resp.status_message = "Forbidden";
        resp.body = "<h1>403 Forbidden</h1>";       //<h1>:html标题标签，显示为大号字体
        resp.headers["Content-Length"] = std::to_string(resp.body.size());
        resp.headers["Content-Type"] = "text/html";
    }
    else{
        std::string file_path = "./www" + path;     //目前目录限制在www文件夹下，硬编码
        Logger::get()->debug("Attempting to serve file: {}", file_path);
        // 尝试打开文件
        int file_fd = open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
        if (file_fd >= 0) {
            // 获取文件大小
            struct stat st;
            if (fstat(file_fd, &st) == 0) {
                resp.status_code = 200;
                resp.status_message = "OK";
                resp.headers["Content-Type"] = get_content_type(path);
                resp.headers["Content-Length"] = std::to_string(st.st_size);
                // 保存文件信息，由 handle_write 用 sendfile 发送
                file_fds_[sock] = file_fd;
                file_offsets_[sock] = 0;
                file_sizes_[sock] = st.st_size;
                // body 留空，不占用内存
                resp.body.clear();
            }
            else {
                close(file_fd);
                resp.status_code = 500;
                resp.status_message = "Internal Server Error";
                resp.body = "<h1>500 Internal Server Error</h1>";
                resp.headers["Content-Type"] = "text/html";
            }
        }
        else {
            resp.status_code = 404;
            resp.status_message = "Not Found";
            resp.body = "<h1>404 Not Found</h1>";
            resp.headers["Content-Type"] = "text/html";
            resp.headers["Content-Length"] = std::to_string(resp.body.size());
        }
    }

    if (keep_alive_[sock]) {
        resp.headers["Connection"] = "keep-alive";
    } 
    else {
        resp.headers["Connection"] = "close";
    }

    // 计算总发送字节数
    size_t total_bytes = 0;
    // 将响应头序列化并放入发送队列
    std::string header_str = headers_to_string(resp);
    total_bytes += header_str.size();
    if (!resp.body.empty()) {
        total_bytes += resp.body.size();
    } else if (resp.status_code == 200) {
        // 文件响应：头部大小 + 文件大小
        total_bytes += file_sizes_[sock];   // 此时文件大小已存入
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
    sock->closefd();
    read_bufs_.erase(sock_ptr);
    send_queues_.erase(sock_ptr);
    request_ready_.erase(sock_ptr);
    parsers_.erase(sock_ptr);
    requests_.erase(sock_ptr);
    keep_alive_.erase(sock_ptr);

    resp_status_.erase(sock_ptr);
    resp_size_.erase(sock_ptr);
    file_fds_.erase(sock_ptr);        // 若未清理过
    file_offsets_.erase(sock_ptr);
    file_sizes_.erase(sock_ptr);
    Logger::get()->info("HttpHandler: connection closed on fd {}", sock->getFd());
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
                int n = sock->send(front.data(), front.size(), 0);
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
                } else {
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

                    while (remaining > 0) {
                        ssize_t n = sendfile(fd, file_fd, &offset, remaining);
                        if (n > 0) {
                            remaining -= n;
                        } else if (n == -1) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                // 保存当前偏移，等待下次可写
                                file_offsets_[sock_ptr] = offset;
                                epoll_.mod(fd, EPOLLOUT | EPOLLET | EPOLLONESHOT);
                                return;
                            } else {
                                // 其他错误：记录日志，关闭文件，清理资源
                                Logger::get()->error("sendfile failed: {}", strerror(errno));
                                close(file_fd);
                                file_fds_.erase(sock_ptr);
                                file_offsets_.erase(sock_ptr);
                                file_sizes_.erase(sock_ptr);
                                cleanup(sock);
                                return;
                            }
                        }
                    }

                    // 文件发送完毕，清理文件描述符与映射
                    close(file_fd);
                    file_fds_.erase(sock_ptr);
                    file_offsets_.erase(sock_ptr);
                    file_sizes_.erase(sock_ptr);
                }

                // 记录响应日志（所有数据已发送）
                Logger::get()->info("HTTP response: status={} size={} (fd {})",resp_status_[sock_ptr], resp_size_[sock_ptr], fd);
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
                    // 解析并生成下一个响应（会填充 send_queues_）
                    handle_read(sock);
                    // 继续循环，尝试发送刚生成的数据
                    continue;
                }

                // 没有待处理的请求了，转为监听读事件，跳出循环
                epoll_.mod(fd, EPOLLIN | EPOLLET | EPOLLONESHOT);
                break;
            } else {
                // 发送队列中仍有数据，保持 EPOLLOUT 监听
                epoll_.mod(fd, EPOLLOUT | EPOLLET | EPOLLONESHOT);
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