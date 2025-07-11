#pragma once

#include <boost/beast/http.hpp>
#include <boost/core/detail/string_view.hpp>
#include <boost/json/serializer.hpp>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "client_pool_ssl.hpp"
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
  std::optional<fs::path> body_file = std::nullopt;
  bool follow_redirect = true;
  logsrc::severity_logger<trivial::severity_level> lg;
  bool no_modify_req = false;
  std::optional<ProxySetting> proxy = std::nullopt;
  Req request;
  std::optional<Res> response = std::nullopt;
  std::optional<fs::path> response_file = std::nullopt;
  urls::url url;
  std::chrono::seconds timeout = std::chrono::seconds(30);

  HttpExchange(const urls::url_view& url_input, Req request)
      : url(url_input), request(std::move(request)) {}

  void contentTypeJson() {
    request.set(http::field::content_type, "application/json");
  }

  void setRequestHeader(const std::string& name, const std::string& value) {
    request.set(name, value);
  }

  void addRequestHeaders(const std::map<std::string, std::string>& headers) {
    for (const auto& [name, value] : headers) {
      request.set(name, value);
    }
  }

  void addRequestHeaders(
      const std::vector<std::pair<std::string, std::string>>& headers) {
    for (const auto& [name, value] : headers) {
      request.set(name, value);
    }
  }

  void setRequestJsonBodyFromString(const std::string& json_str) {
    request.body() = json_str;
    request.prepare_payload();
    contentTypeJson();
  }
  void setRequestJsonBody(json::value&& json_body) {
    request.body() = json::serialize(json_body);
    request.prepare_payload();
    contentTypeJson();
  }

  std::optional<std::string> getResponseCookie(const std::string& cookie_name) {
    if (!response.has_value()) return std::nullopt;

    const auto& fields = response->base();
    auto range = fields.equal_range(http::field::set_cookie);

    for (auto it = range.first; it != range.second; ++it) {
      const std::string& cookie_header = it->value();

      // e.g., "access_token=abc; Path=/; HttpOnly"
      std::istringstream stream(cookie_header);
      std::string token;

      // Split by ';'
      while (std::getline(stream, token, ';')) {
        // Trim leading spaces
        token.erase(0, token.find_first_not_of(" \t"));

        if (token.starts_with(cookie_name + "=")) {
          std::string value = token.substr(cookie_name.length() + 1);

          // Strip quotes if present
          if (!value.empty() && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.length() - 2);
          }

          return value;
        }
      }
    }

    return std::nullopt;
  }

  std::string createRequestCookie(
      std::initializer_list<std::pair<std::string, std::string>> cookies) {
    std::string result;
    bool first = true;
    for (const auto& [key, value] : cookies) {
      if (!first) {
        result += "; ";
      } else {
        first = false;
      }
      result += std::format("{}={}", key, value);
    }
    return result;
  }

  std::optional<json::object> getJsonResponse() {
    try {
      if (response.has_value()) {
        const auto& response_string = response->body();
        if (response_string.empty()) {
          BOOST_LOG_SEV(lg, trivial::warning) << "Received empty response body";
          return std::nullopt;
        }
        json::value jv = json::parse(response_string);
        if (jv.is_object()) {
          return jv.as_object();
        } else {
          BOOST_LOG_SEV(lg, trivial::error)
              << "Response is not a valid JSON object: " << response_string;
          return std::nullopt;
        }
      } else {
        BOOST_LOG_SEV(lg, trivial::error)
            << "Response is not available or empty";
        return std::nullopt;
      }
    } catch (const std::exception& e) {
      BOOST_LOG_SEV(lg, trivial::error)
          << "Failed to get JSON response: " << e.what();
      return std::nullopt;
    }
    return std::nullopt;
  }
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

template <typename>
inline constexpr bool always_false = false;

template <typename Tag>
monad::IO<HttpExchangePtr<typename TagTraits<Tag>::Request,
                          typename TagTraits<Tag>::Response>>
http_io(const urls::url_view& url) {
  using Req = typename TagTraits<Tag>::Request;
  using Res = typename TagTraits<Tag>::Response;
  using ExchangePtr = HttpExchangePtr<Req, Res>;

  return monad::IO<ExchangePtr>([url](auto cb) {
    auto make_exchange = [&](Req&& req) {
      cb(ExchangePtr{
          std::make_shared<HttpExchange<Req, Res>>(url, std::move(req))});
    };

    if constexpr (std::is_same_v<Tag, GetStatusTag>) {
      make_exchange({http::verb::head, DEFAULT_TARGET, 11});
    } else if constexpr (std::is_same_v<Tag, GetStringTag>) {
      make_exchange({http::verb::get, DEFAULT_TARGET, 11});
    } else if constexpr (std::is_same_v<Tag, PostJsonTag>) {
      Req req{http::verb::post, DEFAULT_TARGET, 11};
      req.set(http::field::content_type, "application/json");
      make_exchange(std::move(req));
    } else {
      static_assert(always_false<Tag>, "Unsupported Tag for http_io.");
    }
  });
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
        std::cerr << "Request query: " << ex->url.query() << std::endl;
        std::cerr << "Request query1: " << ex->url.encoded_query() << std::endl;
        if (!ex->url.query().empty()) {
          target = std::format("{}?{}", target,
                               std::string(ex->url.encoded_query()));
        }
        ex->request.target(target);
      }
      Req request_copy = ex->request;  // preserve original
      std::cerr << "Before request headers: " << request_copy.base()
                << std::endl;
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
