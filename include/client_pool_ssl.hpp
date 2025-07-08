#pragma once

#include "explicit_instantiations.hpp"
#include "http_session.hpp"

namespace client_async {

class ClientPoolSsl {
 private:
  // The io_context is required for all I/O
  net::io_context ioc;
  // The SSL context is required, and holds certificates
  ssl::context& client_ssl_ctx;
  int threads;
  std::vector<std::thread> thread_pool;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type>
      work_guard;

 public:
  ClientPoolSsl(int threads, ssl::context& ctx)
      : threads(threads),
        ioc{threads},
        client_ssl_ctx(ctx),
        work_guard(boost::asio::make_work_guard(ioc)) {
    client_ssl_ctx.set_default_verify_paths();
    client_ssl_ctx.set_verify_mode(boost::asio::ssl::verify_peer);
  }

  ClientPoolSsl& start() {
    for (size_t i = 0; i < threads; ++i) {
      thread_pool.emplace_back([this] {
        ioc.run();  // Each thread runs the io_context
      });
    }
    return *this;
  }

  void stop() {
    ioc.stop();
    for (auto& t : thread_pool) {
      if (t.joinable()) {
        t.join();
      }
    }
  }
  template <class RequestBody, class ResponseBody>
  void http_request(
      // const std::string& url,
      const urls::url_view& url_input,
      http::request<RequestBody, http::basic_fields<std::allocator<char>>>&&
          req,
      std::function<
          void(std::optional<http::response<
                   ResponseBody, http::basic_fields<std::allocator<char>>>>&&,
               int)>&& callback,
      HttpClientRequestParams&& params = {},
      const std::optional<ProxySetting>& proxy_setting = std::nullopt) {
    urls::url url(url_input);
    if (url.scheme() == "https") {
      auto session = std::make_shared<
          session_ssl<RequestBody, ResponseBody, std::allocator<char>>>(
          this->ioc, this->client_ssl_ctx, std::move(url), std::move(params),
          std::move(callback), proxy_setting);
      session->set_req(std::move(req));
      session->run();
    } else {
      auto session = std::make_shared<
          session_plain<RequestBody, ResponseBody, std::allocator<char>>>(
          this->ioc, std::move(url), std::move(params), std::move(callback),
          proxy_setting);
      session->set_req(std::move(req));
      session->run();
    }
  }
};

EXTERN_CLIENTPOOL_HTTP_REQUEST(http::string_body, http::string_body)
EXTERN_CLIENTPOOL_HTTP_REQUEST(http::empty_body, http::string_body)
EXTERN_CLIENTPOOL_HTTP_REQUEST(http::file_body, http::empty_body)

}  // namespace client_async