#include "json_util.hpp"

#include <boost/json.hpp>
#include <boost/json/fwd.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <charconv>
#include <cstdint>
#include <format>
#include <iostream>
#include <string>

#include "result_monad.hpp"

namespace jsonutil {
MyResult<json::object> expect_object_at(json::value&& val,
                                        std::string_view k1) {
  if (auto* ob_p = val.if_object()) {
    if (auto* k1_p = ob_p->if_contains(k1)) {
      if (auto* k1_o_p = k1_p->if_object()) {
        return MyResult<json::object>::Ok(std::move(*k1_o_p));
      }
    }
  }
  return MyResult<json::object>::Err(
      {.code = 1,
       .what = std::format("Expect object but not an object. body: {}",
                           json::serialize(val))});
}

MyResult<json::value> expect_value_at(json::value&& val, std::string_view k1) {
  if (auto* ob_p = val.if_object()) {
    if (auto* k1_p = ob_p->if_contains(k1)) {
      return MyResult<json::value>::Ok(std::move(*k1_p));
    }
  }
  return MyResult<json::value>::Err(
      {.code = 1,
       .what = std::format("Expect object but not an object. body: {}",
                           json::serialize(val))});
}

MyResult<json::object> expect_object_at(json::value&& val, std::string_view k1,
                                        std::string_view k2) {
  if (!val.is_object())
    return MyResult<json::object>::Err({1, "Not an json::object at root"});

  auto& obj1 = val.as_object();
  auto it1 = obj1.find(k1);
  if (it1 == obj1.end())
    return MyResult<json::object>::Err(
        {2, "Key not found: " + std::string(k1)});
  if (!it1->value().is_object())
    return MyResult<json::object>::Err(
        {3, "Expected json::object at key: " + std::string(k1)});

  auto& obj2 = it1->value().as_object();
  auto it2 = obj2.find(k2);
  if (it2 == obj2.end())
    return MyResult<json::object>::Err(
        {4, "Key not found: " + std::string(k2)});
  if (!it2->value().is_object())
    return MyResult<json::object>::Err(
        {5, "Expected json::object at key: " + std::string(k2)});

  return MyResult<json::object>::Ok(std::move(it2->value().as_object()));
}

MyResult<json::object> expect_object_at(json::value&& val, std::string_view k1,
                                        std::string_view k2,
                                        std::string_view k3) {
  if (!val.is_object())
    return MyResult<json::object>::Err({1, "Not an json::object at root"});

  auto& obj1 = val.as_object();
  auto it1 = obj1.find(k1);
  if (it1 == obj1.end())
    return MyResult<json::object>::Err(
        {2, "Key not found: " + std::string(k1)});
  if (!it1->value().is_object())
    return MyResult<json::object>::Err(
        {3, "Expected json::object at key: " + std::string(k1)});

  auto& obj2 = it1->value().as_object();
  auto it2 = obj2.find(k2);
  if (it2 == obj2.end())
    return MyResult<json::object>::Err(
        {4, "Key not found: " + std::string(k2)});
  if (!it2->value().is_object())
    return MyResult<json::object>::Err(
        {5, "Expected json::object at key: " + std::string(k2)});

  auto& obj3 = it2->value().as_object();
  auto it3 = obj3.find(k3);
  if (it3 == obj3.end())
    return MyResult<json::object>::Err(
        {6, "Key not found: " + std::string(k3)});
  if (!it3->value().is_object())
    return MyResult<json::object>::Err(
        {7, "Expected json::object at key: " + std::string(k3)});

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
      {1, "Expected true at key: " + std::string(k1)});
}
// Helper function to replace ${VARIABLE} or ${VARIABLE:-default} with the
// environment variable
std::string replace_env_var(
    const std::string& input,
    const std::map<std::string, std::string>& extra_map) {
  std::string output = input;

  // Basic parsing for ${VARIABLE} or ${VARIABLE:-default} patterns
  size_t start = output.find("${");
  size_t end = output.find('}', start);
  if (start != std::string::npos && end != std::string::npos) {
    std::string env_var = output.substr(start + 2, end - start - 2);
    std::string default_val;

    // Check for the ":-" syntax for default values
    size_t delim = env_var.find(":-");
    if (delim != std::string::npos) {
      default_val = env_var.substr(delim + 2);
      env_var = env_var.substr(0, delim);
    }

    if (extra_map.find(env_var) != extra_map.end()) {
      output.replace(start, end - start + 1, extra_map.at(env_var));
    } else {
      // Substitute environment variable or default
      const char* env_value = std::getenv(env_var.c_str());
      if (env_value) {
        output.replace(start, end - start + 1, env_value);
      } else {
        return default_val;
      }
    }
  }
  return output;
}

// Function to substitute environment variables in JSON values
void substitue_envs(boost::json::value& jv,
                    const std::map<std::string, std::string>& extra_map) {
  switch (jv.kind()) {
    case boost::json::kind::object: {
      auto& obj = jv.get_object();
      for (auto& [key, value] : obj) {
        substitue_envs(value, extra_map);  // Recurse into nested values
      }
      break;
    }
    case boost::json::kind::array: {
      auto& arr = jv.get_array();
      for (auto& element : arr) {
        substitue_envs(element, extra_map);  // Recurse into array elements
      }
      break;
    }
    case boost::json::kind::string: {
      std::string original = jv.get_string().c_str();
      std::string substituted = replace_env_var(original, extra_map);
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

uint64_t uint64_from_json_ob(const json::value& jv, const std::string& key) {
  const json::value* jv_p =
      key.empty()
          ? &jv
          : (jv.is_object()
                 ? (jv.as_object().contains(key) ? &jv.at(key) : nullptr)
                 : nullptr);

  if (jv_p) {
    if (jv_p->is_number()) {
      return jv_p->to_number<uint64_t>();
    } else if (auto* id_v_p = jv_p->if_string()) {
      if (id_v_p->empty()) {
        return 0;
      }
      uint64_t id = 0;
      auto [ptr, ec] =
          std::from_chars(id_v_p->data(), id_v_p->data() + id_v_p->size(), id);
      if (ec != std::errc{} || ptr != id_v_p->data() + id_v_p->size()) {
        std::cerr << "I can't convert it to an uint64: " << *jv_p << std::endl;
        return 0;
      }
      return id;
    } else {
      std::cerr << "I can't convert it to an uint64: " << *jv_p << std::endl;
      return 0;
    }
  } else {
    return 0;
  }
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
      if (*id_v_p == "true") {
        return true;
      } else if (*id_v_p == "false") {
        return false;
      } else {
        std::cerr << "I can't convert it to a bool: " << *jv_p << std::endl;
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
      return std::format(R"("{}")", json::value_to<std::string>(val));

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

bool could_be_uint64(const json::value& jv, uint64_t& out_value) {
  if (jv.is_uint64()) {
    out_value = jv.to_number<uint64_t>();
    return true;
  } else if (jv.is_int64()) {
    auto v = jv.to_number<int64_t>();
    if (v >= 0) {
      out_value = static_cast<uint64_t>(v);
      return true;
    }
  } else if (jv.is_double()) {
    double v = jv.to_number<double>();
    if (v >= 0 && v == std::floor(v) && v <= static_cast<double>(UINT64_MAX)) {
      out_value = static_cast<uint64_t>(v);
      return true;
    }
  }
  return false;
}

MyResult<uint64_t> to_uint64(const json::value& jv) {
  if (jv.is_uint64()) {
    return MyResult<uint64_t>::Ok(jv.to_number<uint64_t>());
  } else if (jv.is_int64()) {
    auto v = jv.to_number<int64_t>();
    if (v >= 0) {
      return MyResult<uint64_t>::Ok(static_cast<uint64_t>(v));
    } else {
      return MyResult<uint64_t>::Err({.code = 1, .what = "less than 0."});
    }
  } else if (jv.is_double()) {
    double v = jv.to_number<double>();
    if (v >= 0 && v == std::floor(v) && v <= static_cast<double>(UINT64_MAX)) {
      return MyResult<uint64_t>::Ok(static_cast<uint64_t>(v));
    } else {
      return MyResult<uint64_t>::Err({.code = 1, .what = "out of range."});
    }
  } else {
    return MyResult<uint64_t>::Err({.code = 1, .what = "not a number."});
  }
}

}  // namespace jsonutil
