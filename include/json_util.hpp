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
bool bool_from_json_ob(const json::value& jv, const std::string& key);

std::string replace_env_var(
    const std::string& input,
    const std::map<std::string, std::string>& cli_map,
    const std::map<std::string, std::string>& properties_map);

void substitue_envs(json::value& jv,
                    const std::map<std::string, std::string>& cli_map,
                    const std::map<std::string, std::string>& properties_map);

void pretty_print(std::ostream& os, json::value const& jv,
                  std::string* indent = nullptr);
std::string prettyPrint(const json::value& val, int level = 0);
}  // namespace jsonutil
