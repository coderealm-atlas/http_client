#pragma once

#include <openssl/ssl.h>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>

#include "http_client_config_provider.hpp"

namespace asio = boost::asio;
namespace ssl = asio::ssl;

namespace cjj365 {

// Callback function to capture SSL debug messages

// inline ssl::context client_ssl_ctx{ssl::context::tls_client};
// inline ssl::context client_ssl_ctx{ssl::context::tls};
inline ssl::context& client_ssl_ctx() {
  static ssl::context client_ssl_ctx{ssl::context::tlsv12};
  static bool init = false;
  if (!init) {
    init = true;
    client_ssl_ctx.set_default_verify_paths();
    // std::string pem_str = "...";
    // asio::const_buffer cb{pem_str.data(), pem_str.size()};
    // client_ssl_ctx.add_certificate_authority(cb);
    client_ssl_ctx.set_verify_mode(boost::asio::ssl::verify_peer);

    // Set the minimum and maximum SSL/TLS versions to use
    // client_ssl_ctx.set_options(ssl::context::default_workarounds |
    //                 ssl::context::no_sslv2 |
    //                 ssl::context::no_sslv3 |
    //                 ssl::context::no_tlsv1 |
    //                 ssl::context::no_tlsv1_1);

    // Set the protocol to a modern version, such as TLS 1.2 or 1.3
    // client_ssl_ctx.set_options(boost::asio::ssl::context::tlsv12);
    // Set SSL message callback for debugging
    // SSL_CTX* ssl_ctx = client_ssl_ctx.native_handle();
    // SSL_CTX_set_msg_callback(ssl_ctx, ssl_msg_callback);
  }
  return client_ssl_ctx;
}

inline ssl::context& client_ssl_ctx_no_verify() {
  static ssl::context client_ssl_ctx{ssl::context::tlsv12_client};
  static bool init = false;
  if (!init) {
    init = true;
    client_ssl_ctx.set_default_verify_paths();
    client_ssl_ctx.set_verify_mode(boost::asio::ssl::verify_none);
    // client_ssl_ctx.set_verify_mode(boost::asio::ssl::verify_peer);

    // Set the minimum and maximum SSL/TLS versions to use
    // client_ssl_ctx.set_options(ssl::context::default_workarounds |
    //                 ssl::context::no_sslv2 |
    //                 ssl::context::no_sslv3 |
    //                 ssl::context::no_tlsv1 |
    //                 ssl::context::no_tlsv1_1);

    // Set the protocol to a modern version, such as TLS 1.2 or 1.3
    // client_ssl_ctx.set_options(boost::asio::ssl::context::tlsv12);
    // Set SSL message callback for debugging
    // SSL_CTX* ssl_ctx = client_ssl_ctx.native_handle();
    // SSL_CTX_set_msg_callback(ssl_ctx, ssl_msg_callback);
  }
  return client_ssl_ctx;
}

class ClientSSLContextWrapper {
  static void ssl_msg_callback(int write_p, int version, int content_type,
                               const void* buf, size_t len, SSL* ssl,
                               void* ctx) {
    std::cout << "SSL Message (write_p=" << write_p << ", version=" << version
              << ", content_type=" << content_type << ", len=" << len
              << "): " << std::string((const char*)buf, len) << std::endl;
    // Print any OpenSSL errors
    ERR_print_errors_fp(stderr);
  }

 public:
  ClientSSLContextWrapper(cjj365::IHttpclientConfigProvider& config_provider)
      : ctx_(config_provider.get().get_ssl_method()) {
    // Set SSL message callback for debugging
    // SSL_CTX* ssl_ctx = ctx_.native_handle();
    // SSL_CTX_set_msg_callback(ssl_ctx, ssl_msg_callback);
    ctx_.set_default_verify_paths();
    ctx_.set_verify_mode(boost::asio::ssl::verify_peer);
  }

  // Get the underlying SSL context
  ssl::context& context() { return ctx_; }

  void add_certificate_authority(const std::string& pem_str) {
    if (pem_str.empty()) {
      throw std::runtime_error("Certificate authority string is empty.");
    }
    asio::const_buffer cb{pem_str.data(), pem_str.size()};
    ctx_.add_certificate_authority(cb);
  }

 private:
  ssl::context ctx_;
};
}  // namespace cjj365
