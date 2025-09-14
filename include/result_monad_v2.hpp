// result_monad_v2.hpp
#pragma once

#include <functional>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// Note: This header is self-contained and does not require the original
// result_monad.hpp. If you want aliases to monad::Error, forward declare it
// below or include the original header in your TU.

namespace monad {
namespace v2 {

// Tag types to disambiguate construction
struct ok_t {
};
struct err_t {
};
inline constexpr ok_t ok_in_place{};
inline constexpr err_t err_in_place{};

// Concept: checks a type looks like Result<*, E>
template <typename X, typename E>
concept result_with_error = requires {
  typename X::value_type;
  typename X::error_type;
  requires std::is_same_v<typename X::error_type, E>;
};

template <typename T, typename E>
class [[nodiscard]] Result {
 public:
  using value_type = T;
  using error_type = E;

  // Constructors (explicit, tag-based to avoid ambiguity)
  constexpr explicit Result(ok_t, const T& v) : data_(v) {}
  constexpr explicit Result(ok_t, T&& v) : data_(std::move(v)) {}
  constexpr explicit Result(err_t, const E& e) : data_(e) {}
  constexpr explicit Result(err_t, E&& e) : data_(std::move(e)) {}

  // Factories
  template <typename U = T>
  [[nodiscard]] static constexpr Result Ok(U&& v) {
    return Result(ok_in_place, std::forward<U>(v));
  }
  template <typename G = E>
  [[nodiscard]] static constexpr Result Err(G&& e) {
    return Result(err_in_place, std::forward<G>(e));
  }

  // Introspection
  [[nodiscard]] constexpr bool is_ok() const noexcept {
    return std::holds_alternative<T>(data_);
  }
  [[nodiscard]] constexpr bool is_err() const noexcept {
    return std::holds_alternative<E>(data_);
  }

  // Convenience: truthiness means success
  constexpr explicit operator bool() const noexcept { return is_ok(); }

  // Accessors (precondition: must be in the corresponding state)
  [[nodiscard]] constexpr const T& value() const& { return std::get<T>(data_); }
  [[nodiscard]] constexpr T& value() & { return std::get<T>(data_); }
  [[nodiscard]] constexpr T&& value() && { return std::get<T>(std::move(data_)); }

  [[nodiscard]] constexpr const E& error() const& { return std::get<E>(data_); }
  [[nodiscard]] constexpr E& error() & { return std::get<E>(data_); }
  [[nodiscard]] constexpr E&& error() && { return std::get<E>(std::move(data_)); }

  // Pointer-style access without throwing
  [[nodiscard]] constexpr const T* ok_ptr() const noexcept {
    return std::get_if<T>(&data_);
  }
  [[nodiscard]] constexpr T* ok_ptr() noexcept { return std::get_if<T>(&data_); }
  [[nodiscard]] constexpr const E* err_ptr() const noexcept {
    return std::get_if<E>(&data_);
  }
  [[nodiscard]] constexpr E* err_ptr() noexcept { return std::get_if<E>(&data_); }

  // Optional view of the value
  [[nodiscard]] std::optional<T> as_optional() const& {
    if (is_ok()) return value();
    return std::nullopt;
  }
  [[nodiscard]] std::optional<T> as_optional() && {
    if (is_ok()) return std::move(*ok_ptr());
    return std::nullopt;
  }

  // Map value (const&)
  template <typename F>
  [[nodiscard]] auto map(F&& f) const& -> Result<std::invoke_result_t<F, const T&>, E> {
    using U = std::invoke_result_t<F, const T&>;
    if (is_ok()) return Result<U, E>::Ok(std::invoke(std::forward<F>(f), value()));
    return Result<U, E>::Err(error());
  }

  // Map value (&&)
  template <typename F>
  [[nodiscard]] auto map(F&& f) && -> Result<std::invoke_result_t<F, T&&>, E> {
    using U = std::invoke_result_t<F, T&&>;
    if (is_ok()) return Result<U, E>::Ok(
        std::invoke(std::forward<F>(f), std::move(*ok_ptr())));
    return Result<U, E>::Err(std::move(*err_ptr()));
  }

  // Helper: detect Result<*, E>
  template <typename X>
  struct is_result_with_same_error : std::false_type {};
  template <typename U>
  struct is_result_with_same_error<Result<U, E>> : std::true_type {};

  // and_then (const&): T -> Result<U, E>
  template <typename F>
  [[nodiscard]] auto and_then(F&& f) const& -> std::invoke_result_t<F, const T&> {
    using Ret = std::invoke_result_t<F, const T&>;
    static_assert(result_with_error<Ret, E>,
                  "and_then must return Result<U,E>");
    if (is_ok()) return std::invoke(std::forward<F>(f), value());
    return Ret::Err(error());
  }

  // and_then (&&): T -> Result<U, E>
  template <typename F>
  [[nodiscard]] auto and_then(F&& f) && -> std::invoke_result_t<F, T&&> {
    using Ret = std::invoke_result_t<F, T&&>;
    static_assert(result_with_error<Ret, E>,
                  "and_then must return Result<U,E>");
    if (is_ok()) return std::invoke(std::forward<F>(f), std::move(*ok_ptr()));
    return Ret::Err(std::move(*err_ptr()));
  }

  // map_err (const&)
  template <typename F>
  [[nodiscard]] auto map_err(F&& f) const&
      -> Result<T, std::invoke_result_t<F, const E&>> {
    using NewError = std::invoke_result_t<F, const E&>;
    if (is_ok()) return Result<T, NewError>::Ok(value());
    return Result<T, NewError>::Err(
        std::invoke(std::forward<F>(f), error()));
  }

  // map_err (&&)
  template <typename F>
  [[nodiscard]] auto map_err(F&& f) && -> Result<T, std::invoke_result_t<F, E&&>> {
    using NewError = std::invoke_result_t<F, E&&>;
    if (is_ok()) return Result<T, NewError>::Ok(std::move(*ok_ptr()));
    return Result<T, NewError>::Err(
        std::invoke(std::forward<F>(f), std::move(*err_ptr())));
  }

  // catch_then (const&): E -> Result<T, F>
  template <typename F>
  [[nodiscard]] auto catch_then(F&& f) const& -> std::invoke_result_t<F, const E&> {
    using Ret = std::invoke_result_t<F, const E&>;
    static_assert(std::is_same_v<typename Ret::value_type, T>,
                  "catch_then must return Result<T,F>");
    if (is_err()) return std::invoke(std::forward<F>(f), error());
    return Ret::Ok(value());
  }

  // catch_then (&&): E -> Result<T, F>
  template <typename F>
  [[nodiscard]] auto catch_then(F&& f) && -> std::invoke_result_t<F, E&&> {
    using Ret = std::invoke_result_t<F, E&&>;
    static_assert(std::is_same_v<typename Ret::value_type, T>,
                  "catch_then must return Result<T,F>");
    if (is_err()) return std::invoke(std::forward<F>(f), std::move(*err_ptr()));
    return Ret::Ok(std::move(*ok_ptr()));
  }

  // or_else: transform error to same-typed Result<T,E>
  template <typename F>
  [[nodiscard]] Result or_else(F&& f) const& {
    if (is_err()) return std::forward<F>(f)(error());
    return *this;
  }
  template <typename F>
  [[nodiscard]] Result or_else(F&& f) && {
    if (is_err()) return std::forward<F>(f)(std::move(*err_ptr()));
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
    return is_ok() ? std::move(*ok_ptr()) : std::move(fallback);
  }

  // Visit underlying variant
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

  // Equality and swap
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

// void specialization
template <typename E>
class Result<void, E> {
 public:
  using value_type = void;
  using error_type = E;

  constexpr Result() = default;  // success
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

  // Optional view of success
  [[nodiscard]] std::optional<std::monostate> as_optional() const {
    return is_ok() ? std::optional<std::monostate>(std::in_place)
                   : std::optional<std::monostate>();
  }

  // and_then: () -> Result<U, E>
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

  // catch_then: E -> Result<void, F>
  template <typename F>
  [[nodiscard]] auto catch_then(F&& f) const& -> std::invoke_result_t<F, const E&> {
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
  [[nodiscard]] auto map_err(F&& f) const&
      -> Result<void, std::invoke_result_t<F, const E&>> {
    using NewError = std::invoke_result_t<F, const E&>;
    if (is_err()) return Result<void, NewError>::Err(
        std::invoke(std::forward<F>(f), *error_));
    return Result<void, NewError>::Ok();
  }
  template <typename F>
  [[nodiscard]] auto map_err(F&& f) &&
      -> Result<void, std::invoke_result_t<F, E&&>> {
    using NewError = std::invoke_result_t<F, E&&>;
    if (is_err()) return Result<void, NewError>::Err(
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

}  // namespace v2
}  // namespace monad

// Forward declare Error so aliases can be formed without pulling heavy deps.
namespace monad {
struct Error;
}  // namespace monad

namespace monad::v2 {
// Convenience aliases using the existing Error type (if defined in TU)
template <typename T>
using my_result = Result<T, ::monad::Error>;
using my_void_result = Result<void, ::monad::Error>;
}  // namespace monad::v2

// Free helpers to combine multiple Results into a tuple when all succeed
namespace monad::v2 {

// Base: empty pack yields Ok(()).
template <typename E>
[[nodiscard]] inline ::monad::v2::Result<std::tuple<>, E> zip_results() {
  return ::monad::v2::Result<std::tuple<>, E>::Ok(std::tuple<>{});
}

// Const& version: copies values
template <typename E, typename T1, typename... Ts>
[[nodiscard]] inline ::monad::v2::Result<std::tuple<T1, Ts...>, E> zip_results(
    const ::monad::v2::Result<T1, E>& r1, const ::monad::v2::Result<Ts, E>&... rest) {
  static_assert(!std::is_same_v<T1, void>,
                "zip_results does not support void; use all_ok for void.");
  if (r1.is_err()) return ::monad::v2::Result<std::tuple<T1, Ts...>, E>::Err(r1.error());
  auto tail = zip_results<E>(rest...);
  if (tail.is_err()) return ::monad::v2::Result<std::tuple<T1, Ts...>, E>::Err(tail.error());
  return ::monad::v2::Result<std::tuple<T1, Ts...>, E>::Ok(
      std::tuple_cat(std::make_tuple(r1.value()), tail.value()));
}

// Rvalue version: moves values
template <typename E, typename T1, typename... Ts>
[[nodiscard]] inline ::monad::v2::Result<std::tuple<T1, Ts...>, E> zip_results(
    ::monad::v2::Result<T1, E>&& r1, ::monad::v2::Result<Ts, E>&&... rest) {
  static_assert(!std::is_same_v<T1, void>,
                "zip_results does not support void; use all_ok for void.");
  if (r1.is_err())
    return ::monad::v2::Result<std::tuple<T1, Ts...>, E>::Err(std::move(r1).error());
  auto tail = zip_results<E>(std::move(rest)...);
  if (tail.is_err())
    return ::monad::v2::Result<std::tuple<T1, Ts...>, E>::Err(std::move(tail).error());
  return ::monad::v2::Result<std::tuple<T1, Ts...>, E>::Ok(std::tuple_cat(
      std::make_tuple(std::move(r1).value()), std::move(tail).value()));
}

// Helper for sequences of void results: returns void result that is ok only if all ok
template <typename E>
[[nodiscard]] inline ::monad::v2::Result<void, E> all_ok() {
  return ::monad::v2::Result<void, E>::Ok();
}

template <typename E, typename... Vs>
[[nodiscard]] inline ::monad::v2::Result<void, E> all_ok(const ::monad::v2::Result<void, E>& v1,
                                            const ::monad::v2::Result<Vs, E>&... rest) {
  if (v1.is_err()) return ::monad::v2::Result<void, E>::Err(v1.error());
  return all_ok<E>(rest...);
}

template <typename E, typename... Vs>
[[nodiscard]] inline ::monad::v2::Result<void, E> all_ok(::monad::v2::Result<void, E>&& v1,
                                            ::monad::v2::Result<Vs, E>&&... rest) {
  if (v1.is_err()) return ::monad::v2::Result<void, E>::Err(std::move(v1).error());
  return all_ok<E>(std::move(rest)...);
}

}  // namespace monad::v2

// Additional combinators
namespace monad::v2 {

// Type-level filter: build tuple of non-void types
template <typename... Ts>
struct filter_void_types;

template <>
struct filter_void_types<> {
  using type = std::tuple<>;
};

template <typename T, typename... Ts>
struct filter_void_types<T, Ts...> {
  using tail = typename filter_void_types<Ts...>::type;
  using type = std::conditional_t<
      std::is_same_v<T, void>,
      tail,
      decltype(std::tuple_cat(std::declval<std::tuple<T>>(), std::declval<tail>()))>;
};

template <typename... Ts>
using filter_void_types_t = typename filter_void_types<Ts...>::type;

// Skip-void zip: returns tuple of only non-void values when all succeed
template <typename E>
[[nodiscard]] inline ::monad::v2::Result<std::tuple<>, E> zip_results_skip_void() {
  return ::monad::v2::Result<std::tuple<>, E>::Ok(std::tuple<>{});
}

// const&
template <typename E, typename T1, typename... Ts>
[[nodiscard]] inline ::monad::v2::Result<filter_void_types_t<T1, Ts...>, E> zip_results_skip_void(
    const ::monad::v2::Result<T1, E>& r1, const ::monad::v2::Result<Ts, E>&... rest) {
  if (r1.is_err()) return ::monad::v2::Result<filter_void_types_t<T1, Ts...>, E>::Err(r1.error());
  auto tail = zip_results_skip_void<E>(rest...);
  if (tail.is_err()) return ::monad::v2::Result<filter_void_types_t<T1, Ts...>, E>::Err(tail.error());

  if constexpr (std::is_same_v<T1, void>) {
    return ::monad::v2::Result<filter_void_types_t<T1, Ts...>, E>::Ok(tail.value());
  } else {
    return ::monad::v2::Result<filter_void_types_t<T1, Ts...>, E>::Ok(
        std::tuple_cat(std::make_tuple(r1.value()), tail.value()));
  }
}

// rvalue
template <typename E, typename T1, typename... Ts>
[[nodiscard]] inline ::monad::v2::Result<filter_void_types_t<T1, Ts...>, E> zip_results_skip_void(
    ::monad::v2::Result<T1, E>&& r1, ::monad::v2::Result<Ts, E>&&... rest) {
  if (r1.is_err())
    return ::monad::v2::Result<filter_void_types_t<T1, Ts...>, E>::Err(std::move(r1).error());
  auto tail = zip_results_skip_void<E>(std::move(rest)...);
  if (tail.is_err())
    return ::monad::v2::Result<filter_void_types_t<T1, Ts...>, E>::Err(std::move(tail).error());

  if constexpr (std::is_same_v<T1, void>) {
    return ::monad::v2::Result<filter_void_types_t<T1, Ts...>, E>::Ok(std::move(tail).value());
  } else {
    return ::monad::v2::Result<filter_void_types_t<T1, Ts...>, E>::Ok(std::tuple_cat(
        std::make_tuple(std::move(r1).value()), std::move(tail).value()));
  }
}

// Collect a vector of Result<T,E> into Result<vector<T>,E> (copy)
template <typename T, typename E>
[[nodiscard]] inline ::monad::v2::Result<std::vector<T>, E> collect_results(
    const std::vector<::monad::v2::Result<T, E>>& items) {
  std::vector<T> out;
  out.reserve(items.size());
  for (const auto& r : items) {
    if (r.is_err()) return ::monad::v2::Result<std::vector<T>, E>::Err(r.error());
    out.push_back(r.value());
  }
  return ::monad::v2::Result<std::vector<T>, E>::Ok(std::move(out));
}

// Collect a vector of Result<T,E> into Result<vector<T>,E> (move)
template <typename T, typename E>
[[nodiscard]] inline ::monad::v2::Result<std::vector<T>, E> collect_results(
    std::vector<::monad::v2::Result<T, E>>&& items) {
  std::vector<T> out;
  out.reserve(items.size());
  for (auto& r : items) {
    if (r.is_err())
      return ::monad::v2::Result<std::vector<T>, E>::Err(std::move(r).error());
    out.push_back(std::move(r).value());
  }
  return ::monad::v2::Result<std::vector<T>, E>::Ok(std::move(out));
}

// Collect from initializer_list (copies values)
template <typename T, typename E>
[[nodiscard]] inline ::monad::v2::Result<std::vector<T>, E> collect_results(
    std::initializer_list<::monad::v2::Result<T, E>> items) {
  std::vector<T> out;
  out.reserve(items.size());
  for (const auto& r : items) {
    if (r.is_err()) return ::monad::v2::Result<std::vector<T>, E>::Err(r.error());
    out.push_back(r.value());
  }
  return ::monad::v2::Result<std::vector<T>, E>::Ok(std::move(out));
}

}  // namespace monad::v2
