# 使用 Ubuntu 22.04 作为基础镜像（你也可以换成 24.04）
FROM ubuntu:22.04 AS builder

# 安装编译依赖
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        g++ \
        cmake \
        make \
        nlohmann-json3-dev \
        libspdlog-dev \
        zlib1g-dev && \
    rm -rf /var/lib/apt/lists/*

# 设置工作目录
WORKDIR /app

# 复制源代码
COPY include/ include/
COPY src/ src/
COPY CMakeLists.txt .
COPY config.json .

# 创建静态文件目录（至少放一个测试页）
RUN mkdir -p www && \
    echo '<h1>It works!</h1>' > www/index.html

# 构建
RUN mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    cmake --build . -j$(nproc) && \
    cp server /app/server

# 最终镜像：轻量运行环境
FROM ubuntu:22.04

# 安装 spdlog 运行时库（仅需 libspdlog1）
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        libspdlog1 \
        zlib1g && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /app/server .
COPY --from=builder /app/www/ ./www/
COPY --from=builder /app/config.json .

# 暴露端口
EXPOSE 5005

# 启动命令
CMD ["./server"]