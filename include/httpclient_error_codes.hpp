// Auto-generated from error_codes.ini
#pragma once

namespace httpclient_errors {

// HttpClientError error codes
constexpr int BAD_REQUEST = 400;
constexpr int UNAUTHORIZED = 401;
constexpr int FORBIDDEN = 403;
constexpr int NOT_FOUND = 404;
constexpr int METHOD_NOT_ALLOWED = 405;

// NetworkError error codes
constexpr int CONNECTION_TIMEOUT = 4001;
constexpr int CONNECTION_REFUSED = 4002;
constexpr int HOST_UNREACHABLE = 4003;
constexpr int DNS_LOOKUP_FAILED = 4004;

}  // namespace httpclient_errors