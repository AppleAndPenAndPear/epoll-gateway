#include <gtest/gtest.h>
#include <openssl/ssl.h>
#include <string>
#include <vector>
#include "tls_utils.h"

namespace {
// SSL_CTX_get_ciphers() also lists the TLS 1.3 suites, which are governed
// separately (SSL_CTX_set_ciphersuites) and stay at the OpenSSL defaults, so the
// whitelist can only be asserted through the TLS 1.2-and-below entries.
std::vector<const SSL_CIPHER*> legacy_ciphers(SSL_CTX* ctx) {
    std::vector<const SSL_CIPHER*> legacy;
    STACK_OF(SSL_CIPHER)* list = SSL_CTX_get_ciphers(ctx);
    for (int i = 0; list && i < sk_SSL_CIPHER_num(list); ++i) {
        const SSL_CIPHER* cipher = sk_SSL_CIPHER_value(list, i);
        // TLS 1.3 suite names are prefixed TLS_, the older ones SSL_
        if (std::string(SSL_CIPHER_get_name(cipher)).compare(0, 4, "TLS_") != 0) {
            legacy.push_back(cipher);
        }
    }
    return legacy;
}

void expect_hardened(SSL_CTX* ctx) {
    EXPECT_EQ(SSL_CTX_get_min_proto_version(ctx), TLS1_2_VERSION);
    EXPECT_TRUE(SSL_CTX_get_options(ctx) & SSL_OP_NO_COMPRESSION);
    // Exactly the six whitelisted suites, nothing OpenSSL added on top
    const std::vector<const SSL_CIPHER*> legacy = legacy_ciphers(ctx);
    ASSERT_EQ(legacy.size(), 6u);
    for (const SSL_CIPHER* cipher : legacy) {
        const std::string name = SSL_CIPHER_get_name(cipher);
        EXPECT_TRUE(name.find("GCM") != std::string::npos ||
                    name.find("CHACHA20") != std::string::npos) << name;
        EXPECT_EQ(name.find("ECDHE"), 0u) << name;
    }
}
}  // namespace

TEST(TlsUtilsTest, RejectsNullContext) {
    std::string err;
    EXPECT_FALSE(apply_tls_hardening(nullptr, &err));
    EXPECT_FALSE(err.empty());
}

TEST(TlsUtilsTest, HardensServerContext) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    ASSERT_NE(ctx, nullptr);
    std::string err;
    EXPECT_TRUE(apply_tls_hardening(ctx, &err)) << err;
    expect_hardened(ctx);
    SSL_CTX_free(ctx);
}

TEST(TlsUtilsTest, HardensClientContext) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    ASSERT_NE(ctx, nullptr);
    std::string err;
    EXPECT_TRUE(apply_tls_hardening(ctx, &err)) << err;
    expect_hardened(ctx);
    SSL_CTX_free(ctx);
}

TEST(TlsUtilsTest, WhitelistHasSixAeadSuites) {
    const std::string whitelist = tls_cipher_whitelist();
    EXPECT_FALSE(whitelist.empty());
    size_t suites = 1;
    for (const char c : whitelist) {
        if (c == ':') ++suites;
    }
    EXPECT_EQ(suites, 6u);
    // Neither CBC modes nor static-RSA key exchange may appear
    EXPECT_EQ(whitelist.find("CBC"), std::string::npos);
    EXPECT_EQ(whitelist.find("_RSA_WITH"), std::string::npos);
}