#pragma once
#include <openssl/ssl.h>
#include <string>

// TLS settings for the companion client.
struct ClientTlsConfig {
  bool enable_tls = true;
  bool insecure = false;                     // skip certificate verification entirely
  std::string ca_path = "certs/server.crt";  // trust anchor used when verifying
  std::string verify_host;                   // hostname to check; empty verifies the chain only
};

// Builds a client SSL_CTX: TLS_client_method, the shared hardening policy
// (minimum TLS 1.2 + AEAD-only cipher whitelist) and the configured
// verification policy. Returns nullptr and fills *error on failure; the caller
// owns the returned context.
SSL_CTX* build_client_ssl_ctx(const ClientTlsConfig& cfg, std::string* error);