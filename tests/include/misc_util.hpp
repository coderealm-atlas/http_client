#pragma once

#include <stdint.h>

#include <any>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <variant>

namespace fs = std::filesystem;

namespace misc {

using Clock = std::chrono::steady_clock;
// Rate Limiter class
template <typename T>
class RateLimiter {
 private:
  struct UserData {
    int tokens;                     // Current number of tokens
    Clock::time_point lastRequest;  // Timestamp of the last request
  };

  const int maxTokens;    // Maximum tokens allowed
  const int consumeRate;  // Tokens consumed per request
  const int refillRate;   // Tokens added per second
  std::unordered_map<T, UserData> userLimits;
  std::mutex mtx;  // Mutex for thread-safe operations

  // Refills tokens based on elapsed time
  void refillTokens(UserData& userData) {
    auto now = Clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                       now - userData.lastRequest)
                       .count();
    if (elapsed > 0) {
      userData.tokens = std::min(
          maxTokens, userData.tokens + static_cast<int>(elapsed * refillRate));
      userData.lastRequest = now;
    }
  }

  void remove_older_than_300s() {
    auto now = Clock::now();
    for (auto it = userLimits.begin(); it != userLimits.end();) {
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                         now - it->second.lastRequest)
                         .count();
      if (elapsed > 300) {
        DEBUG_PRINT("Removing user: " << it->first);
        it = userLimits.erase(it);
      } else {
        ++it;
      }
    }
  }

 public:
  RateLimiter(int maxTokens, int consumeRate, int refillRate)
      : maxTokens(maxTokens),
        consumeRate(consumeRate),
        refillRate(refillRate) {}

  // Checks if the user is allowed to make a request
  bool allowRequest(const T& userID) {
    std::lock_guard<std::mutex> lock(mtx);
    remove_older_than_300s();
    auto& userData = userLimits[userID];

    // If it's the user's first request, initialize their data
    if (userData.lastRequest.time_since_epoch().count() == 0) {
      userData.tokens = maxTokens;
      userData.lastRequest = Clock::now();
    }

    // Refill tokens based on elapsed time
    refillTokens(userData);
    // Allow the request if there are tokens available
    if (userData.tokens > 0) {
      userData.tokens -= consumeRate;
      return true;
    } else {
      userData.tokens -= consumeRate;
      return false;  // Deny request if no tokens are available
    }
  }
};

class ThreadNotifier {
 public:
  ThreadNotifier(uint64_t milliseconds = 0ull)
      : milliseconds_(milliseconds), notified_(false) {}

  void waitForNotification() {
    // std::unique_lock is a more flexible and powerful wrapper around the
    // mutex, compared to std::lock_guard. It allows for deferred locking,
    // manual unlocking, and lock management in more complex scenarios.
    std::unique_lock<std::mutex> lock(
        mutex_);  // will release when goes out the scope.
    // Wait until notified

    // The lock is automatically released during the wait() call in
    // waitForNotification(), and the waiting thread will reacquire it once
    // notified.
    if (milliseconds_ == 0) {
      condition_.wait(
          lock, [this] { return notified_; });  // predicate checks if notified
    } else {
      condition_.wait_for(lock, std::chrono::milliseconds(milliseconds_),
                          [this] { return notified_; });
    }
    if (notified_) {
      notified_ = false;
    } else {
    }
  }

  void reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    notified_ = false;
  }

  void notify() {
    {
      // std::lock_guard<std::mutex> is used to automatically acquire and
      // release the lock within its scope.
      std::lock_guard<std::mutex> lock(mutex_);
      notified_ = true;  // Set the notification status, shared data must be
                         // protected orelse will lead to a UB.
    }
    condition_.notify_one();  // Notify one waiting thread
  }

  template <typename T>
  void set(T value) {
    data[typeid(T)] = std::move(value);
  }

  template <typename T>
  T* get() {
    auto it = data.find(typeid(T));
    std::cout << "get data: " << typeid(T).name() << std::endl;
    if (it != data.end()) {
      return std::any_cast<T>(&it->second);
    }
    return nullptr;
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  uint64_t milliseconds_;
  bool notified_;
  std::unordered_map<std::type_index, std::any> data;
};

std::string append_GITHUB_HOST(const std::string& content,
                               const std::string& host);

void modify_vcpkg_ports(const fs::path& directory);

}  // namespace misc