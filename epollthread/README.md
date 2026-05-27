# epollthread
基于 C++17 实现的高性能多线程网络服务器框架，采用 SO_REUSEPORT + epoll + One Loop Per Thread 架构，配合动态线程池和异步日志，支持海量并发连接。项目已从最初的 Echo 演示进化为一个完整的轻量级 HTTP 静态文件服务器，支持 HTTP/1.1 协议、Keep-Alive 长连接、零拷贝文件传输，并具备生产级的日志记录与状态码处理。

## 特性
多线程 Reactor 模型：每个 Worker 线程独立运行 epoll 事件循环，持有独立的 listen socket（SO_REUSEPORT），实现内核级负载均衡，无锁竞争。
HTTP/1.1 协议支持：内置状态机 HTTP 解析器，支持 GET 请求、请求头解析、方法合法性校验。
静态文件服务：根据 URL 路径映射本地文件，使用 sendfile 系统调用实现零拷贝传输，性能高效；自动设置 Content-Type（支持 HTML、CSS、JS、图片等常见格式）。
连接复用与管线化：正确处理 Connection: keep-alive，支持在同一条 TCP 连接上串行处理多个请求（HTTP Pipelining），保证响应顺序。
错误处理与状态码：支持 200、400、403、404、405、500 等状态码，返回友好 HTML 错误页面，并防御路径穿越攻击。
非阻塞 I/O + 边缘触发：所有套接字使用非阻塞模式，结合 EPOLLET 和 EPOLLONESHOT，精细控制事件通知，避免惊群和重复触发。
动态线程池：可配置最小/最大线程数，依据任务负载自动扩缩容（目前预留接口，用于未来异步业务处理）。
异步日志系统：基于 spdlog 的全局线程池，支持控制台彩色输出与文件滚动存储，可分别控制各级别日志输出，性能开销低。
请求/响应日志：记录每个请求的方法、路径、状态码、User-Agent 及响应大小，便于监控与分析。
RAII 资源管理：Socket、Epoll 等资源封装为 RAII 类，支持移动语义，杜绝描述符泄漏。
优雅关闭：捕获 SIGINT/SIGTERM 信号，安全通知所有 Worker 线程退出，保证日志完整、资源正确回收。
配套非阻塞客户端：独立的状态机客户端，支持连接、发送、接收全流程，展示 epoll 在客户端的使用方法（保留 Echo 示例）。

## 架构概览
               Master Thread
                    |
      ┌─────────────┼─────────────┐
      │             │             │
  TcpWorker 1   TcpWorker 2  ... TcpWorker N
  (epoll loop)  (epoll loop)     (epoll loop)
      │             │             │
 listen fd 1   listen fd 2   listen fd N  (SO_REUSEPORT)
      └─────────────┴─────────────┘
               客户端连接
                    │
            HttpHandler (HTTP 解析 + 静态文件服务)
                    │
         DynamicThreadPool (可选异步任务)

- **Tcpserver**：负责创建 N 个 listen socket，启动对应数量的 `TcpWorker` 线程。
- **TcpWorker**：每个 Worker 持有独立的 epoll 实例、连接表、Handler，全权处理归属连接的所有 I/O 事件，无锁竞争。
- HttpHandler：HTTP/1.1 协议实现，包含请求解析、Keep-Alive 管理、文件服务、错误响应等。
- **DynamicThreadPool**：可选的共享线程池，用于将耗时任务从 I/O 线程卸载到工作线程。
- **Logger**：全局异步日志器，通过 spdlog 全局线程池实现高性能日志记录。

## 快速开始

### 环境要求
- Linux (内核 3.9+，支持 `SO_REUSEPORT`)
- GCC 7+ 或 Clang 5+ （支持 C++17）
- CMake 3.20+
- [spdlog](https://github.com/gabime/spdlog)（异步日志需要）

### 构建与运行
```bash
# 克隆仓库
git clone https://gitee.com/appleandpenanpear/multithread_epoll.git
cd epollthread

# 安装 spdlog（如果已安装可跳过）
sudo apt install libspdlog-dev   # Ubuntu/Debian

# 准备静态文件目录（可选）
mkdir www
echo "<h1>It works!</h1>" > www/index.html

# 构建
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)

# 启动服务端
./server

# 启动客户端（新终端）
./client

访问服务
浏览器打开 http://localhost:5005 查看默认页面。
访问 http://localhost:5005/index.html 或其他静态文件。
使用 curl -v http://localhost:5005/ 查看详细请求/响应头。
测试 Keep-Alive：
echo -ne "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\nGET /index.html HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n" | nc localhost 5005
按下 Ctrl+C 优雅关闭服务器，日志完整保存在 logs/epollserver.log

技术栈
技术	说明
C++17	核心语言，使用 RAII、移动语义、std::atomic、std::call_once 等
epoll	Linux I/O 多路复用，边缘触发（ET）+ ONESHOT
SO_REUSEPORT	多 Worker 负载均衡
spdlog	高性能异步日志库
CMake	跨平台构建系统
sendfile	零拷贝文件传输

项目结构
.
├── include/                  # 头文件
│   ├── common/
│   │   ├── mysocket.h
│   │   ├── myepoll.h
│   │   ├── mylogger.h
│   │   └── error_utils.h
│   ├── server.h
│   ├── tcpworker.h
│   ├── http_handler.h        # HTTP 请求处理
│   ├── http_parser.h         # HTTP 解析器
│   ├── content_type.h        # MIME 类型映射
│   ├── pool.h
│   ├── client.h
│   └── clienthandler.h
├── src/
│   ├── common/               # 公共组件
│   │   ├── mysocket.cpp
│   │   ├── myepoll.cpp
│   │   ├── mylogger.cpp
│   │   └── error_utils.cpp
│   ├── server/               # 服务端
│   │   ├── main.cpp
│   │   ├── server.cpp
│   │   ├── tcpworker.cpp
│   │   ├── http_handler.cpp
│   │   ├── http_parser.cpp
│   │   ├── content_type.cpp
│   │   └── pool.cpp
│   └── client/               # 客户端（Echo 测试）
│       ├── main.cpp
│       ├── client.cpp
│       └── clienthandler.cpp
├── www/                      # 静态文件根目录（可选）
├── CMakeLists.txt
└── README.md

核心设计细节
1. HTTP 协议解析与管线化
使用状态机解析请求行和头部，支持分片接收，无需完整报文。
正确处理 Connection: keep-alive 和 Connection: close。
管线化（Pipelining）：按顺序依次处理同一连接上的多个请求，响应顺序与请求严格一致。

2. 零拷贝文件发送
对于静态文件，使用 open + fstat 获取文件大小，直接通过 sendfile 将数据从内核文件缓存发送到 socket，避免用户态内存拷贝。
发送大文件时，若 socket 缓冲区满，会保存偏移量并重新注册写事件，实现异步断点续传。

3. 发送队列与 EPOLLONESHOT 协作
发送队列采用 std::deque<std::vector<char>> 减少头删开销。
每次事件处理完成后，根据队列状态重新设置 EPOLLIN 或 EPOLLOUT，并重新应用 EPOLLET | EPOLLONESHOT，确保同一时间只有一个线程处理该 fd。

4. 错误处理与路径安全
拦截包含 .. 的请求，返回 403。
不支持的 HTTP 方法返回 405。
文件不存在返回 404，内部错误返回 500。
错误响应自动设置 Content-Length 和 Content-Type，并关闭连接。

5. 日志与监控
请求日志包含方法、路径、HTTP 版本、User-Agent。
响应日志包含状态码和发送字节数，便于后续接入 ELK/Prometheus 等监控系统。
超时、调试日志可配置为 trace 级别，日常运行不会刷屏。

6. 信号处理与优雅关闭
全局 std::atomic<bool> 标志，SIGINT/SIGTERM 处理器置位。
Worker 在每次超时返回时检查标志，主动退出事件循环。
析构顺序保证日志最后关闭，所有日志可靠刷盘。
析构顺序保证：Tcpserver → DynamicThreadPool → Logger::Guard，确保日志在最后关闭。

性能指标
并发连接数：轻松应对 10,000+ 并发连接（受系统 fd 限制）。
吞吐量：静态小文件（如 index.html）在使用 sendfile 后，单 Worker 可达到数万 QPS。
延迟：请求处理在微秒级，零拷贝极低 CPU 占用。
具体压测数据请参见后续压测报告。

后续计划
支持 HEAD 方法完整实现
增加缓存机制（内存缓存、文件描述符缓存）
集成定时器管理空闲连接
完善线程池与 I/O 线程的 eventfd 通知机制
支持 CGI/FastCGI 动态内容
支持 HTTPS（集成 OpenSSL）
跨平台 kqueue（macOS）兼容
单元测试与压力测试套件
配置文件（如 JSON）解析
Docker 容器化部署

许可
本项目采用 MIT License

致谢
spdlog 提供优秀的 C++ 日志库
Nginx 架构思想启发

欢迎 Star 和 PR！