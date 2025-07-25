// Generated from error_codes.ini
#pragma once

#include <string>
#include <ostream>

namespace httpclient_error {

enum class HttpClientError : int {
  BadRequest = 400,
  Unauthorized = 401,
  Forbidden = 403,
  NotFound = 404,
  MethodNotAllowed = 405,
};

enum class NetworkError : int {
  ConnectionTimeout = 1001,
  ConnectionRefused = 1002,
  HostUnreachable = 1003,
  DNSLookupFailed = 1004,
};

inline int to_int(HttpClientError e) { return static_cast<int>(e); }
inline int to_int(NetworkError e) { return static_cast<int>(e); }
}  // namespace error
