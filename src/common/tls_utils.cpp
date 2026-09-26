// Project-wide TLS policy shared by the server and the client context builders.
#include "tls_utils.h"
#include <openssl/err.h>

// AEAD ciphers with forward secrecy only (TLS 1.2 section of the whitelist;
// TLS 1.3 suites are governed separately by SSL_CTX_set_ciphersuites and are
// secure by default). No static RSA, no CBC, no 3DES/RC4.
namespace {
constexpr const char* kCipherWhitelist =
    "ECDHE-ECDSA-AES256-GCM-SHA384:"
    "ECDHE-RSA-AES256-GCM-SHA384:"
    "ECDHE-ECDSA-CHACHA20-POLY1305:"
    "ECDHE-RSA-CHACHA20-POLY1305:"
    "ECDHE-ECDSA-AES128-GCM-SHA256:"
    "ECDHE-RSA-AES128-GCM-SHA256";
}  // namespace

const char* tls_cipher_whitelist() {
  return kCipherWhitelist;
}

bool apply_tls_hardening(SSL_CTX* ctx, std::string* error) {
  if (!ctx) {
    *error = "null SSL_CTX";
    return false;
  }
  // P2 hardening: refuse TLS 1.0/1.1 and anything older
  if (SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1) {
    *error = "cannot set minimum TLS version";
    return false;
  }
  SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
  if (SSL_CTX_set_cipher_list(ctx, kCipherWhitelist) != 1) {
    *error = "cipher whitelist rejected by OpenSSL";
    return false;
  }
  return true;
}