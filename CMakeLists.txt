cmake_minimum_required(VERSION 3.20)

project(epollthread VERSION 0.1.0 LANGUAGES CXX)

# Use AddressSanitizer to detect memory leaks and out-of-bounds accesses
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address -fno-omit-frame-pointer")
set(CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fsanitize=address")

# 1. C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 2. Default build type is Release; to use Debug, specify it on the command line
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Use find_package to locate spdlog installed via vcpkg
find_package(spdlog CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# Create the static library libcommon with code shared by the server and the client
set(COMMON_SOURCES
    src/common/poller.cpp
    src/common/mysocket.cpp
    src/common/myepoll.cpp
    src/common/error_utils.cpp
    src/common/mylogger.cpp
    src/common/fd_cache.cpp
    src/common/file_cache.cpp
    src/common/http_parser.cpp
)
add_library(common STATIC ${COMMON_SOURCES})
target_include_directories(common PUBLIC include/common)
target_link_libraries(common PUBLIC
    spdlog::spdlog
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
)
# Optional: set compiler warning options for common
target_compile_options(common PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-Wall -Wextra>
)

# Server executable
set(SERVER_SOURCES
    src/server/main.cpp
    src/server/server.cpp
    src/server/http_handler.cpp
    src/server/http_client.cpp
    src/server/pool.cpp
    src/server/content_type.cpp
    src/server/tcpworker.cpp
    src/server/tls_context.cpp
    src/server/gzip_utils.cpp
    src/server/metrics.cpp
    src/server/upstream_manager.cpp
    src/server/rate_limiter.cpp
)
# Create the server executable
add_executable(server ${SERVER_SOURCES})
# Link against the common library
target_link_libraries(server PRIVATE common z OpenSSL::SSL OpenSSL::Crypto)
target_include_directories(server PRIVATE include/server)

# Client executable
set(CLIENT_SOURCES
    src/client/main.cpp
    src/client/client.cpp
    src/client/clienthandler.cpp
)
add_executable(client ${CLIENT_SOURCES})
target_link_libraries(client PRIVATE common OpenSSL::SSL OpenSSL::Crypto)
target_include_directories(client PRIVATE include/client)

# Generate the compile_commands.json file
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Install rules (install the executables into the bin directory)
install(TARGETS server client RUNTIME DESTINATION bin)

# Unit test option
option(BUILD_TESTS "Build unit tests" OFF)

if(BUILD_TESTS)
    find_package(GTest REQUIRED)      # Locate the system-installed gtest
    enable_testing()
    add_subdirectory(tests)
endif()