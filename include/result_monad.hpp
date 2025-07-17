// result.hpp
#pragma once

#include <functional>
#include <string>
#include <type_traits>
#include <variant>
#include <ostream>

namespace monad {

struct Error {
  int code;
  std::string what;
};

inline std::ostream& operator<<(std::ostream& os, const Error& e) {
  return os << "[Error " << e.code << "] " << e.what;
}

// Generic Result<T, E>
template <typename T, typename E>
class Result {
  std::variant<T, E> data_;

 public:
  // Constructors
  Result(const T& value) : data_(value) {}
  Result(T&& value) : data_(std::move(value)) {}
  Result(const E& error) : data_(error) {}
  Result(E&& error) : data_(std::move(error)) {}

  // Factory helpers
  static Result Ok(T value) { return Result(std::move(value)); }
  static Result Err(E error) { return Result(std::move(error)); }

  // Introspection
  bool is_ok() const { return std::holds_alternative<T>(data_); }
  bool is_err() const { return std::holds_alternative<E>(data_); }

  const T& value() const { return std::get<T>(data_); }
  T& value() { return std::get<T>(data_); }
  const E& error() const { return std::get<E>(data_); }
  E& error() { return std::get<E>(data_); }

  // map: T -> U
  template <typename F>
  auto map(F&& f) const -> Result<std::invoke_result_t<F, T>, E> {
    using U = std::invoke_result_t<F, T>;
    if (is_ok()) return Result<U, E>(std::invoke(f, value()));
    return Result<U, E>(error());
  }

  // and_then: T -> Result<U, E>
  template <typename F>
  auto and_then(F&& f) const -> std::invoke_result_t<F, T> {
    using Ret = std::invoke_result_t<F, T>;
    static_assert(std::is_same_v<Ret, Result<typename Ret::value_type, E>>,
                  "and_then must return Result<U,E>");
    if (is_ok()) return std::invoke(f, value());
    return Ret::Err(error());
  }

  // catch_then: E -> Result<T, F>
  template <typename F>
  auto catch_then(F&& f) const -> std::invoke_result_t<F, E> {
    using Ret = std::invoke_result_t<F, E>;
    static_assert(std::is_same_v<Ret, Result<T, typename Ret::error_type>>,
                  "catch_then must return Result<T,F>");
    if (is_err()) return std::invoke(f, error());
    return Ret::Ok(value());
  }

  // type aliases for introspection
  using value_type = T;
  using error_type = E;
};

template <typename T>
using MyResult = monad::Result<T, Error>;

}  // namespace monad
