#pragma once

#include <bits/chrono.h>
#include <fmt/base.h>
#include <fmt/format.h>
#include <openssl/err.h>
#include <openssl/ssl3.h>
#include <openssl/types.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "aliases.hpp"
#include "base64.h"
#include "proxy_pool.hpp"

using tcp = boost::asio::ip::tcp;  // from <boost/asio/ip/tcp.hpp>

namespace client_async {
//------------------------------------------------------------------------------

struct HttpClientRequestParams {
  std::string body_file = "";
  bool follow_redirect = true;
  bool no_modify_req = false;
  std::chrono::seconds timeout = std::chrono::seconds(30);
};

// Performs an HTTP GET and prints the response
template <class Derived, class RequestBody, class ResponseBody, class Allocator>
class session {
 public:
  using response_t = std::optional<
      http::response<ResponseBody, http::basic_fields<Allocator>>>;
  using callback_t = std::function<void(response_t&&, int error_code)>;
  // Objects are constructed with a strand to
  // ensure that handlers do not execute concurrently.
  Derived& derived() { return static_cast<Derived&>(*this); }

  explicit session(
      net::io_context& ioc,  //
      // const std::string& url,            //
      urls::url&& url,                   //
      HttpClientRequestParams&& params,  //
      callback_t&& callback,             //
      const std::string& default_port,   //
      const std::optional<ProxySetting>& proxy_setting = std::nullopt)
      : ioc_(ioc),
        resolver_(net::make_strand(ioc)),
        url_(std::move(url)),
        body_file_(params.body_file),
        callback_(std::move(callback)),
        default_port_(default_port),
        timeout_(params.timeout),
        no_modify_req_(params.no_modify_req),
        proxy_setting_(proxy_setting) {}

  // Start the asynchronous operation
  void run() {
    std::string_view port = this->url_.port();
    if (port.empty()) {
      port = default_port_;
    }
    if (no_modify_req_) {
      return do_resolve(this->url_.host(), port);
    }
    if (port.empty()) {
      req_.set(http::field::host, this->url_.host());
    } else {
      req_.set(http::field::host, this->url_.host() + ":" + std::string(port));
    }
    // Look up the domain name
    if (proxy_setting_.has_value()) {
      do_resolve_proxy();
    } else {
      do_resolve(this->url_.host(), port);
    }
  }

  void do_resolve_proxy() {
    resolver_.async_resolve(
        proxy_setting_->host, proxy_setting_->port,
        [self = derived().shared_from_this()](
            beast::error_code ec, tcp::resolver::results_type results) {
          if (ec) {
            BOOST_LOG_SEV(self->lg, trivial::error)
                << "resolve: " << ec.message();
            self->callback_(std::nullopt, 1);
          } else {
            self->do_connect_proxy_server(results);
          }
        });
  }

  void do_connect_proxy_server(tcp::resolver::results_type results) {
    // Set a timeout on the operation
    BOOST_LOG_SEV(lg, trivial::debug)
        << "before async_connect to proxy server.";
    proxy_stream_.emplace(ioc_);
    // beast::get_lowest_layer(derived().stream())
    //     .expires_after(std::chrono::seconds(30));
    proxy_stream_->expires_after(std::chrono::seconds(30));

    // Make the connection on the IP address we get from a lookup
    // beast::get_lowest_layer(derived().stream())
    proxy_stream_->async_connect(
        results,
        [self = derived().shared_from_this()](
            beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
          if (ec) {
            BOOST_LOG_SEV(self->lg, trivial::error)
                << "proxy connect: " << ec.message();
            self->callback_(std::nullopt, 1);
          } else {
            self->do_request_proxy();
          }
        });
  }

  void do_request_proxy() {
    urls::url_view urlv(this->url_);
    std::string_view port = urlv.port();
    if (port.empty()) {
      port = default_port_;
    }
    std::string url = fmt::format("{}:{}", urlv.host(), port);
    proxy_req_.emplace(http::verb::connect, url, 11);
    proxy_req_->set(http::field::host, urlv.host());
    if (!(proxy_setting_->username.empty() ||
          proxy_setting_->password.empty())) {
      std::string auth =
          proxy_setting_->username + ":" + proxy_setting_->password;
      proxy_req_->set(http::field::proxy_authorization,
                      fmt::format("Basic {}", base64_encode(auth)));
    }

    // beast::get_lowest_layer(derived().stream())
    //     .expires_after(std::chrono::seconds(30));
    proxy_stream_->expires_after(std::chrono::seconds(30));
    BOOST_LOG_SEV(lg, trivial::debug)
        << "proxy request: " << proxy_req_.value();
    http::async_write(proxy_stream_.value(), proxy_req_.value(),
                      [self = derived().shared_from_this()](
                          beast::error_code ec, std::size_t bytes_transferred) {
                        BOOST_LOG_SEV(self->lg, trivial::debug)
                            << "proxy request done, bytes transferred: "
                            << bytes_transferred;
                        if (ec) {
                          BOOST_LOG_SEV(self->lg, trivial::error)
                              << "write to proxy server: " << ec.message();
                          self->callback_(std::nullopt, 2);
                        } else {
                          self->do_read_proxy_response();
                        }
                      });
  }

  void do_read_proxy_response() {
    proxy_response_parser_.emplace();
    // beast::get_lowest_layer(derived().stream())
    //     .expires_after(std::chrono::seconds(30));
    proxy_stream_->expires_after(std::chrono::seconds(30));
    http::async_read_header(
        proxy_stream_.value(), buffer_, *proxy_response_parser_,
        [self = derived().shared_from_this()](beast::error_code ec,
                                              std::size_t bytes_transferred) {
          BOOST_LOG_SEV(self->lg, trivial::debug)
              << "proxy response: " << self->proxy_response_parser_->get();
          if (ec) {
            BOOST_LOG_SEV(self->lg, trivial::error)
                << "read from proxy server: " << ec.message();
            self->callback_(std::nullopt, 3);
          } else {
            // self->do_request();
            if (self->proxy_response_parser_->get().result_int() != 200) {
              BOOST_LOG_SEV(self->lg, trivial::error)
                  << "proxy response: "
                  << self->proxy_response_parser_->get().result_int();
              self->callback_(std::nullopt, 4);
              return;
            } else {
              std::cout << "connected to proxy server." << std::endl;
              self->derived().replace_stream(
                  std::move(self->proxy_stream_.value()));
              self->after_connect();
            }
          }
        });
  }

  void do_resolve(const std::string& host, std::string_view port) {
    resolver_.async_resolve(
        host, port,
        [self = derived().shared_from_this()](
            beast::error_code ec, tcp::resolver::results_type results) {
          if (ec) {
            BOOST_LOG_SEV(self->lg, trivial::error)
                << "resolve: " << ec.message();
            self->callback_(std::nullopt, 1);
          } else {
            self->do_connect(results);
          }
        });
  }

  void do_connect(tcp::resolver::results_type results) {
    // Set a timeout on the operation
    beast::get_lowest_layer(derived().stream())
        .expires_after(std::chrono::seconds(30));
    // Make the connection on the IP address we get from a lookup
    beast::get_lowest_layer(derived().stream())
        .async_connect(results,
                       [self = derived().shared_from_this()](
                           beast::error_code ec,
                           tcp::resolver::results_type::endpoint_type) {
                         if (ec) {
                           BOOST_LOG_SEV(self->lg, trivial::error)
                               << "connect: " << ec.message();
                           self->callback_(std::nullopt, 5);
                         } else {
                           self->derived().after_connect();
                         }
                       });
  }

  void do_request() {
    // Receive the HTTP response
    http::async_write(derived().stream(), req_,
                      [self = derived().shared_from_this()](
                          beast::error_code ec, size_t bytes_transferred) {
                        if (ec) {
                          BOOST_LOG_SEV(self->lg, trivial::error)
                              << "write: " << ec.message();
                          self->callback_(std::nullopt, 6);
                        } else {
                          self->do_read();
                        }
                      });
  }

  void do_read() {
    this->parser_.emplace();
    if constexpr (std::is_same_v<ResponseBody, http::empty_body>) {
      this->parser_->body_limit(0);
      this->parser_->skip(true);
    } else if constexpr (std::is_same_v<ResponseBody, http::string_body>) {
      this->parser_->body_limit(1024 * 1024 * 4);
    } else if constexpr (std::is_same_v<ResponseBody, http::file_body>) {
      if (!this->body_file_.has_value() || this->body_file_->empty()) {
        BOOST_LOG_SEV(this->lg, trivial::error) << "body_file_ is not set.";
        this->callback_(std::nullopt, 7);
        return;
      }
      http::file_body::value_type body;
      beast::error_code ec;
      body.open(this->body_file_->c_str(), beast::file_mode::write, ec);
      parser_->get().body() = std::move(body);
      this->parser_->body_limit(static_cast<std::int64_t>(1024) * 1024 * 1024 *
                                10);
    }

    auto cb = [self = derived().shared_from_this()](beast::error_code ec,
                                                    size_t bytes_transferred) {
      if (ec) {
        if (ec == http::error::body_limit) {
          // Special handling for body limit errors
          BOOST_LOG_SEV(self->lg, trivial::warning)
              << "Body limit exceeded for empty_body response";
          // Try to recover by forcing empty body
          if constexpr (std::is_same_v<ResponseBody, http::empty_body>) {
            self->parser_->get().body() = typename ResponseBody::value_type();
            self->callback_(self->parser_->release(), 0);
            return;
          }
        }
        BOOST_LOG_SEV(self->lg, trivial::error) << "read: " << ec.message();
        self->callback_(self->parser_->release(), 8);
      } else {
        self->callback_(self->parser_->release(), 0);
      }
    };
    // set time out before read.
    beast::get_lowest_layer(derived().stream()).expires_after(this->timeout_);
    if constexpr (std::is_same_v<ResponseBody, http::empty_body>) {
      http::async_read_header(derived().stream(), buffer_,
                              this->parser_.value(), cb);
    } else {
      http::async_read(derived().stream(), buffer_, this->parser_.value(), cb);
    }
  }

  void set_req(
      http::request<RequestBody, http::basic_fields<Allocator>>&& req) {
    req_ = std::move(req);
  }

 private:
  net::io_context& ioc_;
  tcp::resolver resolver_;
  beast::flat_buffer buffer_;  // (Must persist between reads)
  http::request<RequestBody, http::basic_fields<Allocator>> req_;
  std::string default_port_;
  std::optional<http::response_parser<ResponseBody>> parser_;
  std::optional<http::response_parser<http::empty_body>> proxy_response_parser_;
  bool no_modify_req_ = false;
  std::optional<beast::tcp_stream> proxy_stream_;
  std::optional<http::request<http::empty_body>> proxy_req_;
  std::optional<std::string> body_file_;
  std::optional<ProxySetting> proxy_setting_;
  std::chrono::seconds timeout_{30};

 protected:
  // std::string url_;
  urls::url url_;
  callback_t callback_;
  logsrc::severity_logger<trivial::severity_level> lg;
};

// Performs an HTTP GET and prints the response
template <class RequestBody, class ResponseBody, class Allocator>
class session_ssl
    : public session<session_ssl<RequestBody, ResponseBody, Allocator>,
                     RequestBody, ResponseBody, Allocator>,
      public std::enable_shared_from_this<
          session_ssl<RequestBody, ResponseBody, Allocator>> {
  std::unique_ptr<ssl::stream<beast::tcp_stream>> stream_;
  ssl::context& ctx_;

 public:
  using response_t = std::optional<
      http::response<ResponseBody, http::basic_fields<Allocator>>>;
  using callback_t = std::function<void(response_t, int)>;
  explicit session_ssl(
      net::io_context& ioc,  //
      ssl::context& ctx,     //
      // const std::string& url,            //
      urls::url&& url,                   //
      HttpClientRequestParams&& params,  //
      callback_t&& callback,             //
      const std::optional<ProxySetting>& proxy_setting = std::nullopt)
      : session<session_ssl, RequestBody, ResponseBody, Allocator>(
            ioc, std::move(url), std::move(params), std::move(callback), "443",
            proxy_setting),
        ctx_(ctx),
        stream_(std::make_unique<ssl::stream<beast::tcp_stream>>(ioc, ctx)) {}

  ssl::stream<beast::tcp_stream>& stream() { return *stream_; }

  void replace_stream(beast::tcp_stream&& stream) {
    stream_ = std::make_unique<ssl::stream<beast::tcp_stream>>(
        std::move(stream), ctx_);
  }

  void after_connect() {
    boost::urls::url_view urlv(this->url_);
    std::string host = urlv.host();
    if (!SSL_set_tlsext_host_name(stream_->native_handle(), host.c_str())) {
      beast::error_code ec{static_cast<int>(::ERR_get_error()),
                           net::error::get_ssl_category()};
      BOOST_LOG_SEV(this->lg, trivial::error)
          << "after connect, set_tlsext_host_name got error: " << ec.message()
          << " host: " << host;
      return this->callback_(std::nullopt, 9);
    }
    stream_->async_handshake(
        ssl::stream_base::client,
        [self = this->shared_from_this()](beast::error_code ec) {
          if (ec) {
            BOOST_LOG_SEV(self->lg, trivial::error)
                << "after connect, handshake got error: " << ec.message();
            return self->callback_(std::nullopt, 10);
          } else {
            self->do_request();
          }
        });
  }
  void do_eof() {
    // Set the timeout.
    beast::get_lowest_layer(*stream_).expires_after(std::chrono::seconds(30));

    // Perform the SSL shutdown
    stream_->async_shutdown(
        [self = this->shared_from_this()](beast::error_code ec) {
          self->on_shutdown(ec);
        });
  }

  void on_shutdown(beast::error_code ec) {
    // ssl::error::stream_truncated, also known as an SSL "short read",
    // indicates the peer closed the connection without performing the
    // required closing handshake (for example, Google does this to
    // improve performance). Generally this can be a security issue,
    // but if your communication protocol is self-terminated (as
    // it is with both HTTP and WebSocket) then you may simply
    // ignore the lack of close_notify.
    //
    // https://github.com/boostorg/beast/issues/38
    //
    // https://security.stackexchange.com/questions/91435/how-to-handle-a-malicious-ssl-tls-shutdown
    //
    // When a short read would cut off the end of an HTTP message,
    // Beast returns the error beast::http::error::partial_message.
    // Therefore, if we see a short read here, it has occurred
    // after the message has been completed, so it is safe to ignore it.

    if (ec != net::ssl::error::stream_truncated) {
      BOOST_LOG_SEV(this->lg, trivial::error) << "shutdown: " << ec.message();
    }
  }
};

template <class RequestBody, class ResponseBody, class Allocator>
class session_plain
    : public session<session_plain<RequestBody, ResponseBody, Allocator>,
                     RequestBody, ResponseBody, Allocator>,
      public std::enable_shared_from_this<
          session_plain<RequestBody, ResponseBody, Allocator>> {
  std::unique_ptr<beast::tcp_stream> stream_;

 public:
  using response_t = std::optional<
      http::response<ResponseBody, http::basic_fields<Allocator>>>;
  using callback_t = std::function<void(response_t&&, int)>;
  explicit session_plain(
      net::io_context& ioc,  //
      // const std::string& url,            //
      urls::url&& url,                   //
      HttpClientRequestParams&& params,  //
      callback_t&& callback,
      const std::optional<ProxySetting>& proxy_setting = std::nullopt)
      : session<session_plain<RequestBody, ResponseBody, Allocator>,
                RequestBody, ResponseBody, Allocator>(
            ioc, std::move(url), std::move(params), std::move(callback), "80",
            proxy_setting),
        stream_(std::make_unique<beast::tcp_stream>(ioc)) {}

  void do_eof() {
    // Gracefully close the socket
    beast::error_code ec;
    if (stream_->socket().shutdown(tcp::socket::shutdown_both, ec)) {
      BOOST_LOG_SEV(this->lg, trivial::error)
          << "Socket shutdown failed: " << ec.message();
    }
  }

  void after_connect() { this->do_request(); }
  beast::tcp_stream& stream() { return *stream_; }

  void replace_stream(beast::tcp_stream&& stream) {
    stream_ = std::make_unique<beast::tcp_stream>(std::move(stream));
  }

  void on_shutdown(beast::error_code ec) {
    if (ec) {
      BOOST_LOG_SEV(this->lg, trivial::error) << "shutdown: " << ec.message();
    }
  }
};

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

  // template <class ResponseBody>
  // void http_get(
  //     // const std::string& url,
  //     const urls::url_view& url_input,
  //     std::function<
  //         void(std::optional<http::response<
  //                  ResponseBody, http::basic_fields<std::allocator<char>>>>,
  //              int)>&& callback,
  //     HttpClientRequestParams&& params = {},
  //     const std::optional<ProxySetting>& proxy_setting = std::nullopt) {
  //   urls::url url(url_input);  // Explicit construction for validation
  //   // if (url.find("https") == 0) {
  //   if (url.scheme() == "https") {
  //     auto session = std::make_shared<
  //         session_ssl<http::empty_body, ResponseBody, std::allocator<char>>>(
  //         this->ioc, this->client_ssl_ctx, std::move(url), std::move(params),
  //         std::move(callback), proxy_setting);
  //     session->run(http::verb::get);
  //   } else {
  //     auto session = std::make_shared<
  //         session_plain<http::empty_body, ResponseBody,
  //         std::allocator<char>>>( this->ioc, std::move(url),
  //         std::move(params), std::move(callback), proxy_setting);
  //     session->run(http::verb::get);
  //   }
  // }

  // void http_get_status(
  //     // const std::string& url, //
  //     const urls::url_view& url_input,
  //     std::function<void(http::status)>&& callback,  //
  //     const std::optional<ProxySetting>& proxy_setting = std::nullopt) {
  //   urls::url url(url_input);  // Explicit construction for validation
  //   // if (url.find("https") == 0) {
  //   if (url.scheme() == "https") {
  //     auto session = std::make_shared<session_ssl<
  //         http::empty_body, http::empty_body, std::allocator<char>>>(
  //         this->ioc, this->client_ssl_ctx, std::move(url),
  //         HttpClientRequestParams(),
  //         [callback = std::move(callback)](
  //             std::optional<http::response<
  //                 http::empty_body,
  //                 http::basic_fields<std::allocator<char>>>> resp,
  //             int ec) {
  //           if (ec != 0) {
  //             if (resp.has_value()) {
  //               return callback(resp->result());
  //             } else {
  //               callback(http::status::unknown);
  //             }
  //           } else {
  //             callback(resp->result());
  //           }
  //         },
  //         proxy_setting);
  //     session->run(http::verb::get);
  //   } else {
  //     auto session = std::make_shared<session_plain<
  //         http::empty_body, http::empty_body, std::allocator<char>>>(
  //         this->ioc, std::move(url), HttpClientRequestParams(),
  //         [callback = std::move(callback)](
  //             std::optional<http::response<
  //                 http::empty_body,
  //                 http::basic_fields<std::allocator<char>>>> resp,
  //             int ec) {
  //           if (ec != 0) {
  //             if (resp.has_value()) {
  //               return callback(resp->result());
  //             } else {
  //               callback(http::status::unknown);
  //             }
  //           } else {
  //             callback(resp->result());
  //           }
  //         },
  //         proxy_setting);
  //     session->run(http::verb::get);
  //   }
  // }

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
    urls::url url(url_input);  // Explicit construction for validation
                               // if (url.find("https") == 0) {
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
}  // namespace client_async
