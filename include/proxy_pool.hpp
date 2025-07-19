#pragma once

#include <chrono>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sources/global_logger_storage.hpp>
#include <boost/log/sources/logger.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/move/utility_core.hpp>

#include "aliases.hpp"
namespace trivial = logging::trivial;
namespace logsrc = boost::log::sources;

namespace client_async {

inline  std::string replace_env_var(
    const std::string& input,
    const std::map<std::string, std::string>& extra_map) {
  std::string output = input;

  // Basic parsing for ${VARIABLE} or ${VARIABLE:-default} patterns
  size_t start = output.find("${");
  size_t end = output.find('}', start);
  if (start != std::string::npos && end != std::string::npos) {
    std::string env_var = output.substr(start + 2, end - start - 2);
    std::string default_val;

    // Check for the ":-" syntax for default values
    size_t delim = env_var.find(":-");
    if (delim != std::string::npos) {
      default_val = env_var.substr(delim + 2);
      env_var = env_var.substr(0, delim);
    }

    if (extra_map.find(env_var) != extra_map.end()) {
      output.replace(start, end - start + 1, extra_map.at(env_var));
    } else {
      // Substitute environment variable or default
      const char* env_value = std::getenv(env_var.c_str());
      if (env_value) {
        output.replace(start, end - start + 1, env_value);
      } else {
        return default_val;
      }
    }
  }
  return output;
}

template <size_t Len>
std::optional<std::array<std::string, Len>> parse_line(
    const std::string& line, const std::string& separator,
    size_t expect_elements = Len) {
  std::array<std::string, Len> result;
  size_t count = 0;

  size_t start = 0;
  size_t end = 0;
  while ((end = line.find(separator, start)) != std::string::npos &&
         count < Len) {
    result[count++] = line.substr(start, end - start);
    start = end + separator.length();
  }

  if (count < Len && start <= line.size()) {
    result[count++] = line.substr(start);
  }

  if (count != expect_elements) {
    return std::nullopt;
  }

  return result;
}

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

  const ProxySetting* next() {
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

  void load_settings(const fs::path proxy_pool_file) {
    std::vector<client_async::ProxySetting> proxy_settings;
    if (!proxy_pool_file.empty()) {
      std::ifstream proxy_file(proxy_pool_file);
      if (!proxy_file.is_open()) {
        BOOST_LOG_SEV(lg, trivial::error)
            << "Cannot open proxy pool file: " << proxy_pool_file;
        return;
      }
      std::string line;
      while (std::getline(proxy_file, line)) {
        auto start = std::find_if_not(line.begin(), line.end(), ::isspace);
        auto end =
            std::find_if_not(line.rbegin(), line.rend(), ::isspace).base();
        line = std::string(start, end);
        auto result = parse_line<4>(line, ",");
        if (result.has_value()) {
          auto& ary = *result;
          if (ary.size() == 4) {
            ary[2] = replace_env_var(ary[2], {});
            ary[3] = replace_env_var(ary[3], {});
            proxy_settings.push_back({ary[0], ary[1], ary[2],
                                      ary[3]});  // host, port, user, password
          } else {
            BOOST_LOG_SEV(lg, trivial::error)
                << "Invalid proxy setting: " << line;
          }
        } else {
          BOOST_LOG_SEV(lg, trivial::error)
              << "Failed to parse proxy setting: " << line;
        }
      }
    }
    set_proxies(std::move(proxy_settings));
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
