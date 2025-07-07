#pragma once

#include <boost/beast/http.hpp>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "http_client_async.hpp"
#include "io_monad.hpp"

namespace monad {

namespace http = boost::beast::http;
using client_async::ClientPoolSsl;
using client_async::HttpClientRequestParams;
using client_async::ProxySetting;

inline constexpr const char* DEFAULT_TARGET = "";

// ----- Shared HttpExchange -----

template <typename Req, typename Res>
struct HttpExchange {
  std::string body_file = "";
  bool follow_redirect = true;
  logsrc::severity_logger<trivial::severity_level> lg;
  bool no_modify_req = false;
  std::optional<ProxySetting> proxy = std::nullopt;
  Req request;
  std::optional<Res> response = std::nullopt;
  urls::url url;
  std::chrono::seconds timeout = std::chrono::seconds(30);

  HttpExchange(const urls::url_view& url_input, Req request)
      : url(url_input), request(std::move(request)) {}
};

template <typename Req, typename Res>
using HttpExchangePtr = std::shared_ptr<HttpExchange<Req, Res>>;

// ----- Tag-Based Type Mapping -----

template <typename Tag>
struct TagTraits;

struct GetStringTag {};  // Example tag
struct GetStatusTag {};  // New tag
struct PostJsonTag {};   // Another example tag

template <>
struct TagTraits<GetStringTag> {
  using Request = http::request<http::empty_body>;
  using Response = http::response<http::string_body>;
};

template <>
struct TagTraits<GetStatusTag> {
  using Request = http::request<http::empty_body>;
  using Response = http::response<http::empty_body>;
};

template <>
struct TagTraits<PostJsonTag> {
  using Request = http::request<http::string_body>;
  using Response = http::response<http::string_body>;
};

// ----- Monadic Constructor -----

template <typename Tag>
using ExchangePtrFor = HttpExchangePtr<typename TagTraits<Tag>::Request,
                                       typename TagTraits<Tag>::Response>;

template <typename Tag>
monad::IO<HttpExchangePtr<typename TagTraits<Tag>::Request,
                          typename TagTraits<Tag>::Response>>
http_io(const std::string& url) {
  using Req = typename TagTraits<Tag>::Request;
  using Res = typename TagTraits<Tag>::Response;
  using ExchangePtr = HttpExchangePtr<Req, Res>;

  return monad::IO<ExchangePtr>([url](auto cb) {
    if constexpr (std::is_same_v<Tag, GetStatusTag>) {
      // For GetStatusTag, we create a request with no body
      auto req = Req{http::verb::head, DEFAULT_TARGET, 11};
      auto ex = std::make_shared<HttpExchange<Req, Res>>(url, std::move(req));
      cb(std::move(ex));
      return;
    } else if constexpr (std::is_same_v<Tag, GetStringTag>) {
      // For GetStringTag, we create a request with an empty body
      auto req = Req{http::verb::get, DEFAULT_TARGET, 11};
      auto ex = std::make_shared<HttpExchange<Req, Res>>(url, std::move(req));
      cb(std::move(ex));
      return;
    } else if constexpr (std::is_same_v<Tag, PostJsonTag>) {
      // For PostJsonTag, we create a request with a JSON body
      auto req = Req{http::verb::post, DEFAULT_TARGET, 11};
      req.set(http::field::content_type, "application/json");
      auto ex = std::make_shared<HttpExchange<Req, Res>>(url, std::move(req));
      cb(std::move(ex));
      return;
    } else {
      auto req = Req{http::verb::get, DEFAULT_TARGET, 11};
      auto ex = std::make_shared<HttpExchange<Req, Res>>(url, std::move(req));
      cb(std::move(ex));
    }
  });
}

// ---- Add json body from string ----
inline auto set_json_body_io(const std::string& json_str) {
  using Req = http::request<http::string_body>;
  using Res = http::response<http::string_body>;
  using ExchangePtr = HttpExchangePtr<Req, Res>;

  return [json_str](ExchangePtr ex) {
    return monad::IO<ExchangePtr>(
        [ex = std::move(ex), json_str](auto cb) mutable {
          ex->request.set(http::field::content_type, "application/json");
          ex->request.body() = json_str;  // Set JSON body
          ex->request.prepare_payload();  // Prepare the request payload
          cb(std::move(ex));
        });
  };
}

// ----- Monadic Request Invoker -----

template <typename Tag>
auto http_request_io(ClientPoolSsl& pool) {
  using Req = typename TagTraits<Tag>::Request;
  using Res = typename TagTraits<Tag>::Response;
  using ExchangePtr = HttpExchangePtr<Req, Res>;

  return [&pool](ExchangePtr ex) {
    return monad::IO<ExchangePtr>([&pool, ex = std::move(ex)](auto cb) mutable {
      if (!ex->no_modify_req) {
        ex->request.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        // Set up an HTTP GET request message
        std::string target = ex->url.path().empty() ? "/" : ex->url.path();
        if (!ex->url.query().empty()) {
          target += "?" + ex->url.query();
        }
        ex->request.target(target);
      }
      Req request_copy = ex->request;  // preserve original
      pool.http_request<typename Req::body_type, typename Res::body_type>(
          ex->url, std::move(request_copy),
          [cb = std::move(cb), ex](std::optional<Res> resp, int err) mutable {
            if (err == 0 && resp.has_value()) {
              ex->response = std::move(resp);
              cb(std::move(ex));
            } else {
              BOOST_LOG_SEV(ex->lg, trivial::error)
                  << "http_request_io failed with error num: " << err;
              cb(monad::Error{err, "http_request_io failed"});
            }
          },
          {
              .body_file = ex->body_file,
              .follow_redirect = ex->follow_redirect,
              .no_modify_req = ex->no_modify_req,
              .timeout = ex->timeout,
          },
          ex->proxy);
    });
  };
}

}  // namespace monad
