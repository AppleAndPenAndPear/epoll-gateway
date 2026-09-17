// Hardened TLS context construction shared by the server and the unit tests.
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <string>

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

std::string openssl_error_reason() {
  unsigned long code = ERR_get_error();
  const char* reason = code == 0 ? nullptr : ERR_reason_error_string(code);
  return reason ? std::string(reason) : "unknown OpenSSL error";
}
}  // namespace

SSL_CTX* build_hardened_ssl_ctx(const std::string& cert_path, const std::string& key_path,
                                std::string* error) {
  SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
  if (!ctx) {
    *error = "SSL_CTX_new failed";
    return nullptr;
  }
  // P2 hardening: refuse TLS 1.0/1.1 and anything older
  if (SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1) {
    *error = "cannot set minimum TLS version";
    SSL_CTX_free(ctx);
    return nullptr;
  }
  SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
  if (SSL_CTX_set_cipher_list(ctx, kCipherWhitelist) != 1) {
    *error = "cipher whitelist rejected by OpenSSL";
    SSL_CTX_free(ctx);
    return nullptr;
  }
  // Session resumption: server-side cache (TLS 1.2) + session tickets (TLS 1.2/1.3)
  SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);
  SSL_CTX_set_num_tickets(ctx, 2);
  if (SSL_CTX_use_certificate_chain_file(ctx, cert_path.c_str()) != 1) {
    *error = "cannot load certificate '" + cert_path + "': " + openssl_error_reason();
    SSL_CTX_free(ctx);
    return nullptr;
  }
  if (SSL_CTX_use_PrivateKey_file(ctx, key_path.c_str(), SSL_FILETYPE_PEM) != 1) {
    *error = "cannot load private key '" + key_path + "': " + openssl_error_reason();
    SSL_CTX_free(ctx);
    return nullptr;
  }
  if (SSL_CTX_check_private_key(ctx) != 1) {
    *error = "private key does not match the certificate";
    SSL_CTX_free(ctx);
    return nullptr;
  }
  return ctx;
}
