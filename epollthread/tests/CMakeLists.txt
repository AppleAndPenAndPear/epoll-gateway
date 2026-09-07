add_executable(runTests
    test_http_parser.cpp
    test_file_cache.cpp
    test_http_response.cpp
    test_route_utils.cpp
    test_auth_and_rate_limit.cpp
    test_http_client.cpp
    test_upstream_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/server/rate_limiter.cpp
    ${CMAKE_SOURCE_DIR}/src/server/http_client.cpp
    ${CMAKE_SOURCE_DIR}/src/server/upstream_manager.cpp
)

# 链接你的公共库和 gtest
target_link_libraries(runTests PRIVATE
    common
    GTest::GTest      # 使用 find_package 导入的目标
    GTest::Main       # 提供 main 入口
)

target_include_directories(runTests PRIVATE ${CMAKE_SOURCE_DIR}/include/server)

include(GoogleTest)
gtest_discover_tests(runTests)