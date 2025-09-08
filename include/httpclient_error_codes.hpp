// Auto-generated from error_codes.ini
#pragma once

namespace httpclient_errors {

namespace RESPONSE {  // RESPONSE errors

constexpr int BAD_REQUEST = 400;                 // Bad Request
constexpr int UNAUTHORIZED = 401;                // Unauthorized
constexpr int FORBIDDEN = 403;                   // Forbidden
constexpr int NOT_FOUND = 404;                   // Not Found
constexpr int METHOD_NOT_ALLOWED = 405;          // Method Not Allowed
constexpr int DOWNLOAD_FILE_OPEN_FAILED = 4999;  // download file open failed
}  // namespace RESPONSE

namespace NETWORK {  // Network errors

constexpr int CONNECTION_TIMEOUT = 4001;  // Connection Timeout
constexpr int CONNECTION_REFUSED = 4002;  // Connection Refused
constexpr int HOST_UNREACHABLE = 4003;    // Host Unreachable
constexpr int DNS_LOOKUP_FAILED = 4004;   // DNS Lookup Failed
}  // namespace NETWORK

}  // namespace httpclient_errors