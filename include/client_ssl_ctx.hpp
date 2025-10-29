#pragma once

// Do not include OpenSSL headers directly in public headers.

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include "http_client_config_provider.hpp"

namespace asio = boost::asio;
namespace ssl = asio::ssl;

namespace cjj365 {

void load_platform_root_certificates(ssl::context& ctx);

// DI-friendly SSL context configured from IHttpclientConfigProvider.
class ClientSSLContext {
 public:
  explicit ClientSSLContext(const IHttpclientConfigProvider& config_provider)
      : ctx_(config_provider.get().get_ssl_method()) {
    const auto& cfg = config_provider.get();

    if (cfg.get_default_verify_path()) {
      ctx_.set_default_verify_paths();
    }
    for (const auto& p : cfg.get_verify_paths()) {
      ctx_.add_verify_path(p);
    }
    for (const auto& f : cfg.get_certificate_files()) {
      if (!f.cert_path.empty()) {
        ctx_.load_verify_file(f.cert_path);
      }
    }
    for (const auto& c : cfg.get_certificates()) {
      if (!c.cert_content.empty()) {
        ctx_.add_certificate_authority(asio::buffer(c.cert_content));
      }
    }

    ctx_.set_verify_mode(cfg.get_insecure_skip_verify() ? ssl::verify_none
                                                        : ssl::verify_peer);

  load_platform_root_certificates(ctx_);
  }

  ssl::context& context() noexcept { return ctx_; }
  const ssl::context& context() const noexcept { return ctx_; }

  // Optional: allow adding CA PEMs programmatically (used in tests/tools).
  void add_certificate_authority(const std::string& pem_str) {
    if (pem_str.empty()) {
      throw std::runtime_error("Certificate authority string is empty.");
    }
    ctx_.add_certificate_authority(
        asio::buffer(pem_str.data(), pem_str.size()));
  }

 private:
  ssl::context ctx_;
};
}  // namespace cjj365
