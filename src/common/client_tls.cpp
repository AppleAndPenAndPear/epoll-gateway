#include "client_tls.h"
#include "tls_utils.h"
#include <openssl/err.h>

SSL_CTX* build_client_ssl_ctx(const ClientTlsConfig& cfg, std::string* error) {
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  if (!ctx) {
    *error = "SSL_CTX_new failed";
    return nullptr;
  }
  if (!apply_tls_hardening(ctx, error)) {
    SSL_CTX_free(ctx);
    return nullptr;
  }
  // This must happen before any hostname check: without SSL_VERIFY_PEER the
  // per-connection SSL_set1_host() call silently does nothing.
  SSL_CTX_set_verify(ctx, cfg.insecure ? SSL_VERIFY_NONE : SSL_VERIFY_PEER, nullptr);
  if (!cfg.insecure) {
    // An explicit CA file pins a private PKI (typical for internal backends);
    // empty falls back to the system default trust store for public CAs.
    if (!cfg.ca_path.empty()) {
      if (SSL_CTX_load_verify_locations(ctx, cfg.ca_path.c_str(), nullptr) != 1) {
        *error = "cannot load CA file '" + cfg.ca_path + "': " +
                 ERR_error_string(ERR_get_error(), nullptr);
        SSL_CTX_free(ctx);
        return nullptr;
      }
    } else if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
      *error = std::string("cannot load the system default CA store: ") +
               ERR_error_string(ERR_get_error(), nullptr);
      SSL_CTX_free(ctx);
      return nullptr;
    }
  }
  return ctx;
}