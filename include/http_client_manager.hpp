#pragma once

#include <boost/asio.hpp>
#include <memory>

#include "beast_connection_pool.hpp"
#include "client_ssl_ctx.hpp"
#include "http_client_config_provider.hpp"
#include "http_session.hpp"
#include "http_session_pooled.hpp"

namespace asio = boost::asio;

namespace client_async {

class HttpClientManager {
 private:
  std::unique_ptr<asio::io_context> ioc;
  cjj365::ClientSSLContext& client_ssl_ctx;
  int threads_;
  std::vector<std::thread> thread_pool;
  std::unique_ptr<
      boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>
      work_guard;
  std::unique_ptr<beast_pool::ConnectionPool> pool_;
  std::atomic<bool> stopped_{false};

 public:
  HttpClientManager(cjj365::ClientSSLContext& ctx,
                    cjj365::IHttpclientConfigProvider& config_provider)
      : client_ssl_ctx(ctx), threads_(config_provider.get().get_threads_num()) {
    ioc = std::make_unique<asio::io_context>(threads_);
    work_guard = std::make_unique<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>>(
        boost::asio::make_work_guard(*ioc));
    // Initialize a shared connection pool (defaults are fine; can be extended)
    pool_ = std::make_unique<beast_pool::ConnectionPool>(
        *ioc, beast_pool::PoolConfig{}, &client_ssl_ctx.context());
    for (size_t i = 0; i < threads_; ++i) {
      thread_pool.emplace_back([this] { ioc->run(); });
    }
  }

  ~HttpClientManager() { stop(); }

  void stop() {
    if (stopped_.exchange(true)) return;  // already stopped
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
      const cjj365::ProxySetting* proxy_setting = nullptr) {
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

  // New: pooled variant (keeps existing APIs intact)
  template <class RequestBody, class ResponseBody>
  void http_request_pooled(
      const urls::url_view& url_input,
      http::request<RequestBody, http::basic_fields<std::allocator<char>>>&&
          req,
      std::function<
          void(std::optional<http::response<
                   ResponseBody, http::basic_fields<std::allocator<char>>>>&&,
               int)>&& callback,
      HttpClientRequestParams&& /*params*/ = {},
      const cjj365::ProxySetting* proxy_setting = nullptr) {
    // Build origin for pool acquisition
    urls::url url(url_input);
    beast_pool::Origin origin;
    origin.scheme = std::string(url.scheme());
    origin.host = std::string(url.host());
    if (url.has_port()) {
      origin.port = static_cast<std::uint16_t>(url.port_number());
    } else {
      origin.port = (origin.scheme == "https") ? 443 : 80;
    }

    std::optional<typename client_async::http_session_pooled<
        RequestBody, ResponseBody, std::allocator<char>>::ProxySetting>
        proxy;
    if (proxy_setting) {
      proxy = {proxy_setting->host, proxy_setting->port,
               proxy_setting->username, proxy_setting->password};
    }

    using Pooled = client_async::http_session_pooled<RequestBody, ResponseBody,
                                                     std::allocator<char>>;
    auto session =
        std::make_shared<Pooled>(*pool_, std::move(origin), std::move(proxy));
    session->set_request(std::move(req));
    session->run(std::move(callback));
  }
};

// EXTERN_CLIENTPOOL_HTTP_REQUEST(http::string_body, http::string_body)
// EXTERN_CLIENTPOOL_HTTP_REQUEST(http::empty_body, http::string_body)
// EXTERN_CLIENTPOOL_HTTP_REQUEST(http::file_body, http::empty_body)

}  // namespace client_async
