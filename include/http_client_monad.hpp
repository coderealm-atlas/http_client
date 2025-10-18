#pragma once

#include <boost/beast/http.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/core/detail/string_view.hpp>
#include <boost/json/serializer.hpp>
#include <exception>
#include <fmt/format.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "common_macros.hpp"
#include "http_client_manager.hpp"
#include "io_monad.hpp"
#include "result_monad.hpp"

namespace monad {

namespace http = boost::beast::http;
using cjj365::ProxySetting;
using client_async::HttpClientManager;
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

  static constexpr int JSON_ERR_MALFORMED = 9000;
  static constexpr int JSON_ERR_DECODE = 9001;
  static constexpr int JSON_ERR_TYPE_MISMATCH = 9003;
  static constexpr int JSON_ERR_MISSING_FIELD = 9004;
  static constexpr int JSON_ERR_INVALID_SCHEMA = 9005;

  static std::string make_preview(std::string_view text) {
    constexpr std::size_t max_preview = 512;
    if (text.size() <= max_preview) {
      return std::string{text};
    }
    std::string preview;
    preview.reserve(max_preview + 3);
    preview.append(text.substr(0, max_preview));
    preview.append("...");
    return preview;
  }

  HttpExchange(const urls::url_view& url_input, Req request)
      : url(url_input), request(std::move(request)) {}

  // Note: call this when the request target must preserve the exact encoded
  // path/query computed by boost::url (e.g. GitHub OAuth token exchange).
  // It bypasses the default behaviour in `http_request_io`, which rebuilds the
  // target using decoded components and may lose reserved characters.  The
  // method also sets the Host header explicitly so upstream proxies or
  // servers (that require the original host:port) see the value untouched.
  // Typical usage is to call `setHostTargetRaw()` and then set
  // `no_modify_req = true` so that the request is forwarded as-is.
  void setHostTargetRaw() {
    std::string target =
        url.encoded_path().empty() ? "/" : std::string(url.encoded_path());
    if (!url.encoded_query().empty()) {
      target = fmt::format("{}?{}", target, std::string(url.encoded_query()));
    }
    request.target(target);
    std::string host_header = std::string(url.host());
    if (url.has_port()) {
      host_header.push_back(':');
      host_header += std::string(url.port());
    }
    request.set(http::field::host, std::move(host_header));
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
      return MyVoidResult::Err(
          Error{status, fmt::format("Expected 2xx response, got {}", status)});
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

  std::optional<std::string> getResponseCookie(
      const std::string& cookie_name = "cjj365") {
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

        std::string prefix = cookie_name + "=";
        if (token.size() >= prefix.size() && token.substr(0, prefix.size()) == prefix) {
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
      result += fmt::format("{}={}", key, value);
    }
    return result;
  }

  template <typename T>
  auto parseJsonResponse() -> MyResult<std::decay_t<T>> {
    using ValueType = std::decay_t<T>;
    static_assert(!is_my_result_v<ValueType>,
                  "Use parseJsonResponseResult for MyResult payloads");
    static_assert(!std::is_void_v<ValueType>,
                  "parseJsonResponse does not support void payloads");

    const int response_status =
        response.has_value() ? static_cast<int>(response->result_int()) : 0;
    const std::string response_body =
        response.has_value() ? response->body() : std::string{};
    const std::string response_body_preview = make_preview(response_body);

    return getJsonResponse().and_then(
        [response_status, response_body_preview](json::value jv)
            -> MyResult<ValueType> {
          DEBUG_PRINT("JSON response before parse: {}\n" << jv);
          try {
            return MyResult<ValueType>::Ok(json::value_to<ValueType>(jv));
          } catch (const std::exception& e) {
            Error err{JSON_ERR_TYPE_MISMATCH,
                      std::string("JSON type mismatch: ") + e.what()};
            err.response_status = response_status;
            auto preview = response_body_preview;
            if (preview.empty()) {
              preview = HttpExchange::make_preview(json::serialize(jv));
            }
            err.params["response_body_preview"] = std::move(preview);
            return MyResult<ValueType>::Err(std::move(err));
          }
        });
  }

  template <typename T>
  auto parseJsonDataResponse() -> MyResult<std::decay_t<T>> {
    using ValueType = std::decay_t<T>;
    static_assert(!is_my_result_v<ValueType>,
                  "Use parseJsonResponseResult for MyResult payloads");
    static_assert(!std::is_void_v<ValueType>,
                  "parseJsonDataResponse does not support void payloads");

    const int response_status =
        response.has_value() ? static_cast<int>(response->result_int()) : 0;
    const std::string response_body =
        response.has_value() ? response->body() : std::string{};
    const std::string response_body_preview = make_preview(response_body);

    return getJsonResponse().and_then(
        [response_status, response_body_preview](json::value jv)
            -> MyResult<ValueType> {
          DEBUG_PRINT("JSON data response before parse: " << jv);
          try {
            if (!jv.is_object()) {
              Error err{JSON_ERR_INVALID_SCHEMA,
                        "JSON does not conform to expected schema"};
              err.response_status = response_status;
              auto preview = response_body_preview;
              if (preview.empty()) {
                preview = HttpExchange::make_preview(json::serialize(jv));
              }
              err.params["response_body_preview"] = std::move(preview);
              return MyResult<ValueType>::Err(std::move(err));
            }
            const auto& obj = jv.as_object();
            auto it = obj.find("data");
            if (it == obj.end()) {
              Error err{JSON_ERR_MISSING_FIELD,
                        "Required JSON field missing: 'data'"};
              err.response_status = response_status;
              auto preview = response_body_preview;
              if (preview.empty()) {
                preview = HttpExchange::make_preview(json::serialize(jv));
              }
              err.params["response_body_preview"] = std::move(preview);
              return MyResult<ValueType>::Err(std::move(err));
            }
            return MyResult<ValueType>::Ok(
                json::value_to<ValueType>(it->value()));
          } catch (const std::exception& e) {
            Error err{JSON_ERR_TYPE_MISMATCH,
                      std::string("JSON type mismatch: ") + e.what()};
            err.response_status = response_status;
            auto preview = response_body_preview;
            if (preview.empty()) {
              preview = HttpExchange::make_preview(json::serialize(jv));
            }
            err.params["response_body_preview"] = std::move(preview);
            return MyResult<ValueType>::Err(std::move(err));
          }
        });
  }

  template <typename ResultT>
  auto parseJsonResponseResult() -> std::decay_t<ResultT> {
    using Requested = std::decay_t<ResultT>;
    static_assert(is_my_result_v<Requested>,
                  "parseJsonResponseResult expects a MyResult payload");

    const int response_status =
        response.has_value() ? static_cast<int>(response->result_int()) : 0;
    const std::string response_body =
        response.has_value() ? response->body() : std::string{};
    const std::string response_body_preview = make_preview(response_body);

    return getJsonResponse().and_then(
        [response_status, response_body_preview](json::value jv) -> Requested {
      DEBUG_PRINT("JSON response result before parse: {}\n" << jv);
      try {
        return json::value_to<Requested>(jv);
      } catch (const std::exception& e) {
        Error err{JSON_ERR_INVALID_SCHEMA,
                  std::string("JSON does not conform to expected schema: ") +
                      e.what()};
        err.response_status = response_status;
        auto preview = response_body_preview;
        if (preview.empty()) {
          preview = HttpExchange::make_preview(json::serialize(jv));
        }
        err.params["response_body_preview"] = std::move(preview);
        return Requested::Err(std::move(err));
      }
    });
  }

  MyResult<json::value> getJsonResponse() {
    try {
      if (response.has_value()) {
        const auto& response_string = response->body();
        if (response_string.empty()) {
          Error err{JSON_ERR_MALFORMED,
                    "Malformed JSON text: response body is empty"};
          err.response_status = static_cast<int>(response->result_int());
          err.params["response_body_preview"] =
              make_preview(response->body());
          return MyResult<json::value>::Err(std::move(err));
        }
        return MyResult<json::value>::Ok(json::parse(response_string));
      } else {
        Error err{JSON_ERR_DECODE,
                  "Failed to decode/parse JSON (low-level): response is not available"};
        err.response_status = 0;
        return MyResult<json::value>::Err(std::move(err));
      }
    } catch (const std::exception& e) {
      BOOST_LOG_SEV(lg, trivial::error)
          << "Failed to get JSON response: " << e.what();
      if (response.has_value()) {
        std::string preview = make_preview(response->body());
        BOOST_LOG_SEV(lg, trivial::error)
            << "Response body preview: " << preview;
        Error err{
            JSON_ERR_DECODE,
            fmt::format("Failed to decode/parse JSON (low-level): {}", e.what())};
        err.response_status = static_cast<int>(response->result_int());
        err.params["response_body_preview"] = std::move(preview);
        return MyResult<json::value>::Err(std::move(err));
      } else {
        Error err{JSON_ERR_DECODE,
                  std::string("Failed to decode/parse JSON (low-level): ") +
                      e.what()};
        err.response_status = 0;
        return MyResult<json::value>::Err(std::move(err));
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
      cb(monad::Result<ExchangePtr, monad::Error>::Ok(
          std::make_shared<HttpExchange<Req, Res>>(url, std::move(req))));
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
auto http_request_io(HttpClientManager& pool, int verbose = 0) {
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
          target = fmt::format("{}?{}", target,
                               std::string(ex->url.encoded_query()));
        }
        ex->request.target(target);
      }
      Req request_copy = ex->request;  // preserve original
      if (verbose > 4) {               // trace
        std::cerr << "Before request headers: " << request_copy.base()
                  << std::endl;
      }
    HttpClientRequestParams request_params;
    request_params.body_file = ex->body_file;
    request_params.follow_redirect = ex->follow_redirect;
    request_params.no_modify_req = ex->no_modify_req;
    request_params.timeout = ex->timeout;
    pool.http_request<typename Req::body_type, typename Res::body_type>(
      ex->url, std::move(request_copy),
      [cb = std::move(cb), ex](std::optional<Res> resp, int err) mutable {
            if (err == 0 && resp.has_value()) {
              ex->response = std::move(resp);
              cb(monad::Result<ExchangePtr, monad::Error>::Ok(std::move(ex)));
            } else {
              BOOST_LOG_SEV(ex->lg, trivial::error)
                  << "http_request_io failed with error num: " << err;
              cb(monad::Result<ExchangePtr, monad::Error>::Err(
                  monad::Error{err, "http_request_io failed"}));
            }
          },
          std::move(request_params),
          ex->proxy);
    });
  };
}

}  // namespace monad
