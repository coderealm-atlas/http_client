#include <fmt/format.h>
#include <gtest/gtest.h>  // Add this line

#include <boost/json/parse.hpp>
#include <boost/url/encode.hpp>
#include <boost/url/format.hpp>
#include <boost/url/rfc/unreserved_chars.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "aliases.hpp"
#include "base64.h"
#include "boost/di.hpp"
#include "client_pool_ssl.hpp"
#include "client_ssl_ctx.hpp"
#include "http_client_monad.hpp"
#include "io_context_manager.hpp"
#include "ioc_manager_config_provider.hpp"
#include "log_stream.hpp"
#include "misc_util.hpp"
#include "proxy_pool.hpp"
#include "tutil.hpp"

TEST(HttpClientTest, Pool) {
  using namespace monad;
  misc::ThreadNotifier notifier{};

  cjj365::ClientSSLContextWrapper client_ssl_ctx;

  auto http_client_ =
      std::make_unique<client_async::ClientPoolSsl>(client_ssl_ctx, 2);

  urls::url_view url("https://example.com/hello?name=world#fragment");
  urls::url target1("/abc?");
  target1.normalize();
  ASSERT_EQ(target1.buffer(), "/abc?") << "Target should be '/abc?'";

  urls::url target = urls::format("{}?{}", url.path(), url.query());
  target.normalize();
  ASSERT_EQ(target.buffer(), "/hello?name=world")
      << "Target should be '/hello?name=world'";
  using namespace monad;

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
        .map([](auto ex) {
          std::cout << ex->response->result() << std::endl;
          return ex;
        })
        .run([&](auto result) {
          if (result.is_err()) {
            std::cerr << result.error() << "\n";
          }
          notifier.notify();
        });
  }
  notifier.waitForNotification();

  {
    http_io<GetStringTag>("https://example.com")
        .map([](auto ex) {
          ex->request.set(http::field::authorization, "Bearer token");
          return ex;
        })
        .then(http_request_io<GetStringTag>(*http_client_))
        .map([](auto ex) {
          std::cout << ex->response->body() << std::endl;
          return ex;
        })
        .run([&](auto result) {
          if (result.is_err()) {
            std::cerr << result.error() << "\n";
          }
          notifier.notify();
        });
  }
  notifier.waitForNotification();
  http_client_->stop();
}

TEST(HttpClientTest, GetOnly) {
  using namespace monad;
  misc::ThreadNotifier notifier{};
  cjj365::ClientSSLContextWrapper client_ssl_ctx;

  auto http_client_ =
      std::make_unique<client_async::ClientPoolSsl>(client_ssl_ctx, 2);

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

  auto httpbin_url = "https://httpbin.org/get?a=b";
  {
    http_io<GetStringTag>(httpbin_url)
        .map([](auto ex) {
          ex->request.set(http::field::authorization, "Bearer token");
          return ex;
        })
        .then(http_request_io<GetStringTag>(*http_client_))
        .catch_then([&](auto err) {
          std::cerr << "Error: " << err << "\n";
          ADD_FAILURE() << "Request failed with error: " << err;
          return IO<ExchangePtrFor<GetStringTag>>::fail(std::move(err));
        })
        .map([](auto ex) {
          json::value jv = json::parse(ex->response->body());
          EXPECT_TRUE(jv.as_object().contains("args"))
              << "Response should contain 'args' field";
          EXPECT_TRUE(jv.as_object().contains("headers"))
              << "Response should contain 'headers' field";
          EXPECT_TRUE(jv.as_object().contains("url"))
              << "Response should contain 'url' field";
          std::cout << "Response: " << jv << std::endl;
          return ex;
        })
        .run([&](auto result) {
          if (result.is_err()) {
            std::cerr << result.error() << "\n";
          }
          notifier.notify();
        });
  }
  notifier.waitForNotification();
  http_client_->stop();
}

TEST(HttpClientTest, PostOnly) {
  using namespace monad;
  misc::ThreadNotifier notifier{};

  cjj365::ClientSSLContextWrapper client_ssl_ctx;

  auto http_client_ =
      std::make_unique<client_async::ClientPoolSsl>(client_ssl_ctx, 2);

  auto httpbin_url = "https://httpbin.org/post?a=b";
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
          json::value jv = json::parse(ex->response->body());
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
          json::value data_jv =
              json::parse(jv.as_object()["data"].as_string().c_str());
          EXPECT_EQ(data_jv.as_object()["key"].as_string(), "value")
              << "Response data should match the sent JSON body";
          std::cout << "Response: " << jv << std::endl;
          return ex;
        })
        .run([&](auto result) {
          EXPECT_TRUE(result.is_ok())
              << "Result should be of type ExchangePtrFor<PostJsonTag>";
          if (result.is_err()) {
            std::cerr << result.error() << "\n";
          }
          notifier.notify();
        });
  }
  notifier.waitForNotification();
  http_client_->stop();
}

TEST(IocontextTest, ioc) {
  namespace di = boost::di;
  static std::shared_ptr<customio::IOutput> output =
      std::make_shared<customio::ConsoleOutputWithColor>(4);
  const auto injector =
      boost::di::make_injector(di::bind<customio::IOutput>().to(*output),
                               di::bind<cjj365::IIocConfigProvider>()
                                   .to<cjj365::IocConfigProviderFile>());
  auto& io_context = injector.create<cjj365::IoContextManager&>();
  io_context.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}