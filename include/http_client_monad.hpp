#pragma once

#include <boost/beast/http.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/core/detail/string_view.hpp>
#include <boost/json/serializer.hpp>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "client_pool_ssl.hpp"
#include "io_monad.hpp"
#include "result_monad.hpp"

namespace monad {

namespace http = boost::beast::http;
using cjj365::ProxySetting;
using client_async::ClientPoolSsl;
using client_async::HttpClientRequestParams;

inline constexpr const char* DEFAULT_TARGET = "";

// ----- Shared HttpExchange -----

template <typename Req, typename Res>
struct HttpExchange {
  std::optional<fs::path> body_file = std::nullopt;
  bool follow_redirect = true;
  logsrc::severity_logger<trivial::severity_level> lg;
  bool no_modify_req = false;
  const ProxySetting* proxy = nullptr;
  Req request;
  std::optional<Res> response = std::nullopt;
  std::optional<fs::path> response_file = std::nullopt;
  urls::url url;
  std::chrono::seconds timeout = std::chrono::seconds(30);

  HttpExchange(const urls::url_view& url_input, Req request)
      : url(url_input), request(std::move(request)) {}

  void setHostTargetRaw() {
    std::string target = url.path().empty() ? "/" : url.path();
    if (!url.query().empty()) {
      target = std::format("{}?{}", target, url.query());
    }
    request.target(target);
    request.set(http::field::host, url.host());
  }

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

  void set_query_param(std::string_view key, std::string_view value) {
    auto params = url.params();
    for (auto it = params.begin(); it != params.end(); ++it) {
      if ((*it).key == key) {
        params.replace(it, std::next(it), {{key, value}});
        return;
      }
    }
    params.insert(params.end(), {key, value});
  }

  MyVoidResult expect_2xx() {
    if (!response.has_value()) {
      return MyVoidResult::Err(Error{400, "Response is not available"});
    }
    int status = static_cast<int>(response->result_int());
    if (status < 200 || status >= 300) {
      BOOST_LOG_SEV(lg, trivial::error)
          << "Error response body: " << std::string{response->body()};
      return MyVoidResult::Err(
          Error{status, std::format("Expected 2xx response, got {}", status)});
    }
    return MyVoidResult::Ok();
  }

  bool is_2xx() {
    if (!response.has_value()) return false;
    auto status = response->result_int();
    return (status >= 200 && status < 300);
  }

  bool not_2xx() {
    if (!response.has_value()) return true;
    auto status = response->result_int();
    return (status < 200 || status >= 300);
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

  template <typename T>
  MyResult<T> parseJsonResponse() {
    return getJsonResponse().and_then(
        [](json::value jv) -> MyResult<T> { return json::value_to<T>(jv); });
  }

  MyResult<json::value> getJsonResponse() {
    try {
      if (response.has_value()) {
        const auto& response_string = response->body();
        if (response_string.empty()) {
          return MyResult<json::value>::Err(
              Error{400, "Response body is empty"});
        }
        return json::parse(response_string);
      } else {
        return MyResult<json::value>::Err(
            Error{400, "Response is not available or empty"});
      }
    } catch (const std::exception& e) {
      BOOST_LOG_SEV(lg, trivial::error)
          << "Failed to get JSON response: " << e.what();
      if (response.has_value()) {
        std::string max_100_chars = response->body();
        if (max_100_chars.length() > 100) {
          max_100_chars = max_100_chars.substr(0, 100) + "...";
        }
        BOOST_LOG_SEV(lg, trivial::error) << "Reponse body: " << max_100_chars;
        return MyResult<json::value>::Err(Error{
            500, std::format("Failed to parse JSON response: {}, body:\n{}",
                             e.what(), max_100_chars)});
      } else {
        return MyResult<json::value>::Err(Error{
            500, std::string("Failed to parse JSON response: ") + e.what()});
      }
    }
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
struct GetHeaderTag {};
struct DeleteTag {};

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

template <>
struct TagTraits<GetHeaderTag> {
  using Request = http::request<http::empty_body>;
  using Response = http::response<http::empty_body>;
};

template <>
struct TagTraits<DeleteTag> {
  using Request = http::request<http::empty_body>;
  using Response = http::response<http::empty_body>;
};

// ----- Monadic Constructor -----

template <typename Tag>
using ExchangePtrFor = HttpExchangePtr<typename TagTraits<Tag>::Request,
                                       typename TagTraits<Tag>::Response>;

template <typename Tag>
using ExchangeIOFor =
    monad::IO<HttpExchangePtr<typename TagTraits<Tag>::Request,
                              typename TagTraits<Tag>::Response>>;

template <typename>
inline constexpr bool always_false = false;

template <typename Tag>
ExchangeIOFor<Tag>
// monad::
//     IO<HttpExchangePtr<typename TagTraits<Tag>::Request,
//                        typename TagTraits<Tag>::Response>>
    /**
     * url_view is not an owner type, so it musts be used immediately. DON'T
     * KEEP IT. and DON'T MOVE THE REFERENCE.
     */
    http_io(const urls::url_view& url_view) {
  using Req = typename TagTraits<Tag>::Request;
  using Res = typename TagTraits<Tag>::Response;
  using ExchangePtr = HttpExchangePtr<Req, Res>;
  // convert to owned type.
  return monad::IO<ExchangePtr>([url = urls::url(url_view)](auto cb) {
    auto make_exchange = [url, cb = std::move(cb)](Req&& req) {
      cb(ExchangePtr{
          std::make_shared<HttpExchange<Req, Res>>(url, std::move(req))});
    };

    if constexpr (std::is_same_v<Tag, GetStatusTag>) {
      make_exchange({http::verb::head, DEFAULT_TARGET, 11});
    } else if constexpr (std::is_same_v<Tag, GetHeaderTag>) {
      make_exchange({http::verb::head, DEFAULT_TARGET, 11});
    } else if constexpr (std::is_same_v<Tag, GetStringTag>) {
      make_exchange({http::verb::get, DEFAULT_TARGET, 11});
    } else if constexpr (std::is_same_v<Tag, DeleteTag>) {
      make_exchange({http::verb::delete_, DEFAULT_TARGET, 11});
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
auto http_request_io(ClientPoolSsl& pool, int verbose = 0) {
  using Req = typename TagTraits<Tag>::Request;
  using Res = typename TagTraits<Tag>::Response;
  using ExchangePtr = HttpExchangePtr<Req, Res>;

  return [&pool, verbose](ExchangePtr ex) {
    return monad::IO<ExchangePtr>([&pool, verbose,
                                   ex = std::move(ex)](auto cb) mutable {
      if (!ex->no_modify_req) {
        ex->request.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        // Set up an HTTP GET request message
        std::string target = ex->url.path().empty() ? "/" : ex->url.path();
        if (!ex->url.query().empty()) {
          target = std::format("{}?{}", target,
                               std::string(ex->url.encoded_query()));
        }
        ex->request.target(target);
      }
      Req request_copy = ex->request;  // preserve original
      if (verbose > 4) {               // trace
        std::cerr << "Before request headers: " << request_copy.base()
                  << std::endl;
      }
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
