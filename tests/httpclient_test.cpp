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
#include "client_pool_ssl.hpp"
#include "client_ssl_ctx.hpp"
#include "http_client_monad.hpp"
#include "misc_util.hpp"
#include "proxy_pool.hpp"
#include "tutil.hpp"

TEST(HttpClientTest, Pool) {
  using namespace monad;
  misc::ThreadNotifier notifier{};

  cjj365::ClientSSLContextWrapper client_ssl_ctx;

  auto http_client_ =
      std::make_unique<client_async::ClientPoolSsl>(client_ssl_ctx);
  http_client_->start();

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
          return ex;
        })
        .then(http_request_io<GetStatusTag>(*http_client_))
        .map([](auto ex) {
          std::cout << ex->response->result() << std::endl;
          return ex;
        })
        .run([&](auto result) {
          if (std::holds_alternative<monad::Error>(result)) {
            std::cerr << std::get<monad::Error>(result) << "\n";
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
          if (std::holds_alternative<monad::Error>(result)) {
            std::cerr << std::get<monad::Error>(result) << "\n";
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
      std::make_unique<client_async::ClientPoolSsl>(client_ssl_ctx);
  http_client_->start();

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
          if (std::holds_alternative<monad::Error>(result)) {
            std::cerr << std::get<monad::Error>(result) << "\n";
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
      std::make_unique<client_async::ClientPoolSsl>(client_ssl_ctx);
  http_client_->start();

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
          EXPECT_TRUE(
              std::holds_alternative<ExchangePtrFor<PostJsonTag>>(result))
              << "Result should be of type ExchangePtrFor<PostJsonTag>";
          if (std::holds_alternative<monad::Error>(result)) {
            std::cerr << std::get<monad::Error>(result) << "\n";
          }
          notifier.notify();
        });
  }
  notifier.waitForNotification();
  http_client_->stop();
}

TEST(HttpClientTest, DfLogin) {
  using namespace monad;
  misc::ThreadNotifier notifier{};

  cjj365::ClientSSLContextWrapper client_ssl_ctx;

  auto http_client_ =
      std::make_unique<client_async::ClientPoolSsl>(client_ssl_ctx);
  http_client_->start();

  std::string base_url = "https://test.datafocus.ai";
  std::string login_url = fmt::format("{}/uc/login", base_url);
  std::string tenant_id = "10001";

  using StringMapIO = IO<std::map<std::string, std::string>>;
  auto login_io =
      http_io<PostJsonTag>(login_url)
          .map([tenant_id](auto ex) {
            ex->setRequestHeader("tenant-id", tenant_id);
            ex->setRequestJsonBody(
                {{"userName", "admin"}, {"password", "focus@2017"}});
            return ex;
          })
          .then(http_request_io<PostJsonTag>(*http_client_))
          .then([](auto ex) {
            std::cerr << ex->response->base() << std::endl;
            auto access_token = ex->getResponseCookie("access_token");
            auto csrf_token = ex->getResponseCookie("csrf_token");
            if (access_token && csrf_token) {
              std::map<std::string, std::string> headers;
              headers["access_token"] = *access_token;
              headers["csrf_token"] = *csrf_token;
              headers["Cookie"] =
                  ex->createRequestCookie({{"access_token", *access_token}});
              return StringMapIO::pure(std::move(headers));
            } else {
              return StringMapIO::fail(
                  Error{401, "Login failed: Access or CSRF token not found"});
            }
          });

  login_io
      .catch_then([&](auto err) {
        std::cerr << "Error: " << err << "\n";
        ADD_FAILURE() << "Request failed with error: " << err;
        return StringMapIO::fail(std::move(err));
      })
      .run([&](auto result) {
        using MapType = std::map<std::string, std::string>;
        EXPECT_TRUE(std::holds_alternative<MapType>(result))
            << "Result should be of type std::map<std::string, std::string>";
        if (std::holds_alternative<monad::Error>(result)) {
          std::cerr << std::get<monad::Error>(result) << "\n";
        }
        notifier.notify();
      });

  notifier.waitForNotification();
  http_client_->stop();
}

TEST(HttpClientTest, DfListTable) {
  using namespace monad;
  misc::ThreadNotifier notifier{};

  cjj365::ClientSSLContextWrapper client_ssl_ctx;

  auto http_client_ =
      std::make_unique<client_async::ClientPoolSsl>(client_ssl_ctx);
  http_client_->start();
  std::string base_url = "https://test.datafocus.ai";
  // https://test.datafocus.ai/df/table/list?name=%E7%94%B5%E5%95%86&sourceType=table&projectId=&pageNum=1&pageSize=60&stickerIds=&sort=timeDesc&sources=&status=&loadTypes=
  urls::url list_url(base_url);
  list_url.set_path("/df/table/list");
  list_url.params().append({"name", "电商"});
  list_url.params().append({"pageSize", "10"});
  list_url.params().append({"pageNum", "1"});

  ASSERT_EQ(list_url.buffer(),
            "https://test.datafocus.ai/df/table/"
            "list?name=%E7%94%B5%E5%95%86&pageSize=10&pageNum=1")
      << "List URL should match the expected format";
  // the query you ask for. will not change the query.
  // urls::url u1("/df/table/list?name=电商&pageSize=10&pageNum=1"); // not
  // allowed. ASSERT_EQ(u1.query(), "name=电商&pageSize=10&pageNum=1")
  //     << "Query should match the expected format";
  // ASSERT_EQ(u1.encoded_query(),
  // "name=%E7%94%B5%E5%95%86&pageSize=10&pageNum=1")
  //     << "Encoded query should match the expected format";

  std::cerr << "list url: " << list_url.buffer() << std::endl;
  std::string login_url = fmt::format("{}/uc/login", base_url);
  std::string tenant_id = "10001";

  using StringMapIO = IO<std::map<std::string, std::string>>;
  auto login_io =
      http_io<PostJsonTag>(login_url)
          .map([tenant_id](auto ex) {
            ex->setRequestHeader("tenant-id", tenant_id);
            ex->setRequestJsonBody(
                {{"userName", "admin"}, {"password", "focus@2017"}});
            return ex;
          })
          .then(http_request_io<PostJsonTag>(*http_client_))
          .then([&](auto ex) {
            std::cerr << ex->response->base() << std::endl;
            auto access_token = ex->getResponseCookie("access_token");
            auto csrf_token = ex->getResponseCookie("csrf_token");
            if (access_token && csrf_token) {
              std::map<std::string, std::string> headers;
              headers["access-token"] = *access_token;
              headers["csrf-token"] = *csrf_token;
              headers["tenant-id"] = tenant_id;
              headers["Cookie"] =
                  ex->createRequestCookie({{"access_token", *access_token},
                                           {"csrf_token", *csrf_token},
                                           {"tenant_id", tenant_id}});
              return StringMapIO::pure(std::move(headers));
            } else {
              return StringMapIO::fail(
                  Error{401, "Login failed: Access or CSRF token not found"});
            }
          });

  login_io
      .then([&](auto headers) {
        return http_io<GetStringTag>(list_url)
            .map([&headers](auto ex) {
              ex->addRequestHeaders(headers);
              return ex;
            })
            .then(http_request_io<GetStringTag>(*http_client_));
      })
      .map([](auto ex) {
        std::cerr << ex->response->body() << std::endl;
        return ex;
      })
      .catch_then([&](auto err) {
        std::cerr << "Error: " << err << "\n";
        ADD_FAILURE() << "Request failed with error: " << err;
        return IO<ExchangePtrFor<GetStringTag>>::fail(std::move(err));
      })
      .run([&](auto result) { notifier.notify(); });

  notifier.waitForNotification();
  http_client_->stop();
}
