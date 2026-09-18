[English](README.md) | [简体中文](README.zh-CN.md)

# epollthread

基于 C++17 实现的高性能多线程 HTTP/HTTPS API 网关与网络服务器，采用 **SO_REUSEPORT + epoll + One Loop Per Thread** 架构，配合异步日志和非阻塞 I/O。项目支持 HTTP/1.1、加固 TLS（最低 1.2、AEAD 套件白名单、证书热加载）、Keep-Alive 与上游连接池、零拷贝文件传输、LRU/FD 缓存、带 schema 校验的配置驱动路由、反向代理与上游健康检查、HTTP 请求走私防护、幂等重试与基础熔断、API Key 鉴权（密钥可从独立文件或环境变量加载）、host/tenant 策略、令牌桶限流、X-Trace-Id 请求追踪、AUDIT/CLF 双通道日志、`SIGHUP` runtime reload、upstream 超时与错误分类、Prometheus 指标、Docker 部署，并包含单元测试（CTest 84/84 通过）、集成测试（42 项断言）及 AddressSanitizer 支持。

## 特性
- **多线程 Reactor 模型**：每个 Worker 线程独立运行 epoll 事件循环，持有独立的 listen socket（`SO_REUSEPORT`），实现内核级负载均衡，无锁竞争。
- **HTTP/1.1 协议支持**：内置状态机 HTTP 解析器，支持 GET / HEAD / POST / PUT / DELETE 方法，解析请求行、头部、查询字符串、消息体，含 Chunked 传输编码解析。
- **HTTPS/TLS 加固传输**：基于 OpenSSL 集成 TLS 加密，最低版本 TLS 1.2、仅允许 AEAD 密码套件（ECDHE + GCM/ChaCha20，禁用 CBC/3DES/RC4），支持会话复用（cache + ticket），启动时 fail-fast；通过 `tls.cert_path`/`tls.key_path` 配置证书路径，`SIGHUP` 可热更换证书且失败时保留旧上下文。
- **请求走私防护**：解析器拒绝重复 `Content-Length`、CL/TE 混用、非 chunked 的 `Transfer-Encoding`、非数字 `Content-Length`、非法头部字符、请求行控制字符、非十六进制 chunk 大小以及超长请求行/头部，非法请求统一返回 400 + `Connection: close`。
- **RESTful 路由系统**：可注册任意方法+路径模式（如 `/users/{id}`）的处理函数，支持动态路由参数提取与分发，轻松构建 JSON API。
- **配置驱动网关路由**：通过 `routes` 配置 method、path、host、tenant、鉴权、限流和执行目标；路由注册与执行分离，支持 `local`、`upstream`、`static` 三类目标。
- **反向代理与上游负载均衡**：通过 `UpstreamTarget` 引用 upstream 服务组，由 `UpstreamManager` 进行轮询选路和主动 TCP 健康检查，自动摘除故障节点。
- **上游访问控制与超时**：支持 route/API Key 的 host、tenant 绑定；连接、发送、读取阶段均具备超时控制，区分 502 与 504。
- **上游连接池**：到各 upstream 的 keep-alive 连接进入池中复用（空闲超时 60s、每 upstream 最多 16 条空闲），每个代理请求省去一次 TCP 三次握手；响应按 Content-Length / chunked（含 trailer）/ EOF 精确分帧，后端已关闭的陈旧连接在取用时被预先探测并透明重建；请求发出后的失败仅对幂等方法自动重试，POST 绝不内部重发。
- **上游错误分类**：区分连接失败、连接超时、发送失败、发送超时、读取失败、读取超时和非法响应，并通过 Prometheus 暴露错误计数，映射到 502/503/504。
- **幂等重试**：仅对幂等方法（GET/HEAD 等）和可重试的临时错误发起重试，路由可通过 `max_retries` 配置额外重试次数，避免非幂等请求重复执行。
- **基础熔断器**：按 upstream/backend 维度维护熔断状态，连续失败达到阈值后进入 OPEN，超时后进入恢复探测（half-open），探测成功自动关闭熔断，防止故障后端持续拖垮网关。
- **Trace-Id 请求追踪**：统一返回 `X-Trace-Id`，客户端携带时沿用并回传，否则由网关生成，贯穿请求日志与审计日志。
- **静态文件服务**：根据 URL 路径映射本地文件，使用 `sendfile` 系统调用实现**零拷贝**传输；自动设置 `Content-Type`（支持 HTML、CSS、JS、JSON、图片、字体等常见格式）。
- **双层缓存机制**：
  - **LRU 内存文件缓存**：每个 Worker 维护独立的文件内容缓存，对频繁访问的小文件进行内存缓存，减少磁盘 I/O，大幅提升重复请求吞吐量。
  - **FD 文件描述符缓存**：基于 TTL 的文件描述符缓存，避免每次请求都执行 `open()`/`stat()` 系统调用，进一步降低文件服务延迟。
- **Gzip 压缩**：支持对文本类响应进行 Gzip 压缩传输，根据客户端 `Accept-Encoding` 头自动协商。
- **Chunked 传输编码**：支持 `Transfer-Encoding: chunked` 响应，适用于动态生成或流式输出的内容。
- **连接复用**：正确处理 `Connection: keep-alive`，支持在同一条 TCP 连接上串行处理多个请求（HTTP Pipelining），保证响应顺序。
- **空闲连接超时**：可配置的超时时间，自动关闭长时间无活动的连接，防止资源泄漏。
- **错误处理与状态码**：支持 200、400、403、404、405、413、429、500、502、503、504 等状态码，区分后端不可达与后端超时，并防御路径穿越攻击；路径匹配但方法不支持时返回 405 并携带 `Allow` Header。
- **令牌桶限流**：基于令牌桶算法的全局限流器，跨 Worker 共享计数，按客户端 IP 精确控制请求速率，超限返回 `429 Too Many Requests`，令牌补充采用小数累积避免截断误差。
- **API Key 认证**：提供 API Key 校验与提取工具（支持 `X-API-Key` 请求头与 `Authorization: Bearer` 两种方式），支持 route allow/deny 以及 API Key 的 host/tenant 范围和差异化限流额度。密钥可内联在 `config.json`，也可通过 `api_keys_file` 指向独立文件或由 `GW_API_KEYS` 环境变量注入（优先级 env > file > 内联），让敏感信息与主配置分离。
- **非阻塞 I/O + 边缘触发**：所有套接字使用非阻塞模式，结合 `EPOLLET` 和 `EPOLLONESHOT`，精细控制事件通知，避免惊群和重复触发。
- **动态线程池**：可配置最小/最大线程数，依据任务负载自动扩缩容（目前预留接口，用于未来异步业务处理）。
- **异步日志系统**：基于 `spdlog` 的全局线程池，支持控制台彩色输出与文件滚动存储，可分别控制各级别日志输出，性能开销低。
- **AUDIT + CLF 双通道日志**：CLF 访问日志记录所有完成请求的 IP、方法、路径、状态码、响应大小与耗时；AUDIT 日志在响应完成时统一记录失败和安全策略事件（含 trace_id、route、Host/Tenant、失败原因和脱敏后的 API Key），避免重复写入。
- **请求/响应日志**：记录每个请求的方法、路径、状态码、User-Agent、客户端 IP 及响应大小，便于监控与分析。
- **RAII 资源管理**：`Socket`、`Epoll` 等资源封装为 RAII 类，支持移动语义，杜绝描述符泄漏。
- **等待器职责分离**：`Socket` 只负责 fd 生命周期和 I/O，`Poller` 封装单 fd 的 poll 等待，`Epoll` 负责长期管理大量连接。
- **优雅关闭**：捕获 `SIGINT`/`SIGTERM` 信号，安全通知所有 Worker 线程退出，保证日志完整、资源正确回收。
- **外部配置驱动 + schema 校验**：通过 JSON 配置文件指定端口、线程数、Web 根目录、线程池参数、缓存大小、超时时间等，方便部署和调整；字段类型/范围、路由重名与冲突、upstream 引用存在性均被校验，非法配置在启动时直接拒绝（fail-fast）。
- **Runtime Reload**：`SIGHUP` 触发热更新，信号处理器仅递增 generation，Worker 在安全检查点读取并应用新配置；reload 更新路由、upstream、API Key、限流、Keep-Alive 超时和 TLS 证书，非法配置会保留当前生效配置并记录 `AUDIT config_reload_rejected`。
- **配套非阻塞客户端**：独立的状态机客户端，支持连接、发送、接收全流程，展示 epoll 在客户端的使用方法。
- **Docker 容器化**：提供多阶段构建 `Dockerfile`，一键构建轻量镜像，随处部署。
- **单元测试**：基于 Google Test，当前 CTest 84/84 通过，覆盖 HTTP 解析器（含走私攻击向量）、LRU 缓存、响应序列化、路由匹配、404/405 语义、路径穿越防护、API Key 策略（含密钥来源优先级）、限流隔离、配置 schema 校验、TLS 加固、连接池语义、HTTP client 超时、幂等重试、upstream 健康检查和熔断等核心模块。
- **集成测试**：42 项端到端断言，基于真实服务器 + mock 上游（TLS 策略、证书热加载、来自独立密钥文件的鉴权、限流、故障转移、熔断、reload、连接复用、chunked trailer），仅依赖 Python 3 标准库。
- **性能基线**：`scripts/benchmark/run_benchmark.sh` 一键复现 wrk 压测（静态缓存命中 / 反向代理 / TLS 握手三场景），完整报告见 [docs/BENCHMARKS.md](docs/BENCHMARKS.md)。
- **AddressSanitizer 支持**：Debug 模式下自动启用 ASAN，便于检测内存泄漏和越界访问。
- **Prometheus 指标暴露**：内置 `/metrics` 端点，输出 Prometheus 格式指标，涵盖请求计数（按状态码分类）、请求延迟直方图、缓存命中率和 upstream 错误类型计数。
- **Grafana 可视化监控**：集成 Grafana + Prometheus 监控栈，通过 `docker-compose` 一键部署，开箱即用的指标采集与仪表盘展示。

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
    (解析+路由+策略) (解析+路由+策略)  (解析+路由+策略)
    (文件服务+缓存)  (文件服务+缓存)    (文件服务+缓存)
         │               │               │
         └───────────────┼───────────────┘
                         │
              UpstreamManager + HttpClient
              (健康检查、选路、超时、代理)
         │               │               │
         └───────────────┴───────────────┘
                         │
              DynamicThreadPool (共享线程池)
                         │
              ┌──────────┴──────────┐
              │                     │
         Prometheus              Grafana
    (每5s刮取 /metrics)    (可视化仪表盘 @ :3000)
         @ :9090
```

- **Tcpserver**：负责创建 N 个 listen socket，启动对应数量的 `TcpWorker` 线程。
- **TcpWorker**：每个 Worker 持有独立的 epoll 实例、连接表、`HttpHandler` 与 SSL 上下文，全权处理归属连接的所有 I/O 事件（含 TLS 握手），并负责超时连接清理与上游健康检查。
- **HttpHandler**：HTTP/1.1 协议核心实现，包含请求解析、配置路由匹配、host/tenant 策略、API Key 鉴权、限流、Keep-Alive、文件服务和错误响应。
- **UpstreamManager**：管理上游服务器集群，使用非阻塞 Socket + Poller 执行带超时的 TCP 健康检查，并负责轮询选路。
- **HttpClient**：执行同步的单后端 HTTP 转发，使用 `Socket + Poller` 实现连接、发送和读取超时，并返回结构化 `BackendError`。
- **Poller**：封装单次 `poll` 等待；它与 `Epoll` 分工不同，前者用于单个后端连接等待，后者用于 worker 事件循环。
- **Metrics**：线程安全的指标收集器（单例），记录请求总数、状态码分布、延迟直方图、多级缓存命中率，通过 `/metrics` 端点以 Prometheus 文本格式暴露。
- **Prometheus**：定期从 `server:5005/metrics` 刮取指标数据，存储时序数据。
- **Grafana**：连接 Prometheus 作为数据源，提供实时可视化仪表盘。
- **DynamicThreadPool**：可选的共享线程池，用于将耗时任务从 I/O 线程卸载到工作线程（预留扩展）。
- **Logger**：全局异步日志器，通过 spdlog 全局线程池实现高性能日志记录。

## 项目结构

```
epollthread/
├── include/                  # 头文件
│   ├── server/               # 服务端头文件
│   │   ├── server.h          # Tcpserver 服务端主类
│   │   ├── tcpworker.h       # TcpWorker 工作线程（含 SSL 状态机）
│   │   ├── http_handler.h    # HTTP 请求处理与路由
│   │   ├── http_client.h     # 反向代理转发与 BackendError
│   │   ├── connection_pool.h # 上游 keep-alive 连接池
│   │   ├── tls_context.h     # 加固 TLS 上下文构建
│   │   ├── upstream_manager.h# 上游服务器管理与健康检查
│   │   ├── rate_limiter.h    # 令牌桶限流器
│   │   ├── rate_limiter_manager.h # 限流器管理器
│   │   ├── api_key_manager.h # API Key 校验
│   │   ├── metrics.h         # Prometheus 指标收集器（单例，线程安全）
│   │   ├── pool.h            # DynamicThreadPool 动态线程池
│   │   ├── config.h          # JSON 配置加载
│   │   ├── gzip_utils.h      # Gzip 压缩工具
│   │   ├── content_type.h    # Content-Type 映射
│   │   ├── route_utils.h     # 路由匹配与参数提取
│   │   └── echohandler.h     # Echo 处理器（早期演示）
│   ├── client/               # 客户端头文件
│   │   ├── client.h          # 非阻塞客户端
│   │   └── clienthandler.h   # 客户端处理器
│   └── common/               # 公共头文件
│       ├── mysocket.h        # Socket RAII 封装（含 SSL 支持）
│       ├── poller.h          # 单 fd poll 等待封装
│       ├── myepoll.h         # Epoll RAII 封装
│       ├── mylogger.h        # 异步日志封装
│       ├── http_parser.h     # HTTP/1.1 协议解析器（状态机）
│       ├── file_cache.h      # LRU 内存文件缓存
│       ├── fd_cache.h        # FD 文件描述符缓存（TTL）
│       └── error_utils.h     # 错误处理工具
├── src/                      # 源文件
│   ├── server/               # 服务端源码
│   │   ├── main.cpp          # 服务端入口
│   │   ├── server.cpp        # Tcpserver 实现
│   │   ├── tcpworker.cpp     # TcpWorker 实现（含 TLS 握手）
│   │   ├── http_handler.cpp  # HttpHandler 实现（含路由注册、限流）
│   │   ├── http_client.cpp   # 反向代理转发实现（响应精确分帧）
│   │   ├── connection_pool.cpp # 上游连接池实现
│   │   ├── tls_context.cpp   # TLS 加固实现（最低版本、密码套件白名单）
│   │   ├── upstream_manager.cpp # 上游管理与健康检查实现
│   │   ├── rate_limiter.cpp  # 令牌桶限流器实现
│   │   ├── metrics.cpp       # Metrics 指标收集实现
│   │   ├── pool.cpp          # 动态线程池实现
│   │   ├── content_type.cpp  # Content-Type 实现
│   │   └── gzip_utils.cpp    # Gzip 压缩实现
│   ├── client/               # 客户端源码
│   │   ├── main.cpp          # 客户端入口
│   │   ├── client.cpp        # Client 实现
│   │   └── clienthandler.cpp # ClientHandler 实现
│   └── common/               # 公共模块源码
│       ├── poller.cpp        # Poller 实现
│       ├── mysocket.cpp      # Socket 实现（含 SSL）
│       ├── myepoll.cpp       # Epoll 实现
│       ├── mylogger.cpp      # Logger 实现
│       ├── error_utils.cpp   # 错误处理实现
│       ├── fd_cache.cpp      # FD 缓存实现
│       ├── file_cache.cpp    # 文件缓存实现
│       └── http_parser.cpp   # HTTP 解析器实现
├── certs/                    # TLS 证书与私钥
│   ├── server.crt
│   └── server.key
├── tests/                    # 单元测试
│   ├── CMakeLists.txt
│   ├── test_http_parser.cpp  # HTTP 解析器测试
│   ├── test_file_cache.cpp   # LRU 缓存测试
│   ├── test_http_response.cpp# HTTP 响应测试
│   ├── test_route_utils.cpp  # 路由匹配测试
│   ├── test_auth_and_rate_limit.cpp # API Key 和限流测试
│   ├── test_http_client.cpp  # upstream 超时和错误分类测试
│   ├── test_upstream_manager.cpp # upstream 健康检查测试
│   ├── test_config_validation.cpp # 配置 schema 校验测试
│   ├── test_tls_context.cpp  # TLS 上下文加固测试
│   ├── test_connection_pool.cpp # 连接池测试
│   └── integration/          # 集成测试（真实服务器 + mock 上游）
│       ├── run_integration_tests.py
│       └── mock_backend.py
├── scripts/
│   └── benchmark/
│       └── run_benchmark.sh  # 一键 wrk 基线压测（静态/代理/握手）
├── www/                      # 静态文件根目录
│   ├── index.html
│   └── big.html
├── logs/                     # 日志输出目录
├── build/                    # 构建输出目录（cmake 生成）
├── CMakeLists.txt            # CMake 构建配置
├── build.sh                  # 一键构建脚本
├── ci.sh                     # 一键 CI（构建 + 单测 + 集成测试）
├── start.sh                  # 快速启动脚本
├── config.json               # 服务器配置文件
├── vcpkg.json                # vcpkg 依赖清单
├── Dockerfile                # Docker 多阶段构建
├── docker-compose.yml        # Docker Compose 编排（server + Prometheus + Grafana）
├── prometheus.yml            # Prometheus 抓取配置
├── docs/
│   ├── PROJECT_STATUS.md     # 项目现状快照（当前能力、限制与下一步）
│   ├── CHANGELOG.md          # 变更记录（按日期回溯）
│   ├── BENCHMARKS.md         # wrk 基线压测报告（P50/P99/QPS）
│   └── ROADMAP.md            # 商业化路线图（定位、技术演进与验证计划）
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
| [OpenSSL](https://www.openssl.org/) | TLS/SSL 加密传输 | 系统自带 / apt |
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
    "keepalive_timeout": 60,        // Keep-Alive 空闲超时（秒）
    "tls": {                        // 可选 TLS 路径（以下为默认值）
        "cert_path": "certs/server.crt",
        "key_path": "certs/server.key"
    },
    "upstreams": {                  // 上游服务定义
        "test-service": {
            "servers": [
                {"host": "127.0.0.1", "port": 8081},
                {"host": "127.0.0.1", "port": 8082}
            ],
            "algorithm": "round_robin"
        }
    },
    "upstream_health_check_timeout_ms": 500, // 上游 TCP 健康检查超时（毫秒）
    "routes": [                     // 配置驱动路由
        {
            "name": "test-service-api",
            "method": "GET",
            "path": "/api/test/*",
            "host": "*",            // 可选，默认 *
            "tenant": "*",          // 可选，默认 *
            "target_type": "upstream",
            "upstream_target": {
                "name": "test-service",
                "timeout_ms": 5000,
                "max_retries": 1,               // 额外重试次数（仅幂等方法）
                "circuit_failure_threshold": 5, // 连续失败多少次打开熔断
                "circuit_recovery_timeout_ms": 10000 // OPEN 后多久允许恢复探测
            },
            "auth_required": true,
            "allowed_api_keys": ["test-key-123", "premium-key-456"]
        }
    ],
    "rate_limit": {                 // 默认限流（令牌桶）
        "capacity": 20,
        "refill_per_second": 5
    },
    "api_keys_file": "api_keys.json", // 可选：从独立文件（或 GW_API_KEYS 环境变量）加载密钥，替代内联配置
    "api_keys": [                   // API Key、限流额度和 host/tenant 范围
        {
            "key": "test-key-123",
            "name": "测试客户端",
            "rate_limit": {"capacity": 200, "refill_per_second": 100},
            "allowed_hosts": ["*"],
            "allowed_tenants": ["*"]
        },
        {
            "key": "premium-key-456",
            "name": "高级客户端",
            "rate_limit": {"capacity": 1000, "refill_per_second": 500}
        }
    ]
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

# 或使用 CTest
ctest --test-dir build --output-on-failure
```

### 运行集成测试

真实启动 server 与 mock 上游后端，验证端到端行为（TLS 策略、证书热加载、鉴权、限流、故障转移、熔断、reload、连接复用、chunked trailer 等 16 类场景）：

```bash
python3 tests/integration/run_integration_tests.py
```

仅依赖 Python3 标准库，无需额外安装。

### 一键 CI

```bash
./ci.sh              # 构建 + 单元测试 + 集成测试
./ci.sh build        # 仅构建
./ci.sh integration  # 仅集成测试
```

GitHub Actions workflow 见 `.github/workflows/ci.yml`（仓库镜像到 GitHub 后自动生效）。

## Docker 构建与运行

### 方式一：Docker Compose 一键部署（推荐，含监控栈）

```bash
# 启动所有服务（server + Prometheus + Grafana）
sudo docker-compose up -d

# 查看服务状态
sudo docker ps -a

# 查看日志
sudo docker-compose logs -f

# 停止所有服务
sudo docker-compose down
```

访问地址：
- 服务主页：https://localhost:5005（自签名证书，需手动信任）
- 服务指标：https://localhost:5005/metrics
- Prometheus：http://localhost:9090
- Grafana：http://localhost:3000（默认用户名/密码：`admin`/`admin`）

### 方式二：单独构建镜像

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
| `GET` | `/metrics` | Prometheus 指标端点（文本格式） |
| `GET` | `/api/test/*` | 配置驱动反向代理，通过 `upstream_target.name` 转发至 `test-service` |
| `GET` | `/<path>` | 静态文件服务（默认行为） |

## Prometheus + Grafana 监控

项目内置了 Prometheus 指标端点 `/metrics`，通过 `docker-compose` 一键集成完整监控栈。

### 架构

```
server:5005/metrics
       │
       ▼ (每 5s scrape)
 Prometheus :9090 ─────▶ Grafana :3000
 (时序数据库)           (可视化仪表盘)
```

### 暴露的指标

| 指标名 | 类型 | 说明 |
|---|---|---|
| `epoll_http_requests_total{code="2xx|3xx|4xx|5xx"}` | Counter | 按状态码分类的请求计数 |
| `epoll_http_requests_total_total` | Counter | 请求总数 |
| `epoll_http_request_duration_seconds_bucket{le="..."}` | Histogram | 请求延迟分布（11 个桶） |
| `epoll_http_request_duration_seconds_sum/_count` | Histogram | 请求延迟累计值与请求数 |
| `epoll_cache_hits_total` / `epoll_cache_misses_total` | Counter | LRU 文件缓存命中/未命中 |
| `epoll_fd_cache_hits_total` / `epoll_fd_cache_misses_total` | Counter | FD 缓存命中/未命中 |
| `epoll_gzip_cache_hits_total` / `epoll_gzip_cache_misses_total` | Counter | Gzip 压缩缓存命中/未命中 |
| `epoll_upstream_errors_total{type="..."}` | Counter | upstream 错误分类计数（connect_failed、connect_timeout、write_failed、write_timeout、read_failed、read_timeout、invalid_response） |
| `epoll_route_requests_total{route,host,tenant,code}` | Counter | route/host/tenant 维度的 2xx/4xx/5xx 请求数 |
| `epoll_route_latency_seconds_sum/_count{route,host,tenant}` | - | route/host/tenant 维度的延迟 sum/count（尚未提供独立 histogram） |

### 在 Grafana 中添加数据源

1. 浏览器打开 http://localhost:3000，使用 `admin`/`admin` 登录
2. 左侧菜单 → **Connections** → **Data sources** → **Add data source**
3. 选择 **Prometheus**
4. 在 **Prometheus server URL** 填入 `http://prometheus:9090`（容器间通过 Docker 网络通信）
5. 点击 **Save & test**，确认显示 "Successfully queried the Prometheus API"

### 常用 PromQL 查询

```promql
# QPS（每秒请求数）
rate(epoll_http_requests_total_total[1m])

# 错误率
sum(rate(epoll_http_requests_total{code="4xx"}[1m]) + rate(epoll_http_requests_total{code="5xx"}[1m])) /
sum(rate(epoll_http_requests_total_total[1m]))

# P99 延迟
histogram_quantile(0.99, rate(epoll_http_request_duration_seconds_bucket[1m]))

# 文件缓存命中率
sum(rate(epoll_cache_hits_total[1m])) /
sum(rate(epoll_cache_hits_total[1m]) + rate(epoll_cache_misses_total[1m]))

# route 维度平均延迟
rate(epoll_route_latency_seconds_sum[1m]) / rate(epoll_route_latency_seconds_count[1m])

# upstream 连接超时错误速率
rate(epoll_upstream_errors_total{type="connect_timeout"}[1m])
```

## HTTPS 加密传输

服务端集成 OpenSSL，在 TCP 连接建立后自动执行 TLS 握手，使用 `certs/server.crt` 与 `certs/server.key` 作为证书与私钥（可通过 `tls` 配置段调整路径）。TLS 1.0/1.1 被拒绝——最低版本为 TLS 1.2，且仅允许 AEAD 密码套件（ECDHE + GCM/ChaCha20），支持会话复用。客户端需以 HTTPS 方式访问：

```bash
# 使用 -k 忽略自签名证书校验
curl -kv https://localhost:5005/

# 或使用 openssl 客户端直接测试 TLS 握手
openssl s_client -connect localhost:5005

# 验证 TLS 1.1 握手被拒绝
openssl s_client -connect localhost:5005 -tls1_1
```

证书支持运行时更换：替换证书文件（或在 `config.json` 中修改 `tls.cert_path`/`tls.key_path`）后发送 `SIGHUP`——新上下文先校验后生效，失败时保留旧证书继续服务。

> 注意：证书为演示用途的自签名证书，生产环境请替换为受信任机构签发的证书。

## 反向代理

通过 `config.json` 中的 `upstreams` 与 `routes` 配置，可将请求透明转发到后端服务：

- **`upstreams`**：定义一组后端服务器及其负载均衡算法（当前支持 `round_robin`）。
- **`routes`**：将匹配的「方法 + 路径」转发到指定的上游（路径支持 `*` 通配符）。
- **健康检查**：每个 Worker 每秒主动对上游节点执行 TCP 连接探测，自动摘除故障节点并在恢复后重新加入。
- **负载均衡**：`UpstreamManager` 采用轮询策略在健康节点间分发请求，转发逻辑由 `http_client::forward_request` 实现。
- **连接池**：到各 upstream 的 keep-alive 连接进入池中复用（空闲超时 60s、每 upstream 最多 16 条空闲），每个请求省去一次 TCP 握手；响应按 Content-Length / chunked（含 trailer）/ EOF 精确分帧，后端已关闭的陈旧连接在使用前被探测并透明重建。
- **超时与错误分类**：连接、发送、读取阶段均具备超时控制，通过结构化 `BackendError` 区分失败类型，映射到 502/503/504。
- **重试**：仅幂等方法与可重试的临时错误会重试，路由可通过 `max_retries` 控制额外重试次数；连接池内部绝不重发 POST。
- **熔断**：按 upstream/backend 维度统计连续失败，达到 `circuit_failure_threshold` 打开熔断，`circuit_recovery_timeout_ms` 后允许半开探测，成功后关闭熔断。

## 可观测性日志

- **X-Trace-Id**：客户端携带 `X-Trace-Id` 时沿用并回传，否则网关生成，贯穿请求日志和 AUDIT 日志。
- **CLF 访问日志**：记录所有完成请求的客户端 IP、方法、路径、状态码、响应大小和耗时。
- **AUDIT 审计日志**：仅在响应完成时记录失败和安全策略事件，包含 trace_id、method/path、Host/Tenant、route、status、failure reason、脱敏后的 API Key 和 User-Agent，避免鉴权/限流/路由分支重复写入。

## Runtime Reload

向服务进程发送 `SIGHUP` 即可触发热更新：

```bash
kill -HUP $(pgrep server)
```

- 信号处理器只递增 reload generation，不做文件 I/O 和 JSON 解析。
- Worker 在安全检查点检查 generation 并应用新配置，各 Worker 不保证同一时刻切换。
- reload 更新路由、upstream、API Key、默认限流配置、Keep-Alive 超时和 TLS 证书。
- reload 前对配置做 schema 校验，无效配置不会覆盖当前生效配置，并记录 `AUDIT config_reload_rejected`。

## 限流与 API Key 认证

- **令牌桶限流**：基于 `RateLimiter` 令牌桶算法，全局限流器跨 Worker 共享（配合 `SO_REUSEPORT` 多 Worker 场景保证总量准确），按客户端 IP 计数，超限返回 `429 Too Many Requests`。
- **API Key 认证**：支持从 `X-API-Key` 请求头或 `Authorization: Bearer <key>` 提取 API Key，配合 `api_keys` 配置为不同客户端分配差异化的限流额度。

```bash
# 触发限流后返回 429
for i in $(seq 1 30); do curl -sk -o /dev/null -w "%{http_code}\n" https://localhost:5005/api/hello; done
```

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


## 访问服务

- 服务器默认启用 HTTPS，浏览器访问 https://localhost:5005 查看默认页面（自签名证书需手动信任）。
- 使用 `curl -kv https://localhost:5005/` 查看详细请求/响应头。
- 测试 HEAD 方法：`curl -kI https://localhost:5005/index.html`
- 检查服务器标识：响应头中可见 `Server: EpollHTTP/0.2`。
- 按下 `Ctrl+C` 优雅关闭服务器，日志完整保存在 `logs/epollserver.log`。

## 技术栈

| 技术 | 说明 |
|---|---|
| C++17 | 核心语言，使用 RAII、移动语义、std::atomic、std::call_once 等 |
| epoll | Linux I/O 多路复用，边缘触发（ET）+ ONESHOT |
| SO_REUSEPORT | 多 Worker 负载均衡 |
| spdlog | 高性能异步日志库 |
| CMake | 跨平台构建系统 |
| sendfile | 零拷贝文件传输 |
| nlohmann/json | JSON 解析（单头文件） |
| Google Test | 单元测试框架 |
| Docker | 容器化部署 |
| Docker Compose | 多容器服务编排 |
| Prometheus | 指标采集与时序数据库 |
| Grafana | 指标可视化仪表盘 |
| zlib | Gzip 压缩 |
| OpenSSL | TLS/SSL 加密传输 |

## 核心设计细节

1. **HTTP 协议解析与管线化**：状态机解析请求行和头部，支持分片接收，无需完整报文；正确处理 `Connection: keep-alive`/`close`；管线化（Pipelining）按顺序处理同一连接上的多个请求，响应顺序与请求严格一致。
2. **零拷贝文件发送与 LRU 缓存**：静态文件优先尝试内存缓存（LRU），未命中则使用 `open + fstat + sendfile` 零拷贝传输；小文件（默认 ≤ 1MB）首次发送后读入缓存；缓存基于文件修改时间自动失效。
3. **发送队列与 EPOLLONESHOT 协作**：发送队列采用 `std::deque<std::vector<char>>` 减少头删开销；每次事件处理完成后根据队列状态重新设置 EPOLLIN/EPOLLOUT，并重新应用 `EPOLLET | EPOLLONESHOT`，确保同一时间只有一个线程处理该 fd。
4. **错误处理与路径安全**：拦截包含 `..` 的请求返回 403；不支持的 HTTP 方法返回 405（带 `Allow` Header）；文件不存在返回 404，内部错误返回 500；错误响应自动设置 Content-Length 和 Content-Type 并关闭连接。
5. **日志与监控**：CLF 访问日志记录所有完成请求，AUDIT 日志记录失败和安全策略事件，均带 trace_id 关联；超时、调试日志可配置为 trace 级别，日常运行不会刷屏。
6. **信号处理与优雅关闭**：`SIGINT`/`SIGTERM` 置位全局原子标志，Worker 在每次超时返回时检查并主动退出事件循环；`SIGHUP` 触发 runtime reload；析构顺序保证 Tcpserver → DynamicThreadPool → Logger::Guard，日志最后关闭。
7. **路由系统**：内置路由通过 `register_default_routes()` 注册，配置路由通过 `register_configured_routes()` 注册；一次请求只匹配一次，`ResolvedRoute` 在鉴权、限流和分发之间复用；支持 method/path/Host/Tenant 四维匹配和 `local`/`upstream`/`static` 三类目标，未命中网关路由时 GET/HEAD 回退到静态文件服务。
8. **空闲超时**：每个连接维护最后活跃时间，epoll_wait 超时时扫描并清理过期连接，支持配置超时阈值。
9. **单元与集成测试**：使用 Google Test，当前 84/84 通过，覆盖解析（含走私攻击向量）、缓存、路由、鉴权、限流、配置 schema 校验、TLS 加固、连接池语义、上游超时/重试/健康检查/熔断等模块，`ctest` 一键运行；集成测试 42 项断言，基于真实服务器 + mock 上游端到端验证。

## 性能指标

在 2 核开发机上使用 wrk 实测；完整报告（环境、方法论、各场景 P50/P99/QPS）见 [docs/BENCHMARKS.md](docs/BENCHMARKS.md)，可通过 `scripts/benchmark/run_benchmark.sh` 一键复现。

| 场景（轻载，8 连接） | 修复前 | TCP_NODELAY 修复后 |
|---|---|---|
| 静态缓存命中 P50 延迟 | 43.0 ms | **8.0 ms（约 5.4×）** |
| TLS 握手吞吐（10 连接） | 224 QPS | **956 QPS（约 4.3×）** |
| 反向代理吞吐（8 连接） | 169 QPS | **1000 QPS（约 5.9×）** |

- 这份基线压测的著名发现：缺失 `TCP_NODELAY` 导致每个请求固定 ~43 ms 的 Nagle + 延迟 ACK 停顿。
- 并发连接数：轻松应对 10,000+ 并发连接（受系统 fd 限制）。
- 上游连接池为每个代理请求省去一次 TCP 握手——收益随后端 RTT 增长（本机回环无感，跨主机显著）。

## 已知限制

- 熔断状态由每个 Worker 独立维护，不是跨 Worker 共享的全局状态。
- route/host/tenant 维度目前只有 latency sum/count，暂无独立 histogram（无法直接得到维度级 P95/P99）。
- runtime reload 采用 Worker 周期检查，各 Worker 不保证同一时刻切换配置。
- 路由匹配为线性遍历，适合当前规模，未引入 Trie/索引。
- 连接池仅维护进程内空闲连接（无跨进程共享）；异步 upstream 与 `round_robin` 之外的多负载均衡算法尚未实现。
- 尚无 `/healthz`/`/readyz` 端点和管理 API（计划于 P4）。
- 尚未接入 OpenTelemetry `traceparent`、分布式 Trace 和外部审计存储。

## 后续计划

1. ~~补充 runtime reload 和真实 HTTP 端到端集成测试。~~ ✅ P1
2. ~~增加配置字段范围校验、路由冲突检查和 reload 失败审计。~~ ✅ P2（含请求走私防护、TLS 加固、密钥管理）
3. 完善 route/host/tenant 维度的 latency histogram 和失败率指标。
4. 评估跨 Worker 共享熔断状态的实现方式。
5. ~~连接池~~ ✅ P3（含 wrk 基线压测报告）；异步 upstream 仍待做。
6. P4：`/healthz` + `/readyz`、管理 API、优雅停机 drain、部署文档。
7. P5：按客户反馈决定商业化功能形态。
8. CI/CD (GitHub Actions / Gitee CI)。

## 许可

本项目采用 MIT License。

## 致谢

感谢 spdlog、nlohmann/json、Google Test 等优秀开源项目，以及 Nginx 的架构启示。

欢迎 Star 和 PR！