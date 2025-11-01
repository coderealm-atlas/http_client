#pragma once

#include <boost/asio/bind_executor.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <iostream>

#include "beast_connection_pool.hpp"

namespace client_async {

// Full-featured pooled HTTP session, mirroring http_session.hpp behaviors.
// - Uses ConnectionPool for transport acquisition and reuse.
// - Supports HTTP/HTTPS, optional HTTP proxy (CONNECT for https).
// - Per-op timeouts come from the pool's PoolConfig.
// - Templated on Body for request/response.
template <class RequestBody, class ResponseBody,
          class Allocator = std::allocator<char>>
class http_session_pooled
    : public std::enable_shared_from_this<
          http_session_pooled<RequestBody, ResponseBody, Allocator>> {
  using Self = http_session_pooled<RequestBody, ResponseBody, Allocator>;

 public:
  using request_t =
      boost::beast::http::request<RequestBody,
                                  boost::beast::http::basic_fields<Allocator>>;
  using response_t =
      boost::beast::http::response<ResponseBody,
                                   boost::beast::http::basic_fields<Allocator>>;
  using callback_t = std::function<void(std::optional<response_t>&&, int)>;

  // Optional simple proxy setting
  struct ProxySetting {
    std::string host;
    std::string port;
    std::string username;
    std::string password;
  };

  http_session_pooled(beast_pool::ConnectionPool& pool,
                      beast_pool::Origin origin,
                      std::optional<ProxySetting> proxy = std::nullopt)
      : pool_(pool), origin_(std::move(origin)), proxy_(std::move(proxy)) {}

  void set_request(request_t req) { req_ = std::move(req); }
  void run(callback_t cb) {
    callback_ = std::move(cb);
    // Acquire a transport connection: use proxy endpoint if configured
    auto self = this->shared_from_this();
    beast_pool::Origin acquire_origin = origin_;
    if (proxy_) {
      acquire_origin.scheme = "http";  // proxy hop is plain TCP
      acquire_origin.host = proxy_->host;
      acquire_origin.port = static_cast<std::uint16_t>(std::stoi(proxy_->port));
    }
    pool_.acquire(acquire_origin, [self](boost::system::error_code ec,
                                         beast_pool::Connection::Ptr c) {
      if (ec || !c) return self->finish(std::nullopt, 1);
      self->conn_ = std::move(c);
      // If HTTP proxy specified and scheme is https, perform CONNECT then
      // upgrade to TLS
      if (self->proxy_ && beast_pool::is_https(self->origin_)) {
        self->do_proxy_connect();
      } else {
        self->do_write();
      }
    });
  }

 private:
  void finish(std::optional<response_t> res, int code) {
    if (code != 0) {
      // Diagnostic: surface non-zero internal finish codes to stderr so tests
      // can show why the pooled session failed (mapping: 1=acquire,2=write,
      // 3=read header,4=proxy response,5=ssl upgrade,6=handshake,7=write,
      // 8=read)
      std::cerr << "[debug] http_session_pooled::finish code=" << code << std::endl;
    }
    // Release connection based on keep-alive and error
    bool reusable = res.has_value() && res->keep_alive();
    if (!reusable && conn_) conn_->close();
    if (conn_) pool_.release(conn_, reusable);
    if (callback_) callback_(std::move(res), code);
  }

  void do_proxy_connect() {
    namespace http = boost::beast::http;
    // Build CONNECT request
    std::string authority;
    if (auto it = req_.find(http::field::host); it != req_.end()) {
      authority = std::string(it->value());
    } else {
      authority = origin_.host + ":" + std::to_string(origin_.port);
    }
    // Keep authority string alive for the lifetime of this CONNECT operation
    connect_authority_ = std::move(authority);

    // Create CONNECT request directly in shared_ptr to avoid string_view
    // lifetime issues
    connect_req_ = std::make_shared<http::request<http::empty_body>>();
    connect_req_->method(http::verb::connect);
    connect_req_->target(
        connect_authority_);  // This copies the string into the request
    connect_req_->version(11);
    connect_req_->set(http::field::host, connect_authority_);
    if (proxy_ && !proxy_->username.empty() && !proxy_->password.empty()) {
      // Basic auth header construction left to caller if needed
    }
    // Write CONNECT then read header
    pool_.set_op_timeout(*conn_, pool_io_timeout());
    std::visit(
        [this](auto& s) mutable {
          using S = std::decay_t<decltype(s)>;
          namespace http = boost::beast::http;
          auto sp = this->shared_from_this();
          http::async_write(
              s, *sp->connect_req_,
              boost::asio::bind_executor(
                  this->conn_->executor(),
                  [sp](boost::system::error_code ec, std::size_t) {
                    if (ec) return sp->finish(std::nullopt, 2);
                    // Keep request until after response header is read
                    sp->do_proxy_read_response();
                  }));
        },
        conn_->stream());
  }

  void do_proxy_read_response() {
    namespace http = boost::beast::http;
    proxy_parser_.emplace();
    pool_.set_op_timeout(*conn_, pool_io_timeout());
    std::visit(
        [this](auto& s) {
          auto sp = this->shared_from_this();
          http::async_read_header(
              s, buffer_, *proxy_parser_,
              boost::asio::bind_executor(
                  this->conn_->executor(),
                  [sp](boost::system::error_code ec, std::size_t) {
                    if (ec) return sp->finish(std::nullopt, 3);
                    if (sp->proxy_parser_->get().result_int() != 200)
                      return sp->finish(std::nullopt, 4);
                    sp->upgrade_to_tls_and_write();
                  }));
        },
        conn_->stream());
  }

  void upgrade_to_tls_and_write() {
    // Switch the connection to SSL and perform handshake, then write request
    auto* ssl_stream = conn_->upgrade_to_ssl();
    if (!ssl_stream) return finish(std::nullopt, 5);
    // Set SNI
    SSL_set_tlsext_host_name(ssl_stream->native_handle(), origin_.host.c_str());
    pool_.set_op_timeout(*conn_, pool_handshake_timeout());
    ssl_stream->async_handshake(
        boost::asio::ssl::stream_base::client,
        boost::asio::bind_executor(
            conn_->executor(),
            [sp = this->shared_from_this()](boost::system::error_code ec) {
              if (ec) return sp->finish(std::nullopt, 6);
              sp->do_write();
            }));
  }
  void do_write() {
    namespace http = boost::beast::http;
    pool_.set_op_timeout(*conn_, pool_io_timeout());
    auto sp = this->shared_from_this();
    // Keep request alive during async_write
    req_ptr_ = std::make_shared<request_t>(std::move(req_));
    std::visit(
        [sp](auto& s) {
          http::async_write(
              s, *sp->req_ptr_,
              boost::asio::bind_executor(
                  sp->conn_->executor(),
                  [sp](boost::system::error_code ec, std::size_t) {
                    if (ec) return sp->finish(std::nullopt, 7);
                    sp->do_read();
                  }));
        },
        conn_->stream());
  }

  void do_read() {
    namespace http = boost::beast::http;
    pool_.set_op_timeout(*conn_, pool_io_timeout());
    buffer_.consume(buffer_.size());
    if (!parser_) parser_.emplace();
    // Optional body limits/streaming can be configured by caller by customizing
    // ResponseBody
    auto sp = this->shared_from_this();
    std::visit(
        [sp](auto& s) {
          http::async_read(s, sp->buffer_, *sp->parser_,
                           boost::asio::bind_executor(
                               sp->conn_->executor(),
                               [sp](boost::system::error_code ec, std::size_t) {
                                 if (ec) return sp->finish(std::nullopt, 8);
                                 auto res = sp->parser_->release();
                                 sp->finish(std::move(res), 0);
                               }));
        },
        conn_->stream());
  }

  std::chrono::seconds pool_io_timeout() const {
    return pool_.config().io_timeout;
  }
  std::chrono::seconds pool_handshake_timeout() const {
    return pool_.config().handshake_timeout;
  }

 private:
  beast_pool::ConnectionPool& pool_;
  beast_pool::Origin origin_;
  std::optional<ProxySetting> proxy_;

  boost::beast::flat_buffer buffer_;
  std::optional<boost::beast::http::response_parser<ResponseBody, Allocator>>
      parser_;
  std::optional<
      boost::beast::http::response_parser<boost::beast::http::empty_body>>
      proxy_parser_;

  request_t req_{};
  std::shared_ptr<request_t> req_ptr_{};
  std::shared_ptr<boost::beast::http::request<boost::beast::http::empty_body>>
      connect_req_{};
  std::string connect_authority_{};
  beast_pool::Connection::Ptr conn_{};
  callback_t callback_{};
};

}  // namespace client_async
