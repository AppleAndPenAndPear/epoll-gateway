<!-- ============ 发帖说明（粘贴时删除本注释块） ============ -->
<!-- 目标版块：r/cpp 独立帖（Text Post）。备选：r/cpp 每月 "What Are You Working On?" 汇总帖（新号更稳妥的入口，只需贴下面正文的头两段 + 仓库链接）。 -->
<!-- 标题用下面 Suggested title；发帖后自己先在评论区补一条背景说明（帖子描述了什么、想听什么反馈），作者在场是 r/cpp 的不成文规则。 -->
<!-- 全文英文，长度约 450 词，链接均为 GitHub 绝对链接。 -->

<!-- ============ Suggested title ============ -->
<!--
I wrote an API gateway from scratch in C++17 (epoll, SO_REUSEPORT, one loop per thread) — architecture retrospective with an honest nginx comparison
-->

<!-- ============ 正文（从这里开始粘贴） ============ -->

An architecture retrospective, not a tutorial. Every number and every bug below comes from one real project: **[epollthread](https://github.com/AppleAndPenAndPear/epoll-gateway)** — a single-binary API gateway in C++17 with no etcd/Postgres/Redis runtime dependency. The full write-up lives [here](https://github.com/AppleAndPenAndPear/epoll-gateway/blob/master/docs/articles/01-architecture.md); this post is the short version, and I'm happy to go deep on anything in the comments.

**Scope.** For small-to-mid API traffic: one binary, one JSON config. Auth, rate limiting, circuit breaking, health checks, Prometheus metrics, hot reload, graceful shutdown are in; a control-plane cluster, Lua and a plugin ABI are deliberately not. Upstreams live in the config file, not in a service-discovery stack. The constraint that drove most decisions: a codebase one person can read end to end.

**An honest number first.** Same-conditions comparison against nginx 1.24 on a dedicated 4-vCPU box — same certificate, same route, same four backends, role-pinned cores, paired-round analysis: throughput is within **~1.4x of nginx** (inside a pre-agreed ≤1.5x stop rule; full table in [BENCHMARKS.md](https://github.com/AppleAndPenAndPear/epoll-gateway/blob/master/docs/BENCHMARKS.md)). An earlier 2.5–2.7x reading turned out to be a measurement artifact of a saturated test fixture plus shared-core contention, which is exactly why I now run interleaved rounds with per-round pairing and report the methodology in a script. The differentiation is not performance — it's the deployment surface: zero runtime dependencies, one config file, and code you can actually audit.

**Three bugs that only showed up under real traffic**, each now covered by a dedicated integration assertion:

1. **The 43 ms latency floor.** Everything was fast in tests, then `wrk` showed a flat ~43 ms floor on small responses. Nagle + delayed ACK interaction; fixed by enabling `TCP_NODELAY`. The embarrassing part: it was invisible until we measured against nginx side by side.
2. **`close()` where `shutdown()` was needed.** A client that sent extra unread bytes before half-closing made `close()` emit an RST that discarded the pending response. Symptom: truncated responses that only happened when a client "talked out of turn". The fix is a proper half-close sequence, and the trigger condition had zero test coverage until it bit.
3. **Unconsumed chunked trailers corrupt the next response.** Returning a keep-alive connection to the pool after the last chunk — but before the trailer block and terminating CRLF — makes the next request on that connection parse trailer bytes as a status line. Random parse errors / 502s. Fixed by consuming the full trailer sequence; the same hardening now rejects smuggling vectors (duplicate `Content-Length`, CL+TE conflicts) with 400 + `Connection: close`.

**Testing**: 137 unit tests + 155 integration assertions, all against a real gateway process with real TLS and real backends (`./ci.sh`). The integration suite asserts observable behavior — e.g. 10 proxied requests must open ≤4 backend connections, and graceful shutdown must fully serve an in-flight 1.5 s request.

Repo: <https://github.com/AppleAndPenAndPear/epoll-gateway> (MIT, v0.3.0, x86_64/aarch64 tarballs + multi-arch image on ghcr.io). If you've solved the "small internal gateway that isn't a nginx config matrix and doesn't need a database" problem differently, I'd genuinely like to hear how.
