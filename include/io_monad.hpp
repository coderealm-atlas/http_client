// io_monad.hpp (v2 as default)
#pragma once

#include <boost/asio.hpp>
#include <boost/json.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "result_monad.hpp"  // monad::Error, monad::Result

namespace monad {

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

// Forward declarations for helpers
template <typename T>
IO<T> delay_for(boost::asio::io_context& ioc,
                std::chrono::milliseconds duration);
template <typename T>
IO<std::decay_t<T>> delay_then(boost::asio::io_context& ioc,
                               std::chrono::milliseconds duration, T&& val);

// IO<T>
template <typename T>
class IO {
 public:
  using IOResult = Result<T, Error>;
  using Callback = std::function<void(IOResult)>;

  explicit IO(std::function<void(Callback)> thunk) : thunk_(std::move(thunk)) {}

  static IO<T> pure(T value) {
    return IO([val = std::make_shared<T>(std::move(value))](
                  Callback cb) mutable { cb(IOResult::Ok(std::move(*val))); });
  }

  static IO<T> fail(Error error) {
    return IO([error = std::move(error)](Callback cb) mutable {
      cb(IOResult::Err(std::move(error)));
    });
  }

  static IO<T> from_result(Result<T, Error> res) {
    return IO([res = std::make_shared<Result<T, Error>>(std::move(res))](
                  Callback cb) mutable { cb(std::move(*res)); });
  }

  IO<T> clone() const { return IO<T>(thunk_); }

  template <typename F>
  auto map(F&& f) const -> IO<decltype(std::declval<F>()(std::declval<T>()))> {
    using RetT = decltype(std::declval<F>()(std::declval<T>()));
    using NextIO = IO<RetT>;
    return NextIO([prev = *this, fn = std::forward<F>(f)](
                      typename NextIO::Callback cb) mutable {
      prev.run([fn = std::move(fn), cb = std::move(cb)](IOResult r) mutable {
        if (r.is_ok()) {
          try {
            if constexpr (std::is_void_v<RetT>) {
              std::invoke(fn, std::move(r).value());
              cb(Result<void, Error>::Ok());
            } else {
              cb(Result<RetT, Error>::Ok(
                  std::invoke(fn, std::move(r).value())));
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

  template <typename F>
  auto then(F&& f) const -> typename std::enable_if<!std::is_void_v<T>, 
      std::invoke_result_t<F, T>>::type {
    using NextIO = std::invoke_result_t<F, T>;
    static_assert(is_io_v<NextIO>, "then() must return IO<U>");
    return NextIO([prev = *this, fn = std::forward<F>(f)](
                      typename NextIO::Callback cb) mutable {
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

  template <typename F>
  IO<T> finally(F&& f) const {
    return IO<T>([prev = *this, fn = std::forward<F>(f)](Callback cb) mutable {
      prev.run([fn = std::move(fn), cb = std::move(cb)](IOResult r) mutable {
        std::invoke(fn);
        cb(std::move(r));
      });
    });
  }

  template <typename F>
  IO<T> finally_then(F&& f) const {
    return IO<T>([prev = *this, fn = std::forward<F>(f)](Callback cb) mutable {
      prev.run([fn = std::move(fn), cb = std::move(cb)](IOResult r) mutable {
        try {
          std::invoke(fn).run([res = std::move(r), cb = std::move(cb)](
                                  auto) mutable { cb(std::move(res)); });
        } catch (...) {
          cb(std::move(r));
        }
      });
    });
  }

  IO<T> timeout(boost::asio::io_context& ioc,
                std::chrono::milliseconds duration) && {
    return IO<T>(
        [ioc_ptr = &ioc, duration, self = std::move(*this)](auto cb) mutable {
          auto timer = std::make_shared<boost::asio::steady_timer>(*ioc_ptr);
          auto fired = std::make_shared<bool>(false);

          timer->expires_after(duration);
          timer->async_wait(
              [cb, fired](const boost::system::error_code& ec) mutable {
                if (*fired) return;
                *fired = true;
                if (!ec)
                  cb(Result<T, Error>::Err(Error{2, "Operation timed out"}));
              });

          self.run([cb, timer, fired](IOResult r) mutable {
            if (*fired) return;
            *fired = true;
            detail::cancel_timer(*timer);
            cb(std::move(r));
          });
        });
  }
  IO<T> timeout(boost::asio::io_context& ioc,
                std::chrono::milliseconds duration) & {
    return std::move(*this).timeout(ioc, duration);
  }

  // Executor-aware timeout
  IO<T> timeout(boost::asio::any_io_executor ex,
                std::chrono::milliseconds duration) && {
    return IO<T>([ex, duration, self = std::move(*this)](auto cb) mutable {
      auto timer = std::make_shared<boost::asio::steady_timer>(ex);
      auto fired = std::make_shared<bool>(false);

      timer->expires_after(duration);
      timer->async_wait(
          [cb, fired](const boost::system::error_code& ec) mutable {
            if (*fired) return;
            *fired = true;
            if (!ec)
              cb(Result<T, Error>::Err(Error{2, "Operation timed out"}));
          });

      self.run([cb, timer, fired](IOResult r) mutable {
        if (*fired) return;
        *fired = true;
        detail::cancel_timer(*timer);
        cb(std::move(r));
      });
    });
  }
  IO<T> timeout(boost::asio::any_io_executor ex,
                std::chrono::milliseconds duration) & {
    return std::move(*this).timeout(ex, duration);
  }

  IO<T> delay(boost::asio::io_context& ioc,
              std::chrono::milliseconds duration) && {
    return IO<T>([ioc_ptr = &ioc, duration,
                  self = std::move(*this)](auto cb) mutable {
      auto timer = std::make_shared<boost::asio::steady_timer>(*ioc_ptr);
      auto result = std::make_shared<std::optional<IOResult>>();
      auto delivered = std::make_shared<bool>(false);
      auto timer_fired = std::make_shared<bool>(false);

      timer->expires_after(duration);
      timer->async_wait([cb, result, delivered, timer_fired,
                         timer](const boost::system::error_code& ec) mutable {
        if (*delivered) return;
        if (ec) {
          *delivered = true;
          cb(Result<T, Error>::Err(
              Error{1, std::string{"Timer error: "} + ec.message()}));
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
  IO<T> delay(boost::asio::io_context& ioc,
              std::chrono::milliseconds duration) & {
    return std::move(*this).delay(ioc, duration);
  }

  // Executor-aware delay
  IO<T> delay(boost::asio::any_io_executor ex,
              std::chrono::milliseconds duration) && {
    return IO<T>([ex, duration, self = std::move(*this)](auto cb) mutable {
      auto timer = std::make_shared<boost::asio::steady_timer>(ex);
      auto result = std::make_shared<std::optional<IOResult>>();
      auto delivered = std::make_shared<bool>(false);
      auto timer_fired = std::make_shared<bool>(false);

      timer->expires_after(duration);
      timer->async_wait([cb, result, delivered, timer_fired,
                         timer](const boost::system::error_code& ec) mutable {
        if (*delivered) return;
        if (ec) {
          *delivered = true;
          cb(Result<T, Error>::Err(
              Error{1, std::string{"Timer error: "} + ec.message()}));
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
  IO<T> delay(boost::asio::any_io_executor ex,
              std::chrono::milliseconds duration) & {
    return std::move(*this).delay(ex, duration);
  }

  IO<T> retry_exponential_if(
      int max_attempts, std::chrono::milliseconds initial_delay,
      boost::asio::io_context& ioc,
      std::function<bool(const Error&)> should_retry) && {
    return IO<T>([max_attempts, initial_delay, ioc_ptr = &ioc,
                  should_retry = std::move(should_retry),
                  self_ptr = std::make_shared<IO<T>>(std::move(*this))](
                     auto cb) mutable {
      struct RetryState {
        std::shared_ptr<int> attempt;
        std::shared_ptr<std::function<void(std::chrono::milliseconds)>> try_run;
        std::shared_ptr<IO<T>> self_ptr;

        void cleanup() {
          if (try_run && *try_run) {
            // clear the stored function object to break reference cycles
            *try_run = {};
          }
          try_run.reset();
          self_ptr.reset();
        }
      };

      auto state = std::make_shared<RetryState>();
      state->attempt = std::make_shared<int>(0);
      state->try_run = std::make_shared<std::function<void(std::chrono::milliseconds)>>();
      state->self_ptr = self_ptr;

      std::weak_ptr<std::function<void(std::chrono::milliseconds)>> weak_try =
          state->try_run;

      // assign explicit capture lambda to avoid capturing `try_run` by value
      *state->try_run = [max_attempts, should_retry, state, weak_try, ioc_ptr, cb](std::chrono::milliseconds current_delay) mutable {
        (*state->attempt)++;
        state->self_ptr->clone().run(
            [max_attempts, state, should_retry, weak_try, cb, ioc_ptr,
             current_delay](IOResult r) mutable {
              if (r.is_ok() || *state->attempt >= max_attempts ||
                  !should_retry(r.error())) {
                // cleanup before delivering final result to break cycles
                state->cleanup();
                cb(std::move(r));
              } else {
                auto timer = std::make_shared<boost::asio::steady_timer>(*ioc_ptr);
                timer->expires_after(current_delay);
        timer->async_wait([
          weak_try, timer, cb, current_delay](const boost::system::error_code& ec) mutable {
                  if (ec) {
                    // nothing to cleanup here as final will be delivered
                    cb(Result<T, Error>::Err(
                        Error{1, std::string{"Timer error: "} + ec.message()}));
                  } else {
                    if (auto sp = weak_try.lock()) {
                      (*sp)(current_delay * 2);
                    }
                  }
                });
              }
            });
      };

      (*state->try_run)(initial_delay);
    });
  }

  IO<T> retry_exponential(int max_attempts,
                          std::chrono::milliseconds initial_delay,
                          boost::asio::io_context& ioc) && {
    return std::move(*this).retry_exponential_if(
        max_attempts, initial_delay, ioc, [](const Error&) { return true; });
  }

  // Poll until condition on value is satisfied or attempts exhausted.
  // - max_attempts: maximum number of executions (>=1)
  // - interval: delay between attempts when not satisfied or retrying on error
  // - ioc: io_context for timers
  // - satisfied: predicate to decide if we can stop successfully
  // - retry_on_error: if returns true, keep retrying on error; otherwise fail immediately
  IO<T> poll_if(int max_attempts, std::chrono::milliseconds interval,
                boost::asio::io_context& ioc,
                std::function<bool(const T&)> satisfied,
                std::function<bool(const Error&)> retry_on_error =
                    [](const Error&) { return true; }) && {
    return IO<T>([max_attempts, interval, ioc_ptr = &ioc,
                  satisfied = std::move(satisfied),
                  retry_on_error = std::move(retry_on_error),
                  self_ptr = std::make_shared<IO<T>>(std::move(*this))](
                     auto cb) mutable {
      struct PollState {
        std::shared_ptr<int> attempt;
        std::shared_ptr<std::function<void()>> do_attempt;
        std::shared_ptr<std::shared_ptr<std::function<void()>>> keep_alive_holder;
        std::shared_ptr<IO<T>> self_ptr;

        // cleanup intentionally left empty; actual cleanup is scheduled
        // via the enclosing lambda to avoid destroying std::function while
        // it may be executing (which would cause use-after-free).
        void cleanup() {}
      };

      auto state = std::make_shared<PollState>();
      state->attempt = std::make_shared<int>(0);
      state->do_attempt = std::make_shared<std::function<void()>>();
      state->keep_alive_holder = std::make_shared<std::shared_ptr<std::function<void()>>>(state->do_attempt);
      state->self_ptr = self_ptr;

      std::weak_ptr<std::function<void()>> weak_attempt = state->do_attempt;

      auto release_keep_alive = [state]() mutable {
        if (state->keep_alive_holder && *state->keep_alive_holder) {
          state->keep_alive_holder->reset();
        }
      };

      // Schedule real cleanup on the io_context to ensure we don't destroy
      // std::function objects while they're running.
      auto cleanup = [state, ioc_ptr]() mutable {
        if (!ioc_ptr) return;
        boost::asio::post(*ioc_ptr, [state]() mutable {
          if (state->keep_alive_holder && *state->keep_alive_holder) {
            *(*state->keep_alive_holder) = {};
            state->keep_alive_holder->reset();
          }
          state->do_attempt.reset();
          state->self_ptr.reset();
        });
      };

      auto handle_result = [state, max_attempts, cb, interval, ioc_ptr, satisfied, retry_on_error, weak_attempt, release_keep_alive, cleanup](IOResult r) mutable {
        auto enqueue_retry = [&]() {
          auto keep_alive_copy = *state->keep_alive_holder;
          auto timer = std::make_shared<boost::asio::steady_timer>(*ioc_ptr);
          timer->expires_after(interval);
          timer->async_wait([weak_attempt, state, keep_alive_copy,
                             timer, cb, release_keep_alive, cleanup](
                                const boost::system::error_code& ec) mutable {
            if (ec) {
              // final failure from timer - schedule cleanup then deliver
              cleanup();
              cb(Result<T, Error>::Err(
                  Error{1, std::string{"Timer error: "} + ec.message()}));
            } else {
              (void)keep_alive_copy;
              if (auto attempt_fn = weak_attempt.lock()) {
                (*attempt_fn)();
              }
            }
          });
        };

        if (r.is_ok()) {
          const T& v = r.value();
          if (satisfied(v)) {
            cleanup();
            cb(std::move(r));
          } else {
            enqueue_retry();
          }
        } else {
          if (!retry_on_error(r.error()) || *state->attempt >= max_attempts) {
            cleanup();
            cb(std::move(r));
          } else {
            enqueue_retry();
          }
        }
      };

      *state->do_attempt = [state, max_attempts, cb, handle_result, release_keep_alive, cleanup]() mutable {
        if (*state->attempt >= max_attempts) {
          cleanup();
          cb(Result<T, Error>::Err(Error{3, "Polling attempts exhausted"}));
          return;
        }
        (*state->attempt)++;
        state->self_ptr->clone().run(handle_result);
      };

      (*state->do_attempt)();
    });
  }
  IO<T> poll_if(int max_attempts, std::chrono::milliseconds interval,
                boost::asio::io_context& ioc,
                std::function<bool(const T&)> satisfied,
                std::function<bool(const Error&)> retry_on_error =
                    [](const Error&) { return true; }) & {
    return std::move(*this).poll_if(max_attempts, interval, ioc,
                                    std::move(satisfied),
                                    std::move(retry_on_error));
  }

  // Executor-aware poll_if for IO<T>
  IO<T> poll_if(int max_attempts, std::chrono::milliseconds interval,
                boost::asio::any_io_executor ex,
                std::function<bool(const T&)> satisfied,
                std::function<bool(const Error&)> retry_on_error =
                    [](const Error&) { return true; }) && {
    return IO<T>([max_attempts, interval, ex,
                  satisfied = std::move(satisfied),
                  retry_on_error = std::move(retry_on_error),
                  self_ptr = std::make_shared<IO<T>>(std::move(*this))](
                     auto cb) mutable {
      auto attempt = std::make_shared<int>(0);
      auto do_attempt = std::make_shared<std::function<void()>>();
      auto keep_alive_holder =
          std::make_shared<std::shared_ptr<std::function<void()>>>(do_attempt);
      std::weak_ptr<std::function<void()>> weak_attempt = do_attempt;

      auto release_keep_alive =
          [keep_alive_holder]() mutable {
            if (keep_alive_holder && *keep_alive_holder) {
              keep_alive_holder->reset();
            }
          };

      auto handle_result = [attempt, max_attempts, cb, self_ptr, interval, ex,
                            satisfied, retry_on_error, weak_attempt,
                            keep_alive_holder, release_keep_alive](IOResult r) mutable {
        auto enqueue_retry = [&]() {
          auto keep_alive_copy = *keep_alive_holder;
          auto timer = std::make_shared<boost::asio::steady_timer>(ex);
          timer->expires_after(interval);
          timer->async_wait([weak_attempt, keep_alive_holder, keep_alive_copy,
                             timer, cb, release_keep_alive](
                                const boost::system::error_code& ec) mutable {
            if (ec) {
              release_keep_alive();
              cb(Result<T, Error>::Err(
                  Error{1, std::string{"Timer error: "} + ec.message()}));
            } else {
              (void)keep_alive_copy;
              if (auto attempt_fn = weak_attempt.lock()) {
                (*attempt_fn)();
              }
            }
          });
        };

        if (r.is_ok()) {
          const T& v = r.value();
          if (satisfied(v)) {
            release_keep_alive();
            cb(std::move(r));
          } else {
            enqueue_retry();
          }
        } else {
          if (!retry_on_error(r.error()) || *attempt >= max_attempts) {
            release_keep_alive();
            cb(std::move(r));
          } else {
            enqueue_retry();
          }
        }
      };

      *do_attempt = [attempt, max_attempts, cb, self_ptr, handle_result,
                     release_keep_alive]() mutable {
        if (*attempt >= max_attempts) {
          release_keep_alive();
          cb(Result<T, Error>::Err(Error{3, "Polling attempts exhausted"}));
          return;
        }
        (*attempt)++;
        self_ptr->clone().run(handle_result);
      };

      (*do_attempt)();
    });
  }
  IO<T> poll_if(int max_attempts, std::chrono::milliseconds interval,
                boost::asio::any_io_executor ex,
                std::function<bool(const T&)> satisfied,
                std::function<bool(const Error&)> retry_on_error =
                    [](const Error&) { return true; }) & {
    return std::move(*this).poll_if(max_attempts, interval, ex,
                                    std::move(satisfied),
                                    std::move(retry_on_error));
  }

  void run(Callback cb) const { thunk_(std::move(cb)); }

 private:
  std::function<void(Callback)> thunk_;
};

// IO<void>
template <>
class IO<void> {
 public:
  using IOResult = Result<void, Error>;
  using Callback = std::function<void(IOResult)>;

  explicit IO(std::function<void(Callback)> thunk) : thunk_(std::move(thunk)) {}

  static IO<void> pure() {
    return IO([](Callback cb) { cb(IOResult::Ok()); });
  }

  static IO<void> fail(Error error) {
    return IO([error = std::move(error)](Callback cb) mutable {
      cb(IOResult::Err(std::move(error)));
    });
  }

  static IO<void> from_result(Result<void, Error> res) {
    return IO([res = std::make_shared<Result<void, Error>>(std::move(res))](
                  Callback cb) mutable {
      if (res->is_ok()) {
        cb(IOResult::Ok());
      } else {
        cb(IOResult::Err(std::move(res->error())));
      }
    });
  }

  IO<void> clone() const { return IO<void>(thunk_); }

  template <typename F>
  auto map(F&& f) const -> IO<void> {
    return IO<void>([prev = *this,
                     fn = std::forward<F>(f)](Callback cb) mutable {
      prev.run([fn = std::move(fn), cb = std::move(cb)](IOResult r) mutable {
        if (r.is_ok()) {
          try {
            std::invoke(fn);
            cb(IOResult::Ok());
          } catch (const std::exception& e) {
            cb(IOResult::Err(Error{-1, e.what()}));
          }
        } else {
          cb(std::move(r));
        }
      });
    });
  }

  template <typename F>
  auto map_to(F&& f) const -> IO<std::invoke_result_t<F>> {
    using U = std::invoke_result_t<F>;
    static_assert(!std::is_void_v<U>, "Use map() for void-returning functions");
    static_assert(!is_io_v<U>, "Return IO via then(), not map_to()");
    return IO<U>([prev = *this, fn = std::forward<F>(f)](
                     typename IO<U>::Callback cb) mutable {
      prev.run([fn = std::move(fn), cb = std::move(cb)](IOResult r) mutable {
        if (r.is_ok()) {
          try {
            U value = std::invoke(fn);
            cb(Result<U, Error>::Ok(std::move(value)));
          } catch (const std::exception& e) {
            cb(Result<U, Error>::Err(Error{-1, e.what()}));
          }
        } else {
          cb(Result<U, Error>::Err(r.error()));
        }
      });
    });
  }

  template <typename F>
  auto then(F&& f) const -> decltype(f()) {
    using NextIO = decltype(f());
    static_assert(is_io_v<NextIO>, "then() must return IO<U>");
    return NextIO([prev = *this, fn = std::forward<F>(f)](
                      typename NextIO::Callback cb) mutable {
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

  template <typename F>
  IO<void> catch_then(F&& f) const {
    return IO<void>([prev = *this,
                     fn = std::forward<F>(f)](Callback cb) mutable {
      prev.run([fn = std::move(fn), cb = std::move(cb)](IOResult r) mutable {
        if (r.is_err()) {
          try {
            std::invoke(fn, r.error()).run(std::move(cb));
          } catch (const std::exception& e) {
            cb(Result<void, Error>::Err(Error{-3, e.what()}));
          }
        } else {
          cb(std::move(r));
        }
      });
    });
  }

  template <typename F>
  IO<void> map_err(F&& f) const {
    return IO<void>([prev = *this,
                     fn = std::forward<F>(f)](Callback cb) mutable {
      prev.run([fn = std::move(fn), cb = std::move(cb)](IOResult r) mutable {
        if (r.is_err()) {
          cb(Result<void, Error>::Err(std::invoke(fn, r.error())));
        } else {
          cb(std::move(r));
        }
      });
    });
  }

  template <typename F>
  IO<void> finally(F&& f) const {
    return IO<void>([prev = *this,
                     fn = std::forward<F>(f)](Callback cb) mutable {
      prev.run([fn = std::move(fn), cb = std::move(cb)](IOResult r) mutable {
        std::invoke(fn);
        cb(std::move(r));
      });
    });
  }

  template <typename F>
  IO<void> finally_then(F&& f) const {
    return IO<void>([prev = *this,
                     fn = std::forward<F>(f)](Callback cb) mutable {
      prev.run([fn = std::move(fn), cb = std::move(cb)](IOResult r) mutable {
        try {
          std::invoke(fn).run([res = std::move(r), cb = std::move(cb)](
                                  auto) mutable { cb(std::move(res)); });
        } catch (...) {
          cb(std::move(r));
        }
      });
    });
  }

  IO<void> timeout(boost::asio::io_context& ioc,
                   std::chrono::milliseconds duration) && {
    return IO<void>(
        [ioc_ptr = &ioc, duration, self = std::move(*this)](auto cb) mutable {
          auto timer = std::make_shared<boost::asio::steady_timer>(*ioc_ptr);
          auto fired = std::make_shared<bool>(false);

          timer->expires_after(duration);
          timer->async_wait(
              [cb, fired](const boost::system::error_code& ec) mutable {
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

  // Executor-aware timeout
  IO<void> timeout(boost::asio::any_io_executor ex,
                   std::chrono::milliseconds duration) && {
    return IO<void>([ex, duration, self = std::move(*this)](auto cb) mutable {
      auto timer = std::make_shared<boost::asio::steady_timer>(ex);
      auto fired = std::make_shared<bool>(false);

      timer->expires_after(duration);
      timer->async_wait(
          [cb, fired](const boost::system::error_code& ec) mutable {
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

  IO<void> timeout(boost::asio::any_io_executor ex,
                   std::chrono::milliseconds duration) & {
    return std::move(*this).timeout(ex, duration);
  }

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
      std::weak_ptr<std::function<void(std::chrono::milliseconds)>> weak_try =
          try_run;

      // cleanup helper to break cycles held by try_run and self_ptr
      auto cleanup = [try_run, self_ptr]() mutable {
        if (try_run && *try_run) *try_run = {};
        try_run.reset();
        self_ptr.reset();
      };

      *try_run = [max_attempts, attempt, ioc_ptr, should_retry, self_ptr,
                  weak_try, cb, cleanup](std::chrono::milliseconds current_delay) mutable {
        (*attempt)++;
        self_ptr->clone().run([
            max_attempts, attempt, should_retry, weak_try, cb, ioc_ptr,
            current_delay, cleanup](IOResult r) mutable {
          if (r.is_ok() || *attempt >= max_attempts ||
              !should_retry(r.error())) {
            // cleanup before delivering final result
            cleanup();
            cb(std::move(r));
          } else {
            auto timer = std::make_shared<boost::asio::steady_timer>(*ioc_ptr);
            timer->expires_after(current_delay);
            timer->async_wait([
                weak_try, timer, cb, current_delay, cleanup](const boost::system::error_code& ec) mutable {
              if (ec) {
                // cleanup then deliver final failure
                cleanup();
                cb(Result<void, Error>::Err(
                    Error{1, std::string{"Timer error: "} + ec.message()}));
              } else {
                if (auto sp = weak_try.lock()) {
                  (*sp)(current_delay * 2);
                }
              }
            });
          }
        });
      };

      (*try_run)(initial_delay);
    });
  }

  IO<void> retry_exponential(int max_attempts,
                             std::chrono::milliseconds initial_delay,
                             boost::asio::io_context& ioc) && {
    return std::move(*this).retry_exponential_if(
        max_attempts, initial_delay, ioc, [](const Error&) { return true; });
  }

  // Poll IO<void> until external condition is satisfied. After each run, call
  // satisfied(); if false, wait interval and retry. Errors are retried if
  // retry_on_error returns true and attempts remain.
  IO<void> poll_if(int max_attempts, std::chrono::milliseconds interval,
                   boost::asio::io_context& ioc,
                   std::function<bool()> satisfied,
                   std::function<bool(const Error&)> retry_on_error =
                       [](const Error&) { return true; }) && {
    return IO<void>([max_attempts, interval, ioc_ptr = &ioc,
                     satisfied = std::move(satisfied),
                     retry_on_error = std::move(retry_on_error),
                     self_ptr = std::make_shared<IO<void>>(std::move(*this))](
                        auto cb) mutable {
      auto attempt = std::make_shared<int>(0);
      auto do_attempt = std::make_shared<std::function<void()>>();
      auto keep_alive_holder =
          std::make_shared<std::shared_ptr<std::function<void()>>>(do_attempt);
      std::weak_ptr<std::function<void()>> weak_attempt = do_attempt;

      auto release_keep_alive =
          [keep_alive_holder]() mutable {
            if (keep_alive_holder && *keep_alive_holder) {
              keep_alive_holder->reset();
            }
          };

      auto handle_result = [attempt, max_attempts, cb, self_ptr, interval,
                            ioc_ptr, satisfied, retry_on_error, weak_attempt,
                            keep_alive_holder, release_keep_alive](IOResult r) mutable {
        auto enqueue_retry = [&]() {
          auto keep_alive_copy = *keep_alive_holder;
          auto timer = std::make_shared<boost::asio::steady_timer>(*ioc_ptr);
          timer->expires_after(interval);
          timer->async_wait([weak_attempt, keep_alive_holder, keep_alive_copy,
                             timer, cb, release_keep_alive](
                                const boost::system::error_code& ec) mutable {
            if (ec) {
              release_keep_alive();
              cb(Result<void, Error>::Err(
                  Error{1, std::string{"Timer error: "} + ec.message()}));
            } else {
              (void)keep_alive_copy;
              if (auto attempt_fn = weak_attempt.lock()) {
                (*attempt_fn)();
              }
            }
          });
        };

        if (r.is_ok()) {
          if (satisfied()) {
            release_keep_alive();
            cb(Result<void, Error>::Ok());
          } else {
            enqueue_retry();
          }
        } else {
          if (!retry_on_error(r.error()) || *attempt >= max_attempts) {
            release_keep_alive();
            cb(std::move(r));
          } else {
            enqueue_retry();
          }
        }
      };

      *do_attempt = [attempt, max_attempts, cb, self_ptr, handle_result,
                     release_keep_alive]() mutable {
        if (*attempt >= max_attempts) {
          release_keep_alive();
          cb(Result<void, Error>::Err(Error{3, "Polling attempts exhausted"}));
          return;
        }
        (*attempt)++;
        self_ptr->clone().run(handle_result);
      };

      (*do_attempt)();
    });
  }

  // Executor-aware poll_if for IO<void>
  IO<void> poll_if(int max_attempts, std::chrono::milliseconds interval,
                   boost::asio::any_io_executor ex,
                   std::function<bool()> satisfied,
                   std::function<bool(const Error&)> retry_on_error =
                       [](const Error&) { return true; }) && {
    return IO<void>([max_attempts, interval, ex,
                     satisfied = std::move(satisfied),
                     retry_on_error = std::move(retry_on_error),
                     self_ptr = std::make_shared<IO<void>>(std::move(*this))](
                        auto cb) mutable {
      auto attempt = std::make_shared<int>(0);
      auto do_attempt = std::make_shared<std::function<void()>>();
      auto keep_alive_holder =
          std::make_shared<std::shared_ptr<std::function<void()>>>(do_attempt);
      std::weak_ptr<std::function<void()>> weak_attempt = do_attempt;

      auto release_keep_alive =
          [keep_alive_holder]() mutable {
            if (keep_alive_holder && *keep_alive_holder) {
              keep_alive_holder->reset();
            }
          };

      auto handle_result = [attempt, max_attempts, cb, self_ptr, interval, ex,
                            satisfied, retry_on_error, weak_attempt,
                            keep_alive_holder, release_keep_alive](IOResult r) mutable {
        auto enqueue_retry = [&]() {
          auto keep_alive_copy = *keep_alive_holder;
          auto timer = std::make_shared<boost::asio::steady_timer>(ex);
          timer->expires_after(interval);
          timer->async_wait([weak_attempt, keep_alive_holder, keep_alive_copy,
                             timer, cb, release_keep_alive](
                                const boost::system::error_code& ec) mutable {
            if (ec) {
              release_keep_alive();
              cb(Result<void, Error>::Err(
                  Error{1, std::string{"Timer error: "} + ec.message()}));
            } else {
              (void)keep_alive_copy;
              if (auto attempt_fn = weak_attempt.lock()) {
                (*attempt_fn)();
              }
            }
          });
        };

        if (r.is_ok()) {
          if (satisfied()) {
            release_keep_alive();
            cb(Result<void, Error>::Ok());
          } else {
            enqueue_retry();
          }
        } else {
          if (!retry_on_error(r.error()) || *attempt >= max_attempts) {
            release_keep_alive();
            cb(std::move(r));
          } else {
            enqueue_retry();
          }
        }
      };

      *do_attempt = [attempt, max_attempts, cb, self_ptr, handle_result,
                     release_keep_alive]() mutable {
        if (*attempt >= max_attempts) {
          release_keep_alive();
          cb(Result<void, Error>::Err(Error{3, "Polling attempts exhausted"}));
          return;
        }
        (*attempt)++;
        self_ptr->clone().run(handle_result);
      };

      (*do_attempt)();
    });
  }

  IO<void> poll_if(int max_attempts, std::chrono::milliseconds interval,
                   boost::asio::any_io_executor ex,
                   std::function<bool()> satisfied,
                   std::function<bool(const Error&)> retry_on_error =
                       [](const Error&) { return true; }) & {
    return std::move(*this).poll_if(max_attempts, interval, ex,
                                    std::move(satisfied),
                                    std::move(retry_on_error));
  }

  IO<void> delay(boost::asio::io_context& ioc,
                 std::chrono::milliseconds duration) && {
    return IO<void>([ioc_ptr = &ioc, duration,
                     self = std::move(*this)](auto cb) mutable {
      auto timer = std::make_shared<boost::asio::steady_timer>(*ioc_ptr);
      auto result = std::make_shared<std::optional<IOResult>>();
      auto delivered = std::make_shared<bool>(false);
      auto timer_fired = std::make_shared<bool>(false);

      timer->expires_after(duration);
      timer->async_wait([cb, result, delivered, timer_fired,
                         timer](const boost::system::error_code& ec) mutable {
        if (*delivered) return;
        if (ec) {
          *delivered = true;
          cb(Result<void, Error>::Err(
              Error{1, std::string{"Timer error: "} + ec.message()}));
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

  // Executor-aware delay
  IO<void> delay(boost::asio::any_io_executor ex,
                 std::chrono::milliseconds duration) && {
    return IO<void>([ex, duration, self = std::move(*this)](auto cb) mutable {
      auto timer = std::make_shared<boost::asio::steady_timer>(ex);
      auto result = std::make_shared<std::optional<IOResult>>();
      auto delivered = std::make_shared<bool>(false);
      auto timer_fired = std::make_shared<bool>(false);

      timer->expires_after(duration);
      timer->async_wait([cb, result, delivered, timer_fired,
                         timer](const boost::system::error_code& ec) mutable {
        if (*delivered) return;
        if (ec) {
          *delivered = true;
          cb(Result<void, Error>::Err(
              Error{1, std::string{"Timer error: "} + ec.message()}));
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

  IO<void> delay(boost::asio::any_io_executor ex,
                 std::chrono::milliseconds duration) & {
    return std::move(*this).delay(ex, duration);
  }

  void run(Callback cb) const { thunk_(std::move(cb)); }

 private:
  std::function<void(Callback)> thunk_;
};

// Free helpers to combine IOs

inline IO<std::tuple<>> zip_io() { return IO<std::tuple<>>::pure({}); }

template <typename T1, typename... Ts>
inline IO<std::tuple<T1, Ts...>> zip_io(IO<T1> first, IO<Ts>... rest) {
  if constexpr (sizeof...(Ts) == 0) {
    return std::move(first).map(
        [](T1 v1) { return std::make_tuple(std::move(v1)); });
  } else {
    return std::move(first).then([rest...](T1 v1) mutable {
      return zip_io(std::move(rest)...).map([v1 = std::move(v1)](auto tail) {
        return std::tuple_cat(std::make_tuple(std::move(v1)), std::move(tail));
      });
    });
  }
}

template <typename... Ts>
struct io_filter_void_types;
template <>
struct io_filter_void_types<> {
  using type = std::tuple<>;
};
template <typename T, typename... Ts>
struct io_filter_void_types<T, Ts...> {
  using tail = typename io_filter_void_types<Ts...>::type;
  using type =
      std::conditional_t<std::is_same_v<T, void>, tail,
                         decltype(std::tuple_cat(std::declval<std::tuple<T>>(),
                                                 std::declval<tail>()))>;
};
template <typename... Ts>
using io_filter_void_types_t = typename io_filter_void_types<Ts...>::type;

inline IO<std::tuple<>> zip_io_skip_void() {
  return IO<std::tuple<>>::pure({});
}

template <typename T1, typename... Ts>
inline IO<io_filter_void_types_t<T1, Ts...>> zip_io_skip_void(IO<T1> first,
                                                              IO<Ts>... rest) {
  if constexpr (sizeof...(Ts) == 0) {
    if constexpr (std::is_same_v<T1, void>) {
      return std::move(first).map([] { return std::tuple<>(); });
    } else {
      return std::move(first).map(
          [](T1 v1) { return std::make_tuple(std::move(v1)); });
    }
  } else {
    if constexpr (std::is_same_v<T1, void>) {
      return std::move(first).then(
          [rest...]() mutable { return zip_io_skip_void(std::move(rest)...); });
    } else {
      return std::move(first).then([rest...](T1 v1) mutable {
        return zip_io_skip_void(std::move(rest)...)
            .map([v1 = std::move(v1)](auto tail) {
              return std::tuple_cat(std::make_tuple(std::move(v1)),
                                    std::move(tail));
            });
      });
    }
  }
}

template <typename T>
inline IO<std::vector<T>> collect_io(std::vector<IO<T>> items) {
  return IO<std::vector<T>>([items = std::make_shared<std::vector<IO<T>>>(
                                 std::move(items))](auto cb) mutable {
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

template <typename T>
inline IO<std::vector<T>> collect_io(std::initializer_list<IO<T>> items) {
  return collect_io(std::vector<IO<T>>(items));
}

inline IO<void> all_ok_io(std::vector<IO<void>> items) {
  return IO<void>([items = std::make_shared<std::vector<IO<void>>>(
                       std::move(items))](auto cb) mutable {
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

// delay helpers
template <typename T = void>
IO<T> delay_for(boost::asio::io_context& ioc,
                std::chrono::milliseconds duration) {
  return IO<T>([&ioc, duration](auto cb) {
    auto timer = std::make_shared<boost::asio::steady_timer>(ioc, duration);
    timer->async_wait([timer, cb](const boost::system::error_code& ec) mutable {
      if (ec) {
        cb(Result<T, Error>::Err(
            Error{1, std::string{"Timer error: "} + ec.message()}));
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

template <typename T>
IO<std::decay_t<T>> delay_then(boost::asio::io_context& ioc,
                               std::chrono::milliseconds duration, T&& val) {
  using U = std::decay_t<T>;
  return delay_for<void>(ioc, duration)
      .map_to([val = std::forward<T>(val)]() mutable {
        return U(std::forward<T>(val));
      });
}

template <typename T>
IO<T> delay(IO<T> io, boost::asio::io_context& ioc,
            std::chrono::milliseconds duration) {
  return delay_for<void>(ioc, duration).then([self = std::move(io)]() mutable {
    return std::move(self);
  });
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

using JsonIO = IO<boost::json::value>;
using JsonObjectIO = IO<boost::json::object>;
using JsonArrayIO = IO<boost::json::array>;
using StringVectorIO = IO<std::vector<std::string>>;
using IntVectorIO = IO<std::vector<int>>;
using BytesIO = IO<std::vector<std::uint8_t>>;

using PathIO = IO<std::filesystem::path>;
using FileStreamIO = IO<std::ifstream>;

using OptionalStringIO = IO<std::optional<std::string>>;
using OptionalIntIO = IO<std::optional<int>>;

}  // namespace monad
