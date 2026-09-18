#include <gtest/gtest.h>
#include <fstream>
#include <sys/stat.h>
#include "config.h"

namespace {
// Writes a temp JSON file under /tmp and returns its path.
std::string write_temp_config(const std::string& name, const std::string& content) {
    const std::string path = "/tmp/gw_test_" + name + ".json";
    std::ofstream ofs(path, std::ios::trunc);
    ofs << content;
    return path;
}
}  // namespace

TEST(ConfigValidationTest, AcceptsDefaults) {
    Config config;
    EXPECT_TRUE(Config::validate(config).empty());
}

TEST(ConfigValidationTest, RejectsAdminEnabledWithoutKeys) {
    Config config;
    config.admin.enabled = true;
    config.admin.api_keys.clear();
    const auto errors = Config::validate(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("admin.api_keys") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(ConfigValidationTest, AcceptsAdminEnabledWithKeys) {
    Config config;
    config.admin.enabled = true;
    config.admin.api_keys = {"admin-secret"};
    EXPECT_TRUE(Config::validate(config).empty());
}

TEST(FromFileSchemaTest, ParsesAdminSection) {
    const std::string path = write_temp_config("admin_section", R"({
        "admin": {"enabled": true, "port": 8105, "bind": "127.0.0.1",
                  "api_keys": ["admin-secret"]}
    })");
    std::vector<std::string> errors;
    const Config config = Config::from_file(path, &errors);
    EXPECT_TRUE(errors.empty()) << (errors.empty() ? "" : errors[0]);
    EXPECT_TRUE(config.admin.enabled);
    EXPECT_EQ(config.admin.port, 8105);
    EXPECT_EQ(config.admin.bind, "127.0.0.1");
    ASSERT_EQ(config.admin.api_keys.size(), 1u);
    EXPECT_EQ(config.admin.api_keys[0], "admin-secret");
}

TEST(ConfigValidationTest, RejectsZeroPort) {
    Config config;
    config.port = 0;
    EXPECT_FALSE(Config::validate(config).empty());
}

TEST(ConfigValidationTest, RejectsBadThreadPool) {
    Config config;
    config.thread_pool.max_threads = 1;
    config.thread_pool.min_threads = 5;
    EXPECT_FALSE(Config::validate(config).empty());

    config = Config{};
    config.thread_pool.min_threads = 0;
    EXPECT_FALSE(Config::validate(config).empty());
}

TEST(ConfigValidationTest, RejectsZeroRateLimit) {
    Config config;
    config.rate_limit_config.capacity = 0;
    EXPECT_FALSE(Config::validate(config).empty());

    config = Config{};
    config.rate_limit_config.refill_per_second = 0;
    EXPECT_FALSE(Config::validate(config).empty());
}

TEST(ConfigValidationTest, RejectsDuplicateRouteName) {
    Config config;
    GatewayRoute a;
    a.name = "dup";
    a.path = "/a";
    GatewayRoute b;
    b.name = "dup";
    b.path = "/b";
    config.upstream_config.routes = {a, b};
    std::vector<std::string> errors = Config::validate(config);
    EXPECT_FALSE(errors.empty());
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("duplicate route name") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(ConfigValidationTest, RejectsConflictingRouteMatch) {
    Config config;
    GatewayRoute a;
    a.name = "one";
    a.method = "GET";
    a.host = "*";
    a.tenant = "*";
    a.path = "/api/x";
    GatewayRoute b = a;
    b.name = "two";
    config.upstream_config.routes = {a, b};
    std::vector<std::string> errors = Config::validate(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("duplicate route match") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(ConfigValidationTest, RejectsUnknownUpstreamReference) {
    Config config;
    GatewayRoute route;
    route.name = "proxy";
    route.path = "/api";
    route.target_type = "upstream";
    route.upstream_target.name = "missing";
    config.upstream_config.routes.push_back(route);
    std::vector<std::string> errors = Config::validate(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("unknown upstream 'missing'") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(ConfigValidationTest, AcceptsKnownUpstreamReference) {
    Config config;
    Upstream up;
    up.servers.push_back(UpstreamServer{"127.0.0.1", 8080});
    config.upstream_config.upstreams["backend"] = up;
    GatewayRoute route;
    route.name = "proxy";
    route.path = "/api";
    route.target_type = "upstream";
    route.upstream_target.name = "backend";
    config.upstream_config.routes.push_back(route);
    EXPECT_TRUE(Config::validate(config).empty());
}

TEST(ConfigValidationTest, RejectsBadTargetTypeAndPolicy) {
    Config config;
    GatewayRoute route;
    route.name = "bad";
    route.target_type = "magic";
    route.rate_limit_policy = "unlimited";
    config.upstream_config.routes.push_back(route);
    std::vector<std::string> errors = Config::validate(config);
    EXPECT_EQ(errors.size(), 2);
}

TEST(ConfigValidationTest, RejectsStaticRouteWithoutRoot) {
    Config config;
    GatewayRoute route;
    route.name = "static";
    route.target_type = "static";
    route.static_root = "";
    config.upstream_config.routes.push_back(route);
    std::vector<std::string> errors = Config::validate(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("static_root") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(ConfigValidationTest, RejectsBadPathAndEmptyName) {
    Config config;
    GatewayRoute route;
    route.name = "";
    route.path = "no-leading-slash";
    config.upstream_config.routes.push_back(route);
    std::vector<std::string> errors = Config::validate(config);
    EXPECT_GE(errors.size(), 2);
}

TEST(ConfigValidationTest, RejectsBadUpstreamServer) {
    Config config;
    Upstream up;
    up.servers.push_back(UpstreamServer{"", 0});
    config.upstream_config.upstreams["backend"] = up;
    std::vector<std::string> errors = Config::validate(config);
    EXPECT_GE(errors.size(), 2);
}

TEST(ConfigValidationTest, RejectsEmptyTlsPaths) {
    Config config;
    config.tls.cert_path = "";
    std::vector<std::string> errors = Config::validate(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("tls.cert_path") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);

    config = Config{};
    config.tls.key_path = "";
    errors = Config::validate(config);
    found = false;
    for (const auto& e : errors) {
        if (e.find("tls.key_path") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(ConfigValidationTest, RejectsDuplicateApiKey) {
    Config config;
    ApiKeyConfig a;
    a.key = "secret";
    ApiKeyConfig b;
    b.key = "secret";
    config.api_keys = {a, b};
    std::vector<std::string> errors = Config::validate(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("duplicate api key") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(ConfigValidationTest, RejectsEmptyApiKey) {
    Config config;
    ApiKeyConfig ak;
    ak.key = "";
    config.api_keys.push_back(ak);
    std::vector<std::string> errors = Config::validate(config);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find(".key: must not be empty") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(FromFileSchemaTest, AcceptsValidConfig) {
    const std::string path = write_temp_config("valid", R"({
        "port": 6000,
        "backlog": 512,
        "keepalive_timeout": 30,
        "thread_pool": {"min": 2, "max": 8, "scale_up": 3, "scale_down": 1},
        "upstreams": {"backend": {"servers": [{"host": "127.0.0.1", "port": 9000}],
                                  "algorithm": "round_robin"}},
        "routes": [{"name": "api", "method": "GET", "path": "/api/*",
                    "target_type": "upstream",
                    "upstream_target": {"name": "backend", "timeout_ms": 2000}}],
        "rate_limit": {"capacity": 10, "refill_per_second": 5},
        "api_keys": [{"key": "k1", "name": "test key"}]
    })");
    std::vector<std::string> errors;
    Config config = Config::from_file(path, &errors);
    for (const std::string& e : Config::validate(config)) errors.push_back(e);
    EXPECT_TRUE(errors.empty()) << errors[0];
    EXPECT_EQ(config.port, 6000);
    EXPECT_EQ(config.keepalive_timeout, 30);
    EXPECT_EQ(config.upstream_config.routes.size(), 1);
    EXPECT_EQ(config.upstream_config.routes[0].upstream_target.timeout_ms, 2000);
}

TEST(FromFileSchemaTest, RejectsOutOfRangePort) {
    // 70000 would silently truncate to a wrapped value without a range check
    const std::string path = write_temp_config("bad_port", R"({"port": 70000})");
    std::vector<std::string> errors;
    Config config = Config::from_file(path, &errors);
    ASSERT_EQ(errors.size(), 1);
    EXPECT_NE(errors[0].find("port"), std::string::npos);
    EXPECT_NE(errors[0].find("out of range"), std::string::npos);
    EXPECT_EQ(config.port, 5005);  // default kept
}

TEST(FromFileSchemaTest, RejectsNonIntegerPort) {
    const std::string path = write_temp_config("str_port", R"({"port": "http"})");
    std::vector<std::string> errors;
    Config config = Config::from_file(path, &errors);
    ASSERT_EQ(errors.size(), 1);
    EXPECT_NE(errors[0].find("expected an integer"), std::string::npos);
    EXPECT_EQ(config.port, 5005);
}

TEST(FromFileSchemaTest, RejectsNegativeThreshold) {
    const std::string path = write_temp_config("neg_rl", R"({"rate_limit": {"capacity": -5}})");
    std::vector<std::string> errors;
    Config config = Config::from_file(path, &errors);
    ASSERT_EQ(errors.size(), 1);
    EXPECT_NE(errors[0].find("rate_limit.capacity"), std::string::npos);
    EXPECT_EQ(config.rate_limit_config.capacity, 100);  // default kept
}

TEST(FromFileSchemaTest, RejectsWrongStringType) {
    const std::string path = write_temp_config("bad_www", R"({"www_root": 123})");
    std::vector<std::string> errors;
    Config config = Config::from_file(path, &errors);
    ASSERT_EQ(errors.size(), 1);
    EXPECT_NE(errors[0].find("www_root"), std::string::npos);
    EXPECT_EQ(config.www_root, "./www");
}

TEST(FromFileSchemaTest, RejectsCorruptedJson) {
    const std::string path = write_temp_config("corrupt", "{not valid json");
    std::vector<std::string> errors;
    Config config = Config::from_file(path, &errors);
    ASSERT_EQ(errors.size(), 1);
    EXPECT_NE(errors[0].find("invalid JSON"), std::string::npos);
}

TEST(FromFileSchemaTest, ParsesTlsSection) {
    const std::string path = write_temp_config("tls", R"({
        "tls": {"cert_path": "/etc/ssl/gw.crt", "key_path": "/etc/ssl/gw.key"}
    })");
    std::vector<std::string> errors;
    Config config = Config::from_file(path, &errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(config.tls.cert_path, "/etc/ssl/gw.crt");
    EXPECT_EQ(config.tls.key_path, "/etc/ssl/gw.key");
}

TEST(FromFileSchemaTest, RejectsNonObjectTlsSection) {
    const std::string path = write_temp_config("tls_bad", R"({"tls": [1, 2]})");
    std::vector<std::string> errors;
    Config config = Config::from_file(path, &errors);
    ASSERT_EQ(errors.size(), 1);
    EXPECT_NE(errors[0].find("tls: expected an object"), std::string::npos);
}

TEST(ApiKeysSourceTest, LoadsKeysFromFile) {
    const std::string keys_path = write_temp_config("keys_file",
        R"([{"key": "file-key-1", "name": "from file",
             "rate_limit": {"capacity": 7, "refill_per_second": 3}}])");
    const std::string cfg_path = write_temp_config("keys_cfg",
        R"({"api_keys_file": ")" + keys_path + R"("})");
    std::vector<std::string> errors;
    Config config = Config::from_file(cfg_path, &errors);
    for (const std::string& e : Config::validate(config)) errors.push_back(e);
    EXPECT_TRUE(errors.empty()) << (errors.empty() ? "" : errors[0]);
    ASSERT_EQ(config.api_keys.size(), 1);
    EXPECT_EQ(config.api_keys[0].key, "file-key-1");
    EXPECT_EQ(config.api_keys[0].rate_limit.capacity, 7);
    EXPECT_EQ(config.api_keys_file, keys_path);
}

TEST(ApiKeysSourceTest, FileOverridesInlineKeys) {
    const std::string keys_path = write_temp_config("keys_file2",
        R"([{"key": "file-key", "name": "from file"}])");
    const std::string cfg_path = write_temp_config("keys_cfg2", R"({
        "api_keys": [{"key": "inline-key", "name": "inline"}],
        "api_keys_file": ")" + keys_path + R"("})");
    std::vector<std::string> errors;
    Config config = Config::from_file(cfg_path, &errors);
    EXPECT_TRUE(errors.empty());
    ASSERT_EQ(config.api_keys.size(), 1);
    EXPECT_EQ(config.api_keys[0].key, "file-key");
}

TEST(ApiKeysSourceTest, EnvVarOverridesFile) {
    const std::string keys_path = write_temp_config("keys_file3",
        R"([{"key": "file-key", "name": "from file"}])");
    const std::string cfg_path = write_temp_config("keys_cfg3",
        R"({"api_keys_file": ")" + keys_path + R"("})");
    setenv("GW_API_KEYS", R"([{"key": "env-key", "name": "from env"}])", 1);
    std::vector<std::string> errors;
    Config config = Config::from_file(cfg_path, &errors);
    unsetenv("GW_API_KEYS");
    EXPECT_TRUE(errors.empty());
    ASSERT_EQ(config.api_keys.size(), 1);
    EXPECT_EQ(config.api_keys[0].key, "env-key");
}

TEST(ApiKeysSourceTest, InvalidEnvJsonIsAnError) {
    setenv("GW_API_KEYS", "not json", 1);
    std::vector<std::string> errors;
    Config config = Config::from_file("/tmp/gw_test_does_not_exist.json", &errors);
    unsetenv("GW_API_KEYS");
    ASSERT_EQ(errors.size(), 1);
    EXPECT_NE(errors[0].find("GW_API_KEYS"), std::string::npos);
}

TEST(ApiKeysSourceTest, MissingKeysFileIsAnError) {
    const std::string cfg_path = write_temp_config("keys_cfg4",
        R"({"api_keys_file": "/nonexistent/keys.json"})");
    std::vector<std::string> errors;
    Config config = Config::from_file(cfg_path, &errors);
    ASSERT_EQ(errors.size(), 1);
    EXPECT_NE(errors[0].find("api_keys_file"), std::string::npos);
}

TEST(FromFileSchemaTest, MissingFileYieldsDefaultsWithoutErrors) {
    std::vector<std::string> errors;
    Config config = Config::from_file("/tmp/gw_test_does_not_exist.json", &errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(config.port, 5005);
}
