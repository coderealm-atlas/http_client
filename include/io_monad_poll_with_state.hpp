#pragma once

#include <boost/asio.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "io_monad.hpp"
#include "io_retry_executor.hpp"

namespace monad {

struct PollControl {
  enum class Kind {
    done,
    retry,
    fail,
  };

  Kind kind{Kind::retry};
  std::optional<std::chrono::milliseconds> retry_after{};
  std::optional<Error> fail_error{};

  static PollControl done() { return PollControl{Kind::done, std::nullopt, std::nullopt}; }

  static PollControl retry(std::optional<std::chrono::milliseconds> delay = std::nullopt) {
    return PollControl{Kind::retry, delay, std::nullopt};
  }

  static PollControl fail(Error err) {
    PollControl out{Kind::fail, std::nullopt, std::move(err)};
    return out;
  }
};

template <typename S>
struct PollWithStateHooks {
  // Optional debug hooks; default empty.
  std::function<void(int /*attempt*/, const S&)> on_attempt_start;
  std::function<void(int /*attempt*/, const S&, std::chrono::milliseconds /*delay*/)> on_retry_scheduled;
  std::function<void(int /*attempt*/, const S&)> on_done;
  std::function<void(int /*attempt*/, const S&, const Error&)> on_fail;
};

namespace detail {

template <typename T>
inline Error default_exhausted_error(int max_attempts) {
  return Error{3, "Polling attempts exhausted"};
}

inline Error make_timer_error(const boost::system::error_code& ec) {
  return Error{1, std::string{"Timer error: "} + ec.message()};
}

template <typename T, typename S, typename JobFn, typename DecideFn,
          typename OnExhaustedFn>
IO<T> poll_with_state_impl(int max_attempts,
                           std::chrono::milliseconds default_interval,
                           boost::asio::any_io_executor ex,
                           S initial_state,
                           JobFn job,
                           DecideFn decide,
                           OnExhaustedFn on_exhausted,
                           PollWithStateHooks<S> hooks) {
  return IO<T>([max_attempts, default_interval, ex,
                state = std::make_shared<S>(std::move(initial_state)),
                job_ptr = std::make_shared<std::decay_t<JobFn>>(std::move(job)),
                decide_ptr = std::make_shared<std::decay_t<DecideFn>>(std::move(decide)),
                on_exhausted_ptr =
                    std::make_shared<std::decay_t<OnExhaustedFn>>(std::move(on_exhausted)),
                hooks_ptr =
                    std::make_shared<PollWithStateHooks<S>>(std::move(hooks))](
                   typename IO<T>::Callback cb) mutable {
    using IOResult = Result<T, Error>;

    auto attempt = std::make_shared<int>(0);
    auto do_attempt = std::make_shared<std::function<void()>>();
    auto keep_alive_holder =
        std::make_shared<std::shared_ptr<std::function<void()>>>(do_attempt);
    std::weak_ptr<std::function<void()>> weak_attempt = do_attempt;

    auto cleanup = [keep_alive_holder]() mutable {
      // Break the (intentional) keep-alive chain synchronously.
      // This avoids relying on another io_context turn for cleanup, which can
      // show up as LSAN leaks at shutdown.
      if (keep_alive_holder) {
        keep_alive_holder->reset();
        keep_alive_holder.reset();
      }
    };

    auto schedule_retry = [ex, weak_attempt, keep_alive_holder, cb, cleanup](
                              std::chrono::milliseconds delay) mutable {
      auto keep_alive_copy = (keep_alive_holder ? *keep_alive_holder
                                                : std::shared_ptr<std::function<void()>>{});
      auto timer = std::make_shared<boost::asio::steady_timer>(ex);
      timer->expires_after(delay);
      timer->async_wait([weak_attempt, keep_alive_holder, keep_alive_copy, timer,
                         cb, cleanup](const boost::system::error_code& ec) mutable {
        if (ec) {
          cleanup();
          cb(IOResult::Err(make_timer_error(ec)));
          return;
        }
        (void)keep_alive_copy;
        if (auto attempt_fn = weak_attempt.lock()) {
          (*attempt_fn)();
        }
      });
    };

    auto handle_result =
        [attempt, max_attempts, default_interval, cb, ex, state, job_ptr,
         decide_ptr, on_exhausted_ptr, hooks_ptr, schedule_retry,
         cleanup](int attempt_no, IOResult r) mutable {
          PollControl ctrl;
          try {
            ctrl = (*decide_ptr)(attempt_no, *state, r);
          } catch (const std::exception& e) {
            cleanup();
            cb(IOResult::Err(Error{-1, e.what()}));
            return;
          }

          if (ctrl.kind == PollControl::Kind::done) {
            if constexpr (std::is_void_v<T>) {
              if (r.is_ok()) {
                if (hooks_ptr->on_done) hooks_ptr->on_done(attempt_no, *state);
                cleanup();
                cb(IOResult::Ok());
              } else {
                if (hooks_ptr->on_fail) hooks_ptr->on_fail(attempt_no, *state, r.error());
                cleanup();
                cb(IOResult::Err(std::move(r.error())));
              }
            } else {
              if (r.is_ok()) {
                if (hooks_ptr->on_done) hooks_ptr->on_done(attempt_no, *state);
                cleanup();
                cb(IOResult::Ok(std::move(r.value())));
              } else {
                if (hooks_ptr->on_fail) hooks_ptr->on_fail(attempt_no, *state, r.error());
                cleanup();
                cb(IOResult::Err(std::move(r.error())));
              }
            }
            return;
          }

          if (ctrl.kind == PollControl::Kind::fail) {
            Error err;
            if (ctrl.fail_error.has_value()) {
              err = std::move(*ctrl.fail_error);
            } else if (r.is_err()) {
              err = std::move(r.error());
            } else {
              err = Error{2, "poll_with_state: fail requested without error"};
            }
            if (hooks_ptr->on_fail) hooks_ptr->on_fail(attempt_no, *state, err);
            cleanup();
            cb(IOResult::Err(std::move(err)));
            return;
          }

          // retry
          if (attempt_no >= max_attempts) {
            Error err;
            try {
              err = (*on_exhausted_ptr)(attempt_no, *state, r);
            } catch (const std::exception& e) {
              err = Error{-1, e.what()};
            }
            if (hooks_ptr->on_fail) hooks_ptr->on_fail(attempt_no, *state, err);
            cleanup();
            cb(IOResult::Err(std::move(err)));
            return;
          }

          auto delay = ctrl.retry_after.value_or(default_interval);
          if (hooks_ptr->on_retry_scheduled) {
            hooks_ptr->on_retry_scheduled(attempt_no, *state, delay);
          }
          schedule_retry(delay);
        };

    *do_attempt = [attempt, max_attempts, cb, state, job_ptr, hooks_ptr,
                   handle_result, cleanup]() mutable {
      if (*attempt >= max_attempts) {
        cleanup();
        cb(IOResult::Err(default_exhausted_error<T>(max_attempts)));
        return;
      }
      (*attempt)++;
      const int attempt_no = *attempt;

      if (hooks_ptr->on_attempt_start) {
        hooks_ptr->on_attempt_start(attempt_no, *state);
      }

      try {
        auto io = (*job_ptr)(attempt_no, *state);
        io.run([attempt_no, handle_result](IOResult r) mutable {
          handle_result(attempt_no, std::move(r));
        });
      } catch (const std::exception& e) {
        cleanup();
        cb(IOResult::Err(Error{-1, e.what()}));
        return;
      }
    };

    (*do_attempt)();
  });
}

}  // namespace detail

// Full form: explicit executor + custom exhausted error.
template <typename T, typename S, typename JobFn, typename DecideFn,
          typename OnExhaustedFn>
IO<T> poll_with_state(int max_attempts, std::chrono::milliseconds default_interval,
                      boost::asio::any_io_executor ex, S initial_state,
                      JobFn job, DecideFn decide, OnExhaustedFn on_exhausted,
                      PollWithStateHooks<S> hooks = {}) {
  return detail::poll_with_state_impl<T>(
      max_attempts, default_interval, ex, std::move(initial_state),
      std::move(job), std::move(decide), std::move(on_exhausted),
      std::move(hooks));
}

// Convenience: explicit executor, default exhausted error.
template <typename T, typename S, typename JobFn, typename DecideFn>
IO<T> poll_with_state(int max_attempts, std::chrono::milliseconds default_interval,
                      boost::asio::any_io_executor ex, S initial_state,
                      JobFn job, DecideFn decide,
                      PollWithStateHooks<S> hooks = {}) {
  auto on_exhausted = [max_attempts](int attempts, S&, const Result<T, Error>&) {
    return detail::default_exhausted_error<T>(max_attempts);
  };
  return detail::poll_with_state_impl<T>(
      max_attempts, default_interval, ex, std::move(initial_state),
      std::move(job), std::move(decide), std::move(on_exhausted),
      std::move(hooks));
}

// Convenience: use internal retry executor (separate from job's executor).
template <typename T, typename S, typename JobFn, typename DecideFn,
      typename OnExhaustedFn,
      typename std::enable_if_t<
        !std::is_convertible_v<std::decay_t<S>, boost::asio::any_io_executor>,
        int> = 0>
IO<T> poll_with_state(int max_attempts, std::chrono::milliseconds default_interval,
                      S initial_state, JobFn job, DecideFn decide,
                      OnExhaustedFn on_exhausted,
                      PollWithStateHooks<S> hooks = {}) {
  return poll_with_state<T>(max_attempts, default_interval, retry_executor(),
                            std::move(initial_state), std::move(job),
                            std::move(decide), std::move(on_exhausted),
                            std::move(hooks));
}

template <typename T, typename S, typename JobFn, typename DecideFn,
      typename std::enable_if_t<
        !std::is_convertible_v<std::decay_t<S>, boost::asio::any_io_executor>,
        int> = 0>
IO<T> poll_with_state(int max_attempts, std::chrono::milliseconds default_interval,
                      S initial_state, JobFn job, DecideFn decide,
                      PollWithStateHooks<S> hooks = {}) {
  return poll_with_state<T>(max_attempts, default_interval, retry_executor(),
                            std::move(initial_state), std::move(job),
                            std::move(decide), std::move(hooks));
}

}  // namespace monad
