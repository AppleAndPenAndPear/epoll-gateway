# epollthread
基于 C++17 实现的高性能多线程网络服务器框架，采用 **SO_REUSEPORT + epoll + One Loop Per Thread** 架构，配合动态线程池和异步日志，支持海量并发连接。项目以经典 Echo 服务为示例，展示了从底层 Socket 封装到多线程 Reactor 事件循环的完整实现

## 特性
- **多线程 Reactor 模型**：每个 Worker 线程独立运行 epoll 事件循环，监听独立的 listen socket（`SO_REUSEPORT`），实现内核级负载均衡。
- **非阻塞 I/O + 边缘触发**：所有 socket 使用非阻塞模式，结合 `EPOLLET` 和 `EPOLLONESHOT`，高效处理读写事件。
- **动态线程池**：支持根据任务负载自动扩缩容，可配置最小/最大线程数及扩缩容阈值。
- **异步日志系统**：基于 `spdlog` 的异步全局线程池，支持控制台彩色输出与文件滚动存储，性能开销低。
- **RAII 资源管理**：Socket、Epoll 等资源封装为 RAII 类，支持移动语义，防止描述符泄漏。
- **优雅关闭**：利用 `SIGINT`/`SIGTERM` 信号安全停止所有 Worker 线程，确保日志完整、资源正确回收。
- **客户端状态机**：配套非阻塞客户端实现，通过状态机管理连接、发送、接收全流程，支持请求半关闭。

## 架构概览
Master Thread
|
┌─────────────┼─────────────┐
│ │ │
TcpWorker 1 TcpWorker 2 ... TcpWorker N
(epoll loop) (epoll loop) (epoll loop)
│ │ │
listen fd 1 listen fd 2 listen fd N (SO_REUSEPORT)
└─────────────┴─────────────┘
客户端连接
│
EchoHandler (业务处理)
│
DynamicThreadPool (可选异步任务)

- **Tcpserver**：负责创建 N 个 listen socket，启动对应数量的 `TcpWorker` 线程。
- **TcpWorker**：每个 Worker 持有独立的 epoll 实例、连接表、Handler，全权处理归属连接的所有 I/O 事件，无锁竞争。
- **EchoHandler**：示例业务处理器，将收到的数据原样返回。
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

# 构建
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)

# 启动服务端
./server

# 启动客户端（新终端）
./client

预期输出
服务端：
[2026-05-07 19:21:52.076] [info] Starting epoll server...
[2026-05-07 19:21:52.077] [info] Starting 2 worker threads with SO_REUSEPORT
[2026-05-07 19:21:52.077] [info] TcpWorker created with listen fd 4 by move
...

客户端：
[2026-05-07 19:21:58.123] [info] Client connection to 192.168.1.100:5005
[2026-05-07 19:21:58.124] [debug] send to fd 4, n = 14
[2026-05-07 19:21:58.125] [info] Client done, received: Hello, server!

按下 Ctrl+C 时，服务端和客户端均可优雅退出，日志完整写入 logs/ 目录

技术栈
技术	说明
C++17	核心语言，使用 RAII、移动语义、std::atomic、std::call_once 等
epoll	Linux I/O 多路复用，边缘触发（ET）+ ONESHOT
SO_REUSEPORT	多 Worker 负载均衡
spdlog	高性能异步日志库
CMake	跨平台构建系统

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
│   ├── echohandler.h
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
│   │   ├── echohandler.cpp
│   │   └── pool.cpp
│   └── client/               # 客户端
│       ├── main.cpp
│       ├── client.cpp
│       └── clienthandler.cpp
├── CMakeLists.txt            # 顶层 CMake 配置

核心设计细节
1. 非阻塞 accept 与连接管理
Listen socket 使用 SOCK_NONBLOCK | SOCK_CLOEXEC 创建，accept4 原子设置客户端 socket 为非阻塞。

客户端 fd 统一注册 EPOLLIN | EPOLLRDHUP | EPOLLET | EPOLLONESHOT，确保事件仅触发一次，处理完后需手动重新注册。

2. 零拷贝的发送队列优化
服务端 EchoHandler 采用 std::deque<std::vector<char>> 作为发送缓冲区，避免 std::string 频繁头部删除导致的 O(n) 内存移动。

3. 线程池集成（预留）
TcpWorker 持有 DynamicThreadPool 指针，未来可将耗时操作（如解析、加密）提交到线程池，通过 eventfd 通知 I/O 线程写回结果，目前 Echo 示例未启用。

4. 信号处理与优雅关闭
全局 std::atomic<bool> 标志，结合 SIGINT/SIGTERM 处理器，让所有 Worker 在空闲时检测并退出事件循环。

析构顺序保证：Tcpserver → DynamicThreadPool → Logger::Guard，确保日志在最后关闭。

性能指标（Echo 服务）
并发连接数：可轻松应对 10,000+ 并发连接（受系统文件描述符限制）。

吞吐量：在 4 核虚拟机上，双 Worker 模式下，短连接 Echo 吞吐可达数万 QPS。

延迟：单线程处理无锁，事件延迟在微秒级

后续计划
集成 HTTP 协议解析，转变为轻量级 Web 服务器

增加定时器功能，管理空闲连接

完善线程池与 I/O 线程的 eventfd 通知机制

支持跨平台 kqueue（macOS）兼容层

加入压测工具与性能调优文档

许可
本项目采用 MIT License

致谢
spdlog 提供优秀的 C++ 日志库
Nginx 架构思想启发

欢迎 Star 和 PR！