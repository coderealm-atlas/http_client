#pragma once

#include <boost/asio.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <boost/json.hpp>
#include "result_monad.hpp"

// This is a simple IO monad implementation in C++.
// It allows for chaining operations that may involve side effects,
// such as I/O operations, while maintaining a functional style.
namespace monad {

template <typename T>
class IO;

namespace detail {
inline void cancel_timer(boost::asio::steady_timer& timer) { timer.cancel(); }
}  // namespace detail

/**
 * Create an IO that completes after a delay, yielding a default-constructed T.
 *
 * Callable requirements:
 * - N/A
 *
 * What NOT to return:
 * - N/A (this is a factory function; no callable is provided).
 *
 * Error semantics:
 * - On timer error, yields Error{1, ec.message()}.
 */
template <typename T = std::monostate>
IO<T> delay_for(boost::asio::io_context& ioc,
                std::chrono::milliseconds duration) {
  return IO<T>([&ioc, duration](auto cb) {
    auto timer = std::make_shared<boost::asio::steady_timer>(ioc, duration);
    timer->async_wait([timer, cb](const boost::system::error_code& ec) {
      if (ec) {
        cb(Error{1, "Timer error: " + ec.message()});
      } else {
        if constexpr (std::is_same_v<T, std::monostate>)
          cb(std::monostate{});
        else
          cb(T{});  // default construct T
      }
    });
  });
}

/**
 * Delay and then yield a provided value.
 *
 * Callable requirements:
 * - N/A
 *
 * What NOT to return:
 * - N/A (this is a convenience factory; no callable is provided).
 *
 * Error semantics:
 * - On timer error, yields Error{1, ec.message()}.
 */
template <typename T>
IO<std::decay_t<T>> delay_then(boost::asio::io_context& ioc,
                               std::chrono::milliseconds duration, T&& val) {
  using U = std::decay_t<T>;
  return delay_for<U>(ioc, duration).map([val = std::forward<T>(val)](auto) {
    return val;
  });
}

template <typename T>
class IO {
 public:
  using IOResult = Result<T, Error>;
  using Callback = std::function<void(IOResult)>;

  explicit IO(std::function<void(Callback)> thunk) : thunk_(std::move(thunk)) {}

  /**
   * Lift a value into IO.
   *
   * Callable requirements:
   * - N/A
   *
   * What NOT to return:
   * - N/A (value is provided directly).
   *
   * Error semantics:
   * - Always succeeds with the given value.
   */
  static IO<T> pure(T value) {
    return IO([val = std::make_shared<T>(std::move(value))](Callback cb) {
      cb(IOResult::Ok(std::move(*val)));
    });
  }

  /**
   * Produce a failed IO with the given Error.
   *
   * Callable requirements:
   * - N/A
   *
   * What NOT to return:
   * - N/A (error is provided directly).
   *
   * Error semantics:
   * - Always fails with the given Error.
   */
  static IO<T> fail(Error error) {
    return IO([error = std::move(error)](Callback cb) { cb(IOResult{error}); });
  }

  /**
   * Lift a MyResult<T> into IO<T> without additional effects.
   * - Useful when an operation already produced a Result and you want to
   *   continue in the IO pipeline.
   */
  static IO<T> from_result(Result<T, Error> res) {
    return IO([res = std::make_shared<Result<T, Error>>(std::move(res))](
                  Callback cb) mutable { cb(std::move(*res)); });
  }

  /**
   * Shallow copy the IO thunk. Useful for retry/backoff.
   *
   * Callable requirements:
   * - N/A
   *
   * What NOT to return:
   * - N/A
   *
   * Error semantics:
   * - N/A (no execution occurs).
   */
  IO<T> clone() const {
    static_assert(std::is_copy_constructible_v<decltype(*this)>,
                  "Cannot clone IO<T>: thunk is not copyable");
    return IO<T>(thunk_);
  }

  template <typename F>
  /**
   * Map over the successful value of this IO.
   *
   * Callable requirements (T != void):
   * - f: T -> U, where U can be a value type or void.
   *   - If U is a value type, the result is IO<U> with that value.
   *   - If U is void, the result is IO<void> and the value is discarded.
   *
   * What NOT to return:
   * - Do not return monad::Error or Result from f. Use then() to return an
   * IO<...> when you need to produce a failure, or use map_err()/catch_then()
   * to handle errors.
   *
   * Error semantics:
   * - If this IO is an error, f is not called and the error is propagated.
   * - If f throws, the exception is caught and converted to Error{-1, what()}.
   *
   * For IO<void>, see the specialization below where f has signature void() ->
   * void.
   */
  auto map(F&& f) const -> IO<decltype(std::declval<F>()(std::declval<T>()))> {
    using RetT = decltype(std::declval<F>()(std::declval<T>()));

    return IO<RetT>([prev_ptr = std::make_shared<IO<T>>(*this),
                     f = std::forward<F>(f)](typename IO<RetT>::Callback cb) {
      prev_ptr->run(
          [cb = std::move(cb), f = std::move(f)](IOResult result) mutable {
            if (result.is_ok()) {
              try {
                if constexpr (std::is_void_v<RetT>) {
                  std::invoke(f, std::move(result.value()));
                  cb(std::monostate{});
                } else {
                  cb(std::invoke(f, std::move(result.value())));
                }
              } catch (const std::exception& e) {
                cb(Error{-1, e.what()});
              }
            } else {
              cb(result.error());
            }
          });
    });
  }

  template <typename F>
  /**
   * Flat-map to another IO on success.
   *
   * Callable requirements:
   * - If T != void: f: T -> IO<U>
   * - If T == void: f: () -> IO<U>
   *   The returned IO<U> is executed and flattened.
   *
   * What NOT to return:
   * - Do not return a plain value U or Result. Use map() to transform to a
   * value or wrap the value with IO<U>::pure(U). Returning Result is not
   * supported here.
   *
   * Error semantics:
   * - If this IO is an error, f is not called and the error is propagated.
   * - If f throws, the exception is caught and converted to Error{-2, what()}.
   */
  auto then(F&& f) const {
    using NextIO = std::invoke_result_t<F, T>;
    static_assert(std::is_same_v<decltype(f(std::declval<T>())), NextIO>,
                  "then() must return IO<U>");

    auto f_wrapped = std::make_shared<std::decay_t<F>>(std::forward<F>(f));
    auto prev_ptr = std::make_shared<IO<T>>(*this);
    return NextIO([prev_ptr, f_wrapped](typename NextIO::Callback cb) mutable {
      prev_ptr->run([f_wrapped, cb = std::move(cb)](IOResult result) mutable {
        if (result.is_ok()) {
          try {
            if constexpr (std::is_same_v<T, void>)
              (*f_wrapped)().run(std::move(cb));
            else
              (*f_wrapped)(std::move(result.value())).run(std::move(cb));
          } catch (const std::exception& e) {
            cb(Error{-2, e.what()});
          }
        } else {
          cb(result.error());
        }
      });
    });
  }

  template <typename F>
  /**
   * Handle errors by running a recovery that returns IO<T>.
   *
   * Callable requirements:
   * - f: Error -> IO<T>
   *
   * What NOT to return:
   * - Do not return a plain T or Result. You must return IO<T> here if you want
   * to recover. To simply transform the Error, use map_err().
   *
   * Error semantics:
   * - Runs only when this IO is an error; otherwise passes through the success
   * value.
   * - If f throws, the exception is caught and converted to Error{-3, what()}.
   */
  IO<T> catch_then(F&& f) const {
    auto prev_ptr = std::make_shared<IO<T>>(*this);
    auto f_ptr = std::make_shared<std::decay_t<F>>(std::forward<F>(f));

    return IO<T>([prev_ptr, f_ptr](Callback cb) mutable {
      prev_ptr->run([f_ptr, cb = std::move(cb)](IOResult result) mutable {
        if (result.is_err()) {
          try {
            (*f_ptr)(result.error()).run(std::move(cb));
          } catch (const std::exception& e) {
            cb(Error{-3, e.what()});
          }
        } else {
          cb(std::move(result));
        }
      });
    });
  }

  /**
   * Transform the error if present; pass through success unchanged.
   *
   * Callable requirements:
   * - f: Error -> Error (pure mapping)
   *
   * What NOT to return:
   * - Do not return IO<...> or throw to recover. To recover asynchronously, use
   * catch_then(). map_err() is for pure Error-to-Error transformation only.
   */
  IO<T> map_err(std::function<Error(Error)> f) const {
    return IO<T>([prev = *this, f = std::move(f)](typename IO<T>::Callback cb) {
      prev.run(
          [f, cb = std::move(cb)](typename IO<T>::IOResult result) mutable {
            if (result.is_err()) {
              cb(f(result.error()));
            } else {
              cb(std::move(result));
            }
          });
    });
  }

  /**
   * Always run a side-effecting finalizer after completion (success or error).
   *
   * Callable requirements:
   * - f: void() -> void
   *
   * What NOT to return:
   * - N/A (return value is ignored). If cleanup must perform IO or may fail,
   *   prefer finally_then() so cleanup can be expressed as IO.
   *
   * Error semantics:
   * - Finalizer exceptions are not caught here; ensure f() is noexcept if
   * needed.
   */
  IO<T> finally(std::function<void()> f) const {
    return IO<T>([prev = *this, f = std::move(f)](Callback cb) {
      prev.run([f, cb = std::move(cb)](IOResult result) mutable {
        f();
        cb(std::move(result));
      });
    });
  }

  template <typename F>
  /**
   * Chain a monadic finalizer regardless of success or error.
   *
   * Callable requirements:
   * - f: void() -> IO<void>
   *
   * What NOT to return:
   * - Do not return a value, Error, or Result; the cleanup must be an IO<void>.
   *   This method does not alter the original result; the cleanup's outcome is
   * ignored.
   *
   * Error semantics:
   * - Cleanup IO is run and its result is ignored; the original result is
   * returned.
   * - If f throws, the original result is still returned.
   */
  IO<T> finally_then(F&& f) const {
    return IO<T>([prev = *this, f = std::forward<F>(f)](Callback cb) mutable {
      prev.run([f = std::move(f), cb = std::move(cb)](IOResult result) mutable {
        try {
          auto cleanup_io = f();
          cleanup_io.run(
              [result = std::move(result), cb = std::move(cb)](auto) mutable {
                // Ignore cleanup result, return original result
                cb(std::move(result));
              });
        } catch (const std::exception& e) {
          // If cleanup throws, still return original result
          cb(std::move(result));
        }
      });
    });
  }

  /**
   * Delay the emission of this IO's result by duration (rvalue-qualified).
   *
   * Callable requirements:
   * - N/A
   *
   * What NOT to return:
   * - N/A
   *
   * Error semantics:
   * - Timer failure yields Error{1, "Timer error: ..."} before running the IO.
   */
  IO<T> delay(boost::asio::io_context& ioc,
              std::chrono::milliseconds duration) && {
    return IO<T>(
        [ioc_ptr = &ioc, duration, self = std::move(*this)](auto cb) mutable {
          auto timer = std::make_shared<boost::asio::steady_timer>(*ioc_ptr);
          timer->expires_after(duration);
          timer->async_wait([cb, timer](const boost::system::error_code& ec) {
            if (ec) {
              cb(monad::Error{1, "Timer error: " + ec.message()});
            }
          });
          self.run([cb, timer](auto r) mutable { cb(std::move(r)); });
        });
  }

  /**
   * Delay the emission of this IO's result by duration (lvalue overload).
   * - Forwards to the rvalue overload.
   */
  IO<T> delay(boost::asio::io_context& ioc,
              std::chrono::milliseconds duration) & {
    return std::move(*this).delay(ioc, duration);
  }

  /**
   * Fail with timeout if this IO doesn't complete within duration
   * (rvalue-qualified).
   *
   * Callable requirements:
   * - N/A
   *
   * What NOT to return:
   * - N/A
   *
   * Error semantics:
   * - On timeout, yields Error{2, "Operation timed out"}.
   * - On timely completion, cancels the timer and yields the original result.
   */
  IO<T> timeout(boost::asio::io_context& ioc,
                std::chrono::milliseconds duration) && {
    return IO<T>(
        [ioc_ptr = &ioc, duration, self = std::move(*this)](auto cb) mutable {
          auto timer = std::make_shared<boost::asio::steady_timer>(*ioc_ptr);
          auto fired = std::make_shared<bool>(false);

          timer->expires_after(duration);
          timer->async_wait([cb, fired](const boost::system::error_code& ec) {
            if (*fired) return;
            *fired = true;
            if (!ec) {
              cb(monad::Error{2, "Operation timed out"});
            }
          });

          self.run([cb, timer, fired](auto r) mutable {
            if (*fired) return;
            *fired = true;
            detail::cancel_timer(*timer);
            cb(std::move(r));
          });
        });
  }

  /**
   * Timeout (lvalue overload); forwards to the rvalue overload.
   */
  IO<T> timeout(boost::asio::io_context& ioc,
                std::chrono::milliseconds duration) & {
    return std::move(*this).timeout(ioc, duration);
  }

  // Conditional exponential backoff retry (IO<T>)
  /**
   * Conditional exponential backoff retry.
   *
   * Callable requirements:
   * - should_retry: const Error& -> bool (decides whether to retry on a given
   * error)
   *
   * What NOT to return:
   * - Do not throw from should_retry; return false to stop retrying.
   * - Do not block/sleep inside should_retry; backoff is handled by the
   * operator.
   *
   * Error semantics:
   * - Retries up to max_attempts while should_retry(error) is true, doubling
   * delay each time starting from initial_delay. Returns the first success or
   * the last error.
   */
  IO<T> retry_exponential_if(
      int max_attempts, std::chrono::milliseconds initial_delay,
      boost::asio::io_context& ioc,
      std::function<bool(const Error&)> should_retry) && {
    return IO<T>([max_attempts, initial_delay, ioc_ptr = &ioc,
                  should_retry = std::move(should_retry),
                  self_ptr = std::make_shared<IO<T>>(std::move(*this))](
                     auto cb) mutable {
      auto attempt = std::make_shared<int>(0);
      auto try_run =
          std::make_shared<std::function<void(std::chrono::milliseconds)>>();

      *try_run = [=](std::chrono::milliseconds current_delay) mutable {
        (*attempt)++;
        self_ptr->clone().run([=](auto r) mutable {
          if (r.is_ok() || *attempt >= max_attempts || !r.is_err() ||
              !should_retry(r.error())) {
            cb(std::move(r));
          } else {
            delay_for(*ioc_ptr, current_delay).run([=](auto) mutable {
              (*try_run)(current_delay * 2);
            });
          }
        });
      };

      (*try_run)(initial_delay);
    });
  }

  /**
   * Exponential backoff retry that retries on any error.
   * - Equivalent to retry_exponential_if(..., [](const Error&){ return true; })
   */
  IO<T> retry_exponential(int max_attempts,
                          std::chrono::milliseconds initial_delay,
                          boost::asio::io_context& ioc) && {
    return std::move(*this).retry_exponential_if(
        max_attempts, initial_delay, ioc, [](const Error&) { return true; });
  }

  // // Exponential backoff retry (IO<T>)
  // IO<T> retry_exponential(int max_attempts,
  //                         std::chrono::milliseconds initial_delay,
  //                         boost::asio::io_context& ioc) && {
  //   return IO<T>([max_attempts, initial_delay, ioc_ptr = &ioc,
  //                 self_ptr = std::make_shared<IO<T>>(std::move(*this))](
  //                    auto cb) mutable {
  //     auto attempt = std::make_shared<int>(0);
  //     auto try_run =
  //         std::make_shared<std::function<void(std::chrono::milliseconds)>>();

  //     *try_run = [=](std::chrono::milliseconds current_delay) mutable {
  //       (*attempt)++;
  //       self_ptr->clone().run([=](auto r) mutable {
  //         if (r.is_ok() || *attempt >= max_attempts) {
  //           cb(std::move(r));
  //         } else {
  //           delay_for(*ioc_ptr, current_delay).run([=](auto) mutable {
  //             (*try_run)(current_delay * 2);
  //           });
  //         }
  //       });
  //     };

  //     (*try_run)(initial_delay);
  //   });
  // }

  /**
   * Execute the IO by providing a callback that receives Result<T, Error>.
   * - This function triggers the side effects encapsulated by the IO.
   */
  void run(Callback cb) const { thunk_(std::move(cb)); }

 private:
  std::function<void(Callback)> thunk_;
};

template <>
class IO<void> {
 public:
  using IOResult = Result<std::monostate, Error>;
  using Callback = std::function<void(IOResult)>;

  explicit IO(std::function<void(Callback)> thunk) : thunk_(std::move(thunk)) {}

  /**
   * Lift success (void) into IO<void>.
   *
   * Error semantics:
   * - Always succeeds with no value.
   */
  static IO<void> pure() {
    return IO([](Callback cb) { cb(IOResult{std::monostate{}}); });
  }

  /**
   * Produce a failed IO<void> with the given Error.
   *
   * Error semantics:
   * - Always fails with the provided Error.
   */
  static IO<void> fail(Error error) {
    return IO([error = std::move(error)](Callback cb) { cb(IOResult{error}); });
  }

  /**
   * Lift a MyVoidResult into IO<void>.
   * - Success maps to std::monostate success; error is propagated.
   */
  static IO<void> from_result(Result<void, Error> res) {
    return IO([res = std::make_shared<Result<void, Error>>(std::move(res))](
                  Callback cb) mutable {
      if (res->is_ok()) {
        cb(std::monostate{});
      } else {
        cb(res->error());
      }
    });
  }

  /**
   * Shallow copy the IO thunk. Useful for retry/backoff.
   * - No execution occurs.
   */
  IO<void> clone() const {
    static_assert(std::is_copy_constructible_v<decltype(*this)>,
                  "Cannot clone IO<T>: thunk is not copyable");
    return IO<void>(thunk_);
  }

  template <typename F>
  /**
   * Map over a successful IO<void>.
   *
   * Callable requirements:
   * - f: void() -> void (runs for side effects only; return is ignored)
   *
   * What NOT to return:
   * - Do not return a value, Error, or Result; the callable must return void.
   *   To produce another IO, use then(); to handle errors, use
   * catch_then/map_err.
   *
   * Error semantics:
   * - If this IO is an error, f is not called and the error is propagated.
   * - If f throws, the exception is caught and converted to Error{-1, what()}.
   */
  auto map(F&& f) const -> IO<void> {
    return IO<void>([prev_ptr = std::make_shared<IO<void>>(*this),
                     f = std::forward<F>(f)](Callback cb) {
      prev_ptr->run(
          [cb = std::move(cb), f = std::move(f)](IOResult result) mutable {
            if (result.is_ok()) {
              try {
                f();
                cb(std::monostate{});
              } catch (const std::exception& e) {
                cb(Error{-1, e.what()});
              }
            } else {
              cb(std::move(result.error()));
            }
          });
    });
  }

  template <typename F>
  /**
   * Flat-map for IO<void>: call f() on success and flatten its IO.
   *
   * Callable requirements:
   * - f: () -> IO<U>
   *
   * What NOT to return:
   * - Do not return a plain value U or Result. Use map() for side effects only,
   *   or wrap a value with IO<U>::pure(U). Returning Result is not supported
   * here.
   *
   * Error semantics:
   * - On error, propagate the error; f is not called.
   * - If f throws, the exception is caught and converted to Error{-2, what()}.
   */
  auto then(F&& f) const -> decltype(f()) {
    using NextIO = decltype(f());
    auto f_wrapped = std::make_shared<std::decay_t<F>>(std::forward<F>(f));
    auto prev_ptr = std::make_shared<IO<void>>(*this);
    return NextIO([prev_ptr, f_wrapped](typename NextIO::Callback cb) mutable {
      prev_ptr->run([f_wrapped, cb = std::move(cb)](IOResult result) mutable {
        if (result.is_ok()) {
          try {
            (*f_wrapped)().run(std::move(cb));
          } catch (const std::exception& e) {
            cb(Error{-2, e.what()});
          }
        } else {
          cb(result.error());
        }
      });
    });
  }

  template <typename F>
  /**
   * Handle errors by running a recovery that returns IO<void>.
   *
   * Callable requirements:
   * - f: Error -> IO<void>
   *
   * What NOT to return:
   * - Do not return plain void or Result. You must return IO<void> to recover.
   *   To only transform the Error, use map_err().
   *
   * Error semantics:
   * - Runs only when this IO is an error; otherwise passes through success.
   * - If f throws, the exception is caught and converted to Error{-3, what()}.
   */
  IO<void> catch_then(F&& f) const {
    auto prev_ptr = std::make_shared<IO<void>>(*this);
    auto f_ptr = std::make_shared<std::decay_t<F>>(std::forward<F>(f));

    return IO<void>([prev_ptr, f_ptr](Callback cb) mutable {
      prev_ptr->run([f_ptr, cb = std::move(cb)](IOResult result) mutable {
        if (result.is_err()) {
          try {
            (*f_ptr)(result.error()).run(std::move(cb));
          } catch (const std::exception& e) {
            cb(Error{-3, e.what()});
          }
        } else {
          cb(std::move(result));
        }
      });
    });
  }

  /**
   * Transform the error if present; pass through success unchanged.
   *
   * Callable requirements:
   * - f: Error -> Error (pure mapping)
   *
   * What NOT to return:
   * - Do not return IO<...> or throw to recover. To recover asynchronously, use
   * catch_then().
   */
  IO<void> map_err(std::function<Error(Error)> f) const {
    return IO<void>([prev = *this, f = std::move(f)](Callback cb) {
      prev.run([f, cb = std::move(cb)](IOResult result) mutable {
        if (result.is_err()) {
          cb(f(result.error()));
        } else {
          cb(std::move(result));
        }
      });
    });
  }

  /**
   * Always run a side-effecting finalizer after completion (success or error).
   *
   * Callable requirements:
   * - f: void() -> void
   *
   * What NOT to return:
   * - N/A (return is ignored). If cleanup must perform IO or can fail, prefer
   * finally_then().
   *
   * Error semantics:
   * - Finalizer exceptions are not caught here; ensure f() is noexcept if
   * needed.
   */
  IO<void> finally(std::function<void()> f) const {
    return IO<void>([prev = *this, f = std::move(f)](Callback cb) {
      prev.run([f, cb = std::move(cb)](IOResult result) mutable {
        f();
        cb(std::move(result));
      });
    });
  }

  template <typename F>
  /**
   * Chain a monadic finalizer regardless of success or error.
   *
   * Callable requirements:
   * - f: void() -> IO<void>
   *
   * What NOT to return:
   * - Do not return a value, Error, or Result; the cleanup must be an IO<void>.
   *   The cleanup's outcome is ignored; the original result passes through.
   *
   * Error semantics:
   * - Cleanup IO is run and ignored; original result is returned unchanged.
   * - If f throws, the original result is still returned.
   */
  IO<void> finally_then(F&& f) const {
    return IO<void>([prev = *this,
                     f = std::forward<F>(f)](Callback cb) mutable {
      prev.run([f = std::move(f), cb = std::move(cb)](IOResult result) mutable {
        try {
          auto cleanup_io = f();
          cleanup_io.run(
              [result = std::move(result), cb = std::move(cb)](auto) mutable {
                // Ignore cleanup result, return original result
                cb(std::move(result));
              });
        } catch (const std::exception& e) {
          // If cleanup throws, still return original result
          cb(std::move(result));
        }
      });
    });
  }

  /**
   * Delay emitting completion by duration (rvalue-qualified).
   */
  IO<void> delay(boost::asio::io_context& ioc,
                 std::chrono::milliseconds duration) && {
    return delay_for(ioc, duration).then([self = std::move(*this)](auto) {
      return std::move(self);
    });
  }

  /**
   * Fail with timeout if not completed within duration (rvalue-qualified).
   * - On timeout: Error{2, "Operation timed out"}.
   * - On success: timer is canceled and the original result is returned.
   */
  IO<void> timeout(boost::asio::io_context& ioc,
                   std::chrono::milliseconds duration) && {
    return IO<void>(
        [ioc_ptr = &ioc, duration, self = std::move(*this)](auto cb) mutable {
          auto timer = std::make_shared<boost::asio::steady_timer>(*ioc_ptr);
          auto fired = std::make_shared<bool>(false);

          timer->expires_after(duration);
          timer->async_wait([cb, fired](const boost::system::error_code& ec) {
            if (*fired) return;
            *fired = true;
            if (!ec) {
              cb(monad::Error{2, "Operation timed out"});
            }
          });

          self.run([cb, timer, fired](auto r) mutable {
            if (*fired) return;
            *fired = true;
            detail::cancel_timer(*timer);
            cb(std::move(r));
          });
        });
  }

  /**
   * Conditional exponential backoff retry for IO<void>.
   * - Behavior matches IO<T>::retry_exponential_if.
   */
  IO<void> retry_exponential_if(
      int max_attempts, std::chrono::milliseconds initial_delay,
      boost::asio::io_context& ioc,
      std::function<bool(const Error&)> should_retry) && {
    return IO<void>([max_attempts, initial_delay, ioc_ptr = &ioc,
                     should_retry = std::move(should_retry),
                     self_ptr = std::make_shared<IO<void>>(std::move(*this))](
                        auto cb) mutable {
      auto attempt = std::make_shared<int>(0);
      auto try_run =
          std::make_shared<std::function<void(std::chrono::milliseconds)>>();

      *try_run = [=](std::chrono::milliseconds current_delay) mutable {
        (*attempt)++;
        self_ptr->clone().run([=](auto r) mutable {
          if (r.is_ok() || *attempt >= max_attempts ||
              !should_retry(r.error())) {
            cb(std::move(r));
          } else {
            delay_for(*ioc_ptr, current_delay).run([=](auto) mutable {
              (*try_run)(current_delay * 2);
            });
          }
        });
      };

      (*try_run)(initial_delay);
    });
  }

  /**
   * Exponential backoff retry that retries on any error.
   */
  IO<void> retry_exponential(int max_attempts,
                             std::chrono::milliseconds initial_delay,
                             boost::asio::io_context& ioc) && {
    return std::move(*this).retry_exponential_if(
        max_attempts, initial_delay, ioc, [](const Error&) { return true; });
  }

  // // Exponential backoff retry (IO<void>)
  // IO<void> retry_exponential(int max_attempts,
  //                            std::chrono::milliseconds initial_delay,
  //                            boost::asio::io_context& ioc) && {
  //   return IO<void>([max_attempts, initial_delay, ioc_ptr = &ioc,
  //                    self_ptr =
  //                    std::make_shared<IO<void>>(std::move(*this))](
  //                       auto cb) mutable {
  //     auto attempt = std::make_shared<int>(0);
  //     auto try_run =
  //         std::make_shared<std::function<void(std::chrono::milliseconds)>>();

  //     *try_run = [=](std::chrono::milliseconds current_delay) mutable {
  //       (*attempt)++;
  //       self_ptr->clone().run([=](auto r) mutable {
  //         if (r.is_ok() || *attempt >= max_attempts) {
  //           cb(std::move(r));
  //         } else {
  //           delay_for(*ioc_ptr, current_delay).run([=](auto) mutable {
  //             (*try_run)(current_delay * 2);
  //           });
  //         }
  //       });
  //     };

  //     (*try_run)(initial_delay);
  //   });
  // }

  /**
   * Execute the IO by providing a callback that receives Result<void, Error>.
   * - This function triggers the side effects encapsulated by the IO.
   */
  void run(Callback cb) const { thunk_(std::move(cb)); }

 private:
  std::function<void(Callback)> thunk_;
};

/**
 * Sequentially chain IO work over a container.
 *
 * Callable requirements:
 * - f: (size_t index, const T& value) -> IO<void>
 *
 * What NOT to return:
 * - Do not return plain void or Result. The function must return IO<void>.
 *
 * Error semantics:
 * - Short-circuits on the first error and returns it.
 */
template <typename T, typename F>
IO<void> chain_io(const std::vector<T>& vec, F&& f) {
  IO<void> acc = IO<void>::pure();
  for (size_t i = 0; i < vec.size(); ++i) {
    acc = acc.then([&, i]() { return f(i, vec[i]); });
  }
  return acc;
}

// Common IO type aliases for convenience
using VoidIO = IO<void>;
using StringIO = IO<std::string>;
using IntIO = IO<int>;
using Int32IO = IO<std::int32_t>;
using Int64IO = IO<std::int64_t>;
using UInt32IO = IO<std::uint32_t>;
using UInt64IO = IO<std::uint64_t>;
using BoolIO = IO<bool>;
using DoubleIO = IO<double>;
using FloatIO = IO<float>;
using SizeTIO = IO<std::size_t>;

// JSON and container types
using JsonIO = IO<boost::json::value>;
using JsonObjectIO = IO<boost::json::object>;
using JsonArrayIO = IO<boost::json::array>;
using StringVectorIO = IO<std::vector<std::string>>;
using IntVectorIO = IO<std::vector<int>>;
using BytesIO = IO<std::vector<std::uint8_t>>;

// Filesystem and path types
using PathIO = IO<std::filesystem::path>;
using FileStreamIO = IO<std::ifstream>;

// Optional and variant types
using OptionalStringIO = IO<std::optional<std::string>>;
using OptionalIntIO = IO<std::optional<int>>;

}  // namespace monad
