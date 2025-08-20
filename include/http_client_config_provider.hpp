#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/json.hpp>
#include <thread>
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

  bool operator==(const ProxySetting& other) const {
    return host == other.host && port == other.port &&
           username == other.username && password == other.password;
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
          proxy.port = std::to_string(port_p->as_int64());
        } else {
          throw std::invalid_argument("Invalid port type in ProxySetting");
        }
      }
      proxy.username = jo->at("username").as_string().c_str();
      proxy.password = jo->at("password").as_string().c_str();
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
  std::vector<std::string> verify_paths;
  std::vector<HttpclientCertificate> certificates;
  std::vector<HttpclientCertificateFile> certificate_files;
  std::vector<cjj365::ProxySetting> proxy_pool;

 public:
  friend HttpclientConfig tag_invoke(
      const json::value_to_tag<HttpclientConfig>&, const json::value& jv) {
    HttpclientConfig config;
    if (auto* jo = jv.if_object()) {
      if (auto* ssl_method_p = jo->if_contains("ssl_method")) {
        config.ssl_method =
            ssl_method_from_string(ssl_method_p->as_string().c_str());
      }
      config.threads_num = jv.at("threads_num").to_number<int>();
      if (config.threads_num < 0) {
        throw std::invalid_argument("threads_num must be non-negative");
      }
      if (auto* verify_paths_p = jo->if_contains("verify_paths")) {
        config.verify_paths =
            json::value_to<std::vector<std::string>>(*verify_paths_p);
      }
      if (auto* certificates_p = jo->if_contains("certificates")) {
        config.certificates =
            json::value_to<std::vector<HttpclientCertificate>>(*certificates_p);
      }
      if (auto* certificate_files_p = jo->if_contains("certificate_files")) {
        config.certificate_files =
            json::value_to<std::vector<HttpclientCertificateFile>>(
                *certificate_files_p);
      }
      if (auto* proxy_pool_p = jo->if_contains("proxy_pool")) {
        config.proxy_pool =
            json::value_to<std::vector<cjj365::ProxySetting>>(*proxy_pool_p);
      }
      return config;
    }
    throw std::invalid_argument("Invalid JSON for HttpclientConfig");
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
};

class HttpclientConfigProviderFile : public IHttpclientConfigProvider {
  HttpclientConfig config_;

 public:
  explicit HttpclientConfigProviderFile(cjj365::AppProperties& app_properties,
                                        cjj365::ConfigSources& config_sources) {
    auto r = config_sources.json_content("httpclient_config");
    if (r.is_err()) {
      throw std::runtime_error("Failed to load HTTP client config: " +
                               r.error().what);
    } else {
      json::value jv = r.value();
      jsonutil::substitue_envs(jv, app_properties.properties);
      config_ = json::value_to<HttpclientConfig>(std::move(jv));
    }
  }

  const HttpclientConfig& get() const { return config_; }
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