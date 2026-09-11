# Project Status

## Current Stage

商用 API 网关基础能力建设。当前已经完成从 epoll HTTP 服务器向配置驱动网关的核心演进，正在进行可维护性、协议语义和生产运维能力的收口。

## Current Validation

- C++17 Release 构建通过。
- CTest：30/30 通过。
- 已覆盖 HTTP parser、文件缓存、响应序列化、路由、安全策略、上游超时、重试、健康检查和熔断基础行为。
- Debug 构建支持 AddressSanitizer。

## Completed

### 1. 网络与并发基础

- 基于 Linux `epoll` 的非阻塞 Reactor。
- `SO_REUSEPORT + One Loop Per Worker`：每个 `TcpWorker` 持有独立 listen socket 和 epoll loop。
- `EPOLLET + EPOLLONESHOT` 事件管理，避免重复触发和惊群。
- Socket、Epoll 等资源使用 RAII 封装。
- 支持优雅关闭：`SIGINT`/`SIGTERM` 通知 Worker 退出并清理连接。
- 支持 Keep-Alive、连接空闲超时和同一连接上的串行多请求处理。

### 2. HTTP/HTTPS 协议能力

- HTTP/1.1 请求状态机解析。
- 支持请求行、Header、Query、Body 和 Chunked 传输解析。
- 当前支持 `GET`、`HEAD`、`POST`、`PUT`、`DELETE`。
- OpenSSL 非阻塞 TLS 握手和加密读写。
- 响应支持 Content-Length、Chunked、Content-Type、Content-Encoding。
- 支持 Gzip 响应压缩和 `Accept-Encoding` 协商。
- 统一返回 `X-Trace-Id`，用于请求和日志关联。

### 3. 路由与请求分发

- 内置路由通过 `register_default_routes()` 注册。
- 配置文件路由通过 `register_configured_routes()` 注册。
- 路由配置来自 `config.json` 的 `routes` 字段。
- 支持 method、path、Host、Tenant 四个匹配条件。
- 支持精确路径、通配符路径和路径参数，例如 `/users/{id}`。
- 一次请求只执行一次路由匹配，`ResolvedRoute` 在鉴权、限流和实际分发之间复用。
- 支持三类目标：
	- `local`：本地 Handler。
	- `upstream`：反向代理到后端服务。
	- `static`：从指定静态根目录读取文件。
- 已区分：
	- `404`：路径、Host 或 Tenant 没有匹配。
	- `405`：路径匹配但方法不允许。
- `405` 响应支持 `Allow` Header，返回允许的方法集合。
- 未命中网关路由时，GET/HEAD 可以进入静态文件 fallback。

### 4. 鉴权、租户与限流

- 支持 `X-API-Key` 和 `Authorization: Bearer` 两种 API Key 提取方式。
- 支持路由级 API Key allow/deny 列表。
- 支持 API Key 的 Host 范围和 Tenant 范围绑定。
- 支持路由级鉴权开关和匿名访问配置。
- 基于 Token Bucket 实现限流。
- 支持按客户端 IP、API Key 或路由维度限流。
- RateLimiterManager 跨 Worker 共享，并定期清理长期未使用的 limiter。
- API Key 可拥有独立的限流容量和补充速率。

### 5. 上游代理与可靠性

- `UpstreamManager` 维护 upstream 服务组和后端节点。
- 支持轮询选择后端节点。
- 支持主动 TCP 健康检查，并标记不健康节点。
- 支持连接、写入和读取阶段的超时控制。
- 使用结构化 `BackendError` 区分：
	- 连接失败
	- 连接超时
	- 写入失败
	- 写入超时
	- 读取失败
	- 读取超时
	- 非法上游响应
- 重试策略只允许幂等方法和可重试的临时错误。
- 路由可配置额外重试次数 `max_retries`。
- 已实现按 upstream/backend 维度的基础熔断器：
	- 连续失败达到 `circuit_failure_threshold` 后进入 OPEN。
	- `circuit_recovery_timeout_ms` 后允许恢复探测。
	- `half_open_probe` 防止多个请求同时冲击正在恢复的后端。
	- 探测成功关闭熔断，失败则继续保持熔断。
- 上游错误映射到 502、503、504，并写入 Metrics 和审计分类。

### 6. 静态文件与缓存

- 支持静态文件服务和常见 Content-Type 推断。
- 大文件使用 `sendfile` 零拷贝。
- 小文件支持 Worker 内独立 LRU 内容缓存。
- 支持基于 TTL 和 mtime 的 FD 缓存。
- Gzip 内容可以进入独立压缩缓存。
- 静态文件路径经过安全检查，防止 `..` 路径穿越。
- 未匹配路由的静态 fallback 只允许 GET/HEAD，避免使用 POST 等方法读取静态资源。

### 7. 可观测性与日志

- 请求没有外部 Trace ID 时由网关生成 `trace_id`。
- 客户端携带 `X-Trace-Id` 时会沿用并回传。
- AUDIT 日志记录失败和安全策略事件，包括：
	- trace_id
	- method/path
	- Host/Tenant
	- route
	- status
	- failure reason
	- 脱敏后的 API Key
	- User-Agent
- AUDIT 在响应完成时统一记录，避免鉴权、限流、路由分支重复写入。
- CLF 访问日志记录所有完成请求的客户端 IP、方法、路径、状态码、响应大小和耗时。
- 普通 `Request:` 入口日志已经降为 debug，避免与 CLF 在生产 info 级别重复。
- Prometheus `/metrics` 端点已提供：
	- 全局 2xx/3xx/4xx/5xx 计数
	- 全局延迟 histogram
	- 文件缓存、FD 缓存、Gzip 缓存统计
	- upstream 错误类型统计
	- route/host/tenant 维度的请求数和延迟 sum/count

### 8. 配置与运行时运维

- `Config::from_file()` 统一解析 JSON 配置。
- 路由、upstream、API Key、限流、超时和静态根目录均可配置。
- 支持 `SIGHUP` 触发 runtime reload。
- 信号处理器只递增 reload generation，不执行文件 IO 和 JSON 解析。
- Worker 在安全检查点读取并应用新配置。
- reload 会更新路由、upstream、API Key、默认限流配置和 Keep-Alive 超时。
- reload 前检查 JSON 文件是否存在且格式有效，避免损坏配置覆盖当前有效配置。
- 提供 Dockerfile、docker-compose、Prometheus 配置和启动脚本。

### 9. 测试与工程能力

- Google Test 测试集当前 30/30 通过。
- 已覆盖：
	- HTTP 请求解析
	- Query 和 Body
	- LRU 文件缓存
	- FD/响应相关基础行为
	- 路由参数与 Host/Tenant 匹配
	- 404/405 路由语义基础
	- 静态路径穿越防护
	- API Key 授权
	- 限流器和不同 Key 的隔离
	- 上游超时、连接失败和幂等重试
	- 上游健康检查
	- 熔断打开、拒绝和恢复探测

## Request Flow

```text
客户端连接
	-> TLS 握手（HTTPS）
	-> epoll 读取数据
	-> HttpParser 解析完整请求
	-> 生成或接收 trace_id
	-> 提取 Host/Tenant
	-> 一次路由匹配
	-> 404/405 判断
	-> API Key 鉴权
	-> 限流
	-> 本地 Handler / 静态文件 / 上游转发
	-> 上游超时、重试和熔断
	-> 构造响应
	-> 写回客户端
	-> Metrics 记录
	-> 失败时写 AUDIT
	-> 所有完成请求写 CLF
```

## Known Limitations

- 熔断状态目前由每个 Worker 独立维护，不是所有 Worker 共享的全局熔断状态。
- route/host/tenant 维度当前提供 latency sum/count，还没有独立 histogram，因此不能直接得到每个维度的 P95/P99。
- runtime reload 采用 Worker 周期检查，各 Worker 不保证在完全相同的时刻切换配置。
- 当前配置合法性检查主要验证 JSON 可解析，尚未提供完整的字段类型、范围和路由冲突校验。
- 路由匹配仍是线性遍历，当前规模下简单可靠，尚未针对大规模路由使用 Trie 或索引结构。
- 尚未实现连接池、异步 upstream、多级负载均衡算法和分布式限流。
- 尚未完整接入 OpenTelemetry `traceparent`、分布式 Trace 和外部审计存储。
- 当前测试以单元测试为主，真实 TLS、HTTP/1.1 长连接、reload、端到端 upstream 场景仍需要补充集成测试。
- 动态线程池接口存在，但当前 HTTP 主流程仍主要在 Worker 线程中执行。

## Next Steps

1. 补充 runtime reload 和真实 HTTP 端到端测试。
2. 增加配置字段范围校验、路由冲突检查和 reload 失败审计。
3. 完善 route/host/tenant 维度的 latency histogram 和失败率指标。
4. 评估跨 Worker 共享熔断状态的实现方式。
5. 根据真实路由规模和压测结果，再决定是否引入路由索引、连接池或异步 upstream。

## Recent Decisions

- `register_default_routes()` 负责内置演示和基础端点；`register_configured_routes()` 负责从配置生成网关路由。
- 路由结果只匹配一次，并在请求生命周期内复用。
- CLF 记录所有访问，AUDIT 主要记录失败和安全策略事件。
- `Request:` 普通入口日志降为 debug，避免与 CLF 重复占用生产 info 日志。
- API Key 在 AUDIT 中只保留脱敏值，避免凭证泄露。
- 路由存在但方法不支持返回 405，并带 `Allow` Header；路径不存在才进入 404 或静态 fallback。