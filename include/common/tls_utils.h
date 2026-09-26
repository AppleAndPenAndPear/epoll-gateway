#pragma once
#include <openssl/ssl.h>
#include <string>

// Project-wide TLS policy applied to any freshly created SSL_CTX, server or
// client side: minimum TLS 1.2, an AEAD-only cipher whitelist with forward
// secrecy, and compression disabled. Returns false and fills *error on failure;
// ctx ownership stays with the caller.
bool apply_tls_hardening(SSL_CTX* ctx, std::string* error);

// The TLS 1.2 cipher whitelist string itself. Exposed for diagnostics and for
// tests that assert the negotiated suite set. TLS 1.3 suites are governed
// separately by OpenSSL defaults and are not part of this list.
const char* tls_cipher_whitelist();
