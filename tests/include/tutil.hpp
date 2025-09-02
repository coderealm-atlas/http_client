#pragma once
#include <boost/asio.hpp>      // IWYU pragma: keep
#include <boost/asio/ssl.hpp>  // IWYU pragma: keep
// #include "http_client_async.hpp"

namespace ssl = boost::asio::ssl;  // from <boost/asio/ssl.hpp>

namespace t {

inline std::unique_ptr<ssl::context> client_ssl_ctx() {
  auto ctx = std::make_unique<ssl::context>(ssl::context::tlsv12);
  ctx->set_default_verify_paths();
  return ctx;
}

// inline  client_async::HttpClientManager httpClientPool(){
//     return client_async::HttpClientManager(8, client_ssl_ctx());
// }

}  // namespace t