#pragma once

#include <boost/asio.hpp>      // IWYU pragma: keep
#include <boost/asio/ssl.hpp>  // IWYU pragma: keep
#include <boost/beast.hpp>
#include <boost/log/trivial.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "http_session.hpp"

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace ssl = boost::asio::ssl;
namespace urls = boost::urls;
namespace trivial = boost::log::trivial;
using tcp = asio::ip::tcp;

namespace client_async {

template <class RequestBody, class Allocator = std::allocator<char>>
class session_stream_ssl
    : public session<session_stream_ssl<RequestBody, Allocator>, RequestBody,
                     http::string_body, Allocator>,
      public std::enable_shared_from_this<
          session_stream_ssl<RequestBody, Allocator>> {
 public:
  using response_t = std::optional<
      http::response<http::string_body, http::basic_fields<Allocator>>>;
  using callback_t = std::function<void(response_t&&, int)>;
  using header_t =
      http::response<http::empty_body, http::basic_fields<Allocator>>;
  using header_cb_t = std::function<void(header_t&&)>;
  using chunk_cb_t = std::function<void(std::string&&)>;

  explicit session_stream_ssl(
      asio::io_context& ioc, ssl::context& ctx, urls::url&& url,
      HttpClientRequestParams&& params, callback_t&& callback,
      header_cb_t on_headers, chunk_cb_t on_chunk,
      const cjj365::ProxySetting* proxy_setting = nullptr)
      : session<session_stream_ssl, RequestBody, http::string_body, Allocator>(
            ioc, std::move(url), std::move(params), std::move(callback), "443",
            proxy_setting),
        ctx_(ctx),
        stream_(std::make_unique<ssl::stream<beast::tcp_stream>>(ioc, ctx)),
        on_headers_(std::move(on_headers)),
        on_chunk_(std::move(on_chunk)) {}

  ssl::stream<beast::tcp_stream>& stream() { return *stream_; }

  void replace_stream(boost::beast::tcp_stream&& stream) {
    stream_ = std::make_unique<ssl::stream<beast::tcp_stream>>(
        std::move(stream), ctx_);
  }

  void after_connect() {
    boost::urls::url_view urlv(this->url_);
    std::string host = std::string(urlv.host());
    auto is_ipv4_literal = [](std::string_view s) {
      if (s.empty()) return false;
      if (s.find(':') != std::string_view::npos) return false;
      if (s.find('.') == std::string_view::npos) return false;
      return s.find_first_not_of("0123456789.") == std::string_view::npos;
    };
    auto is_ipv6_literal = [](std::string_view s) {
      return !s.empty() && s.find(':') != std::string_view::npos;
    };

    if (!is_ipv4_literal(host) && !is_ipv6_literal(host)) {
      if (!SSL_set_tlsext_host_name(stream_->native_handle(), host.c_str())) {
        beast::error_code ec{static_cast<int>(::ERR_get_error()),
                             asio::error::get_ssl_category()};
        BOOST_LOG_SEV(this->lg, trivial::error)
            << "set_tlsext_host_name failed: " << ec.message();
        return this->deliver(std::nullopt, 9);
      }
    }
    beast::get_lowest_layer(*stream_).expires_after(this->op_timeout());
    stream_->async_handshake(
        ssl::stream_base::client,
        [self = this->shared_from_this()](beast::error_code ec) {
          if (ec) {
            BOOST_LOG_SEV(self->lg, trivial::error)
                << "ssl handshake failed: " << ec.message();
            return self->deliver(std::nullopt, 10);
          }
          self->do_request();
        });
  }

  void do_read() {
    parser_.emplace();
    parser_->body_limit(this->accumulate_response_body()
                            ? boost::optional<std::uint64_t>(
                                  static_cast<std::uint64_t>(1024) * 1024 * 64)
                            : boost::none);
    this->read_buffer().consume(this->read_buffer().size());
    read_some_loop();
  }

 private:
  void read_some_loop() {
    beast::get_lowest_layer(*stream_).expires_after(this->op_timeout());
    http::async_read_some(
        *stream_, this->read_buffer(), *parser_,
        [self = this->shared_from_this()](beast::error_code ec,
                                          std::size_t /*bytes_transferred*/) {
          if (ec) {
            self->deliver(self->parser_->release(), 8);
            return;
          }

          if (!self->headers_delivered_ && self->parser_->is_header_done()) {
            self->headers_delivered_ = true;
            if (self->on_headers_) {
              header_t header;
              header.result(self->parser_->get().result());
              header.reason(self->parser_->get().reason());
              header.version(self->parser_->get().version());
              for (const auto& field : self->parser_->get().base()) {
                header.set(field.name_string(), field.value());
              }
              self->on_headers_(std::move(header));
            }
          }

          const auto& body = self->parser_->get().body();
          if (body.size() > self->last_body_size_ && self->on_chunk_) {
            self->on_chunk_(body.substr(self->last_body_size_));
            self->last_body_size_ = body.size();
            if (!self->accumulate_response_body()) {
              auto& parser_body = self->parser_->get().body();
              typename http::string_body::value_type{}.swap(parser_body);
              self->last_body_size_ = 0;
            }
          }

          if (self->parser_->is_done()) {
            self->deliver(self->parser_->release(), 0);
            return;
          }
          self->read_some_loop();
        });
  }

  ssl::context& ctx_;
  std::unique_ptr<ssl::stream<beast::tcp_stream>> stream_;
  std::optional<http::response_parser<http::string_body>> parser_;
  std::size_t last_body_size_{0};
  bool headers_delivered_{false};
  header_cb_t on_headers_;
  chunk_cb_t on_chunk_;
};

template <class RequestBody, class Allocator = std::allocator<char>>
class session_stream_plain
    : public session<session_stream_plain<RequestBody, Allocator>, RequestBody,
                     http::string_body, Allocator>,
      public std::enable_shared_from_this<
          session_stream_plain<RequestBody, Allocator>> {
 public:
  using response_t = std::optional<
      http::response<http::string_body, http::basic_fields<Allocator>>>;
  using callback_t = std::function<void(response_t&&, int)>;
  using header_t =
      http::response<http::empty_body, http::basic_fields<Allocator>>;
  using header_cb_t = std::function<void(header_t&&)>;
  using chunk_cb_t = std::function<void(std::string&&)>;

  explicit session_stream_plain(
      asio::io_context& ioc, urls::url&& url, HttpClientRequestParams&& params,
      callback_t&& callback, header_cb_t on_headers, chunk_cb_t on_chunk,
      const cjj365::ProxySetting* proxy_setting = nullptr)
      : session<session_stream_plain, RequestBody, http::string_body, Allocator>(
            ioc, std::move(url), std::move(params), std::move(callback), "80",
            proxy_setting),
        stream_(std::make_unique<beast::tcp_stream>(ioc)),
        on_headers_(std::move(on_headers)),
        on_chunk_(std::move(on_chunk)) {}

  beast::tcp_stream& stream() { return *stream_; }

  void replace_stream(beast::tcp_stream&& stream) {
    stream_ = std::make_unique<beast::tcp_stream>(std::move(stream));
  }

  void after_connect() { this->do_request(); }

  void do_read() {
    parser_.emplace();
    parser_->body_limit(this->accumulate_response_body()
                            ? boost::optional<std::uint64_t>(
                                  static_cast<std::uint64_t>(1024) * 1024 * 64)
                            : boost::none);
    this->read_buffer().consume(this->read_buffer().size());
    read_some_loop();
  }

 private:
  void read_some_loop() {
    beast::get_lowest_layer(*stream_).expires_after(this->op_timeout());
    http::async_read_some(
        *stream_, this->read_buffer(), *parser_,
        [self = this->shared_from_this()](beast::error_code ec,
                                          std::size_t /*bytes_transferred*/) {
          if (ec) {
            self->deliver(self->parser_->release(), 8);
            return;
          }

          if (!self->headers_delivered_ && self->parser_->is_header_done()) {
            self->headers_delivered_ = true;
            if (self->on_headers_) {
              header_t header;
              header.result(self->parser_->get().result());
              header.reason(self->parser_->get().reason());
              header.version(self->parser_->get().version());
              for (const auto& field : self->parser_->get().base()) {
                header.set(field.name_string(), field.value());
              }
              self->on_headers_(std::move(header));
            }
          }

          const auto& body = self->parser_->get().body();
          if (body.size() > self->last_body_size_ && self->on_chunk_) {
            self->on_chunk_(body.substr(self->last_body_size_));
            self->last_body_size_ = body.size();
            if (!self->accumulate_response_body()) {
              auto& parser_body = self->parser_->get().body();
              typename http::string_body::value_type{}.swap(parser_body);
              self->last_body_size_ = 0;
            }
          }

          if (self->parser_->is_done()) {
            self->deliver(self->parser_->release(), 0);
            return;
          }
          self->read_some_loop();
        });
  }

  std::unique_ptr<beast::tcp_stream> stream_;
  std::optional<http::response_parser<http::string_body>> parser_;
  std::size_t last_body_size_{0};
  bool headers_delivered_{false};
  header_cb_t on_headers_;
  chunk_cb_t on_chunk_;
};

}  // namespace client_async
