# Changelog

记录项目的重要变更。格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，按日期倒序排列。

> 详细的项目现状快照见 [docs/PROJECT_STATUS.md](docs/PROJECT_STATUS.md)，本文件用于回溯「什么时候做了什么、为什么」。

## 2026-09-11

### 文档

- 根据 [PROJECT_STATUS.md](docs/PROJECT_STATUS.md) 与实际代码核对，全面更新 README：
  - 修正 `/metrics` 指标名为实际输出（`epoll_http_*` 系列，原文的 `epoll_server_*` 在代码中不存在）。
  - 补充 route/host/tenant 维度指标（`epoll_route_requests_total`、`epoll_route_latency_seconds_*`）和 upstream 错误分类指标。
  - 特性列表新增幂等重试、基础熔断器、`X-Trace-Id`、AUDIT/CLF 双通道日志、`SIGHUP` runtime reload。
  - 新增「可观测性日志」「Runtime Reload」「已知限制」章节。
  - 配置示例补充 `max_retries`、`circuit_failure_threshold`、`circuit_recovery_timeout_ms`。
  - 修复后半部分丢失的 markdown 格式（技术栈、核心设计细节等重构为标准表格/列表）。

## 2026-09-08

### 新增

- 熔断器：按 upstream/backend 维度维护，连续失败达 `circuit_failure_threshold` 进入 OPEN，`circuit_recovery_timeout_ms` 后允许半开探测，探测成功关闭熔断（决策：先做 per-Worker 独立熔断，跨 Worker 共享状态待评估）。
- 幂等重试：仅幂等方法与可重试的临时错误重试，路由可配置 `max_retries`。
- `X-Trace-Id` 请求追踪：客户端携带时沿用并回传，否则网关生成，贯穿请求与审计日志。
- AUDIT/CLF 双通道日志：CLF 记录所有完成请求；AUDIT 在响应完成时统一记录失败与安全策略事件，API Key 只保留脱敏值。
- 405 语义收口：路径匹配但方法不支持返回 405 并携带 `Allow` Header；未命中网关路由时仅 GET/HEAD 回退静态文件。
- runtime reload：`SIGHUP` 触发，信号处理器只递增 generation，Worker 在安全检查点应用新配置；reload 前校验 JSON 有效性。
- upstream 错误统一映射到 502/503/504，并写入 Metrics 与审计分类。

### 变更

- 普通 `Request:` 入口日志降为 debug，避免与 CLF 在生产 info 级别重复。
- 一次请求只匹配一次路由，`ResolvedRoute` 在鉴权、限流和分发之间复用。

### 测试

- 单元测试扩充至 30/30 通过，新增熔断打开/拒绝/恢复探测、幂等重试、上游超时等用例。

## 2026-08-30

### 新增

- API Key 管理：支持 `X-API-Key` 与 `Authorization: Bearer` 提取，route allow/deny 列表，API Key 的 host/tenant 范围绑定和独立限流额度。

## 2026-08-13 ~ 2026-08-14

### 新增

- 反向代理核心功能：`UpstreamManager` 轮询选路 + 主动 TCP 健康检查（自动摘除/恢复故障节点），`HttpClient` 带超时的单后端转发，结构化 `BackendError` 错误分类。

## 2026-08-11

### 新增

- HTTPS 支持：OpenSSL 非阻塞 TLS 握手与加密读写，运行在 epoll 事件循环内。

## 2026-08-04

### 新增

- Prometheus 指标端点 `/metrics`；docker-compose 集成 Prometheus + Grafana 监控栈。

## 2026-07-21 ~ 2026-07-23

### 新增

- FD 文件描述符缓存（TTL）、多级缓存命中率指标、P99 延迟 histogram。

### 变更

- `http_parser` 移入 common 库；调整日志队列、异步线程数与滚动策略。

## 2026-06-11

### 新增

- Google Test 单元测试框架接入、Docker 多阶段构建、`build.sh`/`start.sh` 脚本、RESTful 路由与静态文件服务。

## 2026-05-08 ~ 2026-05-27

### 新增

- 项目初始化：SO_REUSEPORT + One Loop Per Thread 多线程 epoll 服务器、HTTP/1.1 状态机解析器、Keep-Alive、零拷贝 sendfile、LRU 文件缓存、spdlog 异步日志、MIT License。
