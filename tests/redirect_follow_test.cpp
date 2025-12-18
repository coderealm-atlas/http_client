#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

#include "client_ssl_ctx.hpp"
#include "http_client_config_provider.hpp"
#include "http_client_manager.hpp"
#include "misc_util.hpp"

namespace fs = std::filesystem;
namespace net = boost::asio;
namespace http = boost::beast::http;
namespace urls = boost::urls;
using tcp = net::ip::tcp;

static cjj365::ConfigSources& config_sources() {
  static const fs::path config_dir = fs::path(__FILE__).parent_path() / "config_dir";
  static cjj365::ConfigSources instance({config_dir}, {});
  return instance;
}

namespace {
struct RedirectServer {
  net::io_context ioc{1};
  tcp::acceptor acceptor;
  std::thread thr;
  unsigned short port{};

  RedirectServer()
      : acceptor(ioc, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0),
                 true) {
    port = acceptor.local_endpoint().port();
  }

  ~RedirectServer() { stop(); }

  void run_async() {
    thr = std::thread([this] {
      do_accept();
      ioc.run();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  void stop() {
    if (!thr.joinable()) {
      ioc.stop();
      return;
    }
    net::post(ioc, [this] {
      boost::system::error_code ec;
      (void)acceptor.close(ec);
    });
    ioc.stop();
    if (thr.joinable()) thr.join();
  }

  void do_accept() {
    acceptor.async_accept(
        [this](boost::system::error_code ec, tcp::socket sock) {
          if (ec) return;
          std::make_shared<Session>(std::move(sock))->start();
          do_accept();
        });
  }

  struct Session : public std::enable_shared_from_this<Session> {
    tcp::socket sock;
    boost::beast::flat_buffer buffer;
    http::request<http::string_body> req;

    explicit Session(tcp::socket s) : sock(std::move(s)) {}

    void start() { do_read(); }

    void do_read() {
      http::async_read(sock, buffer, req,
                       [self = shared_from_this()](boost::system::error_code ec,
                                                   std::size_t) {
                         if (ec) return;
                         self->do_write();
                       });
    }

    void do_write() {
      auto target = std::string(req.target());

      auto res = std::make_shared<http::response<http::string_body>>(
          http::status::ok, req.version());
      res->set(http::field::server, "local-redirect");
      res->keep_alive(req.keep_alive());

      if (target == "/redir") {
        res->result(http::status::found);
        res->set(http::field::location, "/final");
        res->body() = "";
      } else if (target == "/final") {
        res->result(http::status::ok);
        res->set(http::field::content_type, "text/plain");
        res->body() = "final";
      } else {
        res->result(http::status::not_found);
        res->set(http::field::content_type, "text/plain");
        res->body() = "not found";
      }

      res->prepare_payload();

      http::async_write(sock, *res,
                        [self = shared_from_this(), res](
                            boost::system::error_code ec, std::size_t) {
                          if (ec) return;
                          if (!self->req.keep_alive()) return;
                          self->req.clear();
                          self->buffer.consume(self->buffer.size());
                          self->do_read();
                        });
    }
  };
};
}  // namespace

TEST(HttpClientRedirectTest, FollowRedirectLocal) {
  RedirectServer srv;
  srv.run_async();

  misc::ThreadNotifier notifier{};

  cjj365::AppProperties app_properties{config_sources()};
  auto http_client_config_provider =
      std::make_shared<cjj365::HttpclientConfigProviderFile>(app_properties,
                                                             config_sources());
  cjj365::ClientSSLContext client_ssl_ctx(*http_client_config_provider);
  auto http_client = std::make_unique<client_async::HttpClientManager>(
      client_ssl_ctx, *http_client_config_provider);

  auto url_str = std::string("http://127.0.0.1:") + std::to_string(srv.port) +
                 "/redir";
  auto parsed = urls::parse_uri(url_str);
  ASSERT_TRUE(parsed.has_value()) << "Failed to parse test URL";
  urls::url url = parsed.value();

  http::request<http::empty_body> req{http::verb::get, "/", 11};
  req.keep_alive(false);

  std::optional<http::response<http::string_body>> resp_r;
  int ec_r = -1;

  client_async::HttpClientRequestParams params;
  params.follow_redirect = true;

  http_client->http_request<http::empty_body, http::string_body>(
      url, std::move(req),
      [&](std::optional<http::response<http::string_body>>&& resp, int ec) {
        resp_r = std::move(resp);
        ec_r = ec;
        notifier.notify();
      },
      std::move(params));

  notifier.waitForNotification();

  srv.stop();

  ASSERT_EQ(ec_r, 0);
  ASSERT_TRUE(resp_r.has_value());
  EXPECT_EQ(resp_r->result_int(), 200);
  EXPECT_EQ(resp_r->body(), "final");
}
