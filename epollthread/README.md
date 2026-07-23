# epollthread

基于 C++17 实现的高性能多线程网络服务器框架，采用 **SO_REUSEPORT + epoll + One Loop Per Thread** 架构，配合动态线程池和异步日志，支持海量并发连接。项目已从最初的 Echo 演示进化为一个**完整的轻量级 HTTP 应用服务器**，支持 HTTP/1.1 协议解析、Keep-Alive 长连接、零拷贝文件传输、LRU 内存文件缓存、FD 文件描述符缓存、RESTful 路由、JSON API、Chunked 传输编码、Gzip 压缩、空闲连接超时、Docker 容器化部署，并包含单元测试及 AddressSanitizer 内存检测。

## 特性
- **多线程 Reactor 模型**：每个 Worker 线程独立运行 epoll 事件循环，持有独立的 listen socket（`SO_REUSEPORT`），实现内核级负载均衡，无锁竞争。
- **HTTP/1.1 协议支持**：内置状态机 HTTP 解析器，支持 GET / HEAD / POST / PUT / DELETE 方法，解析请求行、头部、查询字符串、消息体，含 Chunked 传输编码解析。
- **RESTful 路由系统**：可注册任意方法+路径模式（如 `/users/{id}`）的处理函数，支持动态路由参数提取与分发，轻松构建 JSON API。
- **静态文件服务**：根据 URL 路径映射本地文件，使用 `sendfile` 系统调用实现**零拷贝**传输；自动设置 `Content-Type`（支持 HTML、CSS、JS、JSON、图片、字体等常见格式）。
- **双层缓存机制**：
  - **LRU 内存文件缓存**：每个 Worker 维护独立的文件内容缓存，对频繁访问的小文件进行内存缓存，减少磁盘 I/O，大幅提升重复请求吞吐量。
  - **FD 文件描述符缓存**：基于 TTL 的文件描述符缓存，避免每次请求都执行 `open()`/`stat()` 系统调用，进一步降低文件服务延迟。
- **Gzip 压缩**：支持对文本类响应进行 Gzip 压缩传输，根据客户端 `Accept-Encoding` 头自动协商。
- **Chunked 传输编码**：支持 `Transfer-Encoding: chunked` 响应，适用于动态生成或流式输出的内容。
- **连接复用**：正确处理 `Connection: keep-alive`，支持在同一条 TCP 连接上串行处理多个请求（HTTP Pipelining），保证响应顺序。
- **空闲连接超时**：可配置的超时时间，自动关闭长时间无活动的连接，防止资源泄漏。
- **错误处理与状态码**：支持 200、400、403、404、405、413、500 等状态码，返回友好错误页面，并防御路径穿越攻击。
- **非阻塞 I/O + 边缘触发**：所有套接字使用非阻塞模式，结合 `EPOLLET` 和 `EPOLLONESHOT`，精细控制事件通知，避免惊群和重复触发。
- **动态线程池**：可配置最小/最大线程数，依据任务负载自动扩缩容（目前预留接口，用于未来异步业务处理）。
- **异步日志系统**：基于 `spdlog` 的全局线程池，支持控制台彩色输出与文件滚动存储，可分别控制各级别日志输出，性能开销低。
- **请求/响应日志**：记录每个请求的方法、路径、状态码、User-Agent、客户端 IP 及响应大小，便于监控与分析。
- **RAII 资源管理**：`Socket`、`Epoll` 等资源封装为 RAII 类，支持移动语义，杜绝描述符泄漏。
- **优雅关闭**：捕获 `SIGINT`/`SIGTERM` 信号，安全通知所有 Worker 线程退出，保证日志完整、资源正确回收。
- **外部配置驱动**：通过 JSON 配置文件指定端口、线程数、Web 根目录、线程池参数、缓存大小、超时时间等，方便部署和调整。
- **配套非阻塞客户端**：独立的状态机客户端，支持连接、发送、接收全流程，展示 epoll 在客户端的使用方法。
- **Docker 容器化**：提供多阶段构建 `Dockerfile`，一键构建轻量镜像，随处部署。
- **单元测试**：基于 Google Test，覆盖 HTTP 解析器、LRU 缓存、响应序列化、路由匹配等核心模块。
- **AddressSanitizer 支持**：Debug 模式下自动启用 ASAN，便于检测内存泄漏和越界访问。

## 架构概览

```
                    Master Thread
                         |
         ┌───────────────┼───────────────┐
         │               │               │
     TcpWorker 1    TcpWorker 2    ... TcpWorker N
    (epoll loop)   (epoll loop)       (epoll loop)
         │               │               │
    listen fd 1     listen fd 2      listen fd N   (SO_REUSEPORT)
         │               │               │
    ┌────┴────┐    ┌────┴────┐      ┌────┴────┐
    │ 客户端连接 │    │ 客户端连接 │      │ 客户端连接 │
    └────┬────┘    └────┬────┘      └────┬────┘
         │               │               │
    HttpHandler    HttpHandler      HttpHandler
    (HTTP解析+路由)  (HTTP解析+路由)    (HTTP解析+路由)
    (文件服务+缓存)  (文件服务+缓存)    (文件服务+缓存)
    (Gzip压缩)     (Gzip压缩)       (Gzip压缩)
         │               │               │
         └───────────────┴───────────────┘
                         │
              DynamicThreadPool (共享线程池)
```

- **Tcpserver**：负责创建 N 个 listen socket，启动对应数量的 `TcpWorker` 线程。
- **TcpWorker**：每个 Worker 持有独立的 epoll 实例、连接表、`HttpHandler`，全权处理归属连接的所有 I/O 事件，并负责超时连接清理。
- **HttpHandler**：HTTP/1.1 协议核心实现，包含请求解析、路由匹配、Keep-Alive 管理、文件服务、错误响应、内存缓存等。
- **DynamicThreadPool**：可选的共享线程池，用于将耗时任务从 I/O 线程卸载到工作线程（预留扩展）。
- **Logger**：全局异步日志器，通过 spdlog 全局线程池实现高性能日志记录。

## 项目结构

```
epollthread/
├── include/                  # 头文件
│   ├── server.h              # Tcpserver 服务端主类
│   ├── tcpworker.h           # TcpWorker 工作线程
│   ├── http_handler.h        # HTTP 请求处理与路由
│   ├── http_parser.h         # HTTP/1.1 协议解析器（状态机）
│   ├── pool.h                # DynamicThreadPool 动态线程池
│   ├── mysocket.h            # Socket RAII 封装
│   ├── myepoll.h             # Epoll RAII 封装
│   ├── mylogger.h            # 异步日志封装
│   ├── config.h              # JSON 配置加载
│   ├── file_cache.h          # LRU 内存文件缓存
│   ├── fd_cache.h            # FD 文件描述符缓存（TTL）
│   ├── gzip_utils.h          # Gzip 压缩工具
│   ├── content_type.h        # Content-Type 映射
│   ├── route_utils.h         # 路由匹配与参数提取
│   ├── error_utils.h         # 错误处理工具
│   ├── client.h              # 非阻塞客户端
│   └── clienthandler.h       # 客户端处理器
├── src/                      # 源文件
│   ├── server/               # 服务端源码
│   │   ├── main.cpp          # 服务端入口
│   │   ├── server.cpp        # Tcpserver 实现
│   │   ├── tcpworker.cpp     # TcpWorker 实现
│   │   ├── http_handler.cpp  # HttpHandler 实现（含路由注册）
│   │   ├── pool.cpp          # 动态线程池实现
│   │   ├── content_type.cpp  # Content-Type 实现
│   │   └── gzip_utils.cpp    # Gzip 压缩实现
│   ├── client/               # 客户端源码
│   │   ├── main.cpp          # 客户端入口
│   │   ├── client.cpp        # Client 实现
│   │   └── clienthandler.cpp # ClientHandler 实现
│   └── common/               # 公共模块源码
│       ├── mysocket.cpp      # Socket 实现
│       ├── myepoll.cpp       # Epoll 实现
│       ├── mylogger.cpp      # Logger 实现
│       ├── error_utils.cpp   # 错误处理实现
│       ├── fd_cache.cpp      # FD 缓存实现
│       ├── file_cache.cpp    # 文件缓存实现
│       └── http_parser.cpp   # HTTP 解析器实现
├── tests/                    # 单元测试
│   ├── CMakeLists.txt
│   ├── test_http_parser.cpp  # HTTP 解析器测试
│   ├── test_file_cache.cpp   # LRU 缓存测试
│   ├── test_http_response.cpp# HTTP 响应测试
│   └── test_route_utils.cpp  # 路由匹配测试
├── www/                      # 静态文件根目录
│   ├── index.html
│   └── big.html
├── logs/                     # 日志输出目录
├── build/                    # 构建输出目录（cmake 生成）
├── CMakeLists.txt            # CMake 构建配置
├── build.sh                  # 一键构建脚本
├── start.sh                  # 快速启动脚本
├── config.json               # 服务器配置文件
├── vcpkg.json                # vcpkg 依赖清单
├── Dockerfile                # Docker 多阶段构建
└── README.md
```

## 快速开始

### 环境要求
- Linux (内核 3.9+，支持 `SO_REUSEPORT`)
- GCC 7+ 或 Clang 5+（需要 C++17 支持）
- CMake 3.20+
- [vcpkg](https://github.com/microsoft/vcpkg)（推荐）或手动安装依赖

### 依赖库
| 库 | 用途 | 安装方式 |
|---|---|---|
| [spdlog](https://github.com/gabime/spdlog) | 异步日志 | vcpkg / apt |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON 配置与 API | vcpkg / apt |
| [zlib](https://zlib.net/) | Gzip 压缩 | 系统自带 / apt |
| [Google Test](https://github.com/google/googletest) | 单元测试 | vcpkg / apt |

### 本地构建与运行

#### 方式一：使用 build.sh 一键构建（推荐）

```bash
# Release 构建
./build.sh

# Debug 构建（启用 AddressSanitizer）
./build.sh Debug
```

#### 方式二：手动 CMake 构建

```bash
# 使用 vcpkg 工具链（推荐）
cmake -B build -S . \
    -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 或使用系统包管理器安装依赖后直接构建
sudo apt install g++ cmake make libspdlog-dev nlohmann-json3-dev zlib1g-dev
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### 配置文件

编辑 `config.json` 进行个性化配置（所有字段均有默认值，文件不存在时自动使用默认值）：

```json
{
    "port": 5005,                   // 监听端口
    "backlog": 1024,                // listen backlog 大小
    "num_workers": 2,               // Worker 线程数（0=自动取 CPU 核数）
    "www_root": "./www",            // 静态文件根目录
    "thread_pool": {
        "min": 2,                   // 最小线程数
        "max": 10,                  // 最大线程数
        "scale_up": 2,              // 扩容触发阈值（任务数/线程数）
        "scale_down": 1             // 缩容触发阈值
    },
    "cache_max_entries": 1024,      // LRU 文件缓存最大条目数
    "cache_max_file_size_mb": 1,    // 可缓存的最大文件大小（MB）
    "keepalive_timeout": 60         // Keep-Alive 空闲超时（秒）
}
```

### 启动服务器

```bash
# 直接启动
./build/server

# 或使用启动脚本
./start.sh
```

### 运行单元测试

```bash
cd build
cmake .. -DBUILD_TESTS=ON
cmake --build . -j$(nproc)
./tests/runTests
```

## Docker 构建与运行

```bash
# 构建镜像
docker build -t epoll-server .

# 运行容器
docker run -d -p 5005:5005 --name my-server epoll-server

# 查看日志
docker logs my-server

# 停止与删除
docker stop my-server && docker rm my-server
```

## API 路由示例

服务端默认注册了以下 RESTful API 路由：

| 方法 | 路径 | 说明 |
|---|---|---|
| `GET` | `/api/hello` | 返回 JSON `{"message": "Hello, World!"}` |
| `POST` | `/api/echo` | 回显请求体中的 JSON |
| `PUT` | `/api/echo` | 返回 `PUT received: <body>` |
| `DELETE` | `/api/resource` | 返回 `{"status": "deleted", ...}` |
| `GET` | `/users/{id}` | 动态路由，返回模拟用户数据 |
| `GET` | `/chunked` | Chunked 分块传输演示 |
| `GET` | `/<path>` | 静态文件服务（默认行为） |

## 客户端

配套的非阻塞 TCP 客户端位于 `src/client/`，可独立编译运行：

```bash
./build/client
```

> 注意：客户端默认连接 `192.168.189.138:5005`，如需修改请在 `src/client/main.cpp` 中调整目标地址。

## 调试与诊断

### AddressSanitizer (ASAN)

Debug 构建模式自动启用 AddressSanitizer，可检测：
- 堆/栈缓冲区溢出
- 使用已释放的内存（use-after-free）
- 内存泄漏

```bash
./build.sh Debug
./build/server   # ASAN 检测报告会输出到 stderr
```

### 日志

- 服务端日志：`logs/epollserver.log`
- 客户端日志：`logs/client.log`
- 控制台同步输出带颜色的日志（开发调试用）


访问服务
浏览器打开 http://localhost:5005 查看默认页面。
访问 http://localhost:5005/index.html 或其他静态文件。
使用 curl -v http://localhost:5005/ 查看详细请求/响应头。
测试 Keep-Alive：
echo -ne "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\nGET /index.html HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n" | nc localhost 5005
测试 HEAD 方法：curl -I http://localhost:5005/index.html
检查服务器标识：响应头中可见 Server: EpollHTTP/0.2
按下 Ctrl+C 优雅关闭服务器，日志完整保存在 logs/epollserver.log

配置文件示例 (config.json)
{
    "port": 5005,
    "backlog": 1024,
    "num_workers": 2,
    "www_root": "./www",
    "cache_max_entries": 1024,
    "cache_max_file_size_mb": 1,
    "thread_pool": {
        "min": 2,
        "max": 10,
        "scale_up": 2,
        "scale_down": 1
    }
}

技术栈
技术	说明
C++17	核心语言，使用 RAII、移动语义、std::atomic、std::call_once 等
epoll	Linux I/O 多路复用，边缘触发（ET）+ ONESHOT
SO_REUSEPORT	多 Worker 负载均衡
spdlog	高性能异步日志库
CMake	跨平台构建系统
sendfile	零拷贝文件传输
nlohmann/json	JSON 解析（单头文件）
Google Test	单元测试框架
Docker	容器化部署

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
│   ├── file_cache.h          # LRU 文件缓存
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
│   │   ├── http_handler.cpp
│   │   ├── http_parser.cpp
│   │   ├── content_type.cpp
│   │   └── pool.cpp
│   └── client/               # 客户端（Echo 测试）
│       ├── main.cpp
│       ├── client.cpp
│       └── clienthandler.cpp
├── tests/                    # 单元测试
│   ├── test_http_parser.cpp
│   ├── test_file_cache.cpp
│   ├── test_http_response.cpp
│   └── CMakeLists.txt
├── www/                      # 静态文件根目录（可选）
├── config.json               # 配置文件（可选）
├── CMakeLists.txt
├── Dockerfile                # Docker 镜像构建文件
├── .dockerignore
├── build.sh, start.sh        # 便捷脚本
└── README.md

核心设计细节
1. HTTP 协议解析与管线化
使用状态机解析请求行和头部，支持分片接收，无需完整报文。
正确处理 Connection: keep-alive 和 Connection: close。
管线化（Pipelining）：按顺序依次处理同一连接上的多个请求，响应顺序与请求严格一致。

2. 零拷贝文件发送与 LRU 缓存
静态文件优先尝试内存缓存（LRU），命中则直接返回内存内容。
未命中则使用 open + fstat + sendfile 进行零拷贝传输。
小文件（默认 ≤ 1MB）在首次发送前读入缓存，后续访问直接命中，避免磁盘 I/O。
缓存基于文件修改时间自动失效，保证内容始终最新。

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
析构顺序保证：Tcpserver → DynamicThreadPool → Logger::Guard，确保日志最后关闭。

7.路由系统
基于 std::unordered_map<std::string, RouteHandler>，键为 "方法:路径"，在 send_response 开头匹配，未命中回退到静态文件服务。

8.空闲超时
每个连接维护最后活跃时间，epoll_wait 超时时扫描并清理过期连接，支持配置超时阈值。

9.单元测试
使用 Google Test，测试 HttpParser、FileCache、HttpResponse 等独立模块，可一键运行。

性能指标
并发连接数：轻松应对 10,000+ 并发连接（受系统 fd 限制）。
吞吐量：静态小文件（如 index.html）在启用缓存后，重复请求的 QPS 可提升数倍，单 Worker 可达数万 QPS。
延迟：请求处理在微秒级，零拷贝 + 内存缓存极低 CPU 占用。
具体压测数据请参见后续压测报告。

后续计划
完整的 Transfer-Encoding: chunked 请求解析
路由参数支持（如 /users/{id}）
支持 CGI/FastCGI 动态处理
支持 HTTPS（OpenSSL）
跨平台 kqueue（macOS）
集成 Prometheus 指标输出
压力测试与性能剖析报告
CI/CD (GitHub Actions / Gitee CI)

许可
本项目采用 MIT License

致谢
spdlog、nlohmann/json、Google Test 等优秀开源项目，以及 Nginx 的架构启示

欢迎 Star 和 PR！