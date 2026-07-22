add_executable(runTests
    test_http_parser.cpp
    test_file_cache.cpp
    test_http_response.cpp
    test_route_utils.cpp
)

# 链接你的公共库和 gtest
target_link_libraries(runTests PRIVATE
    common
    GTest::GTest      # 使用 find_package 导入的目标
    GTest::Main       # 提供 main 入口
)

include(GoogleTest)
gtest_discover_tests(runTests)