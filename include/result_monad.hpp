// result.hpp
#pragma once

#include <boost/json.hpp>
#include <functional>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <type_traits>
#include <variant>

namespace json = boost::json;

namespace monad {

template <typename T>
struct WithMessage {
  T value;
  std::string message;
};

// Specialization for void
template <>
struct WithMessage<void> {
  std::string message;
};

using WithMessageVoid = WithMessage<void>;

struct ErrorData {
  int code;
  std::optional<json::value> data_content;
};

struct Error {
  int code;
  std::string what;
  std::optional<ErrorData> error_data;

  friend void tag_invoke(const json::value_from_tag&, json::value& jv,
                         const Error& e) {
    json::object jo;
    jo["code"] = e.code;
    jo["what"] = e.what;
    jv = std::move(jo);
  }
};

inline std::ostream& operator<<(std::ostream& os, const Error& e) {
  return os << "[Error " << e.code << "] " << e.what;
}

inline static const Error JUST_AN_ERROR = {std::numeric_limits<int>::min(), ""};

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

  std::optional<T> as_optional() const {
    if (is_ok()) return value();
    return std::nullopt;
  }

  // Return the value if ok, otherwise return the provided default
  // T value_or(const T& default_value) const {
  //   return is_ok() ? value() : default_value;
  // }

  // // Return the value if ok, otherwise compute fallback via function
  // template <typename F>
  // T value_or_else(F&& fallback_fn) const {
  //   return is_ok() ? value() : std::invoke(std::forward<F>(fallback_fn));
  // }

  // map_err: Transform the error, if present, to a new error type (const ref
  // version)
  template <typename F>
  auto map_err(F&& f) const& -> Result<T, std::invoke_result_t<F, E>> {
    using NewError = std::invoke_result_t<F, E>;
    if (is_ok()) return Result<T, NewError>::Ok(value());
    return Result<T, NewError>::Err(std::invoke(std::forward<F>(f), error()));
  }

  // map_err: Transform the error, if present, to a new error type (rvalue
  // version)
  template <typename F>
  auto map_err(F&& f) && -> Result<T, std::invoke_result_t<F, E>> {
    using NewError = std::invoke_result_t<F, E>;
    if (is_ok()) return Result<T, NewError>::Ok(std::move(value()));
    return Result<T, NewError>::Err(
        std::invoke(std::forward<F>(f), std::move(error())));
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

  template <typename F>
  auto map_err(F&& f) const& -> Result<void, std::invoke_result_t<F, E>> {
    using NewError = std::invoke_result_t<F, E>;
    if (is_err())
      return Result<void, NewError>::Err(
          std::invoke(std::forward<F>(f), error()));
    return Result<void, NewError>::Ok();
  }

  template <typename F>
  auto map_err(F&& f) && -> Result<void, std::invoke_result_t<F, E>> {
    using NewError = std::invoke_result_t<F, E>;
    if (is_err())
      return Result<void, NewError>::Err(
          std::invoke(std::forward<F>(f), std::move(error())));
    return Result<void, NewError>::Ok();
  }

  using value_type = void;
  using error_type = E;
};

template <typename T>
using MyResult = monad::Result<T, Error>;

using MyVoidResult = Result<void, Error>;

}  // namespace monad
