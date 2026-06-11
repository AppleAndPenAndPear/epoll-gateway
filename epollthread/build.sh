#!/bin/bash
set -e  # 遇到错误立即退出

BUILD_DIR="build"
BUILD_TYPE="${1:-Release}"   # 第一个参数作为构建类型，默认 Release;Debug 在命令后带参数Debug即可：./build.sh Debug

echo "==> 清理旧的构建文件..."
if [ -d "$BUILD_DIR" ]; then
    rm -rf "${BUILD_DIR:?}"/*           # 删除所有可见文件/文件夹
    find "$BUILD_DIR" -mindepth 1 -maxdepth 1 -name '.*' ! -name '.' ! -name '..' -exec rm -rf {} +  2>/dev/null  # 删除隐藏文件（如 .cache）
fi
mkdir -p "$BUILD_DIR"

echo "==> 配置 CMake (${BUILD_TYPE}) ..."
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE="$BUILD_TYPE"

echo "==> 编译 (使用 $(nproc) 个核心) ..."
cmake --build . -j$(nproc)

echo "==> 编译完成！可执行文件位于 $BUILD_DIR/server 和 $BUILD_DIR/client"
cd ..