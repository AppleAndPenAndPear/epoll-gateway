// Hardened TLS context construction shared by the server and the unit tests.
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "tls_utils.h"
#include <string>

namespace {
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
  // Shared policy: minimum TLS 1.2, AEAD-only cipher whitelist, no compression
  if (!apply_tls_hardening(ctx, error)) {
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
