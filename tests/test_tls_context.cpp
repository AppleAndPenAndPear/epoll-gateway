#include <gtest/gtest.h>
#include <fstream>
#include "tcpworker.h"

#ifndef GW_SOURCE_DIR
#define GW_SOURCE_DIR "."
#endif

TEST(TlsContextTest, RejectsMissingCertificate) {
    std::string err;
    SSL_CTX* ctx = build_hardened_ssl_ctx("/nonexistent/cert.pem", "/nonexistent/key.pem", &err);
    EXPECT_EQ(ctx, nullptr);
    EXPECT_FALSE(err.empty());
}

TEST(TlsContextTest, AcceptsValidCertKeyPair) {
    std::string err;
    SSL_CTX* ctx = build_hardened_ssl_ctx(std::string(GW_SOURCE_DIR) + "/certs/server.crt",
                                          std::string(GW_SOURCE_DIR) + "/certs/server.key", &err);
    EXPECT_NE(ctx, nullptr) << err;
    if (ctx) SSL_CTX_free(ctx);
}

TEST(TlsContextTest, RejectsGarbageKey) {
    const std::string bad_key = "/tmp/gw_test_garbage.key";
    {
        std::ofstream ofs(bad_key, std::ios::trunc);
        ofs << "this is not a PEM private key\n";
    }
    std::string err;
    SSL_CTX* ctx = build_hardened_ssl_ctx(std::string(GW_SOURCE_DIR) + "/certs/server.crt",
                                          bad_key, &err);
    EXPECT_EQ(ctx, nullptr);
    EXPECT_FALSE(err.empty());
}

TEST(TlsContextTest, RejectsMissingKey) {
    std::string err;
    SSL_CTX* ctx = build_hardened_ssl_ctx(std::string(GW_SOURCE_DIR) + "/certs/server.crt",
                                          "/nonexistent/key.pem", &err);
    EXPECT_EQ(ctx, nullptr);
    EXPECT_NE(err.find("private key"), std::string::npos);
}
