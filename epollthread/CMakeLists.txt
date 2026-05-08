cmake_minimum_required(VERSION 3.20)

project(epollthread VERSION 0.1.0 LANGUAGES CXX)

# 1. C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 2. 构建类型默认为 release,命令行不修改构建类型即为Release；想使用Debug，在命令行指定即可
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 使用 find_package 来查找 vcpkg 安装的 spdlog
find_package(spdlog CONFIG REQUIRED)
find_package(Threads REQUIRED)

#创建静态库 libcommon，包含服务端和客户端共享的代码
set(COMMON_SOURCES
    src/common/mysocket.cpp
    src/common/myepoll.cpp
    src/common/error_utils.cpp
    src/common/mylogger.cpp
)
add_library(common STATIC ${COMMON_SOURCES})
target_include_directories(common PUBLIC include)
target_link_libraries(common PUBLIC
    spdlog::spdlog
    Threads::Threads
)
# 可选：为 common 设置编译警告选项
target_compile_options(common PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-Wall -Wextra>
)

#服务端可执行文件
set(SERVER_SOURCES
    src/server/main.cpp
    src/server/server.cpp
    src/server/echohandler.cpp
    src/server/pool.cpp
)
#创建服务端的可执行文件
add_executable(server ${SERVER_SOURCES})
#链接第common库
target_link_libraries(server PRIVATE common)

#客户端可执行文件
set(CLIENT_SOURCES
    src/client/main.cpp
    src/client/client.cpp
    src/client/clienthandler.cpp
)
add_executable(client ${CLIENT_SOURCES})
target_link_libraries(client PRIVATE common)

# 生成json文件
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 安装规则（将可执行文件安装到 bin 目录）
install(TARGETS server client RUNTIME DESTINATION bin)