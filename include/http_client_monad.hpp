#pragma once

#include <fmt/format.h>

#include <algorithm>
#include <boost/beast/http.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/file_body.hpp>
#include <boost/core/detail/string_view.hpp>
#include <boost/json/serializer.hpp>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

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
  std::shared_ptr<const ProxySetting> proxy{};
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
        if (token.size() >= prefix.size() &&
            token.substr(0, prefix.size()) == prefix) {
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

  // Parses the full JSON response body into `T`.
  // Use this when the server returns the structure you need at the top level,
  // e.g. `{ "cursor": "...", "signals": [...] }` → struct with those fields.
  // For `{"data": {...}}` wrappers prefer parseJsonDataResponse.
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
        [response_status,
         response_body_preview](json::value jv) -> MyResult<ValueType> {
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

  // Parses the JSON response body and extracts the `data` member before
  // converting to `T`. Intended for APIs that wrap payloads as
  // `{ "data": {...} }`. If the desired structure lives at the top level, use
  // parseJsonResponse instead.
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
        [response_status,
         response_body_preview](json::value jv) -> MyResult<ValueType> {
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

  // Parses the full JSON body into a `MyResult<...>` payload. This variant is
  // reserved for endpoints that already encode success/error in the JSON
  // document, typically produced by ApiHandler helpers. For regular structs use
  // parseJsonResponse / parseJsonDataResponse.
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

    return getJsonResponse().and_then([response_status, response_body_preview](
                                          json::value jv) -> Requested {
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
          err.params["response_body_preview"] = make_preview(response->body());
          return MyResult<json::value>::Err(std::move(err));
        }
        return MyResult<json::value>::Ok(json::parse(response_string));
      } else {
        Error err{JSON_ERR_DECODE,
                  "Failed to decode/parse JSON (low-level): response is not "
                  "available"};
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
        Error err{JSON_ERR_DECODE,
                  fmt::format("Failed to decode/parse JSON (low-level): {}",
                              e.what())};
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
struct GetFileTag {};    // Downloads response to file_body
struct GetHeaderTag {};
struct DeleteTag {};

template <>
struct TagTraits<GetStringTag> {
  using Request = http::request<http::empty_body>;
  using Response = http::response<http::string_body>;
};

template <>
struct TagTraits<GetFileTag> {
  using Request = http::request<http::empty_body>;
  using Response = http::response<http::file_body>;
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
    } else if constexpr (std::is_same_v<Tag, GetFileTag>) {
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
      auto trim = [](std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                              s.front() == '\n' || s.front() == '\r')) {
          s.remove_prefix(1);
        }
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                              s.back() == '\n' || s.back() == '\r')) {
          s.remove_suffix(1);
        }
        return s;
      };

      auto to_lower = [](std::string_view s) {
        std::string out{s};
        std::transform(
            out.begin(), out.end(), out.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
      };

      auto should_bypass_env_proxy_for_url = [&](const urls::url& url) {
        const char* no_proxy_c = std::getenv("NO_PROXY");
        if (!no_proxy_c || !*no_proxy_c) {
          no_proxy_c = std::getenv("no_proxy");
        }
        if (!no_proxy_c || !*no_proxy_c) {
          return false;
        }

        auto host_sv = url.host();
        if (host_sv.empty()) {
          return false;
        }

        const std::string host_lc = to_lower(host_sv);
        std::string_view list{no_proxy_c};
        while (!list.empty()) {
          auto comma = list.find(',');
          auto token =
              comma == std::string_view::npos ? list : list.substr(0, comma);
          token = trim(token);

          if (!token.empty()) {
            if (token == "*") {
              return true;
            }

            // Strip optional port in token (best-effort; minimal support).
            // Examples: "example.com:8080" -> "example.com"
            if (auto pos = token.rfind(':'); pos != std::string_view::npos) {
              auto port_part = token.substr(pos + 1);
              bool all_digits = !port_part.empty();
              for (char c : port_part) {
                if (c < '0' || c > '9') {
                  all_digits = false;
                  break;
                }
              }
              if (all_digits) {
                token = token.substr(0, pos);
                token = trim(token);
              }
            }

            const std::string token_lc = to_lower(token);
            if (token_lc.empty()) {
              // continue
            } else if (host_lc == token_lc) {
              return true;
            } else {
              // Suffix match:
              // - token ".example.com" matches "a.example.com" (but not
              // "example.com")
              // - token "example.com" matches "example.com" and "a.example.com"
              std::string_view suffix = token_lc;
              bool require_dot = false;
              if (!suffix.empty() && suffix.front() == '.') {
                suffix.remove_prefix(1);
                require_dot = true;
              }

              if (!suffix.empty() && host_lc.size() > suffix.size() &&
                  host_lc.compare(host_lc.size() - suffix.size(), suffix.size(),
                                  suffix) == 0) {
                const auto dot_pos = host_lc.size() - suffix.size();
                if (!require_dot ||
                    (dot_pos > 0 && host_lc[dot_pos - 1] == '.')) {
                  return true;
                }
              }
            }
          }

          if (comma == std::string_view::npos) {
            break;
          }
          list.remove_prefix(comma + 1);
        }
        return false;
      };

      HttpClientRequestParams request_params;
      request_params.body_file = ex->body_file;
      request_params.follow_redirect = ex->follow_redirect;
      request_params.no_modify_req = ex->no_modify_req;
      request_params.timeout = ex->timeout;

      if (!ex->proxy && pool.has_proxy_pool()) {
        ex->proxy = pool.borrow_proxy();
      }

      // If proxy is inherited from env, honor NO_PROXY for this request.
      if (ex->proxy && ex->proxy->from_env &&
          should_bypass_env_proxy_for_url(ex->url)) {
        ex->proxy.reset();
      }

      auto req = ex->request;
      if (!ex->no_modify_req) {
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        std::string target = ex->url.encoded_path().empty()
                                 ? "/"
                                 : std::string(ex->url.encoded_path());
        if (!ex->url.encoded_query().empty()) {
          target = fmt::format("{}?{}", target,
                               std::string(ex->url.encoded_query()));
        }
        req.target(target);
      }

      if (verbose > 4) {  // trace
        std::cerr << "Before request headers: " << req.base() << std::endl;
      }

      pool.http_request<typename Req::body_type, typename Res::body_type>(
          ex->url, std::move(req),
          [cb = std::move(cb), ex](std::optional<Res> resp, int err) mutable {
            if (err == 0 && resp.has_value()) {
              ex->response = std::move(resp);
              cb(monad::Result<ExchangePtr, monad::Error>::Ok(std::move(ex)));
              return;
            }

            const auto url_view = ex->url.buffer();
            BOOST_LOG_SEV(ex->lg, trivial::error)
                << "http_request_io failed with error num: " << err
                << ", url:  " << url_view;
            cb(monad::Result<ExchangePtr, monad::Error>::Err(monad::Error{
                err,
                fmt::format("http_request_io failed, url: {}", url_view)}));
          },
          HttpClientRequestParams{request_params}, ex->proxy.get());
    });
  };
}

}  // namespace monad
