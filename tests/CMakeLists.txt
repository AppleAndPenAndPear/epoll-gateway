add_executable(runTests
    test_http_parser.cpp
    test_file_cache.cpp
    test_http_response.cpp
    test_route_utils.cpp
    test_auth_and_rate_limit.cpp
    test_http_client.cpp
    test_connection_pool.cpp
    test_upstream_manager.cpp
    test_config_validation.cpp
    test_tls_context.cpp
    test_tls_utils.cpp
    test_client_tls.cpp
    ${CMAKE_SOURCE_DIR}/src/server/rate_limiter.cpp
    ${CMAKE_SOURCE_DIR}/src/server/http_client.cpp
    ${CMAKE_SOURCE_DIR}/src/server/connection_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/server/upstream_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/server/tls_context.cpp
)

target_compile_definitions(runTests PRIVATE GW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# Link your common library and gtest
target_link_libraries(runTests PRIVATE
    common
    GTest::GTest      # Imported targets from find_package
    GTest::Main       # Provides the main entry point
)

target_include_directories(runTests PRIVATE
    ${CMAKE_SOURCE_DIR}/include/server
    ${CMAKE_SOURCE_DIR}/include/client
)

include(GoogleTest)
gtest_discover_tests(runTests)