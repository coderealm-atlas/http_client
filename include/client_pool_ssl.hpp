#pragma once

#include <memory>

#include "client_ssl_ctx.hpp"
// #include "explicit_instantiations.hpp"
#include "http_session.hpp"

namespace client_async {

class ClientPoolSsl {
 private:
  // The io_context is required for all I/O
  std::unique_ptr<net::io_context> ioc;
  // The SSL context is required, and holds certificates
  cjj365::ClientSSLContextWrapper& client_ssl_ctx;
  int threads_;
  std::vector<std::thread> thread_pool;
  std::unique_ptr<
      boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>
      work_guard;

 public:
  ClientPoolSsl(cjj365::ClientSSLContextWrapper& ctx) : client_ssl_ctx(ctx) {}

  ClientPoolSsl& start(int threads = 2) {
    ioc = std::make_unique<net::io_context>(threads);
    work_guard = std::make_unique<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>>(
        boost::asio::make_work_guard(*ioc));
    threads_ = threads;
    for (size_t i = 0; i < threads; ++i) {
      thread_pool.emplace_back([this] {
        ioc->run();  // Each thread runs the io_context
      });
    }
    return *this;
  }

  void stop() {
    work_guard->reset();
    ioc->stop();
    for (auto& t : thread_pool) {
      if (t.joinable()) {
        t.join();
      }
    }
  }
  template <class RequestBody, class ResponseBody>
  void http_request(
      const urls::url_view& url_input,
      http::request<RequestBody, http::basic_fields<std::allocator<char>>>&&
          req,
      std::function<
          void(std::optional<http::response<
                   ResponseBody, http::basic_fields<std::allocator<char>>>>&&,
               int)>&& callback,
      HttpClientRequestParams&& params = {},
      const ProxySetting* proxy_setting = nullptr) {
    urls::url url(url_input);
    if (url.scheme() == "https") {
      auto session = std::make_shared<
          session_ssl<RequestBody, ResponseBody, std::allocator<char>>>(
          *(this->ioc), this->client_ssl_ctx.context(), std::move(url),
          std::move(params), std::move(callback), proxy_setting);
      session->set_req(std::move(req));
      session->run();
    } else {
      auto session = std::make_shared<
          session_plain<RequestBody, ResponseBody, std::allocator<char>>>(
          *(this->ioc), std::move(url), std::move(params), std::move(callback),
          proxy_setting);
      session->set_req(std::move(req));
      session->run();
    }
  }
};

// EXTERN_CLIENTPOOL_HTTP_REQUEST(http::string_body, http::string_body)
// EXTERN_CLIENTPOOL_HTTP_REQUEST(http::empty_body, http::string_body)
// EXTERN_CLIENTPOOL_HTTP_REQUEST(http::file_body, http::empty_body)

}  // namespace client_async
