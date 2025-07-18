// result.hpp
#pragma once

#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <type_traits>
#include <variant>

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

  // into: convert to rvalue
  Result&& into() && { return std::move(*this); }

  // map: T -> U (copy version)
  template <typename F>
  auto map(F&& f) const& -> Result<std::invoke_result_t<F, T>, E> {
    using U = std::invoke_result_t<F, T>;
    if (is_ok()) return Result<U, E>(std::invoke(f, value()));
    return Result<U, E>(error());
  }

  // map: T -> U (move version)
  template <typename F>
  auto map(F&& f) && -> Result<std::invoke_result_t<F, T>, E> {
    using U = std::invoke_result_t<F, T>;
    if (is_ok()) return Result<U, E>(std::invoke(f, std::move(value())));
    return Result<U, E>(std::move(error()));
  }

  // and_then: T -> Result<U, E> (copy version)
  template <typename F>
  auto and_then(F&& f) const& -> std::invoke_result_t<F, T> {
    using Ret = std::invoke_result_t<F, T>;
    static_assert(std::is_same_v<Ret, Result<typename Ret::value_type, E>>,
                  "and_then must return Result<U,E>");
    if (is_ok()) return std::invoke(f, value());
    return Ret::Err(error());
  }

  // and_then: T -> Result<U, E> (move version) look at &&
  template <typename F>
  auto and_then(F&& f) && -> std::invoke_result_t<F, T> {
    using Ret = std::invoke_result_t<F, T>;
    static_assert(std::is_same_v<Ret, Result<typename Ret::value_type, E>>,
                  "and_then must return Result<U,E>");
    if (is_ok()) return std::invoke(f, std::move(value()));
    return Ret::Err(std::move(error()));
  }

  // catch_then: E -> Result<T, F> (copy version)
  template <typename F>
  auto catch_then(F&& f) const& -> std::invoke_result_t<F, E> {
    using Ret = std::invoke_result_t<F, E>;
    static_assert(std::is_same_v<Ret, Result<T, typename Ret::error_type>>,
                  "catch_then must return Result<T,F>");
    if (is_err()) return std::invoke(f, error());
    return Ret::Ok(value());
  }

  // catch_then: E -> Result<T, F> (move version)
  template <typename F>
  auto catch_then(F&& f) && -> std::invoke_result_t<F, E> {
    using Ret = std::invoke_result_t<F, E>;
    static_assert(std::is_same_v<Ret, Result<T, typename Ret::error_type>>,
                  "catch_then must return Result<T,F>");
    if (is_err()) return std::invoke(f, std::move(error()));
    return Ret::Ok(std::move(value()));
  }

  // type aliases for introspection
  using value_type = T;
  using error_type = E;
};

template <typename E>
class Result<void, E> {
  std::optional<E> error_;

 public:
  Result() = default;  // success
  Result(const E& error) : error_(error) {}
  Result(E&& error) : error_(std::move(error)) {}

  static Result Ok() { return Result(); }
  static Result Err(E error) { return Result(std::move(error)); }

  bool is_ok() const { return !error_.has_value(); }
  bool is_err() const { return error_.has_value(); }

  const E& error() const { return *error_; }
  E& error() { return *error_; }

  Result&& into() && { return std::move(*this); }

  template <typename F>
  auto catch_then(F&& f) const& -> std::invoke_result_t<F, E> {
    using Ret = std::invoke_result_t<F, E>;
    static_assert(std::is_same_v<typename Ret::value_type, void>,
                  "catch_then must return Result<void,F>");
    if (is_err()) return std::invoke(f, *error_);
    return Ret::Ok();
  }

  template <typename F>
  auto catch_then(F&& f) && -> std::invoke_result_t<F, E> {
    using Ret = std::invoke_result_t<F, E>;
    static_assert(std::is_same_v<typename Ret::value_type, void>,
                  "catch_then must return Result<void,F>");
    if (is_err()) return std::invoke(f, std::move(*error_));
    return Ret::Ok();
  }

  using value_type = void;
  using error_type = E;
};

template <typename T>
using MyResult = monad::Result<T, Error>;

using MyVoidResult = Result<void, Error>;

}  // namespace monad
