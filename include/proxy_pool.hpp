#pragma once

#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sources/global_logger_storage.hpp>
#include <boost/log/sources/logger.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/move/utility_core.hpp>
#include <chrono>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "http_client_config_provider.hpp"
namespace logging = boost::log;
namespace trivial = logging::trivial;
namespace logsrc = logging::sources;

namespace client_async {

class ProxyPool {
  using ProxyList = std::vector<cjj365::ProxySetting>;

  std::shared_ptr<const ProxyList> proxies_;
  std::unordered_map<cjj365::ProxySetting,
                     std::chrono::steady_clock::time_point>
      blacklist_;
  std::size_t index_ = 0;
  mutable std::mutex mutex_;
  logsrc::severity_logger<trivial::severity_level> lg;

 public:
  ProxyPool(cjj365::IHttpclientConfigProvider& config_provider,
            std::string_view profile = {})
      : proxies_(std::make_shared<ProxyList>(
            select_proxies(config_provider, profile))) {}

  void replace_entries(ProxyList entries) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto next = std::make_shared<ProxyList>(std::move(entries));
    proxies_ = std::move(next);
    index_ = 0;
  }

  void merge_entries(ProxyList additions) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto current = proxies_ ? *proxies_ : ProxyList{};

    std::unordered_set<cjj365::ProxySetting> seen;
    seen.reserve(current.size() + additions.size());
    for (const auto& p : current) {
      seen.insert(p);
    }

    for (auto& p : additions) {
      if (seen.insert(p).second) {
        current.emplace_back(std::move(p));
      }
    }

    proxies_ = std::make_shared<ProxyList>(std::move(current));
    if (proxies_->empty()) {
      index_ = 0;
    } else {
      index_ %= proxies_->size();
    }
  }

  // empty could also mean disabled.
  bool empty() {
    std::lock_guard<std::mutex> lock(mutex_);
    return !proxies_ || proxies_->empty();
  }
  std::size_t size() {
    std::lock_guard<std::mutex> lock(mutex_);
    return proxies_ ? proxies_->size() : 0;
  }

  std::shared_ptr<const cjj365::ProxySetting> next() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!proxies_ || proxies_->empty()) {
      return {};
    }
    clean_expired();
    std::size_t tries = 0;
    while (tries < proxies_->size()) {
      const auto& proxy = (*proxies_)[index_];
      index_ = (index_ + 1) % proxies_->size();
      if (!is_blacklisted(proxy)) {
        BOOST_LOG_SEV(lg, trivial::debug)
            << "Returning proxy: " << proxy.host << ":" << proxy.port;
        return std::shared_ptr<const cjj365::ProxySetting>(proxies_, &proxy);
      }
      ++tries;
    }

    BOOST_LOG_SEV(lg, trivial::warning)
        << "All proxies are currently blacklisted";
    return {};
  }

  void blacklist(const cjj365::ProxySetting& proxy,
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

  const std::vector<cjj365::ProxySetting>& entries() const {
    static const ProxyList kEmpty;
    std::lock_guard<std::mutex> lock(mutex_);
    return proxies_ ? *proxies_ : kEmpty;
  }

 private:
  bool is_blacklisted(const cjj365::ProxySetting& proxy) {
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
  static ProxyList select_proxies(cjj365::IHttpclientConfigProvider& provider,
                                  std::string_view profile) {
    const auto& cfg = profile.empty() ? provider.get() : provider.get(profile);
    return cfg.get_proxy_pool();
  }
};
}  // namespace client_async
