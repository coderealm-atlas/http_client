// io_monad_v2.hpp
//
// A v2 IO monad built on top of monad::v2::Result. This API mirrors the
// original IO (include/io_monad.hpp) but uses Result<void, Error> instead of
// Result<std::monostate, Error>, and tightens value-category handling.
//
// Design principles
// - Laziness: IO wraps a thunk taking a callback; nothing executes until run().
// - Single-callback discipline: the thunk must invoke the callback exactly once.
// - Composability: map/then/catch_then/map_err/finally/finally_then compose IOs.
// - Clear error semantics: mapping exceptions map to Error{-1}, then exceptions
//   map to Error{-2}, catch_then exceptions map to Error{-3}. Timeout produces
//   Error{2, "Operation timed out"}. Timer failures produce Error{1, ...}.
// - Delay/timeout: delay holds back emission; timeout races the IO against a
//   timer and returns whichever finishes first.
#pragma once

#include <boost/asio.hpp>

#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include "result_monad.hpp"   // monad::Error
#include "result_monad_v2.hpp" // monad::v2::Result

namespace monad {
namespace v2 {

template <typename T>
class IO;

// Type trait to detect IO<U>
template <typename X>
struct is_io : std::false_type {};
template <typename U>
struct is_io<IO<U>> : std::true_type {};
template <typename X>
inline constexpr bool is_io_v = is_io<X>::value;

namespace detail {
inline void cancel_timer(boost::asio::steady_timer& timer) { timer.cancel(); }
}  // namespace detail

// Forward declarations for helpers defined later
//
// delay_for: Create an IO that completes after a duration.
//   - For T=void: yields Ok().
//   - For T!=void: yields Ok(T{}).
//   - On timer error: Err(Error{1, "Timer error: ..."}).
template <typename T>
IO<T> delay_for(boost::asio::io_context& ioc, std::chrono::milliseconds duration);
// delay_then: After a duration, yield a provided value (copy/move).
template <typename T>
IO<std::decay_t<T>> delay_then(boost::asio::io_context& ioc,
                               std::chrono::milliseconds duration, T&& val);

// delay_for and delay_then definitions are placed after IO classes.

/**
 * IO<T> wraps an asynchronous computation that yields Result<T, Error>.
 *
 * Contract:
 * - The underlying thunk must invoke the provided callback exactly once.
 * - No work is performed until run() is called.
 * - All combinators preserve the single-callback discipline.
 */
template <typename T>
class IO {
 public:
  using IOResult = Result<T, Error>;
  using Callback = std::function<void(IOResult)>;

  /**
   * Construct from a thunk that will eventually call the given callback with
   * Result<T, Error>. The thunk must call the callback exactly once.
   * No work starts until run() is called.
   */
  explicit IO(std::function<void(Callback)> thunk) : thunk_(std::move(thunk)) {}

  /**
   * Lift a value into IO.
   * - Always succeeds with Ok(std::move(value)).
   */
  static IO<T> pure(T value) {
    return IO([val = std::make_shared<T>(std::move(value))](Callback cb) mutable {
      cb(IOResult::Ok(std::move(*val)));
    });
  }

  /**
   * Produce a failed IO with the given Error.
   * - Always fails with the provided Error.
   */
  static IO<T> fail(Error error) {
    return IO([error = std::move(error)](Callback cb) mutable {
      cb(IOResult::Err(std::move(error)));
    });
  }

  /**
   * Lift a Result<T, Error> into IO<T>.
   * - Useful when a synchronous operation has already produced a Result.
   */
  static IO<T> from_result(Result<T, Error> res) {
    return IO([res = std::make_shared<Result<T, Error>>(std::move(res))](Callback cb) mutable {
      cb(std::move(*res));
    });
  }

  /**
   * Shallow copy the thunk (useful for retries/backoff and branching chains).
   * - Does not execute any work.
   */
  IO<T> clone() const { return IO<T>(thunk_); }

  /**
   * Map over the success value.
   *
   * Callable requirements:
   * - F: T -> U, where U may be void or a value type.
   *   - If U is void, returns IO<void> and discards the mapped value.
   *   - If U is a value type, returns IO<U> with that mapped value.
   *
   * Error semantics:
   * - If this IO is Err, f is not called and the error propagates.
   * - If f throws, maps to Err(Error{-1, what()}).
   */
  template <typename F>
  auto map(F&& f) const -> IO<decltype(std::declval<F>()(std::declval<T>()))> {
    using RetT = decltype(std::declval<F>()(std::declval<T>()));
    using NextIO = IO<RetT>;
    return NextIO([prev = *this, fn = std::forward<F>(f)](typename NextIO::Callback cb) mutable {
      prev.run([fn = std::move(fn), cb = std::move(cb)](IOResult r) mutable {
        if (r.is_ok()) {
          try {
            if constexpr (std::is_void_v<RetT>) {
              std::invoke(fn, std::move(r).value());
              cb(Result<void, Error>::Ok());
            } else {
              cb(Result<RetT, Error>::Ok(std::invoke(fn, std::move(r).value())));
            }
          } catch (const std::exception& e) {
            cb(Result<RetT, Error>::Err(Error{-1, e.what()}));
          }
        } else {
          if constexpr (std::is_void_v<RetT>) {
            cb(Result<void, Error>::Err(std::move(r).error()));
          } else {
            cb(Result<RetT, Error>::Err(std::move(r).error()));
          }
        }
      });
    });
  }

  /**
   * Flat-map on success.
   *
   * Callable requirements:
   * - F: T -> IO<U>.
   *
   * Error semantics:
   * - If this IO is Err, f is not called and the error propagates.
   * - If f throws, maps to Err(Error{-2, what()}).
   */
  template <typename F>
    requires(!std::is_void_v<T>)
  auto then(F&& f) const {
    using NextIO = std::invoke_result_t<F, T>;
    static_assert(is_io_v<NextIO>, "then() must return IO<U>");
    return NextIO([prev = *this, fn = std::forward<F>(f)](typename NextIO::Callback cb) mutable {
      prev.run([fn = std::move(fn), cb = std::move(cb)](IOResult r) mutable {
        if (r.is_ok()) {
          try {
            std::invoke(fn, std::move(r).value()).run(std::move(cb));
          } catch (const std::exception& e) {
            cb(NextIO::IOResult::Err(Error{-2, e.what()}));
          }
        } else {
          cb(NextIO::IOResult::Err(std::move(r).error()));
        }
      });
    });
  }

  /**
   * Handle errors by running a recovery that returns IO<T>.
   *
   * Callable requirements:
   * - F: Error -> IO<T>.
   *
   * Error semantics:
   * - Runs only when this IO is Err; otherwise the success result passes
   *   through unchanged.
   * - If f throws, maps to Err(Error{-3, what()}).
   */
  template <typename F>
  IO<T> catch_then(F&& f) const {
    return IO<T>([prev = *this, fn = std::forward<F>(f)](Callback cb) mutable {
      prev.run([fn = std::move(fn), cb = std::move(cb)](IOResult r) mutable {
        if (r.is_err()) {
          try {
            std::invoke(fn, std::move(r).error()).run(std::move(cb));
          } catch (const std::exception& e) {
            cb(IOResult::Err(Error{-3, e.what()}));
          }
        } else {
          cb(std::move(r));
        }
      });
    });
  }

  /**
   * Transform the error; pass through success unchanged.
   *
   * Callable requirements:
   * - F: Error -> Error.
   *
   * Error semantics:
   * - Pure mapping; does not attempt recovery. To recover, use catch_then.
   */
  template <typename F>
  IO<T> map_err(F&& f) const {
    return IO<T>([prev = *this, fn = std::forward<F>(f)](Callback cb) mutable {
      prev.run([fn = std::move(fn), cb = std::move(cb)](IOResult r) mutable {
        if (r.is_err()) {
          cb(IOResult::Err(std::invoke(fn, std::move(r).error())));
        } else {
          cb(std::move(r));
        }
      });
    });
  }

  /**
   * Always run a side-effect after completion (success or error).
   * - f: void() -> void
   * - The original result is returned unchanged.
   * - Exceptions thrown by f propagate to the caller.
   */
  template <typename F>
  IO<T> finally(F&& f) const {
    return IO<T>([prev = *this, fn = std::forward<F>(f)](Callback cb) mutable {
      prev.run([fn = std::move(fn), cb = std::move(cb)](IOResult r) mutable {
        std::invoke(fn);
        cb(std::move(r));
      });
    });
  }

  /**
   * Chain a monadic finalizer regardless of outcome.
   * - f: void() -> IO<void>
   * - The cleanup IO runs and its result is ignored; the original result is
   *   returned unchanged.
   * - If f throws, the original result is still returned.
   */
  template <typename F>
  IO<T> finally_then(F&& f) const {
    return IO<T>([prev = *this, fn = std::forward<F>(f)](Callback cb) mutable {
      prev.run([fn = std::move(fn), cb = std::move(cb)](IOResult r) mutable {
        try {
          std::invoke(fn).run([res = std::move(r), cb = std::move(cb)](auto) mutable {
            cb(std::move(res));
          });
        } catch (...) {
          cb(std::move(r));
        }
      });
    });
  }

  /**
   * Fail with timeout if not completed within duration (rvalue-qualified).
   * - On timeout: Err(Error{2, "Operation timed out"}).
   * - On timely completion: cancels the timer and returns the original result.
   */
  IO<T> timeout(boost::asio::io_context& ioc, std::chrono::milliseconds duration) && {
    return IO<T>([ioc_ptr = &ioc, duration, self = std::move(*this)](auto cb) mutable {
      auto timer = std::make_shared<boost::asio::steady_timer>(*ioc_ptr);
      auto fired = std::make_shared<bool>(false);

      timer->expires_after(duration);
      timer->async_wait([cb, fired](const boost::system::error_code& ec) mutable {
        if (*fired) return;
        *fired = true;
        if (!ec) cb(Result<T, Error>::Err(Error{2, "Operation timed out"}));
      });

      self.run([cb, timer, fired](IOResult r) mutable {
        if (*fired) return;
        *fired = true;
        detail::cancel_timer(*timer);
        cb(std::move(r));
      });
    });
  }

  /**
   * Timeout (lvalue overload); forwards to rvalue-qualified overload.
   */
  IO<T> timeout(boost::asio::io_context& ioc, std::chrono::milliseconds duration) & {
    return std::move(*this).timeout(ioc, duration);
  }

  /**
   * Delay the emission of this IO's result by duration (rvalue-qualified).
   * - Timer errors: Err(Error{1, "Timer error: ..."}).
   * - Otherwise, emits the original result after the delay expires.
   */
  IO<T> delay(boost::asio::io_context& ioc, std::chrono::milliseconds duration) && {
    return IO<T>([ioc_ptr = &ioc, duration, self = std::move(*this)](auto cb) mutable {
      auto timer = std::make_shared<boost::asio::steady_timer>(*ioc_ptr);
      auto result = std::make_shared<std::optional<IOResult>>();
      auto delivered = std::make_shared<bool>(false);
      auto timer_fired = std::make_shared<bool>(false);

      timer->expires_after(duration);
      timer->async_wait([cb, result, delivered, timer_fired, timer](const boost::system::error_code& ec) mutable {
        if (*delivered) return;
        if (ec) {
          *delivered = true;
          cb(Result<T, Error>::Err(Error{1, std::string{"Timer error: "} + ec.message()}));
          return;
        }
        *timer_fired = true;
        if (result->has_value()) {
          *delivered = true;
          cb(std::move(**result));
        }
      });

      self.run([cb, result, delivered, timer_fired, timer](IOResult r) mutable {
        if (*delivered) return;
        *result = std::move(r);
        if (*timer_fired) {
          *delivered = true;
          cb(std::move(**result));
        }
      });
    });
  }

  /**
   * Delay (lvalue overload); forwards to rvalue-qualified overload.
   */
  IO<T> delay(boost::asio::io_context& ioc, std::chrono::milliseconds duration) & {
    return std::move(*this).delay(ioc, duration);
  }

  /**
   * Conditional exponential backoff retry (rvalue-qualified).
   *
   * Parameters:
   * - max_attempts: total attempts including the first (must be >= 1).
   * - initial_delay: backoff start duration; doubles each retry.
   * - ioc: Boost.Asio io_context to schedule timers.
   * - should_retry: bool(const Error&) — return true to retry on the error.
   *
   * Semantics:
   * - Re-runs the IO on error while should_retry(error) is true, with
   *   exponential backoff. Stops on success, on reaching max_attempts, or when
   *   should_retry returns false. Returns the first success or last error.
   */
  IO<T> retry_exponential_if(int max_attempts, std::chrono::milliseconds initial_delay,
                             boost::asio::io_context& ioc,
                             std::function<bool(const Error&)> should_retry) && {
    return IO<T>([max_attempts, initial_delay, ioc_ptr = &ioc,
                  should_retry = std::move(should_retry),
                  self_ptr = std::make_shared<IO<T>>(std::move(*this))](auto cb) mutable {
      auto attempt = std::make_shared<int>(0);
      auto try_run = std::make_shared<std::function<void(std::chrono::milliseconds)>>();

      *try_run = [=](std::chrono::milliseconds current_delay) mutable {
        (*attempt)++;
        self_ptr->clone().run([=](IOResult r) mutable {
          if (r.is_ok() || *attempt >= max_attempts || !should_retry(r.error())) {
            cb(std::move(r));
          } else {
            auto timer = std::make_shared<boost::asio::steady_timer>(*ioc_ptr);
            timer->expires_after(current_delay);
            timer->async_wait([=, timer = timer](const boost::system::error_code& ec) mutable {
              if (ec) {
                cb(Result<T, Error>::Err(Error{1, std::string{"Timer error: "} + ec.message()}));
              } else {
                (*try_run)(current_delay * 2);
              }
            });
          }
        });
      };

      (*try_run)(initial_delay);
    });
  }

  /**
   * Exponential backoff retry (rvalue-qualified) that retries on any error.
   * - Equivalent to retry_exponential_if(max_attempts, initial_delay, ioc,
   *   [](const Error&){ return true; }).
   */
  IO<T> retry_exponential(int max_attempts, std::chrono::milliseconds initial_delay,
                          boost::asio::io_context& ioc) && {
    return std::move(*this).retry_exponential_if(max_attempts, initial_delay, ioc,
                                                 [](const Error&) { return true; });
  }

  /**
   * Execute the IO by providing a callback that receives Result<T, Error>.
   * - Triggers the side effect captured by this IO.
   */
  void run(Callback cb) const { thunk_(std::move(cb)); }

 private:
  std::function<void(Callback)> thunk_;
};

// IO<void> specialization
/**
 * IO<void> specialization. Uses Result<void, Error> for the outcome.
 */
template <>
class IO<void> {
 public:
  using IOResult = Result<void, Error>;
  using Callback = std::function<void(IOResult)>;

  explicit IO(std::function<void(Callback)> thunk) : thunk_(std::move(thunk)) {}

  /** Lift success (void) into IO<void>. */
  static IO<void> pure() { return IO([](Callback cb) { cb(IOResult::Ok()); }); }

  /** Produce a failed IO<void> with the given Error. */
  static IO<void> fail(Error error) {
    return IO([error = std::move(error)](Callback cb) mutable {
      cb(IOResult::Err(std::move(error)));
    });
  }

  /** Lift a Result<void, Error> into IO<void>. */
  static IO<void> from_result(Result<void, Error> res) {
    return IO([res = std::make_shared<Result<void, Error>>(std::move(res))](Callback cb) mutable {
      cb(std::move(*res));
    });
  }

  /** Shallow copy the thunk; no execution occurs. */
  IO<void> clone() const { return IO<void>(thunk_); }

  /**
   * Map over a successful IO<void>.
   * - F: void() -> void or F: void() -> U, returning IO<void> or IO<U>.
   * - Exceptions from f map to Err(Error{-1, what()}).
   */
  template <typename F>
  auto map(F&& f) const -> IO<decltype(std::declval<F>()())> {
    using RetT = decltype(std::declval<F>()());
    using NextIO = IO<RetT>;
    return NextIO([prev = *this, fn = std::forward<F>(f)](typename NextIO::Callback cb) mutable {
      prev.run([fn = std::move(fn), cb = std::move(cb)](IOResult r) mutable {
        if (r.is_ok()) {
          try {
            if constexpr (std::is_void_v<RetT>) {
              std::invoke(fn);
              cb(Result<void, Error>::Ok());
            } else {
              cb(Result<RetT, Error>::Ok(std::invoke(fn)));
            }
          } catch (const std::exception& e) {
            if constexpr (std::is_void_v<RetT>) {
              cb(Result<void, Error>::Err(Error{-1, e.what()}));
            } else {
              cb(Result<RetT, Error>::Err(Error{-1, e.what()}));
            }
          }
        } else {
          if constexpr (std::is_void_v<RetT>) {
            cb(Result<void, Error>::Err(std::move(r).error()));
          } else {
            cb(Result<RetT, Error>::Err(std::move(r).error()));
          }
        }
      });
    });
  }

  /**
   * then for IO<void>: () -> IO<U> (flat-map).
   * - Exceptions from f map to Err(Error{-2, what()}).
   */
  template <typename F>
  auto then(F&& f) const {
    using NextIO = std::invoke_result_t<F>;
    static_assert(is_io_v<NextIO>, "then() must return IO<U>");
    return NextIO([prev = *this, fn = std::forward<F>(f)](typename NextIO::Callback cb) mutable {
      prev.run([fn = std::move(fn), cb = std::move(cb)](IOResult r) mutable {
        if (r.is_ok()) {
          try {
            std::invoke(fn).run(std::move(cb));
          } catch (const std::exception& e) {
            cb(NextIO::IOResult::Err(Error{-2, e.what()}));
          }
        } else {
          cb(NextIO::IOResult::Err(std::move(r).error()));
        }
      });
    });
  }

  /**
   * Handle errors for IO<void> by returning IO<void>.
   * - F: Error -> IO<void>. Exceptions map to Err(Error{-3, what()}).
   */
  template <typename F>
  IO<void> catch_then(F&& f) const {
    return IO<void>([prev = *this, fn = std::forward<F>(f)](Callback cb) mutable {
      prev.run([fn = std::move(fn), cb = std::move(cb)](IOResult r) mutable {
        if (r.is_err()) {
          try {
            std::invoke(fn, std::move(r).error()).run(std::move(cb));
          } catch (const std::exception& e) {
            cb(IOResult::Err(Error{-3, e.what()}));
          }
        } else {
          cb(std::move(r));
        }
      });
    });
  }

  /** Transform the error for IO<void>; success passes through unchanged. */
  template <typename F>
  IO<void> map_err(F&& f) const {
    return IO<void>([prev = *this, fn = std::forward<F>(f)](Callback cb) mutable {
      prev.run([fn = std::move(fn), cb = std::move(cb)](IOResult r) mutable {
        if (r.is_err()) {
          cb(IOResult::Err(std::invoke(fn, std::move(r).error())));
        } else {
          cb(std::move(r));
        }
      });
    });
  }

  /** Always run a side-effect after completion (success or error). */
  template <typename F>
  IO<void> finally(F&& f) const {
    return IO<void>([prev = *this, fn = std::forward<F>(f)](Callback cb) mutable {
      prev.run([fn = std::move(fn), cb = std::move(cb)](IOResult r) mutable {
        std::invoke(fn);
        cb(std::move(r));
      });
    });
  }

  /** Chain a monadic finalizer for IO<void>; original result is returned. */
  template <typename F>
  IO<void> finally_then(F&& f) const {
    return IO<void>([prev = *this, fn = std::forward<F>(f)](Callback cb) mutable {
      prev.run([fn = std::move(fn), cb = std::move(cb)](IOResult r) mutable {
        try {
          std::invoke(fn).run([res = std::move(r), cb = std::move(cb)](auto) mutable {
            cb(std::move(res));
          });
        } catch (...) {
          cb(std::move(r));
        }
      });
    });
  }

  /**
   * Fail with timeout for IO<void> if not completed within duration.
   * - On timeout: Err(Error{2, "Operation timed out"}).
   */
  IO<void> timeout(boost::asio::io_context& ioc, std::chrono::milliseconds duration) && {
    return IO<void>([ioc_ptr = &ioc, duration, self = std::move(*this)](auto cb) mutable {
      auto timer = std::make_shared<boost::asio::steady_timer>(*ioc_ptr);
      auto fired = std::make_shared<bool>(false);

      timer->expires_after(duration);
      timer->async_wait([cb, fired](const boost::system::error_code& ec) mutable {
        if (*fired) return;
        *fired = true;
        if (!ec) cb(IOResult::Err(Error{2, "Operation timed out"}));
      });

      self.run([cb, timer, fired](IOResult r) mutable {
        if (*fired) return;
        *fired = true;
        detail::cancel_timer(*timer);
        cb(std::move(r));
      });
    });
  }

  /**
   * Conditional exponential backoff retry for IO<void> (rvalue-qualified).
   * - Same semantics as IO<T>::retry_exponential_if.
   */
  IO<void> retry_exponential_if(int max_attempts, std::chrono::milliseconds initial_delay,
                                boost::asio::io_context& ioc,
                                std::function<bool(const Error&)> should_retry) && {
    return IO<void>([max_attempts, initial_delay, ioc_ptr = &ioc,
                     should_retry = std::move(should_retry),
                     self_ptr = std::make_shared<IO<void>>(std::move(*this))](auto cb) mutable {
      auto attempt = std::make_shared<int>(0);
      auto try_run = std::make_shared<std::function<void(std::chrono::milliseconds)>>();

      *try_run = [=](std::chrono::milliseconds current_delay) mutable {
        (*attempt)++;
        self_ptr->clone().run([=](IOResult r) mutable {
          if (r.is_ok() || *attempt >= max_attempts || !should_retry(r.error())) {
            cb(std::move(r));
          } else {
            auto timer = std::make_shared<boost::asio::steady_timer>(*ioc_ptr);
            timer->expires_after(current_delay);
            timer->async_wait([=, timer = timer](const boost::system::error_code& ec) mutable {
              if (ec) {
                cb(Result<void, Error>::Err(Error{1, std::string{"Timer error: "} + ec.message()}));
              } else {
                (*try_run)(current_delay * 2);
              }
            });
          }
        });
      };

      (*try_run)(initial_delay);
    });
  }

  /** Retry on any error for IO<void> (rvalue-qualified). */
  IO<void> retry_exponential(int max_attempts, std::chrono::milliseconds initial_delay,
                             boost::asio::io_context& ioc) && {
    return std::move(*this).retry_exponential_if(max_attempts, initial_delay, ioc,
                                                 [](const Error&) { return true; });
  }

  // Delay emitting completion by duration (rvalue-qualified)
  IO<void> delay(boost::asio::io_context& ioc, std::chrono::milliseconds duration) && {
    return IO<void>([ioc_ptr = &ioc, duration, self = std::move(*this)](auto cb) mutable {
      auto timer = std::make_shared<boost::asio::steady_timer>(*ioc_ptr);
      auto result = std::make_shared<std::optional<IOResult>>();
      auto delivered = std::make_shared<bool>(false);
      auto timer_fired = std::make_shared<bool>(false);

      timer->expires_after(duration);
      timer->async_wait([cb, result, delivered, timer_fired, timer](const boost::system::error_code& ec) mutable {
        if (*delivered) return;
        if (ec) {
          *delivered = true;
          cb(Result<void, Error>::Err(Error{1, std::string{"Timer error: "} + ec.message()}));
          return;
        }
        *timer_fired = true;
        if (result->has_value()) {
          *delivered = true;
          cb(std::move(**result));
        }
      });

      self.run([cb, result, delivered, timer_fired, timer](IOResult r) mutable {
        if (*delivered) return;
        *result = std::move(r);
        if (*timer_fired) {
          *delivered = true;
          cb(std::move(**result));
        }
      });
    });
  }

  /** Execute the IO<void> by providing a callback that receives Result<void, Error>. */
  void run(Callback cb) const { thunk_(std::move(cb)); }

 private:
  std::function<void(Callback)> thunk_;
};

}  // namespace v2
}  // namespace monad

// Free helpers to combine multiple IOs (sequentially) similar to Result combinators
namespace monad::v2 {

/** Base case: no IOs -> IO of empty tuple (Ok(())) */
inline IO<std::tuple<>> zip_io() { return IO<std::tuple<>>::pure({}); }

/**
 * Sequentially zip multiple IOs into IO<tuple<...>>.
 * - Executes IOs left-to-right, short-circuits on the first error.
 * - Returns IO<tuple<Ts...>> on success.
 * - Useful when subsequent IOs can depend on earlier results and you prefer
 *   explicit sequencing.
 */
template <typename T1, typename... Ts>
inline IO<std::tuple<T1, Ts...>> zip_io(IO<T1> first, IO<Ts>... rest) {
  if constexpr (sizeof...(Ts) == 0) {
    return std::move(first).map([](T1 v1) { return std::make_tuple(std::move(v1)); });
  } else {
    return std::move(first).then([rest...](T1 v1) mutable {
      return zip_io(std::move(rest)...).map([v1 = std::move(v1)](auto tail) {
        return std::tuple_cat(std::make_tuple(std::move(v1)), std::move(tail));
      });
    });
  }
}

// Type-level filter for removing voids from tuples (reuses Result v2's trait if available)
template <typename... Ts>
struct io_filter_void_types;
template <>
struct io_filter_void_types<> { using type = std::tuple<>; };
template <typename T, typename... Ts>
struct io_filter_void_types<T, Ts...> {
  using tail = typename io_filter_void_types<Ts...>::type;
  using type = std::conditional_t<std::is_same_v<T, void>, tail,
                                  decltype(std::tuple_cat(std::declval<std::tuple<T>>(), std::declval<tail>()))>;
};
template <typename... Ts>
using io_filter_void_types_t = typename io_filter_void_types<Ts...>::type;

/** Zip IOs, skipping void values in the resulting tuple; still short-circuits on error. */
inline IO<std::tuple<>> zip_io_skip_void() { return IO<std::tuple<>>::pure({}); }

template <typename T1, typename... Ts>
inline IO<io_filter_void_types_t<T1, Ts...>> zip_io_skip_void(IO<T1> first, IO<Ts>... rest) {
  if constexpr (sizeof...(Ts) == 0) {
    if constexpr (std::is_same_v<T1, void>) {
      return std::move(first).map([] { return std::tuple<>(); });
    } else {
      return std::move(first).map([](T1 v1) { return std::make_tuple(std::move(v1)); });
    }
  } else {
    if constexpr (std::is_same_v<T1, void>) {
      return std::move(first).then([rest...]() mutable {
        return zip_io_skip_void(std::move(rest)...);
      });
    } else {
      return std::move(first).then([rest...](T1 v1) mutable {
        return zip_io_skip_void(std::move(rest)...)
            .map([v1 = std::move(v1)](auto tail) {
              return std::tuple_cat(std::make_tuple(std::move(v1)), std::move(tail));
            });
      });
    }
  }
}

/** Collect a vector of IO<T> into IO<vector<T>>; sequential, short-circuits on error. */
template <typename T>
inline IO<std::vector<T>> collect_io(std::vector<IO<T>> items) {
  return IO<std::vector<T>>([items = std::make_shared<std::vector<IO<T>>>(std::move(items))](auto cb) mutable {
    auto out = std::make_shared<std::vector<T>>();
    out->reserve(items->size());
    auto idx = std::make_shared<size_t>(0);

    auto step = std::make_shared<std::function<void()>>();
    *step = [=]() mutable {
      if (*idx >= items->size()) {
        cb(Result<std::vector<T>, Error>::Ok(std::move(*out)));
        return;
      }
      auto current = (*items)[*idx].clone();
      current.run([=](Result<T, Error> r) mutable {
        if (r.is_err()) {
          cb(Result<std::vector<T>, Error>::Err(r.error()));
        } else {
          out->push_back(std::move(r).value());
          ++(*idx);
          (*step)();
        }
      });
    };

    (*step)();
  });
}

/** Collect from initializer_list (copies items). */
template <typename T>
inline IO<std::vector<T>> collect_io(std::initializer_list<IO<T>> items) {
  return collect_io(std::vector<IO<T>>(items));
}

/** All-ok for a vector of IO<void>; returns Ok() only if all succeed; short-circuits on first error. */
inline IO<void> all_ok_io(std::vector<IO<void>> items) {
  return IO<void>([items = std::make_shared<std::vector<IO<void>>>(std::move(items))](auto cb) mutable {
    auto idx = std::make_shared<size_t>(0);

    auto step = std::make_shared<std::function<void()>>();
    *step = [=]() mutable {
      if (*idx >= items->size()) {
        cb(Result<void, Error>::Ok());
        return;
      }
      auto current = (*items)[*idx].clone();
      current.run([=](Result<void, Error> r) mutable {
        if (r.is_err()) {
          cb(Result<void, Error>::Err(r.error()));
        } else {
          ++(*idx);
          (*step)();
        }
      });
    };

    (*step)();
  });
}

inline IO<void> all_ok_io(std::initializer_list<IO<void>> items) {
  return all_ok_io(std::vector<IO<void>>(items));
}

}  // namespace monad::v2

// Now that IO<T> and IO<void> are fully declared, define delay_for / delay_then
namespace monad::v2 {

/**
 * Create an IO that completes after a delay.
 * - For T=void: yields Ok().
 * - For T!=void: yields Ok(T{}).
 * - On timer error: Err(Error{1, "Timer error: ..."}).
 */
template <typename T = void>
IO<T> delay_for(boost::asio::io_context& ioc, std::chrono::milliseconds duration) {
  return IO<T>([&ioc, duration](auto cb) {
    auto timer = std::make_shared<boost::asio::steady_timer>(ioc, duration);
    timer->async_wait([timer, cb](const boost::system::error_code& ec) mutable {
      if (ec) {
        cb(Result<T, Error>::Err(Error{1, std::string{"Timer error: "} + ec.message()}));
      } else {
        if constexpr (std::is_same_v<T, void>) {
          cb(Result<void, Error>::Ok());
        } else {
          cb(Result<T, Error>::Ok(T{}));
        }
      }
    });
  });
}

/** Delay and then yield a provided value (copy/move). */
template <typename T>
IO<std::decay_t<T>> delay_then(boost::asio::io_context& ioc,
                               std::chrono::milliseconds duration, T&& val) {
  using U = std::decay_t<T>;
  return delay_for<void>(ioc, duration)
      .map([val = std::forward<T>(val)]() mutable { return U(std::forward<T>(val)); });
}

/** Provide delay as a free helper for convenience; returns the given IO after the delay. */
template <typename T>
IO<T> delay(IO<T> io, boost::asio::io_context& ioc, std::chrono::milliseconds duration) {
  return delay_for<void>(ioc, duration).then([self = std::move(io)]() mutable { return std::move(self); });
}

}  // namespace monad::v2
