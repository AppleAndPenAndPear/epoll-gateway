#include <gtest/gtest.h>
#include <openssl/ssl.h>
#include <memory>
#include <string>
#include "client_tls.h"

#ifndef GW_SOURCE_DIR
#define GW_SOURCE_DIR "."
#endif

namespace {
using CtxPtr = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;

std::string source_path(const std::string& relative) {
    return std::string(GW_SOURCE_DIR) + "/" + relative;
}
}  // namespace

TEST(ClientTlsTest, RejectsMissingCaFile) {
    ClientTlsConfig cfg;
    cfg.ca_path = "/nonexistent/ca.pem";
    std::string err;
    CtxPtr ctx(build_client_ssl_ctx(cfg, &err), &SSL_CTX_free);
    EXPECT_EQ(ctx.get(), nullptr);
    EXPECT_NE(err.find("/nonexistent/ca.pem"), std::string::npos);
}

TEST(ClientTlsTest, AcceptsRepoCertificate) {
    ClientTlsConfig cfg;
    cfg.ca_path = source_path("certs/server.crt");
    std::string err;
    CtxPtr ctx(build_client_ssl_ctx(cfg, &err), &SSL_CTX_free);
    ASSERT_NE(ctx.get(), nullptr) << err;
    // Verification must be on: SSL_set1_host() is a no-op without it
    EXPECT_EQ(SSL_CTX_get_verify_mode(ctx.get()), SSL_VERIFY_PEER);
}

TEST(ClientTlsTest, InsecureSkipsCaLoading) {
    ClientTlsConfig cfg;
    cfg.insecure = true;
    cfg.ca_path = "/nonexistent/ca.pem";  // not read at all when verification is off
    std::string err;
    CtxPtr ctx(build_client_ssl_ctx(cfg, &err), &SSL_CTX_free);
    ASSERT_NE(ctx.get(), nullptr) << err;
    EXPECT_EQ(SSL_CTX_get_verify_mode(ctx.get()), SSL_VERIFY_NONE);
}

TEST(ClientTlsTest, InsecureWithEmptyCaPath) {
    ClientTlsConfig cfg;
    cfg.insecure = true;
    cfg.ca_path.clear();
    std::string err;
    CtxPtr ctx(build_client_ssl_ctx(cfg, &err), &SSL_CTX_free);
    EXPECT_NE(ctx.get(), nullptr) << err;
}

TEST(ClientTlsTest, EmptyCaPathFallsBackToSystemTrustStore) {
    // Empty ca_path no longer fails: it means "verify against the system
    // default CA store", which is what public-CA upstreams need.
    ClientTlsConfig cfg;
    cfg.ca_path.clear();
    std::string err;
    CtxPtr ctx(build_client_ssl_ctx(cfg, &err), &SSL_CTX_free);
    ASSERT_NE(ctx.get(), nullptr) << err;
    EXPECT_EQ(SSL_CTX_get_verify_mode(ctx.get()), SSL_VERIFY_PEER);
}