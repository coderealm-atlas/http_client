#pragma once

#include <algorithm>
#include <array>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/log/trivial.hpp>
#include <boost/json.hpp>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "json_util.hpp"
#include "simple_data.hpp"

namespace ssl = boost::asio::ssl;

namespace json = boost::json;

namespace cjj365 {

inline ssl::context::method ssl_method_from_string(const std::string& name) {
  static const std::unordered_map<std::string, ssl::context::method>
      kMethodMap = {{"sslv2", ssl::context::sslv2},
                    {"sslv2_client", ssl::context::sslv2_client},
                    {"sslv2_server", ssl::context::sslv2_server},
                    {"sslv3", ssl::context::sslv3},
                    {"sslv3_client", ssl::context::sslv3_client},
                    {"sslv3_server", ssl::context::sslv3_server},
                    {"tlsv1", ssl::context::tlsv1},
                    {"tlsv1_client", ssl::context::tlsv1_client},
                    {"tlsv1_server", ssl::context::tlsv1_server},
                    {"sslv23", ssl::context::sslv23},
                    {"sslv23_client", ssl::context::sslv23_client},
                    {"sslv23_server", ssl::context::sslv23_server},
                    {"tlsv11", ssl::context::tlsv11},
                    {"tlsv11_client", ssl::context::tlsv11_client},
                    {"tlsv11_server", ssl::context::tlsv11_server},
                    {"tlsv12", ssl::context::tlsv12},
                    {"tlsv12_client", ssl::context::tlsv12_client},
                    {"tlsv12_server", ssl::context::tlsv12_server},
                    {"tlsv13", ssl::context::tlsv13},
                    {"tlsv13_client", ssl::context::tlsv13_client},
                    {"tlsv13_server", ssl::context::tlsv13_server},
                    {"tls", ssl::context::tls},
                    {"tls_client", ssl::context::tls_client},
                    {"tls_server", ssl::context::tls_server}};

  auto it = kMethodMap.find(name);
  if (it == kMethodMap.end()) {
    throw std::invalid_argument("Invalid SSL method name: " + name);
  }
  return it->second;
}

struct ProxySetting {
  std::string host;
  std::string port;
  std::string username;
  std::string password;
  bool disabled = false;
  // True when this entry was inherited from process environment variables
  // (HTTP_PROXY/HTTPS_PROXY/ALL_PROXY). Used to support NO_PROXY bypass.
  bool from_env = false;

  bool operator==(const ProxySetting& other) const {
    return host == other.host && port == other.port &&
           username == other.username && password == other.password &&
           disabled == other.disabled && from_env == other.from_env;
  }

  friend ProxySetting tag_invoke(const json::value_to_tag<ProxySetting>&,
                                 const json::value& jv) {
    ProxySetting proxy;
    if (auto* jo = jv.if_object()) {
      proxy.host = jo->at("host").as_string().c_str();
      if (auto* port_p = jo->if_contains("port")) {
        if (auto* port_string_p = port_p->if_string()) {
          proxy.port = port_string_p->c_str();
        } else if (port_p->is_number()) {
          proxy.port = std::to_string(port_p->to_number<int64_t>());
        } else {
          throw std::invalid_argument("Invalid port type in ProxySetting");
        }
      }
      proxy.username = jo->at("username").as_string().c_str();
      proxy.password = jo->at("password").as_string().c_str();
      if (auto* disabled_p = jo->if_contains("disabled")) {
        proxy.disabled = json::value_to<bool>(*disabled_p);
      } else {
        proxy.disabled = false;
      }
    } else {
      throw std::invalid_argument("Invalid JSON for ProxySetting");
    }
    return proxy;
  }
};

struct HttpclientCertificate {
  std::string cert_content;
  std::string file_format;

  friend HttpclientCertificate tag_invoke(
      const json::value_to_tag<HttpclientCertificate>&, const json::value& jv) {
    HttpclientCertificate cert;
    if (auto* jo = jv.if_object()) {
      cert.cert_content = jo->at("cert_content").as_string().c_str();
      cert.file_format = jo->at("file_format").as_string().c_str();
    } else {
      throw std::invalid_argument("Invalid JSON for HttpclientCertificate");
    }
    return cert;
  }
};

struct HttpclientCertificateFile {
  std::string cert_path;
  std::string file_format;

  friend HttpclientCertificateFile tag_invoke(
      const json::value_to_tag<HttpclientCertificateFile>&,
      const json::value& jv) {
    HttpclientCertificateFile cert;
    if (auto* jo = jv.if_object()) {
      cert.cert_path = jo->at("cert_path").as_string().c_str();
      cert.file_format = jo->at("file_format").as_string().c_str();
    } else {
      throw std::invalid_argument("Invalid JSON for HttpclientCertificateFile");
    }
    return cert;
  }
};

class HttpclientConfig {
  ssl::context::method ssl_method = ssl::context::method::tlsv12_client;
  int threads_num = 0;
  bool default_verify_path = true;
  bool insecure_skip_verify = false;
  std::vector<std::string> verify_paths;
  std::vector<HttpclientCertificate> certificates;
  std::vector<HttpclientCertificateFile> certificate_files;
  std::vector<cjj365::ProxySetting> proxy_pool;

 public:
  void inherit_env_proxy_if_empty(cjj365::ProxySetting proxy) {
    if (proxy.disabled) {
      return;
    }
      proxy.from_env = true;
    if (proxy_pool.empty()) {
      proxy_pool.emplace_back(std::move(proxy));
    }
  }

  friend HttpclientConfig tag_invoke(
      const json::value_to_tag<HttpclientConfig>&, const json::value& jv) {
    HttpclientConfig config;
    try {
      if (auto* jo = jv.if_object()) {
        if (auto* ssl_method_p = jo->if_contains("ssl_method")) {
          config.ssl_method = ssl_method_from_string(
              json::value_to<std::string>(*ssl_method_p));
        }
        config.threads_num = jv.at("threads_num").to_number<int>();
        if (config.threads_num < 0) {
          throw std::invalid_argument("threads_num must be non-negative");
        }
        if (auto* verify_paths_p = jo->if_contains("verify_paths")) {
          config.verify_paths =
              json::value_to<std::vector<std::string>>(*verify_paths_p);
        }
        if (auto* insecure_p = jo->if_contains("insecure_skip_verify")) {
          config.insecure_skip_verify = json::value_to<bool>(*insecure_p);
        }
        if (auto* certificates_p = jo->if_contains("certificates")) {
          config.certificates =
              json::value_to<std::vector<HttpclientCertificate>>(
                  *certificates_p);
        }
        if (auto* certificate_files_p = jo->if_contains("certificate_files")) {
          config.certificate_files =
              json::value_to<std::vector<HttpclientCertificateFile>>(
                  *certificate_files_p);
        }
        if (auto* proxy_pool_p = jo->if_contains("proxy_pool")) {
          config.proxy_pool =
              json::value_to<std::vector<cjj365::ProxySetting>>(*proxy_pool_p);
          config.proxy_pool.erase(
              std::remove_if(config.proxy_pool.begin(), config.proxy_pool.end(),
                             [](const cjj365::ProxySetting& proxy) {
                               return proxy.disabled;
                             }),
              config.proxy_pool.end());

          auto has_unresolved_env = [](const std::string& s) {
            // ConfigSources leaves ${VAR} intact when VAR is not set.
            // Treat such entries as unusable so we don't attempt proxy auth
            // with literal placeholders.
            return s.find("${") != std::string::npos;
          };
          config.proxy_pool.erase(
              std::remove_if(config.proxy_pool.begin(), config.proxy_pool.end(),
                             [&](const cjj365::ProxySetting& proxy) {
                               return has_unresolved_env(proxy.username) ||
                                      has_unresolved_env(proxy.password);
                             }),
              config.proxy_pool.end());
        }
        return config;
      } else {
        throw std::invalid_argument("HttpclientConfig must be an object.");
      }
    } catch (const std::exception& e) {
      throw std::invalid_argument("Invalid JSON for HttpclientConfig: " +
                                  std::string(e.what()));
    }
  }

  int get_threads_num() const {
    int hthreads_num = std::thread::hardware_concurrency();
    if (threads_num == 0) {
      return hthreads_num;
    }
    return (threads_num > hthreads_num) ? hthreads_num : threads_num;
  }
  ssl::context::method get_ssl_method() const { return ssl_method; }
  bool get_default_verify_path() const { return default_verify_path; }
  bool get_insecure_skip_verify() const { return insecure_skip_verify; }
  const std::vector<std::string>& get_verify_paths() const {
    return verify_paths;
  }
  const std::vector<HttpclientCertificate>& get_certificates() const {
    return certificates;
  }
  const std::vector<HttpclientCertificateFile>& get_certificate_files() const {
    return certificate_files;
  }
  const std::vector<cjj365::ProxySetting>& get_proxy_pool() const {
    return proxy_pool;
  }
};

class IHttpclientConfigProvider {
 public:
  virtual ~IHttpclientConfigProvider() = default;

  virtual const HttpclientConfig& get() const = 0;
  virtual const HttpclientConfig& get(std::string_view name) const = 0;
  virtual std::vector<std::string> names() const = 0;
  virtual std::string_view default_name() const = 0;
};

class HttpclientConfigProviderFile : public IHttpclientConfigProvider {
  std::unordered_map<std::string, HttpclientConfig> configs_;
  std::vector<std::string> ordered_names_;
  std::string default_name_;

 public:
  explicit HttpclientConfigProviderFile(cjj365::AppProperties& app_properties,
                                        cjj365::ConfigSources& config_sources) {
    auto r = config_sources.json_content("httpclient_config");
    if (r.is_err()) {
      throw std::runtime_error("Failed to load HTTP client config: " +
                               r.error().what);
    } else {
      json::value jv = r.value();
      jsonutil::substitue_envs(jv, config_sources.cli_overrides(),
                               app_properties.properties);
      parse_configs(jv);
      inherit_env_proxy_if_enabled(config_sources);
    }
  }

  const HttpclientConfig& get() const override {
    return configs_.at(default_name_);
  }

  const HttpclientConfig& get(std::string_view name) const override {
    auto it = configs_.find(std::string(name));
    if (it == configs_.end()) {
      throw std::out_of_range("Unknown httpclient config profile: " +
                              std::string(name));
    }
    return it->second;
  }

  std::vector<std::string> names() const override { return ordered_names_; }

  std::string_view default_name() const override { return default_name_; }

 private:
  static bool is_truthy(std::string_view v) {
    if (v.empty()) return false;
    std::string s{v};
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return s == "1" || s == "true" || s == "yes" || s == "on";
  }

  static std::optional<std::string> getenv_any(
      std::initializer_list<const char*> names) {
    for (auto name : names) {
      const char* v = std::getenv(name);
      if (v && *v) {
        return std::string(v);
      }
    }
    return std::nullopt;
  }

  struct EnvVar {
    std::string name;
    std::string value;
  };

  static std::optional<EnvVar> getenv_any_named(
      std::initializer_list<const char*> names) {
    for (auto name : names) {
      const char* v = std::getenv(name);
      if (v && *v) {
        return EnvVar{std::string(name), std::string(v)};
      }
    }
    return std::nullopt;
  }

    static std::optional<cjj365::ProxySetting> parse_proxy_env_value(
      std::string_view raw, std::string_view env_name_for_error = {}) {
    auto trim = [](std::string_view s) {
      while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                            s.front() == '\n' || s.front() == '\r')) {
        s.remove_prefix(1);
      }
      while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                            s.back() == '\n' || s.back() == '\r')) {
        s.remove_suffix(1);
      }
      return s;
    };

    raw = trim(raw);
    if (raw.empty()) {
      return std::nullopt;
    }

    // Handle scheme if present.
    if (auto pos = raw.find("://"); pos != std::string_view::npos) {
      auto scheme = raw.substr(0, pos);
      std::string scheme_lc{scheme};
      std::transform(scheme_lc.begin(), scheme_lc.end(), scheme_lc.begin(),
                     [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                     });

      // We only implement HTTP proxy (incl. CONNECT for HTTPS). SOCKS URLs are
      // common in some environments (e.g. ALL_PROXY=socks5://...), but treating
      // them as HTTP proxy would fail in confusing ways.
      if (scheme_lc.rfind("socks", 0) == 0) {
        std::string msg = "Unsupported proxy scheme '" + scheme_lc +
                          "' in environment";
        if (!env_name_for_error.empty()) {
          msg += " variable '" + std::string(env_name_for_error) + "'";
        }
        msg += ". Only HTTP proxies are supported. Use an http:// proxy, or "
               "pass --ignore-env-proxy.";
        throw std::invalid_argument(msg);
      }

      // Accept http/https schemes and ignore the scheme for proxy host parsing.
      raw.remove_prefix(pos + 3);
    }

    // Drop any path/query fragment.
    if (auto slash = raw.find('/'); slash != std::string_view::npos) {
      raw = raw.substr(0, slash);
    }

    std::string_view auth_part;
    std::string_view host_part = raw;
    if (auto at = raw.rfind('@'); at != std::string_view::npos) {
      auth_part = raw.substr(0, at);
      host_part = raw.substr(at + 1);
    }

    cjj365::ProxySetting proxy;
    proxy.disabled = false;

    if (!auth_part.empty()) {
      if (auto colon = auth_part.find(':'); colon != std::string_view::npos) {
        proxy.username = std::string(auth_part.substr(0, colon));
        proxy.password = std::string(auth_part.substr(colon + 1));
      } else {
        proxy.username = std::string(auth_part);
      }
    }

    // Parse host[:port], with optional IPv6 literal.
    if (!host_part.empty() && host_part.front() == '[') {
      auto rb = host_part.find(']');
      if (rb == std::string_view::npos) {
        return std::nullopt;
      }
      proxy.host = std::string(host_part.substr(1, rb - 1));
      if (rb + 1 < host_part.size() && host_part[rb + 1] == ':') {
        proxy.port = std::string(host_part.substr(rb + 2));
      }
    } else {
      auto colon = host_part.rfind(':');
      if (colon != std::string_view::npos && colon + 1 < host_part.size()) {
        proxy.host = std::string(host_part.substr(0, colon));
        proxy.port = std::string(host_part.substr(colon + 1));
      } else {
        proxy.host = std::string(host_part);
      }
    }

    if (proxy.host.empty()) {
      return std::nullopt;
    }
    if (proxy.port.empty()) {
      proxy.port = "80";
    }
    return proxy;
  }

  void inherit_env_proxy_if_enabled(const cjj365::ConfigSources& config_sources) {
    auto it = config_sources.cli_overrides().find("ignore_env_proxy");
    if (it != config_sources.cli_overrides().end() && is_truthy(it->second)) {
      return;
    }

    // Precedence: HTTPS_PROXY, HTTP_PROXY, ALL_PROXY (and lowercase variants).
    auto env = getenv_any_named({"HTTPS_PROXY", "https_proxy", "HTTP_PROXY",
                                 "http_proxy", "ALL_PROXY", "all_proxy"});
    if (!env.has_value()) {
      return;
    }

    auto proxy_opt = parse_proxy_env_value(env->value, env->name);
    if (!proxy_opt.has_value()) {
      return;
    }

    std::size_t applied_profiles = 0;
    for (auto& kv : configs_) {
      const bool was_empty = kv.second.get_proxy_pool().empty();
      kv.second.inherit_env_proxy_if_empty(*proxy_opt);
      if (was_empty && !kv.second.get_proxy_pool().empty()) {
        ++applied_profiles;
      }
    }

    // Log once at startup; do not log credentials.
    if (applied_profiles > 0) {
      BOOST_LOG_TRIVIAL(info)
          << "Detected env proxy via " << env->name << ": "
          << proxy_opt->host << ':' << proxy_opt->port
          << (proxy_opt->username.empty() ? "" : " (with credentials)")
          << ", applied to " << applied_profiles << " profile(s).";
    } else {
      BOOST_LOG_TRIVIAL(info)
          << "Detected env proxy via " << env->name << ": "
          << proxy_opt->host << ':' << proxy_opt->port
          << ", but all profiles already have proxy_pool configured; skipping env proxy.";
    }
  }

  void parse_configs(const json::value& jv) {
    if (!jv.is_object()) {
      throw std::invalid_argument(
          "Httpclient config root must be an object (map of profiles).");
    }

    const auto& root = jv.as_object();
    if (looks_like_single_config(root)) {
      configs_.emplace("default", json::value_to<HttpclientConfig>(jv));
      ordered_names_.push_back("default");
      default_name_ = "default";
      return;
    }

    for (const auto& kv : root) {
      if (!kv.value().is_object()) {
        throw std::invalid_argument(
            "Each httpclient config entry must be an object.");
      }
      const std::string key = kv.key_c_str();
      if (configs_.count(key) > 0) {
        throw std::invalid_argument("Duplicate httpclient config entry: " +
                                    key);
      }
      configs_.emplace(key, json::value_to<HttpclientConfig>(kv.value()));
      ordered_names_.push_back(key);
    }

    if (configs_.empty()) {
      throw std::invalid_argument("No httpclient configurations provided.");
    }

    auto it = configs_.find("default");
    if (it != configs_.end()) {
      default_name_ = it->first;
    } else {
      default_name_ = ordered_names_.front();
    }
  }

  static bool looks_like_single_config(const json::object& jo) {
    static constexpr std::array<std::string_view, 6> known_keys = {
        "threads_num",    "ssl_method",     "verify_paths",
        "certificates",   "certificate_files", "proxy_pool"};
    for (auto key : known_keys) {
      if (jo.if_contains(key.data())) {
        return true;
      }
    }
    return false;
  }
};

}  // namespace cjj365

namespace std {
template <>
struct hash<cjj365::ProxySetting> {
  std::size_t operator()(const cjj365::ProxySetting& p) const {
    std::size_t h1 = std::hash<std::string>()(p.host);
    std::size_t h2 = std::hash<std::string>()(p.port);
    std::size_t h3 = std::hash<std::string>()(p.username);
    std::size_t h4 = std::hash<std::string>()(p.password);
    // Combine hashes (example hash combining strategy)
    return (((h1 ^ (h2 << 1)) >> 1) ^ (h3 << 1)) ^ (h4 << 1);
  }
};
}  // namespace std
