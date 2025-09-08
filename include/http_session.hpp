#pragma once

#include <boost/asio.hpp>      // IWYU pragma: keep
#include <boost/asio/ssl.hpp>  // IWYU pragma: keep
#include <boost/beast.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sources/global_logger_storage.hpp>
#include <boost/log/sources/logger.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/url.hpp>
#include <chrono>
#include <filesystem>
#include <optional>

#include "base64.h"
#include "http_client_config_provider.hpp"
// #include "explicit_instantiations.hpp"

namespace fs = std::filesystem;

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace ssl = boost::asio::ssl;
namespace urls = boost::urls;
namespace trivial = boost::log::trivial;
namespace logsrc = boost::log::sources;
using tcp = asio::ip::tcp;

namespace client_async {

struct HttpClientRequestParams {
  std::optional<fs::path> body_file = std::nullopt;
  bool follow_redirect = true;
  bool no_modify_req = false;
  std::chrono::seconds timeout = std::chrono::seconds(30);
  // Split timeouts (default 30s each). These complement 'timeout'.
  std::chrono::seconds resolve_timeout = std::chrono::seconds(30);
  std::chrono::seconds connect_timeout = std::chrono::seconds(30);
  std::chrono::seconds handshake_timeout = std::chrono::seconds(30);
  std::chrono::seconds io_timeout = std::chrono::seconds(30);
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

  explicit session(asio::io_context& ioc,             //
                   urls::url&& url,                   //
                   HttpClientRequestParams&& params,  //
                   callback_t&& callback,             //
                   const std::string& default_port,   //
                   const cjj365::ProxySetting* proxy_setting = nullptr)
      : ioc_(ioc),
        resolver_(asio::make_strand(ioc)),
        resolve_timer_(resolver_.get_executor()),
        no_modify_req_(params.no_modify_req),
        body_file_(params.body_file
                       ? std::make_optional(params.body_file->string())
                       : std::nullopt),
        proxy_setting_(proxy_setting),
        default_port_(default_port),
        timeout_(params.timeout),
        resolve_to_(params.resolve_timeout),
        connect_to_(params.connect_timeout),
        handshake_to_(params.handshake_timeout),
        io_to_(params.io_timeout),
        url_(std::move(url)),
        callback_(std::move(callback)) {}

  // Use the resolver strand's executor to construct streams safely
 protected:
  asio::any_io_executor executor() { return resolver_.get_executor(); }
  std::chrono::seconds op_timeout() const { return io_to_; }
  std::chrono::seconds resolve_timeout() const { return resolve_to_; }
  std::chrono::seconds connect_timeout() const { return connect_to_; }
  std::chrono::seconds handshake_timeout() const { return handshake_to_; }

  void deliver(response_t&& r, int code) noexcept {
    try {
      callback_(std::move(r), code);
    } catch (...) {
      // prevent exceptions escaping Asio handlers
    }
  }

 public:
  // Start the asynchronous operation
 public:
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
    if (proxy_setting_) {
      do_resolve_proxy();
    } else {
      do_resolve(this->url_.host(), port);
    }
  }

  void do_resolve_proxy() {
    // Start resolve timeout watchdog
    resolve_timer_.expires_after(this->resolve_timeout());
    resolve_timer_.async_wait(asio::bind_executor(
        resolver_.get_executor(), [self = derived().shared_from_this()](
                                      const boost::system::error_code& ec) {
          if (!ec) {
            // timeout fired
            self->resolver_.cancel();
          }
        }));

    resolver_.async_resolve(proxy_setting_->host, proxy_setting_->port,
                            [self = derived().shared_from_this()](
                                boost::beast::error_code ec,
                                asio::ip::tcp::resolver::results_type results) {
                              self->resolve_timer_.cancel();
                              if (ec) {
                                BOOST_LOG_SEV(self->lg, trivial::error)
                                    << "resolve: " << ec.message();
                                self->deliver(std::nullopt, 1);
                              } else {
                                self->do_connect_proxy_server(results);
                              }
                            });
  }

  void do_connect_proxy_server(asio::ip::tcp::resolver::results_type results) {
    // Set a timeout on the operation
    BOOST_LOG_SEV(lg, trivial::debug)
        << "before async_connect to proxy server.";
    proxy_stream_.emplace(executor());
    proxy_stream_->expires_after(this->op_timeout());

    // Make the connection on the IP address we get from a lookup
    // beast::get_lowest_layer(derived().stream())
    proxy_stream_->async_connect(
        results,
        [self = derived().shared_from_this()](
            beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
          if (ec) {
            BOOST_LOG_SEV(self->lg, trivial::error)
                << "proxy connect: " << ec.message();
            self->deliver(std::nullopt, 1);
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
    std::string url = std::format("{}:{}", urlv.host(), port);
    proxy_req_.emplace(http::verb::connect, url, 11);
    // Host header for CONNECT should be host:port
    proxy_req_->set(http::field::host, url);
    if (!(proxy_setting_->username.empty() ||
          proxy_setting_->password.empty())) {
      std::string auth =
          proxy_setting_->username + ":" + proxy_setting_->password;
      proxy_req_->set(http::field::proxy_authorization,
                      std::format("Basic {}", base64_encode(auth)));
    }

    proxy_stream_->expires_after(this->op_timeout());
    BOOST_LOG_SEV(lg, trivial::debug)
        << "proxy request: " << proxy_req_.value();
    http::async_write(
        proxy_stream_.value(), proxy_req_.value(),
        [self = derived().shared_from_this()](boost::beast::error_code ec,
                                              std::size_t bytes_transferred) {
          BOOST_LOG_SEV(self->lg, trivial::debug)
              << "proxy request done, bytes transferred: " << bytes_transferred;
          if (ec) {
            BOOST_LOG_SEV(self->lg, trivial::error)
                << "write to proxy server: " << ec.message();
            self->deliver(std::nullopt, 2);
          } else {
            self->do_read_proxy_response();
          }
        });
  }

  void do_read_proxy_response() {
    proxy_response_parser_.emplace();
    proxy_stream_->expires_after(this->op_timeout());
    http::async_read_header(
        proxy_stream_.value(), buffer_, *proxy_response_parser_,
        [self = derived().shared_from_this()](boost::beast::error_code ec,
                                              std::size_t bytes_transferred) {
          BOOST_LOG_SEV(self->lg, trivial::debug)
              << "proxy response: " << self->proxy_response_parser_->get();
          if (ec) {
            BOOST_LOG_SEV(self->lg, trivial::error)
                << "read from proxy server: " << ec.message();
            self->deliver(std::nullopt, 3);
          } else {
            // self->do_request();
            if (self->proxy_response_parser_->get().result_int() != 200) {
              BOOST_LOG_SEV(self->lg, trivial::error)
                  << "proxy response: "
                  << self->proxy_response_parser_->get().result_int();
              self->deliver(std::nullopt, 4);
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
    // Start resolve timeout watchdog
    resolve_timer_.expires_after(this->resolve_timeout());
    resolve_timer_.async_wait(asio::bind_executor(
        resolver_.get_executor(), [self = derived().shared_from_this()](
                                      const boost::system::error_code& ec) {
          if (!ec) {
            self->resolver_.cancel();
          }
        }));

    resolver_.async_resolve(host, port,
                            [self = derived().shared_from_this()](
                                boost::beast::error_code ec,
                                asio::ip::tcp::resolver::results_type results) {
                              self->resolve_timer_.cancel();
                              if (ec) {
                                BOOST_LOG_SEV(self->lg, trivial::error)
                                    << "resolve: " << ec.message();
                                self->deliver(std::nullopt, 1);
                              } else {
                                self->do_connect(results);
                              }
                            });
  }

 protected:
  void do_connect(asio::ip::tcp::resolver::results_type results) {
    // Set a timeout on the operation
    boost::beast::get_lowest_layer(derived().stream())
        .expires_after(this->connect_timeout());
    // Make the connection on the IP address we get from a lookup
    boost::beast::get_lowest_layer(derived().stream())
        .async_connect(
            results, [self = derived().shared_from_this()](
                         boost::beast::error_code ec,
                         asio::ip::tcp::resolver::results_type::endpoint_type) {
              if (ec) {
                BOOST_LOG_SEV(self->lg, trivial::error)
                    << "connect: " << ec.message();
                self->deliver(std::nullopt, 5);
              } else {
                self->derived().after_connect();
              }
            });
  }

 public:
  void do_request() {
    // Receive the HTTP response
    // Apply per-operation timeout
    boost::beast::get_lowest_layer(derived().stream())
        .expires_after(this->op_timeout());
    http::async_write(
        derived().stream(), req_,
        [self = derived().shared_from_this()](boost::beast::error_code ec,
                                              size_t bytes_transferred) {
          if (ec) {
            BOOST_LOG_SEV(self->lg, trivial::error)
                << "write: " << ec.message();
            self->deliver(std::nullopt, 6);
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
        this->deliver(std::nullopt, 7);
        return;
      }
      http::file_body::value_type body;
      boost::beast::error_code ec;
      body.open(this->body_file_->c_str(), boost::beast::file_mode::write, ec);
      parser_->get().body() = std::move(body);
      this->parser_->body_limit(static_cast<std::int64_t>(1024) * 1024 * 1024 *
                                10);
    }

    auto cb = [self = derived().shared_from_this()](boost::beast::error_code ec,
                                                    size_t bytes_transferred) {
      if (ec) {
        if (ec == http::error::body_limit) {
          // Special handling for body limit errors
          BOOST_LOG_SEV(self->lg, trivial::warning)
              << "Body limit exceeded for empty_body response";
          // Try to recover by forcing empty body
          if constexpr (std::is_same_v<ResponseBody, http::empty_body>) {
            self->parser_->get().body() = typename ResponseBody::value_type();
            self->deliver(self->parser_->release(), 0);
            return;
          }
        }
        BOOST_LOG_SEV(self->lg, trivial::error) << "read: " << ec.message();
        self->deliver(self->parser_->release(), 8);
      } else {
        self->deliver(self->parser_->release(), 0);
      }
    };
    // set time out before read.
    boost::beast::get_lowest_layer(derived().stream())
        .expires_after(this->op_timeout());
    if constexpr (std::is_same_v<ResponseBody, http::empty_body>) {
      http::async_read_header(derived().stream(), buffer_,
                              this->parser_.value(), cb);
    } else {
      http::async_read(derived().stream(), buffer_, this->parser_.value(), cb);
    }
  }

 public:
  void set_req(
      http::request<RequestBody, http::basic_fields<Allocator>>&& req) {
    req_ = std::move(req);
  }

 private:
  asio::io_context& ioc_;
  asio::ip::tcp::resolver resolver_;
  asio::steady_timer resolve_timer_;
  boost::beast::flat_buffer buffer_;  // (Must persist between reads)
  http::request<RequestBody, http::basic_fields<Allocator>> req_;
  std::string default_port_;
  std::optional<http::response_parser<ResponseBody>> parser_;
  std::optional<http::response_parser<http::empty_body>> proxy_response_parser_;
  bool no_modify_req_ = false;
  std::optional<boost::beast::tcp_stream> proxy_stream_;
  std::optional<http::request<http::empty_body>> proxy_req_;
  std::optional<std::string> body_file_;
  const cjj365::ProxySetting* proxy_setting_;
  std::chrono::seconds timeout_{30};
  std::chrono::seconds resolve_to_{30};
  std::chrono::seconds connect_to_{30};
  std::chrono::seconds handshake_to_{30};
  std::chrono::seconds io_to_{30};

 protected:
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
  std::unique_ptr<ssl::stream<boost::beast::tcp_stream>> stream_;
  ssl::context& ctx_;

 public:
  using response_t = std::optional<
      http::response<ResponseBody, http::basic_fields<Allocator>>>;
  using callback_t = std::function<void(response_t, int)>;
  explicit session_ssl(asio::io_context& ioc,  //
                       ssl::context& ctx,      //
                       // const std::string& url,            //
                       urls::url&& url,                   //
                       HttpClientRequestParams&& params,  //
                       callback_t&& callback,             //
                       const cjj365::ProxySetting* proxy_setting = nullptr)
      : session<session_ssl, RequestBody, ResponseBody, Allocator>(
            ioc, std::move(url), std::move(params), std::move(callback), "443",
            proxy_setting),
        ctx_(ctx),
        stream_(std::make_unique<ssl::stream<beast::tcp_stream>>(ioc, ctx)) {}

  ssl::stream<boost::beast::tcp_stream>& stream() { return *stream_; }

  void replace_stream(boost::beast::tcp_stream&& stream) {
    stream_ = std::make_unique<ssl::stream<beast::tcp_stream>>(
        std::move(stream), ctx_);
  }

  void after_connect() {
    boost::urls::url_view urlv(this->url_);
    std::string host = urlv.host();
    if (!SSL_set_tlsext_host_name(stream_->native_handle(), host.c_str())) {
      beast::error_code ec{static_cast<int>(::ERR_get_error()),
                           asio::error::get_ssl_category()};
      BOOST_LOG_SEV(this->lg, trivial::error)
          << "after connect, set_tlsext_host_name got error: " << ec.message()
          << " host: " << host;
      return this->deliver(std::nullopt, 9);
    }
    // Apply handshake timeout
    beast::get_lowest_layer(*stream_).expires_after(this->op_timeout());
    stream_->async_handshake(
        ssl::stream_base::client,
        [self = this->shared_from_this()](beast::error_code ec) {
          if (ec) {
            BOOST_LOG_SEV(self->lg, trivial::error)
                << "after connect, handshake got error: " << ec.message();
            return self->deliver(std::nullopt, 10);
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

    if (ec != ssl::error::stream_truncated) {
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
  explicit session_plain(asio::io_context& ioc,             //
                         urls::url&& url,                   //
                         HttpClientRequestParams&& params,  //
                         callback_t&& callback,
                         const cjj365::ProxySetting* proxy_setting = nullptr)
      : session<session_plain<RequestBody, ResponseBody, Allocator>,
                RequestBody, ResponseBody, Allocator>(
            ioc, std::move(url), std::move(params), std::move(callback), "80",
            proxy_setting),
        stream_(std::make_unique<beast::tcp_stream>(ioc)) {}

  void do_eof() {
    // Gracefully close the socket
    beast::error_code ec;
    if (stream_->socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec)) {
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

// EXTERN_HTTP_SESSION(http::string_body, http::string_body)
// EXTERN_HTTP_SESSION(http::empty_body, http::string_body)
// EXTERN_HTTP_SESSION(http::file_body, http::empty_body)

}  // namespace client_async
