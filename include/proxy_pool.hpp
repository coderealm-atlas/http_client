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
#include <mutex>
#include <unordered_map>
#include <vector>

#include "http_client_config_provider.hpp"
namespace logging = boost::log;
namespace trivial = logging::trivial;
namespace logsrc = logging::sources;

// namespace client_async {

// inline  std::string replace_env_var(
//     const std::string& input,
//     const std::map<std::string, std::string>& extra_map) {
//   std::string output = input;

//   // Basic parsing for ${VARIABLE} or ${VARIABLE:-default} patterns
//   size_t start = output.find("${");
//   size_t end = output.find('}', start);
//   if (start != std::string::npos && end != std::string::npos) {
//     std::string env_var = output.substr(start + 2, end - start - 2);
//     std::string default_val;

//     // Check for the ":-" syntax for default values
//     size_t delim = env_var.find(":-");
//     if (delim != std::string::npos) {
//       default_val = env_var.substr(delim + 2);
//       env_var = env_var.substr(0, delim);
//     }

//     if (extra_map.find(env_var) != extra_map.end()) {
//       output.replace(start, end - start + 1, extra_map.at(env_var));
//     } else {
//       // Substitute environment variable or default
//       const char* env_value = std::getenv(env_var.c_str());
//       if (env_value) {
//         output.replace(start, end - start + 1, env_value);
//       } else {
//         return default_val;
//       }
//     }
//   }
//   return output;
// }

// template <size_t Len>
// std::optional<std::array<std::string, Len>> parse_line(
//     const std::string& line, const std::string& separator,
//     size_t expect_elements = Len) {
//   std::array<std::string, Len> result;
//   size_t count = 0;

//   size_t start = 0;
//   size_t end = 0;
//   while ((end = line.find(separator, start)) != std::string::npos &&
//          count < Len) {
//     result[count++] = line.substr(start, end - start);
//     start = end + separator.length();
//   }

//   if (count < Len && start <= line.size()) {
//     result[count++] = line.substr(start);
//   }

//   if (count != expect_elements) {
//     return std::nullopt;
//   }

//   return result;
// }

// }  // namespace client_async

namespace client_async {

class ProxyPool {
  const std::vector<cjj365::ProxySetting>& proxies_;
  std::unordered_map<cjj365::ProxySetting,
                     std::chrono::steady_clock::time_point>
      blacklist_;
  std::size_t index_ = 0;
  std::mutex mutex_;
  logsrc::severity_logger<trivial::severity_level> lg;

 public:
  ProxyPool(cjj365::IHttpclientConfigProvider& config_provider)
      : proxies_(config_provider.get().get_proxy_pool()) {}

  // empty could also mean disabled.
  bool empty() {
    std::lock_guard<std::mutex> lock(mutex_);
    return proxies_.empty();
  }
  std::size_t size() {
    std::lock_guard<std::mutex> lock(mutex_);
    return proxies_.size();
  }

  const cjj365::ProxySetting* next() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (proxies_.empty()) {
      return nullptr;
    }
    clean_expired();
    std::size_t tries = 0;
    while (tries < proxies_.size()) {
      const auto& proxy = proxies_[index_];
      index_ = (index_ + 1) % proxies_.size();
      if (!is_blacklisted(proxy)) {
        BOOST_LOG_SEV(lg, trivial::debug)
            << "Returning proxy: " << proxy.host << ":" << proxy.port;
        return &proxy;
      }
      ++tries;
    }

    BOOST_LOG_SEV(lg, trivial::warning)
        << "All proxies are currently blacklisted";
    return nullptr;
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
};
}  // namespace client_async
