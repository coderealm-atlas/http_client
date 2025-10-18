#include "json_util.hpp"

#include <boost/json.hpp>
#include <boost/json/fwd.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <charconv>
#include <cstdint>
#include <cstdlib>  // for std::getenv
#include <fmt/format.h>
#include <iostream>
#include <string>

#include "result_monad.hpp"

namespace jsonutil {
MyResult<json::object> consume_object_at(json::value&& val,
                                         std::string_view k1) {
  if (auto* ob_p = val.if_object()) {
    if (auto* k1_p = ob_p->if_contains(k1)) {
      if (auto* k1_o_p = k1_p->if_object()) {
        return MyResult<json::object>::Ok(std::move(*k1_o_p));
      }
    }
  }
  return MyResult<json::object>::Err(
    monad::make_error(
      1, fmt::format("Expect object but not an object. body: {}",
              json::serialize(val))));
}

MyResult<std::reference_wrapper<const json::object>> reference_object_at(
    const json::value& val, std::string_view k1) {
  if (auto* ob_p = val.if_object()) {
    if (auto* k1_p = ob_p->if_contains(k1)) {
      if (auto* k1_o_p = k1_p->if_object()) {
        return MyResult<std::reference_wrapper<const json::object>>::Ok(
            std::cref(*k1_o_p));
      }
    }
  }
  return MyResult<std::reference_wrapper<const json::object>>::Err(
    monad::make_error(
      1, fmt::format("Expect object but not an object. body: {}",
              json::serialize(val))));
}

MyResult<json::value> consume_value_at(json::value&& val, std::string_view k1) {
  if (auto* ob_p = val.if_object()) {
    if (auto* k1_p = ob_p->if_contains(k1)) {
      return MyResult<json::value>::Ok(std::move(*k1_p));
    }
  }
  return MyResult<json::value>::Err(
    monad::make_error(
      1, fmt::format("Expect object but not an object. body: {}",
              json::serialize(val))));
}
MyResult<std::reference_wrapper<const json::value>> reference_value_at(
    const json::value& val, std::string_view k1) {
  if (auto* ob_p = val.if_object()) {
    if (auto* k1_p = ob_p->if_contains(k1)) {
      return MyResult<std::reference_wrapper<const json::value>>::Ok(
          std::cref(*k1_p));
    }
  }
  return MyResult<std::reference_wrapper<const json::value>>::Err(
    monad::make_error(
      1, fmt::format("Expect object but not an object. body: {}",
              json::serialize(val))));
}

MyResult<json::object> expect_object_at(json::value&& val, std::string_view k1,
                                        std::string_view k2) {
  if (!val.is_object())
  return MyResult<json::object>::Err(
    monad::make_error(1, "Not an json::object at root"));

  auto& obj1 = val.as_object();
  auto it1 = obj1.find(k1);
  if (it1 == obj1.end())
  return MyResult<json::object>::Err(
    monad::make_error(2, "Key not found: " + std::string(k1)));
  if (!it1->value().is_object())
  return MyResult<json::object>::Err(
    monad::make_error(3, "Expected json::object at key: " + std::string(k1)));

  auto& obj2 = it1->value().as_object();
  auto it2 = obj2.find(k2);
  if (it2 == obj2.end())
  return MyResult<json::object>::Err(
    monad::make_error(4, "Key not found: " + std::string(k2)));
  if (!it2->value().is_object())
  return MyResult<json::object>::Err(
    monad::make_error(5, "Expected json::object at key: " + std::string(k2)));

  return MyResult<json::object>::Ok(std::move(it2->value().as_object()));
}

MyResult<json::object> expect_object_at(json::value&& val, std::string_view k1,
                                        std::string_view k2,
                                        std::string_view k3) {
  if (!val.is_object())
  return MyResult<json::object>::Err(
    monad::make_error(1, "Not an json::object at root"));

  auto& obj1 = val.as_object();
  auto it1 = obj1.find(k1);
  if (it1 == obj1.end())
  return MyResult<json::object>::Err(
    monad::make_error(2, "Key not found: " + std::string(k1)));
  if (!it1->value().is_object())
  return MyResult<json::object>::Err(
    monad::make_error(3, "Expected json::object at key: " + std::string(k1)));

  auto& obj2 = it1->value().as_object();
  auto it2 = obj2.find(k2);
  if (it2 == obj2.end())
  return MyResult<json::object>::Err(
    monad::make_error(4, "Key not found: " + std::string(k2)));
  if (!it2->value().is_object())
  return MyResult<json::object>::Err(
    monad::make_error(5, "Expected json::object at key: " + std::string(k2)));

  auto& obj3 = it2->value().as_object();
  auto it3 = obj3.find(k3);
  if (it3 == obj3.end())
  return MyResult<json::object>::Err(
    monad::make_error(6, "Key not found: " + std::string(k3)));
  if (!it3->value().is_object())
  return MyResult<json::object>::Err(
    monad::make_error(7, "Expected json::object at key: " + std::string(k3)));

  return MyResult<json::object>::Ok(std::move(it3->value().as_object()));
}

monad::MyVoidResult expect_true_at(const json::value& val,
                                   std::string_view k1) {
  if (auto* jo_p = val.if_object()) {
    if (auto* k1_p = jo_p->if_contains(k1)) {
      if (auto* b_p = k1_p->if_bool()) {
        if (*b_p) {
          return monad::MyVoidResult::Ok();
        }
      }
    }
  }
  return monad::MyVoidResult::Err(
    monad::make_error(1, "Expected true at key: " + std::string(k1)));
}
// Helper function to replace ${VARIABLE} or ${VARIABLE:-default} with the
// environment variable
std::string replace_env_var(
    const std::string& input, const std::map<std::string, std::string>& cli_map,
    const std::map<std::string, std::string>& properties_map) {
  std::string output = input;
  size_t pos = 0;
  while (true) {
    size_t start = output.find("${", pos);
    if (start == std::string::npos) break;
    size_t end = output.find('}', start + 2);
    if (end == std::string::npos) break;  // unmatched, stop processing

    std::string token =
        output.substr(start + 2, end - start - 2);  // VAR or VAR:-default
    std::string var = token;
    std::string default_val;

    if (auto delim = token.find(":-"); delim != std::string::npos) {
      var = token.substr(0, delim);
      default_val = token.substr(delim + 2);
    }

    // Trim possible whitespace around var (optional; comment out if not
    // desired) while (!var.empty() && isspace(static_cast<unsigned
    // char>(var.front()))) var.erase(var.begin()); while (!var.empty() &&
    // isspace(static_cast<unsigned char>(var.back()))) var.pop_back();

    std::string replacement;
    if (auto cli_it = cli_map.find(var); cli_it != cli_map.end()) {
      replacement = cli_it->second;  // 1. CLI overrides
    } else if (const char* env_value = std::getenv(var.c_str());
               env_value && *env_value) {
      replacement = env_value;  // 2. environment variables
    } else if (auto it = properties_map.find(var); it != properties_map.end()) {
      replacement = it->second;  // 3. properties fallback
    } else if (!default_val.empty()) {
      replacement = default_val;  // 4. default in pattern
    } else {
      // 5. leave unresolved pattern intact; advance past it
      pos = end + 1;
      continue;
    }

    output.replace(start, end - start + 1, replacement);
    pos = start + replacement.size();
  }
  return output;
}

// Function to substitute environment variables in JSON values
void substitue_envs(boost::json::value& jv,
                    const std::map<std::string, std::string>& cli_map,
                    const std::map<std::string, std::string>& properties_map) {
  switch (jv.kind()) {
    case boost::json::kind::object: {
      auto& obj = jv.get_object();
      for (auto& [key, value] : obj) {
        substitue_envs(value, cli_map, properties_map);  // Recurse nested
      }
      break;
    }
    case boost::json::kind::array: {
      auto& arr = jv.get_array();
      for (auto& element : arr) {
        substitue_envs(element, cli_map, properties_map);  // Recurse arrays
      }
      break;
    }
    case boost::json::kind::string: {
      std::string original = jv.get_string().c_str();
      std::string substituted =
          replace_env_var(original, cli_map, properties_map);
      jv = substituted;  // Update the JSON value with substituted string
      break;
    }
    case boost::json::kind::uint64:
    case boost::json::kind::int64:
    case boost::json::kind::double_:
    case boost::json::kind::bool_:
    case boost::json::kind::null:
      // No substitution needed for these types
      break;
  }
}

void pretty_print(std::ostream& os, json::value const& jv,
                  std::string* indent) {
  std::string indent_;
  if (!indent) indent = &indent_;
  switch (jv.kind()) {
    case json::kind::object: {
      os << "{\n";
      indent->append(4, ' ');
      auto const& obj = jv.get_object();
      if (!obj.empty()) {
        auto it = obj.begin();
        for (;;) {
          os << *indent << json::serialize(it->key()) << " : ";
          pretty_print(os, it->value(), indent);
          if (++it == obj.end()) break;
          os << ",\n";
        }
      }
      os << "\n";
      indent->resize(indent->size() - 4);
      os << *indent << "}";
      break;
    }

    case json::kind::array: {
      os << "[\n";
      indent->append(4, ' ');
      auto const& arr = jv.get_array();
      if (!arr.empty()) {
        auto it = arr.begin();
        for (;;) {
          os << *indent;
          pretty_print(os, *it, indent);
          if (++it == arr.end()) break;
          os << ",\n";
        }
      }
      os << "\n";
      indent->resize(indent->size() - 4);
      os << *indent << "]";
      break;
    }

    case json::kind::string: {
      os << json::serialize(jv.get_string());
      break;
    }

    case json::kind::uint64:
    case json::kind::int64:
    case json::kind::double_:
      os << jv;
      break;

    case json::kind::bool_:
      if (jv.get_bool())
        os << "true";
      else
        os << "false";
      break;

    case json::kind::null:
      os << "null";
      break;
  }

  if (indent->empty()) os << "\n";
}

bool bool_from_json_ob(const json::value& jv, const std::string& key) {
  const json::value* jv_p =
      key.empty()
          ? &jv
          : (jv.is_object()
                 ? (jv.as_object().contains(key) ? &jv.at(key) : nullptr)
                 : nullptr);

  if (jv_p) {
    if (jv_p->is_bool()) {
      return jv_p->get_bool();
    } else if (auto* id_v_p = jv_p->if_string()) {
      if (id_v_p->empty()) {
        return false;
      }
      if (*id_v_p == "true" || *id_v_p == "1" || *id_v_p == "yes" ||
          *id_v_p == "on") {
        return true;
      } else {
        return false;
      }
    } else {
      std::cerr << "I can't convert it to a bool: " << *jv_p << std::endl;
      return false;
    }
  } else {
    std::cerr << "bool_from_json_ob, " << key << "  not found in json: " << jv
              << std::endl;
    return false;
  }
}

std::string indent(int level) { return std::string(level * 2, ' '); }

std::string prettyPrint(const boost::json::value& val, int level) {
  using namespace boost::json;

  switch (val.kind()) {
    case kind::null:
      return "null";

    case kind::bool_:
      return val.get_bool() ? "true" : "false";

    case kind::int64:
      return std::to_string(val.get_int64());

    case kind::uint64:
      return std::to_string(val.get_uint64());

    case kind::double_:
      return std::to_string(val.get_double());

    case kind::string:
      return fmt::format(R"("{}")", json::value_to<std::string>(val));

    case kind::array: {
      const array& arr = val.get_array();
      if (arr.empty()) return "[]";
      std::string out = "[\n";
      for (size_t i = 0; i < arr.size(); ++i) {
        out += indent(level + 1) + prettyPrint(arr[i], level + 1);
        if (i < arr.size() - 1) out += ",";
        out += "\n";
      }
      out += indent(level) + "]";
      return out;
    }

    case kind::object: {
      const object& obj = val.get_object();
      if (obj.empty()) return "{}";
      std::string out = "{\n";
      size_t count = 0;
      for (const auto& [key, v] : obj) {
        out += indent(level + 1) + "\"" + std::string(key) +
               "\": " + prettyPrint(v, level + 1);
        if (++count < obj.size()) out += ",";
        out += "\n";
      }
      out += indent(level) + "}";
      return out;
    }

    default:
      return "";  // Should not happen
  }
}
}  // namespace jsonutil
