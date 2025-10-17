// result_monad.hpp (v2 as default)
#pragma once

#include <boost/json.hpp>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <functional>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace json = boost::json;

namespace monad {

template <typename T>
struct WithMessage {
  T value;
  std::string message;
};

template <>
struct WithMessage<void> {
  std::string message;
};

using WithMessageVoid = WithMessage<void>;

struct Error {
  int code;
  std::string what;
  std::string key;
  int response_status = 500;
  json::object params;
  std::string content_type = "application/json";
  std::optional<json::value> alternative_body = std::nullopt;

  friend void tag_invoke(const json::value_from_tag&, json::value& jv,
                         const Error& e) {
    json::object jo;
    jo["code"] = e.code;
    jo["what"] = e.what;
    jo["key"] = e.key;
    jo["params"] = e.params;
    jv = std::move(jo);
  }
};

struct ErrorResponse {
  Error error;
  friend void tag_invoke(const json::value_from_tag&, json::value& jv,
                         const ErrorResponse& resp) {
    json::object jo;
    jo["error"] = json::value_from(resp.error);
    jv = std::move(jo);
  }
};

inline std::string error_to_string(const Error& e) {
  if (e.content_type == "application/json") {
    return json::serialize(json::value_from(e));
  } else {
    return fmt::format("code: {}\nwhat: {}", e.code, e.what);
  }
}

inline std::string error_to_response(const Error& e) {
  if (e.alternative_body.has_value()) {
    return json::serialize(e.alternative_body.value());
  } else {
    if (e.content_type == "application/json") {
      ErrorResponse resp{e};
      return json::serialize(json::value_from(resp));
    } else {
      return fmt::format("code: {}\nwhat: {}", e.code, e.what);
    }
  }
}

inline std::ostream& operator<<(std::ostream& os, const Error& e) {
  return os << "[Error " << e.code << "] " << e.what;
}

inline static const Error JUST_AN_ERROR = {std::numeric_limits<int>::min(), ""};

// Tag types for explicit construction (avoid ambiguity)
struct ok_t {};
struct err_t {};
inline constexpr ok_t ok_in_place{};
inline constexpr err_t err_in_place{};

// Concept: type looks like Result<*, E>
template <typename X, typename E>
concept result_with_error = requires {
  typename X::value_type;
  typename X::error_type;
  requires std::is_same_v<typename X::error_type, E>;
};

// Result<T,E>
template <typename T, typename E>
class [[nodiscard]] Result {
 public:
  using value_type = T;
  using error_type = E;

  constexpr explicit Result(ok_t, const T& v) : data_(v) {}
  constexpr explicit Result(ok_t, T&& v) : data_(std::move(v)) {}
  constexpr explicit Result(err_t, const E& e) : data_(e) {}
  constexpr explicit Result(err_t, E&& e) : data_(std::move(e)) {}

  template <typename U = T>
  [[nodiscard]] static constexpr Result Ok(U&& v) {
    return Result(ok_in_place, std::forward<U>(v));
  }
  template <typename G = E>
  [[nodiscard]] static constexpr Result Err(G&& e) {
    return Result(err_in_place, std::forward<G>(e));
  }

  [[nodiscard]] constexpr bool is_ok() const noexcept {
    return std::holds_alternative<T>(data_);
  }
  [[nodiscard]] constexpr bool is_err() const noexcept {
    return std::holds_alternative<E>(data_);
  }
  constexpr explicit operator bool() const noexcept { return is_ok(); }

  [[nodiscard]] constexpr const T& value() const& { return std::get<T>(data_); }
  [[nodiscard]] constexpr T& value() & { return std::get<T>(data_); }
  [[nodiscard]] constexpr T&& value() && {
    return std::get<T>(std::move(data_));
  }

  [[nodiscard]] constexpr const E& error() const& { return std::get<E>(data_); }
  [[nodiscard]] constexpr E& error() & { return std::get<E>(data_); }
  [[nodiscard]] constexpr E&& error() && {
    return std::get<E>(std::move(data_));
  }

  // Pointer-style access without throwing
  [[nodiscard]] constexpr const T* ok_ptr() const noexcept {
    return std::get_if<T>(&data_);
  }
  [[nodiscard]] constexpr T* ok_ptr() noexcept {
    return std::get_if<T>(&data_);
  }
  [[nodiscard]] constexpr const E* err_ptr() const noexcept {
    return std::get_if<E>(&data_);
  }
  [[nodiscard]] constexpr E* err_ptr() noexcept {
    return std::get_if<E>(&data_);
  }

  [[nodiscard]] std::optional<T> as_optional() const& {
    if (is_ok()) return value();
    return std::nullopt;
  }
  [[nodiscard]] std::optional<T> as_optional() && {
    if (is_ok()) return std::move(*std::get_if<T>(&data_));
    return std::nullopt;
  }

  // map (const&)
  template <typename F>
  [[nodiscard]] auto map(
      F&& f) const& -> Result<std::invoke_result_t<F, const T&>, E> {
    using U = std::invoke_result_t<F, const T&>;
    if (is_ok())
      return Result<U, E>::Ok(std::invoke(std::forward<F>(f), value()));
    return Result<U, E>::Err(error());
  }
  // map (&&)
  template <typename F>
  [[nodiscard]] auto map(F&& f) && -> Result<std::invoke_result_t<F, T&&>, E> {
    using U = std::invoke_result_t<F, T&&>;
    if (is_ok())
      return Result<U, E>::Ok(
          std::invoke(std::forward<F>(f), std::move(*std::get_if<T>(&data_))));
    return Result<U, E>::Err(std::move(*std::get_if<E>(&data_)));
  }

  // and_then (const&): T -> Result<U,E>
  template <typename F>
  [[nodiscard]] auto and_then(
      F&& f) const& -> std::invoke_result_t<F, const T&> {
    using Ret = std::invoke_result_t<F, const T&>;
    static_assert(result_with_error<Ret, E>,
                  "and_then must return Result<U,E>");
    if (is_ok()) return std::invoke(std::forward<F>(f), value());
    return Ret::Err(error());
  }
  // and_then (&&): T -> Result<U,E>
  template <typename F>
  [[nodiscard]] auto and_then(F&& f) && -> std::invoke_result_t<F, T&&> {
    using Ret = std::invoke_result_t<F, T&&>;
    static_assert(result_with_error<Ret, E>,
                  "and_then must return Result<U,E>");
    if (is_ok())
      return std::invoke(std::forward<F>(f),
                         std::move(*std::get_if<T>(&data_)));
    return Ret::Err(std::move(*std::get_if<E>(&data_)));
  }

  // map_err (const&)
  template <typename F>
  [[nodiscard]] auto map_err(
      F&& f) const& -> Result<T, std::invoke_result_t<F, const E&>> {
    using NewError = std::invoke_result_t<F, const E&>;
    if (is_ok()) return Result<T, NewError>::Ok(value());
    return Result<T, NewError>::Err(std::invoke(std::forward<F>(f), error()));
  }
  // map_err (&&)
  template <typename F>
  [[nodiscard]] auto map_err(
      F&& f) && -> Result<T, std::invoke_result_t<F, E&&>> {
    using NewError = std::invoke_result_t<F, E&&>;
    if (is_ok())
      return Result<T, NewError>::Ok(std::move(*std::get_if<T>(&data_)));
    return Result<T, NewError>::Err(
        std::invoke(std::forward<F>(f), std::move(*std::get_if<E>(&data_))));
  }

  // catch_then (const&): E -> Result<T,F>
  template <typename F>
  [[nodiscard]] auto catch_then(
      F&& f) const& -> std::invoke_result_t<F, const E&> {
    using Ret = std::invoke_result_t<F, const E&>;
    static_assert(std::is_same_v<typename Ret::value_type, T>,
                  "catch_then must return Result<T,F>");
    if (is_err()) return std::invoke(std::forward<F>(f), error());
    return Ret::Ok(value());
  }
  // catch_then (&&): E -> Result<T,F>
  template <typename F>
  [[nodiscard]] auto catch_then(F&& f) && -> std::invoke_result_t<F, E&&> {
    using Ret = std::invoke_result_t<F, E&&>;
    static_assert(std::is_same_v<typename Ret::value_type, T>,
                  "catch_then must return Result<T,F>");
    if (is_err())
      return std::invoke(std::forward<F>(f),
                         std::move(*std::get_if<E>(&data_)));
    return Ret::Ok(std::move(*std::get_if<T>(&data_)));
  }

  // or_else: transform error into same-typed Result
  template <typename F>
  [[nodiscard]] Result or_else(F&& f) const& {
    if (is_err()) return std::forward<F>(f)(error());
    return *this;
  }
  template <typename F>
  [[nodiscard]] Result or_else(F&& f) && {
    if (is_err()) return std::forward<F>(f)(std::move(*std::get_if<E>(&data_)));
    return std::move(*this);
  }

  // value fallbacks
  [[nodiscard]] T value_or(const T& fallback) const& {
    return is_ok() ? value() : fallback;
  }
  template <typename F>
  [[nodiscard]] T value_or_else(F&& fb) const& {
    return is_ok() ? value() : std::invoke(std::forward<F>(fb));
  }
  [[nodiscard]] T value_or(T&& fallback) && {
    return is_ok() ? std::move(*std::get_if<T>(&data_)) : std::move(fallback);
  }

  // visit
  template <typename V>
  [[nodiscard]] decltype(auto) visit(V&& v) & {
    return std::visit(std::forward<V>(v), data_);
  }
  template <typename V>
  [[nodiscard]] decltype(auto) visit(V&& v) const& {
    return std::visit(std::forward<V>(v), data_);
  }
  template <typename V>
  [[nodiscard]] decltype(auto) visit(V&& v) && {
    return std::visit(std::forward<V>(v), std::move(data_));
  }

  friend bool operator==(const Result& a, const Result& b) {
    return a.data_ == b.data_;
  }
  friend void swap(Result& a, Result& b) noexcept {
    using std::swap;
    swap(a.data_, b.data_);
  }

 private:
  std::variant<T, E> data_;
};

// Result<void,E>
template <typename E>
class Result<void, E> {
 public:
  using value_type = void;
  using error_type = E;

  constexpr Result() = default;
  constexpr explicit Result(err_t, const E& e) : error_(e) {}
  constexpr explicit Result(err_t, E&& e) : error_(std::move(e)) {}

  [[nodiscard]] static constexpr Result Ok() { return Result(); }
  template <typename G = E>
  [[nodiscard]] static constexpr Result Err(G&& e) {
    Result r;
    r.error_.emplace(std::forward<G>(e));
    return r;
  }

  [[nodiscard]] constexpr bool is_ok() const noexcept {
    return !error_.has_value();
  }
  [[nodiscard]] constexpr bool is_err() const noexcept {
    return error_.has_value();
  }

  [[nodiscard]] const E& error() const& { return *error_; }
  [[nodiscard]] E& error() & { return *error_; }
  [[nodiscard]] E&& error() && { return std::move(*error_); }

  [[nodiscard]] std::optional<std::monostate> as_optional() const {
    return is_ok() ? std::optional<std::monostate>(std::in_place)
                   : std::optional<std::monostate>();
  }

  // and_then: () -> Result<U,E>
  template <typename F>
  [[nodiscard]] auto and_then(F&& f) const& -> std::invoke_result_t<F> {
    using Ret = std::invoke_result_t<F>;
    static_assert(std::is_same_v<typename Ret::error_type, E>,
                  "and_then must keep same error type");
    if (is_ok()) return std::invoke(std::forward<F>(f));
    return Ret::Err(*error_);
  }
  template <typename F>
  [[nodiscard]] auto and_then(F&& f) && -> std::invoke_result_t<F> {
    using Ret = std::invoke_result_t<F>;
    static_assert(std::is_same_v<typename Ret::error_type, E>,
                  "and_then must keep same error type");
    if (is_ok()) return std::invoke(std::forward<F>(f));
    return Ret::Err(std::move(*error_));
  }

  // catch_then: E -> Result<void,F>
  template <typename F>
  [[nodiscard]] auto catch_then(
      F&& f) const& -> std::invoke_result_t<F, const E&> {
    using Ret = std::invoke_result_t<F, const E&>;
    static_assert(std::is_same_v<typename Ret::value_type, void>,
                  "catch_then must return Result<void,F>");
    if (is_err()) return std::invoke(std::forward<F>(f), *error_);
    return Ret::Ok();
  }
  template <typename F>
  [[nodiscard]] auto catch_then(F&& f) && -> std::invoke_result_t<F, E&&> {
    using Ret = std::invoke_result_t<F, E&&>;
    static_assert(std::is_same_v<typename Ret::value_type, void>,
                  "catch_then must return Result<void,F>");
    if (is_err()) return std::invoke(std::forward<F>(f), std::move(*error_));
    return Ret::Ok();
  }

  // map_err
  template <typename F>
  [[nodiscard]] auto map_err(
      F&& f) const& -> Result<void, std::invoke_result_t<F, const E&>> {
    using NewError = std::invoke_result_t<F, const E&>;
    if (is_err())
      return Result<void, NewError>::Err(
          std::invoke(std::forward<F>(f), *error_));
    return Result<void, NewError>::Ok();
  }
  template <typename F>
  [[nodiscard]] auto map_err(
      F&& f) && -> Result<void, std::invoke_result_t<F, E&&>> {
    using NewError = std::invoke_result_t<F, E&&>;
    if (is_err())
      return Result<void, NewError>::Err(
          std::invoke(std::forward<F>(f), std::move(*error_)));
    return Result<void, NewError>::Ok();
  }

  // or_else
  template <typename F>
  [[nodiscard]] Result or_else(F&& f) const& {
    if (is_err()) return std::forward<F>(f)(*error_);
    return *this;
  }
  template <typename F>
  [[nodiscard]] Result or_else(F&& f) && {
    if (is_err()) return std::forward<F>(f)(std::move(*error_));
    return std::move(*this);
  }

  friend bool operator==(const Result& a, const Result& b) {
    return a.error_ == b.error_;
  }

 private:
  std::optional<E> error_{};
};

// Convenience aliases using Error
template <typename T>
using MyResult = monad::Result<T, Error>;
using MyVoidResult = Result<void, Error>;

namespace detail {
template <typename X>
struct unwrap_result_type {
  using type = X;
  static constexpr bool is_wrapped = false;
};

template <typename U>
struct unwrap_result_type<Result<U, Error>> {
  using type = U;
  static constexpr bool is_wrapped = true;
};
}  // namespace detail

template <typename X>
using unwrap_result_t = typename detail::unwrap_result_type<X>::type;

template <typename X>
inline constexpr bool is_my_result_v = detail::unwrap_result_type<X>::is_wrapped;

// Common Result aliases (E = Error)
using StringResult = Result<std::string, Error>;
using BoolResult = Result<bool, Error>;
using IntResult = Result<int, Error>;
using Int64Result = Result<int64_t, Error>;
using UInt64Result = Result<uint64_t, Error>;
using SizeTResult = Result<std::size_t, Error>;
using JsonValueResult = Result<json::value, Error>;
using JsonObjectResult = Result<json::object, Error>;
using JsonArrayResult = Result<json::array, Error>;
using VoidResult = Result<void, Error>;

// Free helpers mirroring v2 combinators

// Base: empty pack -> Ok(())
template <typename E>
[[nodiscard]] inline Result<std::tuple<>, E> zip_results() {
  return Result<std::tuple<>, E>::Ok(std::tuple<>{});
}

// const&: copies values
template <typename E, typename T1, typename... Ts>
[[nodiscard]] inline Result<std::tuple<T1, Ts...>, E> zip_results(
    const Result<T1, E>& r1, const Result<Ts, E>&... rest) {
  static_assert(!std::is_same_v<T1, void>,
                "zip_results does not support void; use all_ok for void.");
  if (r1.is_err()) return Result<std::tuple<T1, Ts...>, E>::Err(r1.error());
  auto tail = zip_results<E>(rest...);
  if (tail.is_err()) return Result<std::tuple<T1, Ts...>, E>::Err(tail.error());
  return Result<std::tuple<T1, Ts...>, E>::Ok(
      std::tuple_cat(std::make_tuple(r1.value()), tail.value()));
}

// rvalue: moves values
template <typename E, typename T1, typename... Ts>
[[nodiscard]] inline Result<std::tuple<T1, Ts...>, E> zip_results(
    Result<T1, E>&& r1, Result<Ts, E>&&... rest) {
  static_assert(!std::is_same_v<T1, void>,
                "zip_results does not support void; use all_ok for void.");
  if (r1.is_err())
    return Result<std::tuple<T1, Ts...>, E>::Err(std::move(r1).error());
  auto tail = zip_results<E>(std::move(rest)...);
  if (tail.is_err())
    return Result<std::tuple<T1, Ts...>, E>::Err(std::move(tail).error());
  return Result<std::tuple<T1, Ts...>, E>::Ok(std::tuple_cat(
      std::make_tuple(std::move(r1).value()), std::move(tail).value()));
}

// All-ok for void sequences
template <typename E>
[[nodiscard]] inline Result<void, E> all_ok() {
  return Result<void, E>::Ok();
}
template <typename E, typename... Vs>
[[nodiscard]] inline Result<void, E> all_ok(const Result<void, E>& v1,
                                            const Result<Vs, E>&... rest) {
  if (v1.is_err()) return Result<void, E>::Err(v1.error());
  return all_ok<E>(rest...);
}
template <typename E, typename... Vs>
[[nodiscard]] inline Result<void, E> all_ok(Result<void, E>&& v1,
                                            Result<Vs, E>&&... rest) {
  if (v1.is_err()) return Result<void, E>::Err(std::move(v1).error());
  return all_ok<E>(std::move(rest)...);
}

// Type-level filter: remove voids from tuple
template <typename... Ts>
struct filter_void_types;
template <>
struct filter_void_types<> {
  using type = std::tuple<>;
};
template <typename T, typename... Ts>
struct filter_void_types<T, Ts...> {
  using tail = typename filter_void_types<Ts...>::type;
  using type =
      std::conditional_t<std::is_same_v<T, void>, tail,
                         decltype(std::tuple_cat(std::declval<std::tuple<T>>(),
                                                 std::declval<tail>()))>;
};
template <typename... Ts>
using filter_void_types_t = typename filter_void_types<Ts...>::type;

template <typename E>
[[nodiscard]] inline Result<std::tuple<>, E> zip_results_skip_void() {
  return Result<std::tuple<>, E>::Ok(std::tuple<>{});
}
template <typename E, typename T1, typename... Ts>
[[nodiscard]] inline Result<filter_void_types_t<T1, Ts...>, E>
zip_results_skip_void(const Result<T1, E>& r1, const Result<Ts, E>&... rest) {
  if (r1.is_err())
    return Result<filter_void_types_t<T1, Ts...>, E>::Err(r1.error());
  auto tail = zip_results_skip_void<E>(rest...);
  if (tail.is_err())
    return Result<filter_void_types_t<T1, Ts...>, E>::Err(tail.error());
  if constexpr (std::is_same_v<T1, void>) {
    return Result<filter_void_types_t<T1, Ts...>, E>::Ok(tail.value());
  } else {
    return Result<filter_void_types_t<T1, Ts...>, E>::Ok(
        std::tuple_cat(std::make_tuple(r1.value()), tail.value()));
  }
}
template <typename E, typename T1, typename... Ts>
[[nodiscard]] inline Result<filter_void_types_t<T1, Ts...>, E>
zip_results_skip_void(Result<T1, E>&& r1, Result<Ts, E>&&... rest) {
  if (r1.is_err())
    return Result<filter_void_types_t<T1, Ts...>, E>::Err(
        std::move(r1).error());
  auto tail = zip_results_skip_void<E>(std::move(rest)...);
  if (tail.is_err())
    return Result<filter_void_types_t<T1, Ts...>, E>::Err(
        std::move(tail).error());
  if constexpr (std::is_same_v<T1, void>) {
    return Result<filter_void_types_t<T1, Ts...>, E>::Ok(
        std::move(tail).value());
  } else {
    return Result<filter_void_types_t<T1, Ts...>, E>::Ok(std::tuple_cat(
        std::make_tuple(std::move(r1).value()), std::move(tail).value()));
  }
}

// Collect a vector of Result<T,E>
template <typename T, typename E>
[[nodiscard]] inline Result<std::vector<T>, E> collect_results(
    const std::vector<Result<T, E>>& items) {
  std::vector<T> out;
  out.reserve(items.size());
  for (const auto& r : items) {
    if (r.is_err()) return Result<std::vector<T>, E>::Err(r.error());
    out.push_back(r.value());
  }
  return Result<std::vector<T>, E>::Ok(std::move(out));
}
template <typename T, typename E>
[[nodiscard]] inline Result<std::vector<T>, E> collect_results(
    std::vector<Result<T, E>>&& items) {
  std::vector<T> out;
  out.reserve(items.size());
  for (auto& r : items) {
    if (r.is_err()) return Result<std::vector<T>, E>::Err(std::move(r).error());
    out.push_back(std::move(r).value());
  }
  return Result<std::vector<T>, E>::Ok(std::move(out));
}
template <typename T, typename E>
[[nodiscard]] inline Result<std::vector<T>, E> collect_results(
    std::initializer_list<Result<T, E>> items) {
  std::vector<T> out;
  out.reserve(items.size());
  for (const auto& r : items) {
    if (r.is_err()) return Result<std::vector<T>, E>::Err(r.error());
    out.push_back(r.value());
  }
  return Result<std::vector<T>, E>::Ok(std::move(out));
}

}  // namespace monad
