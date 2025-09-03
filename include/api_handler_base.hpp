#pragma once
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/file_body_fwd.hpp>
#include <boost/beast/http/message_fwd.hpp>
#include <filesystem>
#include <iostream>

#include "httpclient_error_codes.hpp"
#include "io_monad.hpp"
#include "resp_datastruct.hpp"
#include "result_monad.hpp"

namespace http = boost::beast::http;
namespace fs = std::filesystem;
namespace apihandler {

template <typename T>
struct ApiResponse {
  std::variant<std::monostate, T, std::vector<T>> data = std::monostate{};
  std::optional<resp::DataMeta> meta;
  std::string content_type = "application/json";

  // Default constructor
  ApiResponse() = default;

  // Constructors
  template <typename U = T>
  requires(!std::same_as<U, std::vector<T>> &&
           !std::same_as<U, resp::ListResult<T>>)
  ApiResponse(T&& val, const std::string& content_type = "application/json")
      : data(std::move(val)), content_type(content_type) {}
  // Deleted constructor for lvalue reference to prevent copy
  ApiResponse(const T& val,
              const std::string& content_type = "application/json") = delete;

  ApiResponse(std::vector<T>&& vec,
              const std::string& content_type = "application/json")
      : data(std::move(vec)),
        meta(resp::DataMeta{vec.size(), 0, vec.size()}),
        content_type(content_type) {}
  // Deleted constructor for lvalue reference to vector
  ApiResponse(const std::vector<T>&,
              const std::string& content_type = "application/json") = delete;

  ApiResponse(resp::ListResult<T>&& result)
      : data(std::move(result.data)),
        meta(std::move(result.meta)),
        content_type("application/json") {}

  ApiResponse(const resp::ListResult<T>&,
              const std::string& content_type = "application/json") = delete;

  // JSON serialization
  friend void tag_invoke(const json::value_from_tag&, json::value& jv,
                         const ApiResponse<T>& resp) {
    json::object jo;

    if (std::holds_alternative<T>(resp.data)) {
      jo["data"] = json::value_from(std::get<T>(resp.data));
    } else if (std::holds_alternative<std::vector<T>>(resp.data)) {
      jo["data"] = json::value_from(std::get<std::vector<T>>(resp.data));
    } else {
      jo["data"] = nullptr;  // or leave it out entirely
    }

    if (resp.meta) {
      jo["meta"] = json::value_from(resp.meta.value());
    }

    jv = std::move(jo);
  }

  bool is_single() const { return std::holds_alternative<T>(data); }
  bool is_list() const { return std::holds_alternative<std::vector<T>>(data); }
  bool is_empty() const { return std::holds_alternative<std::monostate>(data); }

  friend ApiResponse<T> tag_invoke(const json::value_to_tag<ApiResponse<T>>&,
                                   const json::value& jv) {
    ApiResponse<T> resp{};  // default-constructed, data is monostate
    if (auto* jo = jv.if_object()) {
      if (auto* data_p = jo->if_contains("data")) {
        if (auto* data_array_p = data_p->if_array()) {
          std::vector<T> vec;
          vec.reserve(data_array_p->size());
          for (size_t i = 0; i < data_array_p->size(); ++i) {
            vec.push_back(json::value_to<T>((*data_array_p)[i]));
          }
          resp.data = std::move(vec);
        } else {
          resp.data = json::value_to<T>(*data_p);
        }
      }
      if (auto* meta_p = jo->if_contains("meta")) {
        resp.meta = json::value_to<resp::DataMeta>(*meta_p);
      }
    }
    return resp;
  }
};

struct NoContent {};

struct Success {
  int64_t code;
  std::string message;

  friend void tag_invoke(const json::value_from_tag&, json::value& jv,
                         const Success& s) {
    jv = json::value{{"code", s.code}, {"message", s.message}};
  }
};

struct DownloadInline {
  std::string content;
  std::string content_type;
  std::string filename;
};

struct DownloadFile {
  fs::path path;
  std::string content_type;
  std::string filename;
};

struct Redirect {
  std::string location;  // Target URL to redirect to
  int status = 302;      // HTTP status code: usually 302, 301, 303, etc.

  Redirect() = default;

  Redirect(std::string location, int status = 302)
      : location(std::move(location)), status(status) {}
};

// ---------- Response Generator ----------

template <typename Resp>
auto make_io_response(Resp&& r) {
  using T = std::decay_t<Resp>;
  return monad::IO<T>::pure(std::move(r));  // enforce move
}
// template <typename Resp>
// http::message_generator make_io_response(http::message_generatorResp&& r) {
//   // using T = std::decay_t<Resp>;
//   return monad::IO<http::message_generator>::pure(
//       std::move(r));  // enforce move
// }

struct ResponseGenerator {
  // ApiResponse<T> → http::string_body
  template <typename T>
  auto operator()(ApiResponse<T>&& resp) const {
    http::response<http::string_body> res;
    res.version(11);
    json::value jv = json::value_from(resp);
    std::string body = json::serialize(jv);
    res.body() = std::move(body);
    res.set(http::field::content_type, "application/json");
    res.result(http::status::ok);
    res.prepare_payload();
    return make_io_response(std::move(res));
  }

  // Download → string_body
  auto operator()(DownloadInline&& d) const {
    http::response<http::string_body> res;
    res.version(11);
    res.result(http::status::ok);
    res.body() = std::move(d.content);
    res.set(http::field::content_type, d.content_type);
    res.set(http::field::content_disposition,
            "attachment; filename=\"" + d.filename + "\"");
    res.prepare_payload();
    return make_io_response(std::move(res));
  }

  auto operator()(DownloadFile&& d) const {
    http::response<http::file_body> res;
    res.version(11);
    res.result(http::status::ok);

    boost::beast::error_code bec;
    res.body().open(d.path.c_str(), boost::beast::file_mode::scan, bec);
    if (bec) {
      return monad::IO<http::response<http::file_body>>::fail(
          monad::Error{httpclient_errors::RESPONSE::DOWNLOAD_FILE_OPEN_FAILED,
                       "open download file failed."});
    }
    res.set(http::field::content_type, d.content_type);
    res.set(http::field::content_disposition,
            "attachment; filename=\"" + d.filename + "\"");
    res.prepare_payload();
    return make_io_response(std::move(res));
  }

  // Redirect → string_body
  auto operator()(Redirect&& r) const {
    http::response<http::string_body> res;
    res.version(11);
    res.result(static_cast<http::status>(r.status));
    res.set(http::field::location, std::move(r.location));
    res.prepare_payload();
    return make_io_response(std::move(res));
  }

  // NoContent → empty_body
  auto operator()(NoContent&&) const {
    http::response<http::empty_body> res;
    res.version(11);
    res.result(http::status::no_content);
    return make_io_response(std::move(res));
  }

  // Success → string_body
  auto operator()(Success&& s) const {
    http::response<http::string_body> res;
    res.version(11);
    res.result(http::status::ok);
    json::value jv = json::value_from(s);
    std::string body = json::serialize(jv);
    res.body() = std::move(body);
    res.set(http::field::content_type, "application/json");
    res.prepare_payload();
    return make_io_response(std::move(res));
  }
};

inline constexpr ResponseGenerator http_response_gen_fn;

}  // namespace apihandler