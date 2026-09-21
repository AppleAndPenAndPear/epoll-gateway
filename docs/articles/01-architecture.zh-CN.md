# 从零用 C++17 写一个 API 网关：线程模型、控制面分离，以及三个真实 Bug

> 这是一篇架构复盘，不是教程。文章里出现的每个数字、每个 Bug、每段结论都来自同一个真实项目（[epollthread](https://github.com/AppleAndPenAndPear/epoll-gateway)）：一个 C++17 写的单二进制 API 网关，不含 etcd/Postgres/Redis 之类的运行时依赖。文中的代码片段与行号指向仓库里的真实文件，压测数据来自 `docs/BENCHMARKS.md`，测试计数来自 CI 的实际输出（91 个单测 / 69 条集成断言）。

## 1. 为什么要再写一个网关

先把定位说清楚，避免"又一个 Nginx 轮子"的误读：

- **目标**：中小规模 API 场景下，一个二进制 + 一个 JSON 配置就能跑起来的网关。要鉴权、限流、熔断、健康检查、Prometheus 指标、热加载、优雅停机这些"上线必需"，但不要控制面集群、不要 Lua、不要插件 ABI。
- **非目标**：通用七层负载均衡（大流量静态分发交给 Nginx/CDN）、服务网格（不做 sidecar）、动态服务发现（upstream 列表在配置里，不在 etcd 里）。
- **约束**：C++17、Linux、epoll，代码量控制在一个人能读懂的范围内——这一点直接决定了很多后面的取舍。

## 2. 线程模型：SO_REUSEPORT + One Loop Per Thread

主流选择有三种：单 Reactor + 线程池、多进程 prefork、以及本项目的 **SO_REUSEPORT + One Loop Per Thread**。

```text
        ┌────────────┐   ┌────────────┐        ┌────────────┐
        │ worker 0   │   │ worker 1   │  ...   │ worker N-1 │
        │ listen fd0 │   │ listen fd1 │        │ listen fdN │
        │ epoll loop │   │ epoll loop │        │ epoll loop │
        └────────────┘   └────────────┘        └────────────┘
              ▲                ▲                     ▲
              └────── 内核按连接散列分发（SO_REUSEPORT）─┘
```

每个 `TcpWorker` 自己 `socket()`+`bind()`+`listen()` 一个带 `SO_REUSEPORT` 的监听套接字，然后跑自己的 epoll 循环（[server.cpp](../../src/server/server.cpp#L28)、[tcpworker.cpp](../../src/server/tcpworker.cpp)）。这样做的收益：

- 没有共享 accept 队列，也就没有多线程争抢 accept 的惊群问题——内核直接把连接散列到某个监听套接字；
- 连接一旦落到某个 worker，之后所有读写都在这个线程里完成，**同一 fd 任意时刻只被一个线程碰**，不需要给连接加锁；
- worker 之间需要共享的状态少得可怜：upstream 健康/熔断状态、全局限流器、日志。前两者是进程级 `shared_ptr`（`UpstreamManager`）+ mutex 保护的快照，日志是 spdlog 异步队列。

代价也要写清楚：`num_workers` 必须是配置项而不能靠"自动感知"，因为一个 worker 就是一条线程一个事件循环；跨 worker 的全局配额（比如按 IP 的全局限流总量）需要共享对象而不是线程局部计数。

### EPOLLET + EPOLLONESHOT 的代价

事件全部用边沿触发（`EPOLLET`）加一次性（`EPOLLONESHOT`）：

```cpp
epoll_.add(clientsock, EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET | EPOLLONESHOT);
// ...
if (has_pending_send) epoll_.mod(fd, EPOLLIN | EPOLLOUT | EPOLLET | EPOLLONESHOT);
else                  epoll_.mod(fd, EPOLLIN | EPOLLET | EPOLLONESHOT);
```

（片段见 [tcpworker.cpp:247-267](../../src/server/tcpworker.cpp#L247-L267)）

这个组合换来的是"不会被同一事件唤醒多次"，但也带来两条铁律：

1. **事件回调里必须把数据读到 `EAGAIN`**，否则边沿触发不会再通知你，连接会静默卡死；
2. **每次处理完必须重新 `mod` 注册**，因为 `EPOLLONESHOT` 已经把关注事件清空。

这就是为什么请求处理的状态机必须在一个 handler 里跑完（而不是 yield 到另一个线程），也是为什么"发送队列没发完"时要显式重新关注 `EPOLLOUT`。整个项目里最复杂、也最容易出 Bug 的代码都在这条约束下面。

## 3. 一次请求的完整路径

```text
accept → 非阻塞读 → HTTP 状态机解析 → 路由匹配（只匹配一次）
      → 鉴权（X-API-Key / Bearer，按 key 的 host/tenant 作用域）
      → 限流（令牌桶，按 key 或 IP）
      → 执行目标：
          static   → 响应缓存命中 / 大文件 sendfile 零拷贝
          upstream → 健康检查选节点 → 连接池取连接 → 精确分帧读响应
      → 写回（TLS 走 OpenSSL BIO 队列，非 TLS 走 sendfile）
      → trace_id 贯穿 + AUDIT/CLF 双通道日志
```

两个实现细节值得单独说：

- **路由只匹配一次**：`ResolvedRoute` 在鉴权、限流、分发之间复用，避免"每个阶段各匹配一次"造成的语义漂移。
- **sendfile 只在非 TLS 路径可用**。`sendfile(2)` 绕不过 OpenSSL 加密层，所以 TLS 连接必须走 OpenSSL 的写队列；代码里两条路径是分开的（[http_handler.cpp:1008-1048](../../src/server/http_handler.cpp#L1008-L1048)）。这是零拷贝在 HTTPS 世界里的真实边界，很多文章不会提。

## 4. 可靠性三件容易被忽略的事

这三件事都是"不做也不会马上报错，但会在生产里以最难看的方式暴露"的类型。

### 4.1 响应必须精确分帧

连接池要复用后端连接，前提是**知道一条响应在字节流里的确切终点**。所以读取响应只有三种合法终止方式：`Content-Length`、`chunked`（含 trailer）、以及 `Connection: close` 关闭定界。任何"读到本次 `recv` 返回就当作响应结束"的实现都会污染连接池——下一个请求会把上一条响应的残留字节当成自己的状态行。

### 4.2 只对幂等方法重试

连接在写入后才失败（后端 RST、半开连接）是最尴尬的情况：请求可能已经被后端执行了。所以重试策略是白名单：只有 GET/HEAD 等幂等方法、且错误属于可重试的瞬时类型才重试，**POST 永不自动重试**；需要更多尝试的路由通过 `max_retries` 显式声明。这是"可用性换正确性"里必须选正确性的地方。

### 4.3 复用前先探活

从池里拿出的空闲连接可能已经被对端关掉了。直接用会在写的时候收到 RST，表现为一次莫名其妙的 502。做法是取用前用 0 超时的 `MSG_PEEK` 预检：返回 0 说明对端已关闭，直接丢弃并新建连接，对上层透明。

## 5. 控制面与数据面分离

运维接口（`/admin/*`）没有和业务流量挤在同一个监听上：

- **独立端口、独立线程**（默认 `127.0.0.1:8105`，loopback，需要显式 `admin.enabled: true` 才启动）；
- 线程内是**阻塞式**的小型 HTTP 服务（`accept` + `poll` 1 秒超时），刻意不接入 epoll 事件循环——管理口流量极低，阻塞模型最简单，也不会因为一个运维请求干扰业务事件循环；
- 每个请求都校验密钥，**常量时间比较**（XOR 累积 + 遍历到较长长度）避免时序侧信道；
- 上线端点：`GET /admin/stats`（计数/延迟/uptime/版本）、`GET /admin/upstreams`（逐后端健康 + 熔断状态）、`POST /admin/reload`（先校验配置，非法则 400 且不应用任何变更）。

### reload 的单一信号源

热加载最怕"两条路径语义不一致"。这里 `SIGHUP` 和 `POST /admin/reload` 收敛到同一个原子计数器（reload generation）：信号处理函数只做 `fetch_add`，worker 在自己的安全检查点发现 generation 变化才去读配置、校验、应用。于是：

- `systemctl reload` 和运维 API 的行为完全一致；
- 非法配置永远不会被应用，且写 AUDIT 日志说明拒绝原因，旧配置继续跑；
- TLS 证书热加载失败时保留旧 SSL_CTX，在途连接不受影响。

管理口自己的密钥也支持热轮换（≤1 秒生效），但 **监听拓扑（enabled/port/bind）在启动期固定**——运行时重建监听套接字带来的失败模式（端口占用、连接中断、权限）远远超过它带来的价值。这是一个明确说"不做"的取舍，而不是遗漏。

## 6. 三个真实 Bug

### Bug 1：每个请求都有的 43ms 延迟地板

第一版压测结果非常"整齐"：轻载下 P50 稳定在 43ms，QPS 卡在 170 左右。整齐得不像噪声，像定时器。

```text
                      before            after (TCP_NODELAY)
static-light (c8)     176 QPS / 43.04ms → 949 QPS / 7.99ms
proxy-light  (c8)     169 QPS / 43.40ms → 1000 QPS / 7.54ms
handshake    (c10)    224 QPS / 42.79ms → 956 QPS / 10.09ms
proxy        (c50)    836 QPS / 53.64ms → 1083 QPS / 42.22ms
```

根因是 Nagle 算法与 delayed ACK 的经典组合：服务端把响应分成多个小段写（TLS 场景下尤其明显：头部一个记录、body 一个记录），第一个小包发出后，Nagle 会把后续小包攒着等 ACK；而对端因为只有一个未确认段，正在等 40ms 的 delayed-ACK 定时器——双方就这么互等到定时器超时。

修复就是在 accepted socket 和 upstream socket 上都设 `TCP_NODELAY`（[mysocket.cpp](../../src/common/mysocket.cpp) 的 `setnodelay()`）。**轻载延迟改善约 5.4 倍，握手吞吐约 4.3 倍**。

这个 Bug 的教训不在"要开 TCP_NODELAY"，而在：**当延迟数字稳定到不像话时，它是协议行为而不是你的代码在慢**。

### Bug 2：`close()` 发的是 RST，会吃掉刚写出的响应

管理口的 `POST /admin/reload` 不解析请求体（没有管理端点需要 body）。于是出现过这样一个场景：客户端发了带 body 的 POST，网关读完请求头、鉴权通过、把 200 响应写出去、然后关闭连接——客户端却报连接被重置、响应为空。

原因在内核：**当 `close()` 时接收缓冲区里还有未读数据，Linux 发送的是 RST 而不是 FIN**；而 RST 会让对端丢弃已收到但尚未交给应用的数据——包括我们刚刚写出的那条响应。

修复是在关闭前按 `Content-Length` 把剩余请求体读掉（有界，并且把这一阶段的 `SO_RCVTIMEO` 收紧到 1 秒，避免一个卡在中途的客户端占住串行的管理口线程）：

```cpp
// Discard the rest of the request body before the socket is closed.
// Closing a socket that still has unread received data makes the kernel send
// RST instead of FIN — and an RST can destroy a response the client has not
// read yet.
```

这个 Bug 的有趣之处在于它的触发条件是"客户端多发了你不关心的数据"，而**当时没有任何测试覆盖它**——所有集成测试和文档示例里的 POST 都不带 body。现在有一条断言专门覆盖这个组合。

### Bug 3：chunked trailer 没消费完，连接复用就会串包

后端返回 `Transfer-Encoding: chunked` 并且带 trailer（`Trailer: X-Sum`）时，一条响应的完整字节流是：若干 chunk + `0\r\n` + trailer 字段 + 结束的 `\r\n`。

如果只读到最后一个 chunk 就认为响应结束、把连接还回池子，那么残留的 trailer 与终止 CRLF 就成了"未读数据"——它们会被下一个复用该连接的请求当成响应开头的状态行来解析，症状是随机的解析错误或 502。修复方式是把 trailer 部分读到终止 CRLF 才算一条响应结束，集成测试里用一条带 trailer 的 chunked 响应 + 紧跟一次同连接请求来验证（`test_chunked_trailer`）。

顺带一提，请求方向上的同类问题也做了处理：重复 `Content-Length`、`Content-Length` 与 `Transfer-Encoding` 并存、非 chunked 的 `Transfer-Encoding`、非数字长度、非法头部字符、超长请求行/头部块一律 400 + `Connection: close`——这是 HTTP 请求走私的经典入口，宁可拒绝也不猜。

## 7. 测试策略：为什么集成测试要起真服务器

单测覆盖纯逻辑：HTTP 解析器（含各种走私向量）、LRU 缓存、路由匹配、鉴权策略、限流隔离、配置 schema 校验、TLS 上下文构建、连接池语义、客户端超时与幂等重试、健康检查与熔断状态机。

但上面三个 Bug 一个都不是单测能抓到的——它们都是"真实 TCP 行为 + 真实内核语义"的产物。所以集成套件启动**真实的网关进程 + 真实 mock 后端**（Python），用真实 TLS 连接跑端到端场景，并且断言的是可观测行为而不是内部状态：

- 10 次代理请求只允许新建 ≤4 条后端连接（证明连接池真的在复用）；
- 带 trailer 的 chunked 响应之后同一条连接还能继续用；
- 杀掉一个后端节点后，要求连续 6 次 200 才判定"故障节点已被摘除"（避免轮询恰好打到健康节点的假阳性）；
- 优雅停机时用一条正在处理 1.5 秒慢请求的连接，验证它在 drain 窗口内被完整服务；
- 管理口密钥轮换后旧 key 401 / 新 key 200。

当前状态：**91 个单测（CTest）+ 69 条集成断言**，一条 `./ci.sh` 跑完构建、单测、集成；另支持 AddressSanitizer 构建。

## 8. 现在还没做的（诚实清单）

- 单机架构，没有分布式配置中心；多实例之间不共享限流配额与熔断状态；
- 没有插件系统/Lua，扩展方式是改代码或走 upstream 代理；
- 管理口的监听拓扑（enabled/port/bind）需要重启进程才能改（密钥可热轮换）；
- 压测数据来自一台 2 vCPU 的机器（客户端、网关、Python mock 挤在同一台机器上），绝对数字意义有限，**比值**才是结论；
- 连接池在 loopback 上的吞吐收益在噪声范围内（本地 TCP 建连只有几十微秒），它的价值随后端网络距离放大——每个请求省掉一次 RTT。

## 9. 复现

```bash
git clone https://github.com/AppleAndPenAndPear/epoll-gateway
cd epoll-gateway
./ci.sh                       # 构建 + 91 单测 + 69 条集成断言
scripts/benchmark/run_benchmark.sh   # wrk 压测（15s/场景）
```

延伸阅读：[README.md](../../README.md)（能力清单）、[docs/BENCHMARKS.md](../BENCHMARKS.md)（完整压测报告）、[docs/DEPLOYMENT.md](../DEPLOYMENT.md)（systemd 部署与滚动升级）、[docs/PROJECT_STATUS.md](../PROJECT_STATUS.md)（当前状态快照）。