#pragma once

#include <boost/json.hpp>
#include <cstdint>
#include <map>

#include "result_monad.hpp"

namespace json = boost::json;
namespace jsonutil {

using monad::MyResult;

MyResult<json::object> consume_object_at(json::value&& val,
                                         std::string_view k1);

MyResult<json::object> expect_object_at(json::value&& val, std::string_view k1,
                                        std::string_view k2);
MyResult<json::object> expect_object_at(json::value&& val, std::string_view k1,
                                        std::string_view k2,
                                        std::string_view k3);
monad::MyVoidResult expect_true_at(const json::value& val, std::string_view k1);

MyResult<json::value> consume_value_at(json::value&& val, std::string_view k1);

MyResult<std::reference_wrapper<const json::object>> reference_object_at(
    const json::object& val, std::string_view k1);

MyResult<std::reference_wrapper<const json::value>> reference_value_at(
    const json::value& val, std::string_view k1);

bool could_be_uint64(const json::value& jv, uint64_t& out_value);
MyResult<uint64_t> to_uint64(const json::value& jv);

std::string replace_env_var(
    const std::string& input,
    const std::map<std::string, std::string>& extra_map);

void substitue_envs(json::value& jv,
                    const std::map<std::string, std::string>& extra_map);

inline bool is_integral(json::value const& jv) {
  return jv.kind() == json::kind::int64 || jv.kind() == json::kind::uint64;
}

uint64_t uint64_from_json_ob(const json::value& jv,
                             const std::string& key = "");
bool bool_from_json_ob(const json::value& jv, const std::string& key = "");

void pretty_print(std::ostream& os, json::value const& jv,
                  std::string* indent = nullptr);
std::string prettyPrint(const json::value& val, int level = 0);
}  // namespace jsonutil
