#pragma once

#include <algorithm>
#include <chrono>
#include <random>

namespace monad {

struct ExponentialBackoffOptions {
  std::chrono::milliseconds initial_delay{std::chrono::milliseconds(100)};
  std::chrono::milliseconds max_delay{std::chrono::seconds(30)};
  std::chrono::milliseconds jitter{std::chrono::milliseconds::zero()};
};

class JitteredExponentialBackoff {
 public:
  explicit JitteredExponentialBackoff(
      ExponentialBackoffOptions options = ExponentialBackoffOptions{})
      : options_(Sanitize(options)) {}

  void UpdateOptions(const ExponentialBackoffOptions& options) {
    options_ = Sanitize(options);
    if (current_delay_ > options_.max_delay) {
      current_delay_ = options_.max_delay;
    }
  }

  void Reset() { current_delay_ = std::chrono::milliseconds::zero(); }

  std::chrono::milliseconds current_delay() const { return current_delay_; }

  template <typename URNG>
  std::chrono::milliseconds NextDelay(URNG& rng) {
    if (current_delay_.count() <= 0) {
      current_delay_ = options_.initial_delay;
    } else {
      current_delay_ =
          std::min(options_.max_delay, current_delay_ * 2);
    }
    return current_delay_ + SampleJitter(rng);
  }

 private:
  static ExponentialBackoffOptions Sanitize(ExponentialBackoffOptions options) {
    if (options.initial_delay <= std::chrono::milliseconds::zero()) {
      options.initial_delay = std::chrono::milliseconds(1);
    }
    if (options.max_delay < options.initial_delay) {
      options.max_delay = options.initial_delay;
    }
    if (options.jitter < std::chrono::milliseconds::zero()) {
      options.jitter = std::chrono::milliseconds::zero();
    }
    return options;
  }

  template <typename URNG>
  std::chrono::milliseconds SampleJitter(URNG& rng) const {
    if (options_.jitter.count() <= 0) {
      return std::chrono::milliseconds::zero();
    }
    using Rep = std::chrono::milliseconds::rep;
    std::uniform_int_distribution<Rep> dist(0, options_.jitter.count());
    return std::chrono::milliseconds(dist(rng));
  }

  ExponentialBackoffOptions options_;
  std::chrono::milliseconds current_delay_{std::chrono::milliseconds::zero()};
};

}  // namespace monad
