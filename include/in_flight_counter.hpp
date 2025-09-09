#pragma once
#include <atomic>
#include <chrono>
#include <thread>

namespace cjj365 {

using namespace std::chrono_literals;
class InFlightCounter {
  std::atomic<int> counter_{0};

 public:
  void increment() { counter_++; }
  void decrement() { counter_--; }
  int value() const { return counter_.load(); }

  void wait_until_zero(std::chrono::milliseconds interval = 100ms,
                       int max_retries = 30) const {
    for (int i = 0; i < max_retries; ++i) {
      if (value() == 0) return;
      std::this_thread::sleep_for(interval);
    }
  }

  struct Guard {
    InFlightCounter& parent;
    Guard(InFlightCounter& c) : parent(c) { parent.increment(); }
    ~Guard() { parent.decrement(); }
  };
};

struct StopIndicator {
  std::atomic<bool> stop_requested = false;

  void stop() { stop_requested.store(true, std::memory_order_relaxed); }
  bool is_stopped() const {
    return stop_requested.load(std::memory_order_relaxed);
  }
};

}  // namespace cjj365
