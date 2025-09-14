#include "io_monad.hpp"
// beast_connection_pool.hpp
#pragma once
#include <boost/asio.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>

namespace beast_pool {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = boost::asio::ssl;
using tcp = net::ip::tcp;

// ------------------------------------
// Origin key
// ------------------------------------
struct Origin {
  std::string scheme;  // "http" or "https" (lowercase)
  std::string host;    // authority host name (used for SNI)
  std::uint16_t port;

  bool operator==(const Origin& o) const noexcept {
    return scheme == o.scheme && host == o.host && port == o.port;
  }
};
struct OriginHash {
  std::size_t operator()(Origin const& o) const noexcept {
    std::size_t h = std::hash<std::string>{}(o.scheme);
    h ^= std::hash<std::string>{}(o.host) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<std::uint16_t>{}(o.port) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
  }
};

inline bool is_https(Origin const& o) noexcept { return o.scheme == "https"; }

// ------------------------------------
// Config
// ------------------------------------
struct PoolConfig {
  std::chrono::seconds idle_reap_interval{15};
  std::chrono::seconds idle_keep_alive{60};  // close if idle > this
  std::size_t max_idle_per_origin{6};        // cap idle deque length
  std::size_t max_total_idle{512};           // coarse global cap
  std::chrono::seconds resolve_timeout{10};
  std::chrono::seconds connect_timeout{10};
  std::chrono::seconds handshake_timeout{10};
  std::chrono::seconds io_timeout{30};  // write/read timeout
};

// ------------------------------------
// Connection (TCP or TLS over TCP)
// ------------------------------------
class Connection : public std::enable_shared_from_this<Connection> {
 public:
  using Ptr = std::shared_ptr<Connection>;
  using TcpStream = beast::tcp_stream;
  using SslStream = ssl::stream<beast::tcp_stream>;
  using StreamVariant = std::variant<TcpStream, SslStream>;

  Connection(net::any_io_executor ex, ssl::context* ssl_ctx, Origin origin)
      : stream_(TcpStream(ex)), ssl_ctx_(ssl_ctx), origin_(std::move(origin)) {}

  bool is_ssl() const noexcept {
    return std::holds_alternative<SslStream>(stream_);
  }

  // Create the right stream type based on scheme (must be called before
  // connect).
  void prepare_stream() {
    // Always build new stream on the same executor as the current one
    auto ex = executor();
    if (is_https(origin_) && ssl_ctx_) {
      // Replace TcpStream with SslStream bound to the strand/io_context
      stream_.template emplace<SslStream>(ex, *ssl_ctx_);
    } else {
      stream_.template emplace<TcpStream>(ex);
    }
  }

  net::any_io_executor executor() {
    return std::visit([](auto& s) { return s.get_executor(); }, stream_);
  }

  tcp::socket& lowest_socket() {
    return std::visit(
        [](auto& s) -> tcp::socket& {
          return beast::get_lowest_layer(s).socket();
        },
        stream_);
  }
  const tcp::socket& lowest_socket() const {
    return std::visit(
        [](auto& s) -> const tcp::socket& {
          return beast::get_lowest_layer(s).socket();
        },
        stream_);
  }

  void set_busy(bool b) {
    busy_ = b;
    if (!b) last_used_ = std::chrono::steady_clock::now();
  }
  bool busy() const { return busy_; }

  bool is_expired(std::chrono::seconds idle_keep_alive) const {
    return (std::chrono::steady_clock::now() - last_used_) > idle_keep_alive;
  }

  bool alive() const { return lowest_socket().is_open(); }

  void close() {
    beast::error_code ec;
    std::visit(
        [&](auto& s) {
          using S = std::decay_t<decltype(s)>;
          if constexpr (std::is_same_v<S, SslStream>) {
            beast::get_lowest_layer(s).expires_after(std::chrono::seconds(2));
            s.shutdown(ec);
            beast::get_lowest_layer(s).socket().shutdown(
                tcp::socket::shutdown_both, ec);
            beast::get_lowest_layer(s).socket().close(ec);
          } else {
            s.socket().shutdown(tcp::socket::shutdown_both, ec);
            s.socket().close(ec);
          }
        },
        stream_);
  }

  StreamVariant& stream() { return stream_; }
  Origin const& origin() const { return origin_; }
  void set_origin(Origin o) { origin_ = std::move(o); }

  // Upgrade an existing TCP stream to SSL, preserving the underlying socket.
  // Returns pointer to the SSL stream, or nullptr on failure (no ssl_ctx_ or
  // already SSL)
  SslStream* upgrade_to_ssl() {
    if (std::holds_alternative<SslStream>(stream_)) {
      return &std::get<SslStream>(stream_);
    }
    if (!ssl_ctx_) return nullptr;
    auto* pTcp = std::get_if<TcpStream>(&stream_);
    if (!pTcp) return nullptr;
    TcpStream moved = std::move(*pTcp);
    stream_.template emplace<SslStream>(std::move(moved), *ssl_ctx_);
    return &std::get<SslStream>(stream_);
  }

 private:
  StreamVariant stream_;  // TCP or TLS over TCP
  ssl::context* ssl_ctx_ = nullptr;
  Origin origin_;
  bool busy_ = false;
  std::chrono::steady_clock::time_point last_used_{
      std::chrono::steady_clock::now()};
};

// ------------------------------------
// ConnectionPool
// ------------------------------------
class ConnectionPool {
  // Monadic acquire: returns IO<Connection::Ptr>
  monad::IO<Connection::Ptr> acquire_monad(Origin origin) {
    return monad::IO<Connection::Ptr>([this, origin = std::move(origin)](
                                          auto cb) mutable {
      this->acquire(std::move(origin), [cb = std::move(cb)](
                                           boost::system::error_code ec,
                                           Connection::Ptr c) mutable {
        if (ec || !c) {
          cb(monad::Result<Connection::Ptr, monad::Error>::Err(
              monad::Error{ec.value(), ec.message()}));
        } else {
          cb(monad::Result<Connection::Ptr, monad::Error>::Ok(std::move(c)));
        }
      });
    });
  }

  // Monadic async_request: returns IO<http::response<ResBody>>
  template <class Request>
  auto async_request_monad(Origin origin, Request req) {
    using ResBody = typename Request::body_type;
    using ResponseT = http::response<ResBody>;
    return monad::IO<ResponseT>([this, origin = std::move(origin),
                                 req = std::move(req)](auto cb) mutable {
      this->async_request(
          std::move(origin), std::move(req),
          [cb = std::move(cb)](boost::system::error_code ec, auto,
                               ResponseT res) mutable {
            if (ec) {
              cb(monad::Result<ResponseT, monad::Error>::Err(
                  monad::Error{ec.value(), ec.message()}));
            } else {
              cb(monad::Result<ResponseT, monad::Error>::Ok(std::move(res)));
            }
          });
    });
  }

 public:
  using AcquireHandler =
      std::function<void(boost::system::error_code, Connection::Ptr)>;

  ConnectionPool(net::io_context& ioc, PoolConfig cfg = {},
                 ssl::context* ssl_ctx = nullptr)
      : ioc_(ioc),
        strand_(net::make_strand(ioc)),
        cfg_(cfg),
        ssl_ctx_(ssl_ctx),
        reap_timer_(ioc) {
    // Reaper is armed lazily when the first idle connection appears.
  }

  // Expose read-only config
  const PoolConfig& config() const { return cfg_; }

  // Expose helper to set per-op timeout on the connection's lowest layer
  void set_op_timeout(Connection& c, std::chrono::seconds t) {
    std::visit([t](auto& s) { beast::get_lowest_layer(s).expires_after(t); },
               c.stream());
  }

  // Acquire a ready connection for the origin (reuses or creates).
  void acquire(Origin origin, AcquireHandler handler) {
    net::post(strand_, [this, origin = std::move(origin),
                        handler = std::move(handler)]() mutable {
      // 1) Try idle list
      auto& dq = idle_[origin];
      while (!dq.empty()) {
        auto c = dq.back();
        dq.pop_back();
        if (c && c->alive() && !c->is_expired(cfg_.idle_keep_alive)) {
          c->set_busy(true);
          return handler({}, std::move(c));
        } else if (c) {
          c->close();
        }
      }
      // 2) Create new
      auto c = std::make_shared<Connection>(strand_, ssl_ctx_, origin);
      c->prepare_stream();  // choose TCP vs TLS stream
      do_resolve_connect(std::move(c), std::move(handler));
    });
  }

  // Return a connection to the pool (if healthy & reusable).
  void release(Connection::Ptr c, bool can_reuse) {
    net::post(strand_, [this, c = std::move(c), can_reuse]() mutable {
      if (!c) return;
      if (!can_reuse || !c->alive()) {
        c->close();
        return;
      }
      c->set_busy(false);
      auto& dq = idle_[c->origin()];
      // Respect per-origin cap
      if (dq.size() >= cfg_.max_idle_per_origin) {
        // Drop the oldest
        auto old = dq.front();
        dq.pop_front();
        if (old) old->close();
      }
      dq.push_back(c);
      shrink_global_if_needed();
      arm_reap_if_needed_locked();
    });
  }

  // Convenience: async one-shot request using a pooled connection.
  template <class Request, class ResponseHandler>
  void async_request(Origin origin, Request req,
                     ResponseHandler&& on_response) {
    acquire(std::move(origin), [this, req = std::move(req),
                                on_response = std::forward<ResponseHandler>(
                                    on_response)](boost::system::error_code ec,
                                                  Connection::Ptr c) mutable {
      if (ec) {
        on_response(ec, Request{},
                    http::response<typename Request::body_type>{});
        return;
      }

      auto buffer = std::make_shared<beast::flat_buffer>();
      using ResBody = typename Request::body_type;
      auto res = std::make_shared<http::response<ResBody>>();

      // Keep-alive requested; request must outlive async_write
      Request req2 = std::move(req);
      req2.keep_alive(true);
      auto req_ptr = std::make_shared<Request>(std::move(req2));

      // Write with per-operation timeout
      set_op_timeout(*c, cfg_.io_timeout);

      std::visit(
          [this, c, buffer, res, req_ptr, on_response](auto& s) mutable {
            using S = std::decay_t<decltype(s)>;

            http::async_write(
                s, *req_ptr,
                net::bind_executor(
                    c->executor(),
                    [this, c, buffer, res, req_ptr, on_response](
                        boost::system::error_code ec, std::size_t) {
                      if (ec) {
                        c->close();
                        this->release(c, /*can_reuse=*/false);
                        on_response(
                            ec, Request{},
                            http::response<typename Request::body_type>{});
                        return;
                      }

                      set_op_timeout(*c, cfg_.io_timeout);

                      std::visit(
                          [this, c, buffer, res, on_response](auto& s2) {
                            http::async_read(
                                s2, *buffer, *res,
                                net::bind_executor(
                                    c->executor(),
                                    [this, c, res, on_response](
                                        boost::system::error_code ec,
                                        std::size_t) {
                                      bool reusable = !ec && res->keep_alive();
                                      if (!reusable) c->close();
                                      this->release(c, reusable);
                                      on_response(ec, Request{}, *res);
                                    }));
                          },
                          c->stream());
                    }));
          },
          c->stream());
    });
  }

 private:
  void do_resolve_connect(Connection::Ptr c, AcquireHandler handler) {
    auto resolver = std::make_shared<tcp::resolver>(strand_);
    // Apply resolve timeout via cancellation timer (optional). Simpler: rely on
    // OS + connect timeout.
    resolver->async_resolve(
        c->origin().host, std::to_string(c->origin().port),
        net::bind_executor(strand_, [this, c, resolver, handler](
                                        boost::system::error_code ec,
                                        tcp::resolver::results_type results) {
          if (ec) {
            handler(ec, {});
            return;
          }

          // Connect with timeout
          std::visit(
              [this, c, results, handler](auto& s) {
                using S = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<S, Connection::SslStream>) {
                  beast::get_lowest_layer(s).expires_after(
                      cfg_.connect_timeout);
                  s.lowest_layer().async_connect(
                      results.begin()->endpoint(),
                      net::bind_executor(
                          c->executor(),
                          [this, c, handler, host = c->origin().host](
                              boost::system::error_code ec) mutable {
                            if (ec) {
                              handler(ec, {});
                              return;
                            }

                            // Set SNI on the current SSL stream
                            auto& ssl_s =
                                std::get<Connection::SslStream>(c->stream());
                            SSL_set_tlsext_host_name(ssl_s.native_handle(),
                                                     host.c_str());

                            // Handshake
                            beast::get_lowest_layer(ssl_s).expires_after(
                                cfg_.handshake_timeout);
                            ssl_s.async_handshake(
                                ssl::stream_base::client,
                                net::bind_executor(
                                    c->executor(),
                                    [c, handler](boost::system::error_code ec) {
                                      if (ec) {
                                        handler(ec, {});
                                        return;
                                      }
                                      c->set_busy(true);
                                      handler({}, c);
                                    }));
                          }));
                } else {
                  s.expires_after(cfg_.connect_timeout);
                  s.async_connect(
                      results.begin()->endpoint(),
                      net::bind_executor(
                          c->executor(),
                          [c, handler](boost::system::error_code ec) {
                            if (ec) {
                              handler(ec, {});
                              return;
                            }
                            c->set_busy(true);
                            handler({}, c);
                          }));
                }
              },
              c->stream());
        }));
  }

  void schedule_reap() {
    if (cfg_.idle_reap_interval.count() <= 0) {
      reaper_armed_ = false;
      return;  // disabled
    }
    reap_timer_.expires_after(cfg_.idle_reap_interval);
    reap_timer_.async_wait(
        net::bind_executor(strand_, [this](boost::system::error_code) {
          // Per-origin prune
          for (auto it = idle_.begin(); it != idle_.end();) {
            auto& dq = it->second;
            for (auto it2 = dq.begin(); it2 != dq.end();) {
              auto& c = *it2;
              if (!c || !c->alive() || c->is_expired(cfg_.idle_keep_alive)) {
                if (c) c->close();
                it2 = dq.erase(it2);
              } else {
                ++it2;
              }
            }
            if (dq.empty())
              it = idle_.erase(it);
            else
              ++it;
          }
          shrink_global_if_needed();
          // Re-arm only if we still have idle connections and reaper enabled
          std::size_t total = 0;
          for (auto const& kv : idle_) total += kv.second.size();
          if (total > 0 && cfg_.idle_reap_interval.count() > 0) {
            schedule_reap();
          } else {
            reaper_armed_ = false;
          }
        }));
  }

  void shrink_global_if_needed() {
    // Coarse global idle cap: drop oldest across origins if needed
    std::size_t total = 0;
    for (auto const& kv : idle_) total += kv.second.size();
    if (total <= cfg_.max_total_idle) return;

    // Repeatedly remove from the largest deques first.
    while (total > cfg_.max_total_idle) {
      auto it = std::max_element(idle_.begin(), idle_.end(),
                                 [](auto const& a, auto const& b) {
                                   return a.second.size() < b.second.size();
                                 });
      if (it == idle_.end() || it->second.empty()) break;
      auto c = it->second.front();
      it->second.pop_front();
      if (c) c->close();
      --total;
      if (it->second.empty()) idle_.erase(it);
    }
  }

 private:
  net::io_context& ioc_;
  net::strand<net::io_context::executor_type> strand_;
  PoolConfig cfg_;
  ssl::context* ssl_ctx_;  // optional
  net::steady_timer reap_timer_;
  bool reaper_armed_ = false;

  std::unordered_map<Origin, std::deque<Connection::Ptr>, OriginHash> idle_;

  // Must be called on strand_
  void arm_reap_if_needed_locked() {
    if (cfg_.idle_reap_interval.count() <= 0) return;  // disabled
    if (reaper_armed_) return;
    std::size_t total = 0;
    for (auto const& kv : idle_) total += kv.second.size();
    if (total == 0) return;
    reaper_armed_ = true;
    schedule_reap();
  }
};

}  // namespace beast_pool

// ------------------------------------------------------------
// Example usage (BUILD: add -lssl -lcrypto if needed)
// ------------------------------------------------------------
#ifdef EXAMPLE_MAIN
#include <iostream>

int main() {
  using namespace beast_pool;
  net::io_context ioc;

  // TLS ctx (optional). For HTTPS, set verify mode/paths as you need.
  ssl::context tls_ctx(ssl::context::tls_client);
  tls_ctx.set_verify_mode(ssl::verify_peer);
  tls_ctx.set_default_verify_paths();

  PoolConfig cfg;
  cfg.max_idle_per_origin = 4;
  cfg.idle_keep_alive = std::chrono::seconds(60);

  ConnectionPool pool(ioc, cfg, &tls_ctx);

  // Build a GET request
  Origin o{"https", "example.com", 443};
  http::request<http::string_body> req{http::verb::get, "/", 11};
  req.set(http::field::host, o.host);
  req.set(http::field::user_agent, "beast-pool/1.0");

  pool.async_request(
      o, std::move(req),
      [&](boost::system::error_code ec, http::request<http::string_body>,
          http::response<http::string_body> res) {
        if (ec) {
          std::cerr << "Request error: " << ec.message() << "\n";
        } else {
          std::cout << "HTTP " << res.result_int() << "\n";
          std::cout << res.base() << "\n";
          std::cout << res.body().substr(0, 200) << "...\n";
        }
      });

  ioc.run();
  return 0;
}
#endif
