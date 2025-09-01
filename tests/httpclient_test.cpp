#include <fmt/format.h>
#include <gtest/gtest.h>  // Add this line

#include <boost/json/parse.hpp>
#include <boost/url/encode.hpp>
#include <boost/url/format.hpp>
#include <boost/url/rfc/unreserved_chars.hpp>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>

#include "boost/di.hpp"
#include "client_pool_ssl.hpp"
#include "client_ssl_ctx.hpp"
#include "http_client_config_provider.hpp"
#include "http_client_monad.hpp"
#include "io_context_manager.hpp"
#include "ioc_manager_config_provider.hpp"
#include "log_stream.hpp"
#include "misc_util.hpp"
#include "result_monad.hpp"
#include "simple_data.hpp"

static cjj365::ConfigSources& config_sources() {
  static cjj365::ConfigSources instance({fs::path{"tests/config_dir"}}, {});
  return instance;
}
static customio::ConsoleOutputWithColor& output() {
  static customio::ConsoleOutputWithColor instance(4);
  return instance;
}

TEST(HttpClientTest, Pool) {
  using namespace monad;
  misc::ThreadNotifier notifier{};

  cjj365::AppProperties app_properties{config_sources()};
  std::shared_ptr<cjj365::IHttpclientConfigProvider>
      http_client_config_provider =
          std::make_shared<cjj365::HttpclientConfigProviderFile>(
              app_properties, config_sources());
  cjj365::ClientSSLContextWrapper client_ssl_ctx(*http_client_config_provider);

  auto http_client_ = std::make_unique<client_async::ClientPoolSsl>(
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

TEST(HttpClientTest, GetOnly) {
  using namespace monad;
  misc::ThreadNotifier notifier{};
  cjj365::AppProperties app_properties{config_sources()};
  std::shared_ptr<cjj365::IHttpclientConfigProvider>
      http_client_config_provider =
          std::make_shared<cjj365::HttpclientConfigProviderFile>(
              app_properties, config_sources());
  cjj365::ClientSSLContextWrapper client_ssl_ctx(*http_client_config_provider);

  auto http_client_ = std::make_unique<client_async::ClientPoolSsl>(
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
        .catch_then([&](auto err) {
          std::cerr << "Error: " << err << "\n";
          ADD_FAILURE() << "Request failed with error: " << err;
          return IO<ExchangePtrFor<GetStringTag>>::fail(std::move(err));
        })
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
  cjj365::ClientSSLContextWrapper client_ssl_ctx(*http_client_config_provider);

  auto http_client_ = std::make_unique<client_async::ClientPoolSsl>(
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
          return ex->response->body(); })
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