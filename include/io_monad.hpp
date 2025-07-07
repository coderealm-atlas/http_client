#pragma once

#include <exception>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

// This is a simple IO monad implementation in C++.
// It allows for chaining operations that may involve side effects,
// such as I/O operations, while maintaining a functional style.
namespace monad {

struct Error {
  int code;
  std::string what;
  Error(int c, std::string msg) : code(c), what(std::move(msg)) {}
};

inline std::ostream& operator<<(std::ostream& os, const Error& e) {
  return os << "[Error " << e.code << "] " << e.what;
}

template <typename T>
class IO {
 public:
  using Result = std::variant<T, Error>;
  using Callback = std::function<void(Result)>;

  explicit IO(std::function<void(Callback)> thunk) : thunk_(std::move(thunk)) {}

  static IO<T> pure(T value) {
    return IO([val = std::make_shared<T>(std::move(value))](Callback cb) {
      cb(Result(std::in_place_type<T>, std::move(*val)));
    });
  }

  static IO<T> fail(Error error) {
    return IO([error = std::move(error)](Callback cb) { cb(Result{error}); });
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
          [cb = std::move(cb), f = std::move(f)](Result result) mutable {
            if constexpr (std::is_same_v<T, void>) {
              if (std::holds_alternative<T>(result)) {
                try {
                  f();
                  cb(std::monostate{});
                } catch (const std::exception& e) {
                  cb(Error{-1, e.what()});
                }
              } else {
                cb(std::get<Error>(result));
              }
            } else {
              if (std::holds_alternative<T>(result)) {
                try {
                  cb(f(std::move(std::get<T>(result))));
                } catch (const std::exception& e) {
                  cb(Error{-1, e.what()});
                }
              } else {
                cb(std::get<Error>(result));
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
      prev_ptr->run([f_wrapped, cb = std::move(cb)](Result result) mutable {
        if (std::holds_alternative<T>(result)) {
          try {
            if constexpr (std::is_same_v<T, void>)
              (*f_wrapped)().run(std::move(cb));
            else
              (*f_wrapped)(std::move(std::get<T>(result))).run(std::move(cb));
          } catch (const std::exception& e) {
            cb(Error{-2, e.what()});
          }
        } else {
          cb(std::get<Error>(result));
        }
      });
    });
  }

  template <typename F>
  IO<T> catch_then(F&& f) const {
    auto prev_ptr = std::make_shared<IO<T>>(*this);
    auto f_ptr = std::make_shared<std::decay_t<F>>(std::forward<F>(f));

    return IO<T>([prev_ptr, f_ptr](Callback cb) mutable {
      prev_ptr->run([f_ptr, cb = std::move(cb)](Result result) mutable {
        if (std::holds_alternative<Error>(result)) {
          try {
            (*f_ptr)(std::get<Error>(result)).run(std::move(cb));
          } catch (const std::exception& e) {
            cb(Error{-3, e.what()});
          }
        } else {
          cb(std::move(result));
        }
      });
    });
  }

  void run(Callback cb) const { thunk_(std::move(cb)); }

 private:
  std::function<void(Callback)> thunk_;
};

template <>
class IO<void> {
 public:
  using Result = std::variant<std::monostate, Error>;
  using Callback = std::function<void(Result)>;

  explicit IO(std::function<void(Callback)> thunk) : thunk_(std::move(thunk)) {}

  static IO<void> pure() {
    return IO([](Callback cb) { cb(Result{std::monostate{}}); });
  }

  static IO<void> fail(Error error) {
    return IO([error = std::move(error)](Callback cb) { cb(Result{error}); });
  }

  template <typename F>
  auto map(F&& f) const -> IO<void> {
    return IO<void>([prev_ptr = std::make_shared<IO<void>>(*this),
                     f = std::forward<F>(f)](Callback cb) {
      prev_ptr->run(
          [cb = std::move(cb), f = std::move(f)](Result result) mutable {
            if (std::holds_alternative<std::monostate>(result)) {
              try {
                f();
                cb(std::monostate{});
              } catch (const std::exception& e) {
                cb(Error{-1, e.what()});
              }
            } else {
              cb(std::get<Error>(result));
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
      prev_ptr->run([f_wrapped, cb = std::move(cb)](Result result) mutable {
        if (std::holds_alternative<std::monostate>(result)) {
          try {
            (*f_wrapped)().run(std::move(cb));
          } catch (const std::exception& e) {
            cb(Error{-2, e.what()});
          }
        } else {
          cb(std::get<Error>(result));
        }
      });
    });
  }

  template <typename F>
  IO<void> catch_then(F&& f) const {
    auto prev_ptr = std::make_shared<IO<void>>(*this);
    auto f_ptr = std::make_shared<std::decay_t<F>>(std::forward<F>(f));

    return IO<void>([prev_ptr, f_ptr](Callback cb) mutable {
      prev_ptr->run([f_ptr, cb = std::move(cb)](Result result) mutable {
        if (std::holds_alternative<Error>(result)) {
          try {
            (*f_ptr)(std::get<Error>(result)).run(std::move(cb));
          } catch (const std::exception& e) {
            cb(Error{-3, e.what()});
          }
        } else {
          cb(std::move(result));
        }
      });
    });
  }

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
