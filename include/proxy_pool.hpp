#pragma once

#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include "aliases.hpp"


namespace client_async {
struct ProxySetting {
  std::string host;
  std::string port;
  std::string username;
  std::string password;

  bool operator==(const ProxySetting& other) const {
    return host == other.host && port == other.port &&
           username == other.username && password == other.password;
  }
};
}  // namespace client_async

namespace std {
template <>
struct hash<client_async::ProxySetting> {
  std::size_t operator()(const client_async::ProxySetting& p) const {
    std::size_t h1 = std::hash<std::string>()(p.host);
    std::size_t h2 = std::hash<std::string>()(p.port);
    std::size_t h3 = std::hash<std::string>()(p.username);
    std::size_t h4 = std::hash<std::string>()(p.password);
    // Combine hashes (example hash combining strategy)
    return (((h1 ^ (h2 << 1)) >> 1) ^ (h3 << 1)) ^ (h4 << 1);
  }
};
}  // namespace std

namespace client_async {

class ProxyPool {
 public:
  void set_proxies(std::vector<ProxySetting>&& proxies) {
    std::lock_guard<std::mutex> lock(mutex_);
    proxies_ = std::move(proxies);
    blacklist_.clear();
    index_ = 0;
    BOOST_LOG_SEV(lg, trivial::info)
        << "Proxy list updated with " << proxies_.size() << " entries";
  }

  // empty could also mean disabled.
  bool empty() {
    std::lock_guard<std::mutex> lock(mutex_);
    return proxies_.empty();
  }
  std::size_t size() {
    std::lock_guard<std::mutex> lock(mutex_);
    return proxies_.size();
  }

  std::optional<ProxySetting> next() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (proxies_.empty()) {
      BOOST_LOG_SEV(lg, trivial::error) << "Proxy list is empty";
      return std::nullopt;
    }
    clean_expired();
    std::size_t tries = 0;
    while (tries < proxies_.size()) {
      const auto& proxy = proxies_[index_];
      index_ = (index_ + 1) % proxies_.size();
      if (!is_blacklisted(proxy)) {
        BOOST_LOG_SEV(lg, trivial::debug)
            << "Returning proxy: " << proxy.host << ":" << proxy.port;
        return proxy;
      }
      ++tries;
    }

    BOOST_LOG_SEV(lg, trivial::warning)
        << "All proxies are currently blacklisted";
    return std::nullopt;
  }

  void blacklist(const ProxySetting& proxy,
                 std::chrono::seconds timeout = std::chrono::seconds(300)) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto expiry = std::chrono::steady_clock::now() + timeout;
    blacklist_[proxy] = expiry;
    BOOST_LOG_SEV(lg, trivial::warning)
        << "Blacklisting proxy: " << proxy.host << ":" << proxy.port << " for "
        << timeout.count() << " seconds";
  }

  void reset_blacklist() {
    std::lock_guard<std::mutex> lock(mutex_);
    blacklist_.clear();
    BOOST_LOG_SEV(lg, trivial::info) << "Blacklist cleared";
  }

 private:
  std::vector<ProxySetting> proxies_;
  std::unordered_map<ProxySetting, std::chrono::steady_clock::time_point>
      blacklist_;
  std::size_t index_ = 0;
  std::mutex mutex_;
  logsrc::severity_logger<trivial::severity_level> lg;

  bool is_blacklisted(const ProxySetting& proxy) {
    auto it = blacklist_.find(proxy);
    if (it == blacklist_.end()) return false;
    return std::chrono::steady_clock::now() < it->second;
  }

  void clean_expired() {
    auto now = std::chrono::steady_clock::now();
    for (auto it = blacklist_.begin(); it != blacklist_.end();) {
      if (now >= it->second) {
        BOOST_LOG_SEV(lg, trivial::debug)
            << "Un-blacklisting proxy: " << it->first.host << ":"
            << it->first.port;
        it = blacklist_.erase(it);
      } else {
        ++it;
      }
    }
  }
};
}  // namespace client_async