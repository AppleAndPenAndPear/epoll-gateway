# Use Ubuntu 22.04 as the base image (you can also switch to 24.04)
FROM ubuntu:22.04 AS builder

# Install build dependencies
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        g++ \
        cmake \
        make \
        nlohmann-json3-dev \
        libspdlog-dev \
        zlib1g-dev \
        libssl-dev && \
    rm -rf /var/lib/apt/lists/*

# Set the working directory
WORKDIR /app

# Copy the source code
COPY include/ include/
COPY src/ src/
COPY CMakeLists.txt .
COPY config.json .

# Create the static files directory (at least one test page)
RUN mkdir -p www && \
    echo '<h1>It works!</h1>' > www/index.html

# Build
RUN mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    cmake --build . -j$(nproc) && \
    cp server /app/server

# Final image: lightweight runtime environment
FROM ubuntu:22.04

# Install the spdlog runtime library (only libspdlog1 is needed)
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        libspdlog1 \
        zlib1g \
        libssl3 && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /app/server .
COPY --from=builder /app/www/ ./www/
COPY --from=builder /app/config.json .

# Expose the port
EXPOSE 5005

# Start command
CMD ["./server"]