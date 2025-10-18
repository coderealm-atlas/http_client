#pragma once

#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>
#include <boost/json/value_to.hpp>

#include <stdexcept>
#include <string>
#include <string_view>

#include "api_handler_base.hpp"
#include "result_monad.hpp"

namespace monad {

inline Error tag_invoke(const json::value_to_tag<Error>&,
                        const json::value& jv) {
  const auto& obj = jv.as_object();

  Error err;
  if (auto it = obj.if_contains("code")) {
    err.code = json::value_to<int>(*it);
  } else {
    throw std::runtime_error("Missing 'code' field for Error");
  }
  if (auto it = obj.if_contains("what")) {
    err.what = json::value_to<std::string>(*it);
  }
  if (auto it = obj.if_contains("key")) {
    err.key = json::value_to<std::string>(*it);
  }
  if (auto it = obj.if_contains("response_status")) {
    err.response_status = json::value_to<int>(*it);
  }
  if (auto it = obj.if_contains("params")) {
    err.params = json::value_to<json::object>(*it);
  }
  if (auto it = obj.if_contains("content_type")) {
    err.content_type = json::value_to<std::string>(*it);
  }
  if (auto it = obj.if_contains("alternative_body")) {
    err.alternative_body = *it;
  }
  return err;
}

}  // namespace monad

namespace apihandler {

namespace api_response_errors {
inline constexpr int invalid_schema = 9000;
inline constexpr int malformed = 9001;
}  // namespace api_response_errors

template <class T>
using ApiResponseResult = monad::MyResult<ApiDataResponse<T>>;

template <class T>
ApiResponseResult<T> tag_invoke(
    const json::value_to_tag<ApiResponseResult<T>>&, const json::value& jv) {
  auto make_result_error = [&](int code, std::string_view msg) {
    return ApiResponseResult<T>::Err(monad::make_error(
        code, std::string(msg) + ", json: " + json::serialize(jv)));
  };

  try {
    if (!jv.is_object()) {
      return make_result_error(api_response_errors::invalid_schema,
                               "ApiResponse is not an object");
    }
    const auto& obj = jv.as_object();

    if (auto* error_p = obj.if_contains("error")) {
      return ApiResponseResult<T>::Err(json::value_to<monad::Error>(*error_p));
    }
    if (obj.if_contains("data")) {
      return ApiResponseResult<T>::Ok(json::value_to<ApiDataResponse<T>>(jv));
    }

    return make_result_error(
        api_response_errors::invalid_schema,
        "Neither data nor error field found in ApiResponse");
  } catch (const std::exception& e) {
    return make_result_error(
        api_response_errors::malformed,
        std::string("error in parsing ApiResponse: ") + e.what());
  }
}

}  // namespace apihandler
