#pragma once

#include <boost/asio.hpp>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>

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

  static IO<T> pure(T value) {
    return IO([val = std::make_shared<T>(std::move(value))](Callback cb) {
      cb(IOResult::Ok(std::move(*val)));
    });
  }

  static IO<T> fail(Error error) {
    return IO([error = std::move(error)](Callback cb) { cb(IOResult{error}); });
  }

  IO<T> clone() const {
    static_assert(std::is_copy_constructible_v<decltype(*this)>,
                  "Cannot clone IO<T>: thunk is not copyable");
    return IO<T>(thunk_);
  }

  template <typename F>
  auto map(F&& f) const
      -> IO<std::conditional_t<std::is_same_v<T, void>, void,
                               decltype(f(std::declval<T>()))>> {
    using RetT = std::conditional_t<std::is_same_v<T, void>, void,
                                    decltype(f(std::declval<T>()))>;

    return IO<RetT>([prev_ptr = std::make_shared<IO<T>>(*this),
                     f = std::forward<F>(f)](typename IO<RetT>::Callback cb) {
      prev_ptr->run(
          [cb = std::move(cb), f = std::move(f)](IOResult result) mutable {
            if constexpr (std::is_same_v<T, void>) {
              if (result.is_ok()) {
                try {
                  f();
                  cb(std::monostate{});
                } catch (const std::exception& e) {
                  cb(Error{-1, e.what()});
                }
              } else {
                cb(result.error());
              }
            } else {
              if (result.is_ok()) {
                try {
                  cb(f(std::move(result.value())));
                } catch (const std::exception& e) {
                  cb(Error{-1, e.what()});
                }
              } else {
                cb(result.error());
              }
            }
          });
    });
  }

  template <typename F>
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

  IO<T> finally(std::function<void()> f) const {
    return IO<T>([prev = *this, f = std::move(f)](Callback cb) {
      prev.run([f, cb = std::move(cb)](IOResult result) mutable {
        f();
        cb(std::move(result));
      });
    });
  }

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

  IO<T> delay(boost::asio::io_context& ioc,
              std::chrono::milliseconds duration) & {
    return std::move(*this).delay(ioc, duration);
  }

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

  IO<T> timeout(boost::asio::io_context& ioc,
                std::chrono::milliseconds duration) & {
    return std::move(*this).timeout(ioc, duration);
  }

  // Conditional exponential backoff retry (IO<T>)
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

  static IO<void> pure() {
    return IO([](Callback cb) { cb(IOResult{std::monostate{}}); });
  }

  static IO<void> fail(Error error) {
    return IO([error = std::move(error)](Callback cb) { cb(IOResult{error}); });
  }

  IO<void> clone() const {
    static_assert(std::is_copy_constructible_v<decltype(*this)>,
                  "Cannot clone IO<T>: thunk is not copyable");
    return IO<void>(thunk_);
  }

  template <typename F>
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

  IO<void> finally(std::function<void()> f) const {
    return IO<void>([prev = *this, f = std::move(f)](Callback cb) {
      prev.run([f, cb = std::move(cb)](IOResult result) mutable {
        f();
        cb(std::move(result));
      });
    });
  }

  IO<void> delay(boost::asio::io_context& ioc,
                 std::chrono::milliseconds duration) && {
    return delay_for(ioc, duration).then([self = std::move(*this)](auto) {
      return std::move(self);
    });
  }

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

  void run(Callback cb) const { thunk_(std::move(cb)); }

 private:
  std::function<void(Callback)> thunk_;
};

template <typename T, typename F>
IO<void> chain_io(const std::vector<T>& vec, F&& f) {
  IO<void> acc = IO<void>::pure();
  for (size_t i = 0; i < vec.size(); ++i) {
    acc = acc.then([&, i]() { return f(i, vec[i]); });
  }
  return acc;
}

}  // namespace monad
