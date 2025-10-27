#pragma once

#include <openssl/crypto.h>

namespace cjj365 {

// Ensures OpenSSL releases thread-local resources before thread exit.
struct OpenSslThreadCleanup {
  OpenSslThreadCleanup() = default;
  ~OpenSslThreadCleanup() { OPENSSL_thread_stop(); }
};

}  // namespace cjj365

