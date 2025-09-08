#include <fmt/format.h>
#include <gtest/gtest.h>  // Add this line

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/json/parse.hpp>
#include <boost/url/encode.hpp>
#include <boost/url/format.hpp>
#include <boost/url/rfc/unreserved_chars.hpp>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>

#include "boost/di.hpp"
#include "client_ssl_ctx.hpp"
#include "http_client_config_provider.hpp"
#include "http_client_manager.hpp"
#include "http_client_monad.hpp"
#include "io_context_manager.hpp"
#include "ioc_manager_config_provider.hpp"
#include "log_stream.hpp"
#include "misc_util.hpp"
#include "result_monad.hpp"
#include "server_certificate.hpp"
#include "simple_data.hpp"

static cjj365::ConfigSources& config_sources() {
  static cjj365::ConfigSources instance({fs::path{"tests/config_dir"}}, {});
  return instance;
}
static customio::ConsoleOutputWithColor& output() {
  static customio::ConsoleOutputWithColor instance(4);
  return instance;
}

// Simple local HTTP server that keeps a single TCP connection alive and tags
// responses
namespace {
namespace net = boost::asio;
namespace http = boost::beast::http;
namespace ssl = boost::asio::ssl;
using tcp = net::ip::tcp;

struct KeepAliveServer {
  net::io_context ioc{1};
  tcp::acceptor acceptor;
  std::thread thr;
  std::atomic<bool> started{false};
  unsigned short port{};

  KeepAliveServer()
      : acceptor(ioc, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0),
                 true) {
    port = acceptor.local_endpoint().port();
  }

  void run_async() {
    thr = std::thread([this] {
      accept();
      started = true;
      ioc.run();
    });
    // Wait briefly for accept to be pending
    while (!started.load())
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  void stop() {
    net::post(ioc, [this] {
      boost::system::error_code ec;
      acceptor.close(ec);
    });
    ioc.stop();
    if (thr.joinable()) thr.join();
  }

  void accept() {
    auto sock = std::make_shared<tcp::socket>(ioc);
    acceptor.async_accept(*sock, [this, sock](boost::system::error_code ec) {
      if (!ec) {
        handle_session(sock);
      }
    });
  }

  void handle_session(std::shared_ptr<tcp::socket> sock) {
    auto buffer = std::make_shared<boost::beast::flat_buffer>();
    auto req = std::make_shared<http::request<http::string_body>>();
    auto self = sock;
    auto read_once = std::make_shared<std::function<void()>>();
    *read_once = [this, self, buffer, req, read_once]() {
      http::async_read(
          *self, *buffer, *req,
          [this, self, buffer, req, read_once](boost::system::error_code ec,
                                               std::size_t) {
            if (ec) {
              return;  // connection closed or error
            }
            // Build response with a stable Conn-Id header based on socket
            // pointer
            auto res = std::make_shared<http::response<http::string_body>>(
                http::status::ok, 11);
            res->set(http::field::server, "local-keep-alive");
            res->set(http::field::content_type, "text/plain");
            res->keep_alive(true);
            res->body() = "ok";
            res->prepare_payload();
            res->set("X-Conn-Id", fmt::format("{}", (const void*)self.get()));

            http::async_write(*self, *res,
                              [this, self, res, buffer, req, read_once](
                                  boost::system::error_code ec, std::size_t) {
                                if (ec) {
                                  return;
                                }
                                // Clear and wait for another request on the
                                // same connection
                                req->clear();
                                buffer->consume(buffer->size());
                                (*read_once)();
                              });
          });
    };
    (*read_once)();
  }
};
}  // namespace

// Self-signed cert and key for local HTTPS server (testing only)
namespace {
struct HttpsServer {
  net::io_context ioc{1};
  ssl::context ctx{ssl::context::tls_server};
  tcp::acceptor acceptor;
  std::thread thr;
  unsigned short port{};

  HttpsServer()
      : acceptor(ioc, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0),
                 true) {
    port = acceptor.local_endpoint().port();
    cjj365::testcert::load_server_certificate(ctx);
  }

  void run_async() {
    thr = std::thread([this] {
      do_accept();
      ioc.run();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  void stop() {
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
          auto sp = std::make_shared<Session>(std::move(sock), ctx);
          sp->run();
        });
  }

  struct Session : public std::enable_shared_from_this<Session> {
    ssl::stream<tcp::socket> stream;
    boost::beast::flat_buffer buffer;
    http::request<http::string_body> req;
    Session(tcp::socket s, ssl::context& ctx) : stream(std::move(s), ctx) {}
    void run() {
      stream.async_handshake(
          ssl::stream_base::server,
          [self = shared_from_this()](boost::system::error_code ec) {
            if (ec) return;
            self->do_read();
          });
    }
    void do_read() {
      http::async_read(stream, buffer, req,
                       [self = shared_from_this()](boost::system::error_code ec,
                                                   std::size_t) {
                         if (ec) return;
                         self->do_write();
                       });
    }
    void do_write() {
      auto res = std::make_shared<http::response<http::string_body>>(
          http::status::ok, req.version());
      res->set(http::field::server, "https-local");
      res->set(http::field::content_type, "text/plain");
      res->keep_alive(true);
      res->body() = "ok-https";
      res->prepare_payload();
      auto sp = shared_from_this();
      http::async_write(stream, *res,
                        [sp, res](boost::system::error_code, std::size_t) {
                          // keep connection open for simplicity
                        });
    }
  };
};

// Minimal CONNECT proxy that tunnels data to target host:port
struct ConnectProxy {
  net::io_context ioc{1};
  tcp::acceptor acceptor;
  std::thread thr;
  unsigned short port{};

  ConnectProxy()
      : acceptor(ioc, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0),
                 true) {
    port = acceptor.local_endpoint().port();
  }
  void run_async() {
    thr = std::thread([this] {
      do_accept();
      ioc.run();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  void stop() {
    net::post(ioc, [this] {
      boost::system::error_code ec;
      (void)acceptor.close(ec);
    });
    ioc.stop();
    if (thr.joinable()) thr.join();
  }

  struct Tunnel : public std::enable_shared_from_this<Tunnel> {
    tcp::socket client;
    tcp::socket upstream;
    boost::beast::flat_buffer buffer;
    std::string target_host;
    std::string target_port;
    Tunnel(tcp::socket s, net::io_context& ioc)
        : client(std::move(s)), upstream(ioc) {}
    void start() {
      // Read CONNECT request header only
      auto parser = std::make_shared<http::request_parser<http::empty_body>>();
      parser->eager(false);
      http::async_read_header(
          client, buffer, *parser,
          [self = shared_from_this(), parser](boost::system::error_code ec,
                                              std::size_t) {
            if (ec) return;
            // Parse authority from target
            std::string authority(parser->get().target());
            auto pos = authority.find(':');
            if (pos == std::string::npos) return;
            self->target_host = authority.substr(0, pos);
            self->target_port = authority.substr(pos + 1);
            // Connect upstream
            tcp::resolver resolver(self->client.get_executor());
            auto results =
                resolver.resolve(self->target_host, self->target_port, ec);
            if (ec) return;
            // Use a single endpoint to avoid lifetime issues with results
            auto ep = results.begin()->endpoint();
            self->upstream.async_connect(
                ep, [self](boost::system::error_code ec) {
                  if (ec) return;
                  // Send 200 established
                  auto res = std::make_shared<http::response<http::empty_body>>(
                      http::status::ok, 11);
                  http::async_write(
                      self->client, *res,
                      [self, res](boost::system::error_code ec, std::size_t) {
                        if (ec) return;
                        self->pump();
                      });
                });
          });
    }
    void pump() {
      auto self = shared_from_this();
      // client -> upstream
      client.async_read_some(
          net::buffer(cbuf),
          [self](boost::system::error_code ec, std::size_t n) {
            if (ec) return;
            net::async_write(self->upstream, net::buffer(self->cbuf, n),
                             [self](boost::system::error_code ec, std::size_t) {
                               if (ec) return;
                               self->pump();
                             });
          });
      // upstream -> client
      upstream.async_read_some(
          net::buffer(sbuf),
          [self](boost::system::error_code ec, std::size_t n) {
            if (ec) return;
            net::async_write(self->client, net::buffer(self->sbuf, n),
                             [self](boost::system::error_code ec, std::size_t) {
                               if (ec) return;
                               self->pump();
                             });
          });
    }
    char cbuf[4096];
    char sbuf[4096];
  };

  void do_accept() {
    acceptor.async_accept(
        [this](boost::system::error_code ec, tcp::socket sock) {
          if (ec) return;
          std::make_shared<Tunnel>(std::move(sock), ioc)->start();
          do_accept();
        });
  }
};
}  // namespace

TEST(HttpClientTest, Pool) {
  using namespace monad;
  misc::ThreadNotifier notifier{};

  cjj365::AppProperties app_properties{config_sources()};
  std::shared_ptr<cjj365::IHttpclientConfigProvider>
      http_client_config_provider =
          std::make_shared<cjj365::HttpclientConfigProviderFile>(
              app_properties, config_sources());
  cjj365::ClientSSLContext client_ssl_ctx(*http_client_config_provider);
  std::cerr << "Client SSL context initialized." << std::endl;
  auto http_client_ = std::make_unique<client_async::HttpClientManager>(
      client_ssl_ctx, *http_client_config_provider);

  urls::url_view url("https://example.com/hello?name=world#fragment");
  urls::url target1("/abc?");
  target1.normalize();
  ASSERT_EQ(target1.buffer(), "/abc?") << "Target should be '/abc?'";

  urls::url target = urls::format("{}?{}", url.path(), url.query());
  target.normalize();
  ASSERT_EQ(target.buffer(), "/hello?name=world")
      << "Target should be '/hello?name=world'";
  using namespace monad;

  std::optional<monad::MyResult<std::string>> response_body_r;
  std::optional<monad::MyResult<unsigned int>> response_status_r;
  {
    http_io<GetStatusTag>("https://example.com")
        .map([&](auto ex) {
          ex->request.set(http::field::authorization, "Bearer token");
          ex->set_query_param("name", "world");
          EXPECT_EQ(ex->url.query(), "name=world")
              << "Query should match the expected format";
          return ex;
        })
        .then(http_request_io<GetStatusTag>(*http_client_))
        .map([](auto ex) { return ex->response->result_int(); })
        .run([&](auto result) {
          response_status_r = std::move(result);
          notifier.notify();
        });
  }
  notifier.waitForNotification();
  EXPECT_FALSE(response_status_r->is_err()) << response_status_r->error();

  {
    http_io<GetStringTag>("https://example.com")
        .map([](auto ex) {
          ex->request.set(http::field::authorization, "Bearer token");
          return ex;
        })
        .then(http_request_io<GetStringTag>(*http_client_))
        .map([](auto ex) { return ex->response->body(); })
        .run([&](monad::MyResult<std::string>&& result) {
          response_body_r = std::move(result);
          notifier.notify();
        });
  }
  notifier.waitForNotification();
  EXPECT_FALSE(response_body_r->is_err()) << response_body_r->error();
  http_client_->stop();
}

TEST(HttpClientTest, PooledReuseLocal) {
  // Start local keep-alive server
  KeepAliveServer server;
  server.run_async();

  cjj365::AppProperties app_properties{config_sources()};
  auto http_client_config_provider =
      std::make_shared<cjj365::HttpclientConfigProviderFile>(app_properties,
                                                             config_sources());
  cjj365::ClientSSLContext client_ssl_ctx(*http_client_config_provider);
  auto http_client_ = std::make_unique<client_async::HttpClientManager>(
      client_ssl_ctx, *http_client_config_provider);

  std::string url = fmt::format("http://127.0.0.1:{}/hello", server.port);

  std::optional<std::string> conn_id_1;
  std::optional<std::string> conn_id_2;
  misc::ThreadNotifier notifier{};

  // First request (pooled)
  {
    http::request<http::string_body> req{http::verb::get, "/hello", 11};
    req.set(http::field::host, fmt::format("127.0.0.1:{}", server.port));
    req.set(http::field::user_agent, "pooled-test");
    http_client_->http_request_pooled<http::string_body, http::string_body>(
        urls::url_view(url), std::move(req),
        [&](std::optional<http::response<http::string_body>>&& res, int code) {
          ASSERT_EQ(code, 0);
          ASSERT_TRUE(res.has_value());
          ASSERT_TRUE(res->find("X-Conn-Id") != res->end());
          conn_id_1 = std::string(
              res->base()["X-Conn-Id"]);  // to_string_view -> std::string
          notifier.notify();
        });
  }
  notifier.waitForNotification();

  // Second request (should reuse the same TCP connection)
  {
    http::request<http::string_body> req{http::verb::get, "/hello", 11};
    req.set(http::field::host, fmt::format("127.0.0.1:{}", server.port));
    req.set(http::field::user_agent, "pooled-test");
    http_client_->http_request_pooled<http::string_body, http::string_body>(
        urls::url_view(url), std::move(req),
        [&](std::optional<http::response<http::string_body>>&& res, int code) {
          ASSERT_EQ(code, 0);
          ASSERT_TRUE(res.has_value());
          ASSERT_TRUE(res->find("X-Conn-Id") != res->end());
          conn_id_2 = std::string(res->base()["X-Conn-Id"]);
          notifier.notify();
        });
  }
  notifier.waitForNotification();

  ASSERT_TRUE(conn_id_1.has_value());
  ASSERT_TRUE(conn_id_2.has_value());
  EXPECT_EQ(*conn_id_1, *conn_id_2) << "expected pooled connection reuse";

  http_client_->stop();
  server.stop();
}

TEST(HttpClientTest, PooledProxyHttps) {
  // Launch HTTPS origin and CONNECT proxy
  HttpsServer https;
  https.run_async();
  ConnectProxy proxy;
  proxy.run_async();

  cjj365::AppProperties app_properties{config_sources()};
  auto http_client_config_provider =
      std::make_shared<cjj365::HttpclientConfigProviderFile>(app_properties,
                                                             config_sources());
  cjj365::ClientSSLContext client_ssl_ctx(*http_client_config_provider);
  // Trust our local self-signed cert
  client_ssl_ctx.add_certificate_authority(cjj365::testcert::certificate_pem());
  auto http_client_ = std::make_unique<client_async::HttpClientManager>(
      client_ssl_ctx, *http_client_config_provider);

  // Build https URL to local server
  // Use SNI/URL host that matches the cert CN, but CONNECT to 127.0.0.1 via
  // Host header
  std::string url = fmt::format("https://www.example.com:{}/hello", https.port);
  http::request<http::string_body> req{http::verb::get, "/hello", 11};
  req.set(http::field::host, fmt::format("127.0.0.1:{}", https.port));
  req.set(http::field::user_agent, "pooled-proxy-test");

  std::optional<int> status;
  std::optional<std::string> body;
  misc::ThreadNotifier notifier{};

  cjj365::ProxySetting pxy{.host = "127.0.0.1",
                           .port = std::to_string(proxy.port)};
  http_client_->http_request_pooled<http::string_body, http::string_body>(
      urls::url_view(url), std::move(req),
      [&](std::optional<http::response<http::string_body>>&& res, int code) {
        ASSERT_EQ(code, 0);
        ASSERT_TRUE(res.has_value());
        status = res->result_int();
        body = res->body();
        notifier.notify();
      },
      {}, &pxy);

  notifier.waitForNotification();
  ASSERT_TRUE(status.has_value());
  ASSERT_TRUE(body.has_value());
  EXPECT_EQ(*status, 200);
  EXPECT_EQ(*body, std::string("ok-https"));

  http_client_->stop();
  proxy.stop();
  https.stop();
}

TEST(HttpClientTest, GetOnly) {
  using namespace monad;
  misc::ThreadNotifier notifier{};
  cjj365::AppProperties app_properties{config_sources()};
  std::shared_ptr<cjj365::IHttpclientConfigProvider>
      http_client_config_provider =
          std::make_shared<cjj365::HttpclientConfigProviderFile>(
              app_properties, config_sources());
  cjj365::ClientSSLContext client_ssl_ctx(*http_client_config_provider);

  auto http_client_ = std::make_unique<client_async::HttpClientManager>(
      client_ssl_ctx, *http_client_config_provider);

  // const gatewayDomain = "gray-quick-dove-13.mypinata.cloud"
  // const cid = "bafkreihxhxdozot7mukwx4hx55bbfxxd6vtesccuzgc2qicjrxhrp3rugy"
  // const url = `https://${gatewayDomain}/ipfs/${cid}`
  auto url = urls::format(
      "https://{}/ipfs/{}", "gray-quick-dove-13.mypinata.cloud",
      "bafkreihxhxdozot7mukwx4hx55bbfxxd6vtesccuzgc2qicjrxhrp3rugy");
  ASSERT_EQ(url.buffer(),
            "https://gray-quick-dove-13.mypinata.cloud/ipfs/"
            "bafkreihxhxdozot7mukwx4hx55bbfxxd6vtesccuzgc2qicjrxhrp3rugy")
      << "URL should match the expected format";

  std::optional<monad::MyResult<std::string>> response_body_r;
  auto httpbin_url = "https://httpbin.org/get?a=b";
  {
    http_io<GetStringTag>(httpbin_url)
        .map([](auto ex) {
          ex->request.set(http::field::authorization, "Bearer token");
          return ex;
        })
        .then(http_request_io<GetStringTag>(*http_client_))
        .map([](auto ex) { return ex->response->body(); })
        .run([&](auto result) {
          response_body_r = std::move(result);
          notifier.notify();
        });
  }
  notifier.waitForNotification();
  EXPECT_FALSE(response_body_r->is_err()) << response_body_r->error();

  json::value jv = json::parse(response_body_r->value());
  EXPECT_TRUE(jv.as_object().contains("args"))
      << "Response should contain 'args' field";
  EXPECT_TRUE(jv.as_object().contains("headers"))
      << "Response should contain 'headers' field";
  EXPECT_TRUE(jv.as_object().contains("url"))
      << "Response should contain 'url' field";
  std::cout << "Response: " << jv << std::endl;
  http_client_->stop();
}

TEST(HttpClientTest, PostOnly) {
  using namespace monad;
  misc::ThreadNotifier notifier{};

  cjj365::AppProperties app_properties{config_sources()};
  std::shared_ptr<cjj365::IHttpclientConfigProvider>
      http_client_config_provider =
          std::make_shared<cjj365::HttpclientConfigProviderFile>(
              app_properties, config_sources());
  cjj365::ClientSSLContext client_ssl_ctx(*http_client_config_provider);

  auto http_client_ = std::make_unique<client_async::HttpClientManager>(
      client_ssl_ctx, *http_client_config_provider);

  auto httpbin_url = "https://httpbin.org/post?a=b";
  std::optional<monad::MyResult<std::string>> response_body_r;
  {
    http_io<PostJsonTag>(httpbin_url)
        .map([](auto ex) {
          ex->setRequestJsonBody({{"key", "value"}});
          return ex;
        })
        .then(http_request_io<PostJsonTag>(*http_client_))
        .catch_then([&](auto err) {
          std::cerr << "Error: " << err << "\n";
          ADD_FAILURE() << "Request failed with error: " << err;
          return IO<ExchangePtrFor<PostJsonTag>>::fail(std::move(err));
        })
        .map([](auto ex) {
          auto vr = ex->expect_2xx();
          return ex->response->body();
        })
        .run([&](auto result) {
          response_body_r = std::move(result);
          notifier.notify();
        });
  }
  notifier.waitForNotification();
  EXPECT_FALSE(response_body_r->is_err()) << response_body_r->error();
  json::value jv = json::parse(response_body_r->value());
  std::cerr << "response body: " << jv << std::endl;
  EXPECT_TRUE(jv.as_object().contains("args"))
      << "Response should contain 'args' field";
  EXPECT_TRUE(jv.as_object().contains("headers"))
      << "Response should contain 'headers' field";
  EXPECT_TRUE(jv.as_object().contains("url"))
      << "Response should contain 'url' field";
  // expect data
  EXPECT_TRUE(jv.as_object().contains("data"))
      << "Response should contain 'data' field";
  json::value data_jv = json::parse(jv.as_object()["data"].as_string().c_str());
  EXPECT_EQ(data_jv.as_object()["key"].as_string(), "value")
      << "Response data should match the sent JSON body";
  std::cout << "Response: " << jv << std::endl;
  http_client_->stop();
}

TEST(IocontextTest, ioc) {
  namespace di = boost::di;
  const auto injector = boost::di::make_injector(
      di::bind<customio::IOutput>().to(output()),
      di::bind<cjj365::ConfigSources>().to(config_sources()),
      di::bind<cjj365::IIocConfigProvider>()
          .to<cjj365::IocConfigProviderFile>());
  auto& io_context = injector.create<cjj365::IoContextManager&>();
  io_context.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
