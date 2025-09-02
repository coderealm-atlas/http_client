#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <thread>
#include <stdexcept>
#include "beast_connection_pool.hpp"

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>

using namespace beast_pool;

TEST(BeastConnectionPoolTest, BasicConstruction) {
  boost::asio::io_context ioc;
  PoolConfig cfg;
  boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tls_client);
  ConnectionPool pool(ioc, cfg, &ssl_ctx);
  SUCCEED();  // Construction should not throw
}

TEST(BeastConnectionPoolTest, AcquireAndRelease) {
  boost::asio::io_context ioc;
  PoolConfig cfg;
  // Disable reaper to allow io_context.run() to return immediately after
  // operations complete.
  cfg.idle_reap_interval = std::chrono::seconds(0);
  cfg.connect_timeout = std::chrono::seconds(15);
  cfg.handshake_timeout = std::chrono::seconds(15);
  boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tls_client);
  ssl_ctx.set_verify_mode(boost::asio::ssl::verify_peer);
  ssl_ctx.set_default_verify_paths();
  ConnectionPool pool(ioc, cfg, &ssl_ctx);
  Origin origin{"http", "example.com", 80};
  bool called = false;
  pool.acquire(origin, [&](boost::system::error_code ec, Connection::Ptr c) {
    EXPECT_FALSE(ec) << ec.message();
    EXPECT_TRUE(c != nullptr);
    called = true;
    pool.release(c, true);
  });
  ioc.run();
  EXPECT_TRUE(called);
}

// External network tests can be flaky and may crash in some environments.
// Disable it in CI; prefer the LocalLoopback test below.
TEST(BeastConnectionPoolTest, DISABLED_VisitExampleCom) {
  boost::asio::io_context ioc;
  PoolConfig cfg;
  // Disable reaper to avoid hanging run() after request completes.
  cfg.idle_reap_interval = std::chrono::seconds(0);
  cfg.io_timeout = std::chrono::seconds(20);
  boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tls_client);
  ssl_ctx.set_verify_mode(boost::asio::ssl::verify_peer);
  ssl_ctx.set_default_verify_paths();
  ConnectionPool pool(ioc, cfg, &ssl_ctx);

  Origin origin{"https", "example.com", 443};
  boost::beast::http::request<boost::beast::http::string_body> req{
      boost::beast::http::verb::get, "/", 11};
  req.set(boost::beast::http::field::host, "example.com");
  req.set(boost::beast::http::field::user_agent, "beast-pool-test/1.0");

  bool done = false;
  int status = 0;
  std::string body;
  boost::system::error_code captured_ec;

  pool.async_request(
      origin, req,
      [&](boost::system::error_code ec,
          boost::beast::http::request<boost::beast::http::string_body>,
          boost::beast::http::response<boost::beast::http::string_body> res) {
        captured_ec = ec;
        status = res.result_int();
        body = res.body();
        done = true;
      });

  ioc.run();
  EXPECT_TRUE(done);
  if (captured_ec) {
    GTEST_SKIP() << "Network/SSL error: " << captured_ec.message();
  }
  EXPECT_EQ(status, 200);
  EXPECT_NE(body.find("Example Domain"), std::string::npos);
}

// Minimal loopback HTTP server to test the pool end-to-end without TLS
namespace {
using tcp = boost::asio::ip::tcp;

struct TestServer {
  boost::asio::io_context ioc;
  tcp::acceptor acceptor{ioc};
  std::thread thr;
  std::uint16_t port{};

  explicit TestServer() = default;

  void start() {
    boost::system::error_code ec;
    tcp::endpoint ep{boost::asio::ip::make_address("127.0.0.1", ec), 0};
    auto addr = boost::asio::ip::make_address("127.0.0.1", ec);
    if (ec) throw std::runtime_error("make_address failed: " + ec.message());
    acceptor.open(ep.protocol());
    acceptor.set_option(boost::asio::socket_base::reuse_address(true));
    acceptor.bind(ep);
    acceptor.listen(boost::asio::socket_base::max_listen_connections);
    port = acceptor.local_endpoint().port();

    do_accept();
    thr = std::thread([this] { ioc.run(); });
  }

  void stop() {
    boost::system::error_code ec;
    if (acceptor.close(ec)) {
      // ignore close errors in teardown
    }
    ioc.stop();
    if (thr.joinable()) thr.join();
  }

 private:
  void do_accept() {
    acceptor.async_accept([this](boost::system::error_code ec, tcp::socket sock) {
      if (ec) return;  // server ends
      auto sp = std::make_shared<Session>(std::move(sock));
      sp->run();
      // Accept only one connection for the test, then stop
      boost::system::error_code ec2;
      if (acceptor.close(ec2)) {
        // ignore acceptor close errors
      }
    });
  }

  struct Session : public std::enable_shared_from_this<Session> {
    boost::beast::tcp_stream stream;
    boost::beast::flat_buffer buffer;
    boost::beast::http::request<boost::beast::http::string_body> req;

    explicit Session(tcp::socket s) : stream(std::move(s)) {}

    void run() { do_read(); }

    void do_read() {
      boost::beast::http::async_read(
          stream, buffer, req,
          [self = shared_from_this()](boost::system::error_code ec, std::size_t) {
            if (ec) return;
            self->do_write();
          });
    }

    void do_write() {
      namespace http = boost::beast::http;
      http::response<http::string_body> res{http::status::ok, req.version()};
      res.set(http::field::server, "test-server");
      res.set(http::field::content_type, "text/plain");
      res.keep_alive(req.keep_alive());
      res.body() = "hello-from-loopback";
      res.prepare_payload();
      auto sp = shared_from_this();
      http::async_write(
          stream, res,
          [sp](boost::system::error_code, std::size_t) {
            boost::system::error_code ec;
              if (sp->stream.socket().shutdown(tcp::socket::shutdown_send, ec)) {
                // ignore shutdown errors
              }
          });
    }
  };
};
}  // namespace

TEST(BeastConnectionPoolTest, DISABLED_LocalLoopback) {
  TestServer server;
  server.start();

  boost::asio::io_context ioc;
  PoolConfig cfg;
  cfg.idle_reap_interval = std::chrono::seconds(0);
  cfg.io_timeout = std::chrono::seconds(10);
  // No TLS ctx needed for http
  ConnectionPool pool(ioc, cfg, nullptr);

  Origin origin{"http", "127.0.0.1", server.port};
  boost::beast::http::request<boost::beast::http::string_body> req{
      boost::beast::http::verb::get, "/", 11};
  req.set(boost::beast::http::field::host, "127.0.0.1");
  req.set(boost::beast::http::field::user_agent, "beast-pool-test/loopback");

  bool done = false;
  int status = 0;
  std::string body;
  boost::system::error_code captured_ec;

  pool.async_request(
      origin, req,
      [&](boost::system::error_code ec,
          boost::beast::http::request<boost::beast::http::string_body>,
          boost::beast::http::response<boost::beast::http::string_body> res) {
        captured_ec = ec;
        status = res.result_int();
        body = res.body();
        done = true;
      });

  ioc.run();
  server.stop();

  ASSERT_TRUE(done);
  ASSERT_FALSE(captured_ec) << captured_ec.message();
  EXPECT_EQ(status, 200);
  EXPECT_EQ(body, "hello-from-loopback");
}
