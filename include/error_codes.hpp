// Generated from error_codes.ini
#pragma once

#include <string>
#include <ostream>

namespace error {

enum class FileError : int {
  FileNotFound = 10001,
  FileCorrupted = 10002,
  PermissionDenied = 10003,
};

enum class NetworkError : int {
  Timeout = 20001,
  ConnectionLost = 20002,
  HostUnreachable = 20003,
};

enum class AuthError : int {
  InvalidToken = 30001,
  ExpiredToken = 30002,
  Unauthorized = 30003,
};

inline int to_int(FileError e) { return static_cast<int>(e); }
inline int to_int(NetworkError e) { return static_cast<int>(e); }
inline int to_int(AuthError e) { return static_cast<int>(e); }
}  // namespace error
