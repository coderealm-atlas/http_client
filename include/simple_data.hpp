#pragma once
#include <sys/types.h>

#include <atomic>
#include <boost/json/conversion.hpp>
#include <boost/system/detail/error_code.hpp>
#include <cstdint>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <cstdlib>
#include <cctype>
#ifndef RYML_HEADER_ONLY
#define RYML_HEADER_ONLY 1
#endif
#include <ryml/ryml.hpp>
#include <ryml/ryml_std.hpp>

#include "env_file_parser.hpp"
#include "result_monad.hpp"

namespace fs = std::filesystem;

namespace cjj365 {

constexpr size_t FIVE_G = static_cast<size_t>(5) * 1024 * 1024 * 1024;  // 5GB
constexpr size_t TEN_M = static_cast<size_t>(10) * 1024 * 1024;         // 10MB

static inline auto HELP_COLUMN_WIDTH = [] {};

struct LoggingConfig {
  std::string level;
  std::string log_dir;
  std::string log_file;
  uint64_t rotation_size;
  friend LoggingConfig tag_invoke(const json::value_to_tag<LoggingConfig>&,
                                  const json::value& jv) {
    std::vector<std::string> all_field_names = {"level", "log_dir", "log_file",
                                                "rotation_size"};
    for (const auto& field_name : all_field_names) {
      if (!jv.as_object().contains(field_name)) {
        throw std::runtime_error(field_name +
                                 " not found in json LoggingConfig");
      }
    }
    LoggingConfig lc;
    lc.level = json::value_to<std::string>(jv.at("level"));
    lc.log_dir = json::value_to<std::string>(jv.at("log_dir"));
    lc.log_file = json::value_to<std::string>(jv.at("log_file"));
    lc.rotation_size = jv.at("rotation_size").to_number<uint64_t>();
    return lc;
  }
};

struct ConfigSources {
  inline static std::atomic<int> instance_count{0};
  std::vector<fs::path> paths_;
  std::vector<std::string> profiles;
  // application_json is the final fallback of configuration.
  // application.{json,yaml} are read first, then application.{profile}.{json,yaml},
  // and values are overridden by later layers. application.override.{json,yaml}
  // is last.
  std::optional<json::value> application_json;
  std::map<std::string, std::string> cli_overrides_;
  std::map<std::string, std::string> env_overrides_;

  ConfigSources(std::vector<fs::path> paths, std::vector<std::string> profiles,
                std::map<std::string, std::string> cli_overrides = {})
      : paths_(std::move(paths)),
        profiles(std::move(profiles)),
        cli_overrides_(std::move(cli_overrides)) {
    // helper: deep merge two json values (objects only)
    // DEBUG_PRINT("initialize ConfigSources with paths_: "
    //             << paths_.size() << ", profiles: " << profiles.size());
    instance_count++;
    if (instance_count > 1) {
#ifndef DEBUG_BUILD
      throw std::runtime_error(
          "ConfigSources should only be instantiated once.");
#endif
    }
    // Filter out non-existing or non-directory paths early; warn for visibility
    {
      std::vector<fs::path> filtered;
      filtered.reserve(paths_.size());
      for (const auto& p : paths_) {
        std::error_code ec;
        bool exists = fs::exists(p, ec);
        if (ec) {
          std::cerr << "ConfigSources: path check error for '" << p
                    << "': " << ec.message() << std::endl;
          continue;
        }
        if (!exists) {
          std::cerr << "ConfigSources: skipping missing config dir '" << p
                    << "'" << std::endl;
          continue;
        }
        if (!fs::is_directory(p, ec)) {
          std::cerr << "ConfigSources: skipping non-directory path '" << p
                    << "'" << (ec ? std::string{" ("} + ec.message() + ")" : std::string{})
                    << std::endl;
          continue;
        }
        filtered.push_back(p);
      }
      paths_.swap(filtered);
    }

    if (paths_.empty()) {
      throw std::runtime_error(
          "ConfigSources paths_ cannot be empty, forget to bind the "
          "ConfigSources in DI?");
    }
    auto append_layers = [this](const fs::path& root, const std::string& base,
                                std::vector<fs::path>& out) {
      out.push_back(root / (base + ".json"));
      out.push_back(root / (base + ".yaml"));
      out.push_back(root / (base + ".yml"));
      for (const auto& profile : this->profiles) {
        out.push_back(root / (base + "." + profile + ".json"));
        out.push_back(root / (base + "." + profile + ".yaml"));
        out.push_back(root / (base + "." + profile + ".yml"));
      }
      out.push_back(root / (base + ".override.json"));
      out.push_back(root / (base + ".override.yaml"));
      out.push_back(root / (base + ".override.yml"));
    };

    std::vector<fs::path> ordered_app_paths;
    for (const auto& path : this->paths_) {
      append_layers(path, "application", ordered_app_paths);
    }
    for (const auto& app_path : ordered_app_paths) {
      auto jv_opt = parse_file_to_json(app_path);
      if (!jv_opt) continue;
      if (application_json) {
        deep_merge_json(*application_json, *jv_opt);
      } else {
        application_json = *jv_opt;
      }
    }
  }

  const std::map<std::string, std::string>& cli_overrides() const {
    return cli_overrides_;
  }

  const std::map<std::string, std::string>& env_overrides() const {
    return env_overrides_;
  }

  void set_cli_override(std::string key, std::string value) {
    cli_overrides_[std::move(key)] = std::move(value);
  }

  void merge_cli_overrides(std::map<std::string, std::string> overrides) {
    for (auto& [k, v] : overrides) {
      cli_overrides_[std::move(k)] = std::move(v);
    }
  }

  void merge_env_overrides(std::map<std::string, std::string> overrides) {
    for (auto& [k, v] : overrides) {
      env_overrides_[std::move(k)] = std::move(v);
    }
  }

  monad::MyResult<json::value> json_content(const std::string& filename) const;

  monad::MyResult<cjj365::LoggingConfig> logging_config() const {
    return json_content("log_config")
        .and_then([](const json::value& jv)
                      -> monad::MyResult<cjj365::LoggingConfig> {
          try {
            return monad::MyResult<cjj365::LoggingConfig>::Ok(
                json::value_to<cjj365::LoggingConfig>(jv));
          } catch (const std::exception& e) {
            return monad::MyResult<cjj365::LoggingConfig>::Err(
                monad::Error{5019, e.what()});
          }
        });
  }

 private:
  static json::value yaml_to_json(const std::string& content,
                                  const fs::path& origin);
  static void deep_merge_json(json::value& dst, const json::value& src);
  void expand_env(json::value& v) const;
  std::optional<std::string> resolve_env_var(const std::string& key) const;
  std::string expand_env_in_string(const std::string& in) const;
  static std::optional<json::value> parse_file_to_json(const fs::path& p);
};

inline bool is_number(const std::string& s) {
  if (s.empty()) return false;
  size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
  if (i >= s.size()) return false;
  for (; i < s.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
  }
  return true;
}

inline json::value ConfigSources::yaml_to_json(const std::string& content,
                                               const fs::path& origin) {
  auto tree = ryml::parse_in_arena(ryml::to_csubstr(content));
  ryml::ConstNodeRef root = tree.rootref();
  auto resolve_ref = [](ryml::ConstNodeRef n) -> ryml::ConstNodeRef {
    if (!(n.is_ref() || n.is_val_ref() || n.is_key_ref())) return n;
    auto const* t = n.tree();
    if (!t) return n;

    c4::csubstr ref;
    if (n.is_val_ref()) {
      ref = n.val_ref();
    } else if (n.is_key_ref()) {
      ref = n.key_ref();
    } else {
      // Fallback: try value ref first.
      if (n.is_val_ref()) ref = n.val_ref();
    }
    if (ref.empty()) return n;

    // Find the node which defines this anchor and return it.
    // Note: ref nodes themselves store the ref name in the same field as anchors,
    // so we must specifically look for anchor-defining nodes.
    for (ryml::id_type i = 0; i < t->size(); ++i) {
      if (t->has_val_anchor(i) && t->val_anchor(i) == ref) {
        return t->cref(i);
      }
      if (t->has_key_anchor(i) && t->key_anchor(i) == ref) {
        return t->cref(i);
      }
    }
    return n;
  };
  std::function<json::value(ryml::ConstNodeRef)> convert =
      [&](ryml::ConstNodeRef n) -> json::value {
    n = resolve_ref(n);
    if (n.is_seq()) {
      json::array arr;
      for (auto ch : n.children()) {
        arr.push_back(convert(resolve_ref(ch)));
      }
      return arr;
    }
    if (n.is_map()) {
      json::object obj;
      for (auto ch : n.children()) {
        std::string key(ch.key().str, ch.key().len);
        auto chv = resolve_ref(ch);
        // YAML merge key: `<<: *anchor` or `<<: [*a, *b]`
        // Rapidyaml parses the referenced node; we must apply the merge.
        if (key == "<<") {
          try {
            if (chv.is_seq()) {
              for (auto entry : chv.children()) {
                auto merged = convert(resolve_ref(entry));
                json::value dstv = obj;
                deep_merge_json(dstv, merged);
                if (dstv.is_object()) obj = std::move(dstv.as_object());
              }
            } else if (chv.is_map()) {
              auto merged = convert(chv);
              json::value dstv = obj;
              deep_merge_json(dstv, merged);
              if (dstv.is_object()) obj = std::move(dstv.as_object());
            } else {
              // If we cannot resolve the merge value, ignore it.
            }
          } catch (...) {
            // Ignore merge failures to avoid breaking config load.
          }
          continue;
        }

        json::value val;
        if (chv.is_map() || chv.is_seq()) {
          val = convert(chv);
        } else if (chv.has_val()) {
          std::string sval(chv.val().str, chv.val().len);
          if (chv.is_val_quoted()) {
            // Preserve quoted scalars as strings (eg: port: "6379").
            val = sval;
          } else if (sval == "true" || sval == "True") {
            val = true;
          } else if (sval == "false" || sval == "False") {
            val = false;
          } else if (is_number(sval)) {
            try {
              val = static_cast<int64_t>(std::stoll(sval));
            } catch (...) {
              val = sval;
            }
          } else {
            try {
              size_t idx = 0;
              double d = std::stod(sval, &idx);
              if (idx == sval.size()) {
                val = d;
              } else {
                val = sval;
              }
            } catch (...) {
              val = sval;
            }
          }
        } else {
          val = json::value();  // null
        }
        obj[key] = std::move(val);
      }
      return obj;
    }
    if (n.has_val()) {
      std::string sval(n.val().str, n.val().len);
      if (n.is_val_quoted()) return json::value(sval);
      if (sval == "true" || sval == "True") return json::value(true);
      if (sval == "false" || sval == "False") return json::value(false);
      if (is_number(sval)) {
        try {
          return json::value(static_cast<int64_t>(std::stoll(sval)));
        } catch (...) {
        }
      }
      try {
        size_t idx = 0;
        double d = std::stod(sval, &idx);
        if (idx == sval.size()) return json::value(d);
      } catch (...) {
      }
      return json::value(sval);
    }
    return json::value();
  };

  try {
    return convert(root);
  } catch (const std::exception& e) {
    std::cerr << "Failed to convert YAML from " << origin << ": " << e.what()
              << std::endl;
    throw;
  }
}

inline void ConfigSources::deep_merge_json(json::value& dst,
                                           const json::value& src) {
  if (!dst.is_object() || !src.is_object()) return;
  for (auto const& kv : src.as_object()) {
    auto const& key = kv.key();
    auto const& val = kv.value();
    if (val.is_object()) {
      if (auto* existing = dst.as_object().if_contains(key)) {
        if (existing->is_object()) {
          json::value& sub = dst.as_object()[key];
          deep_merge_json(sub, val);
          continue;
        }
      }
      dst.as_object()[key] = val;  // replace non-object with object
    } else {
      // overwrite scalars / arrays
      dst.as_object()[key] = val;
    }
  }
}

inline std::optional<std::string> ConfigSources::resolve_env_var(
    const std::string& key) const {
  if (auto it = env_overrides_.find(key); it != env_overrides_.end()) {
    return it->second;
  }
  if (auto it = cli_overrides_.find(key); it != cli_overrides_.end()) {
    return it->second;
  }
  if (const char* val = std::getenv(key.c_str())) {
    return std::string(val);
  }
  return std::nullopt;
}

inline std::string ConfigSources::expand_env_in_string(
    const std::string& in) const {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size();) {
    if (in[i] == '$' && i + 1 < in.size() && in[i + 1] == '{') {
      size_t end = in.find('}', i + 2);
      if (end != std::string::npos) {
        std::string var = in.substr(i + 2, end - (i + 2));
        if (auto resolved = resolve_env_var(var)) {
          out.append(*resolved);
        } else {
          out.append(in.substr(i, end - i + 1));
        }
        i = end + 1;
        continue;
      }
    }
    out.push_back(in[i]);
    ++i;
  }
  return out;
}

inline void ConfigSources::expand_env(json::value& v) const {
  if (v.is_object()) {
    for (auto& kv : v.as_object()) {
      expand_env(kv.value());
    }
  } else if (v.is_array()) {
    for (auto& item : v.as_array()) {
      expand_env(item);
    }
  } else if (v.is_string()) {
    auto s = v.as_string();
    std::string replaced = expand_env_in_string(std::string(s));
    v = replaced;
  }
}

inline std::optional<json::value> ConfigSources::parse_file_to_json(
    const fs::path& p) {
  if (!fs::exists(p) || !fs::is_regular_file(p)) return std::nullopt;
  std::ifstream ifs(p);
  if (!ifs.is_open()) return std::nullopt;
  std::string content((std::istreambuf_iterator<char>(ifs)),
                      std::istreambuf_iterator<char>());
  ifs.close();
  try {
    auto ext = p.extension().string();
    if (ext == ".yaml" || ext == ".yml") {
      return yaml_to_json(content, p);
    }
    boost::system::error_code ec;
    json::value jv = json::parse(content, ec);
    if (ec) {
      std::cerr << "Failed to parse JSON " << p << ": " << ec.message()
                << std::endl;
      return std::nullopt;
    }
    return jv;
  } catch (const std::exception& e) {
    std::cerr << "Failed to parse config file " << p << ": " << e.what()
              << std::endl;
    return std::nullopt;
  }
}

inline monad::MyResult<json::value> ConfigSources::json_content(
    const std::string& filename) const {
  std::vector<fs::path> ordered_paths;
  auto append_layers = [this](const fs::path& root, const std::string& base,
                              std::vector<fs::path>& out) {
    out.push_back(root / (base + ".json"));
    out.push_back(root / (base + ".yaml"));
    out.push_back(root / (base + ".yml"));
    for (const auto& profile : this->profiles) {
      out.push_back(root / (base + "." + profile + ".json"));
      out.push_back(root / (base + "." + profile + ".yaml"));
      out.push_back(root / (base + "." + profile + ".yml"));
    }
    out.push_back(root / (base + ".override.json"));
    out.push_back(root / (base + ".override.yaml"));
    out.push_back(root / (base + ".override.yml"));
  };

  for (const auto& path : paths_) {
    append_layers(path, filename, ordered_paths);
  }

  json::value merged_json = json::object{};
  if (application_json) {
    if (auto* a_p = application_json->if_object()) {
      if (auto* f_p = a_p->if_contains(filename)) {
        if (f_p->is_object()) {
          merged_json = *f_p;
        }
      }
    }
  }

  for (const auto& path : ordered_paths) {
    auto jv_opt = parse_file_to_json(path);
    if (!jv_opt) continue;
    if (!merged_json.is_object()) merged_json = json::object{};
    deep_merge_json(merged_json, *jv_opt);
  }

  if (merged_json.is_object() && !merged_json.as_object().empty()) {
    json::value copy = merged_json;
    expand_env(copy);
    return monad::MyResult<json::value>::Ok(copy);
  }

  std::ostringstream oss;
  for (auto& path : paths_) {
    oss << "Failed to find config for '" << filename
        << "' in: " << fs::absolute(path) << std::endl;
  }
  std::string error_msg =
      "Failed to find config file: " + filename + ", in: " + oss.str();
  return monad::MyResult<json::value>::Err(
      monad::Error{5019, std::move(error_msg)});
}
/*
 AppProperties — layered .properties loader with deterministic merge order

 Purpose
   Load key/value properties from one or more configuration directories and
   profiles, then merge them into a single map<string,string> with predictable
   override rules. Later entries override earlier ones (last write wins).

 Sources and search roots
   - Roots come from ConfigSources.paths_ (in the provided order).
   - Profiles come from ConfigSources.profiles (e.g., ["develop", "prod"]).

 File discovery per root (directory-by-directory, file-by-file)
   Within each config directory, files are appended to an ordered list in four
   phases. The order below defines the base → overrides chain (earlier files
   are overridden by later ones when keys collide):

   1) application.properties
      - If present, this is the base layer for the directory.

   2) application.{profile}.properties for each profile in
 ConfigSources.profiles
      - For example: application.develop.properties, application.prod.properties
      - Appended in the order of profiles; each can override keys from (1).

   3) Per-module global properties: "*.properties" excluding all application*
 files
      - Constraints:
        • Filename ends with ".properties".
        • Filename is NOT "application.properties".
        • Filename does NOT start with "application.".
        • Filename contains exactly one '.' (i.e., no profile suffix), so
          names like "mail.properties" or "service.properties" qualify.
      - These files are appended in directory iteration order and can override
        keys from (1) and (2).

   4) Per-module profile properties: "*.{profile}.properties" (non-application)
      - For each profile, include files whose names end with
 ".{profile}.properties", excluding the special application.{profile}.properties
 handled in (2).
      - Additional constraints:
        • Filename contains exactly two '.' characters.
        • Example: "mail.develop.properties", "service.prod.properties".
      - These are appended after (3) and can override any previous phase.

 Merge/precedence semantics
   - Files are processed in the exact order constructed above for each root,
     and the roots are processed in the order given by ConfigSources.paths_.
   - For each file, lines are parsed via parse_envrc() which extracts
     shell-style assignments of the form:
       export KEY=VALUE
     Leading spaces and commented lines (#) are ignored.
   - Each parsed key/value is inserted into the AppProperties::properties map
     with simple assignment (properties[key] = value). Therefore, the last
     occurrence of a key across the ordered file set wins.

 Bookkeeping
   - processed_files records successfully parsed files in the order they were
     applied.
   - failed_files records files that failed to parse/open.

 Practical implications
   - Put defaults in application.properties.
   - Override per environment in application.{profile}.properties.
   - Split domain-specific settings into module.properties and refine with
     module.{profile}.properties as needed.
   - When the same key appears in multiple places, the most specific and latest
     file in the search order takes precedence.
*/
struct AppProperties {
  ConfigSources& config_sources_;
  std::map<std::string, std::string> properties;
  std::vector<fs::path> processed_files;
  std::vector<fs::path> failed_files;

  using EnvParseResult = monad::MyResult<std::map<std::string, std::string>>;
  AppProperties(ConfigSources& config_sources)
      : config_sources_(config_sources) {
    std::vector<fs::path> ordered_paths;
    // the main order is dirctory by directory, then file by file
    for (const auto& path : config_sources.paths_) {
      if (fs::exists(path) && fs::is_directory(path)) {
        // read application.properties
        fs::path app_properties_path = path / "application.properties";
        if (fs::exists(app_properties_path)) {
          ordered_paths.push_back(app_properties_path);
        }
        // read application.{profile}.properties
        for (const auto& profile : config_sources.profiles) {
          fs::path profile_properties_path =
              path / ("application." + profile + ".properties");
          if (fs::exists(profile_properties_path)) {
            ordered_paths.push_back(profile_properties_path);
          }
        }
        // read xxx.properties exclude application.properties
        for (const auto& entry : fs::directory_iterator(path)) {
          if (entry.is_regular_file()) {
            const auto& filename = entry.path().filename().string();
            bool starts_app = filename.rfind("application.", 0) == 0;
            bool ends_props = filename.size() >= 11 && filename.substr(filename.size() - 11) == ".properties";
            if (filename == "application.properties" ||
                starts_app ||
                !ends_props ||
                std::count(filename.begin(), filename.end(), '.') != 1) {
              continue;  // Skip files that don't match the criteria
            }
            ordered_paths.push_back(entry.path());
          }
        }
        // read xxx.profile.properties, exclude
        // application.{profile}.properties
        for (const auto& entry : fs::directory_iterator(path)) {
          if (entry.is_regular_file()) {
            const auto& filename = entry.path().filename().string();
            for (const auto& profile : config_sources.profiles) {
              std::string app_profile_props = "application." + profile + ".properties";
              std::string profile_suffix = "." + profile + ".properties";
              bool ends_profile = filename.size() >= profile_suffix.size() && 
                                  filename.substr(filename.size() - profile_suffix.size()) == profile_suffix;
              if (filename == app_profile_props ||
                  !ends_profile ||
                  std::count(filename.begin(), filename.end(), '.') != 2) {
                continue;  // Skip files that don't match the criteria
              }
              ordered_paths.push_back(entry.path());
            }
          }
        }
      }
    }
    // Process ordered paths_
    for (const auto& path : ordered_paths) {
      if (fs::exists(path) && fs::is_regular_file(path)) {
        auto r = parse_env_file(path).and_then(
            [this](const std::map<std::string, std::string>& env) {
              for (const auto& [key, value] : env) {
                properties[key] = value;
              }
              return monad::MyResult<void>::Ok();
            });
        if (r.is_err()) {
          // DEBUG_PRINT("Failed to parse envrc: {}" << path);
          failed_files.push_back(path);
        } else {
          // DEBUG_PRINT("Successfully parsed envrc: {}" << path);
          processed_files.push_back(path);
        }
      }
    }

    for (const auto& [key, value] : config_sources.env_overrides()) {
      properties[key] = value;
    }

    for (const auto& [key, value] : config_sources.cli_overrides()) {
      properties[key] = value;
    }

    config_sources_.merge_env_overrides(properties);
  }
};

enum class AuthBy { USERNAME_PASSWORD, API_KEY, JWT_TOKEN };

struct Permission {
  std::string obtype;
  std::string obid;
  std::vector<std::string> actions;

  bool isAll() {
    return obtype == "*" && obid == "*" &&
           actions == std::vector<std::string>{"*"};
  }

  static Permission All() { return Permission{"*", "*", {"*"}}; }

  friend void tag_invoke(const json::value_from_tag&, json::value& jv,
                         const Permission& p) {
    jv = json::object{{"obtype", p.obtype},
                      {"obid", p.obid},
                      {"actions", json::value_from(p.actions)}};
  }

  friend Permission tag_invoke(const json::value_to_tag<Permission>&,
                               const json::value& jv) {
    Permission p;
    p.obtype = jv.at("obtype").as_string();
    p.obid = jv.at("obid").as_string();
    p.actions = json::value_to<std::vector<std::string>>(jv.at("actions"));
    return p;
  }
};

struct SessionAttributes {
  std::optional<int64_t> user_id;
  std::optional<std::string> user_name;
  std::optional<std::string> user_email;
  std::optional<int64_t> created_at;
  std::optional<int64_t> user_quota_id;
  std::vector<std::string> user_roles;
  std::vector<Permission> user_permissions;
  std::optional<std::string> country_of_residence;
  std::optional<std::string> preferred_market_id;
  std::optional<std::string> preferred_locale;
  std::optional<std::string> preferred_currency;
  std::optional<std::string> user_state;
  std::optional<int64_t> email_verified_at;
  std::optional<bool> email_verified;
  AuthBy auth_by = AuthBy::USERNAME_PASSWORD;

  // Auth context fields
  // Authentication Methods References, e.g. ["pwd"], ["webauthn"],
  // ["pwd","totp"]
  std::vector<std::string> amr;
  // Assurance level, e.g. "aal1", "aal2", "aal3"
  std::optional<std::string> acr;
  // Epoch seconds of primary authentication
  std::optional<int64_t> auth_time;
  // Optional flags
  std::optional<bool> mfa;                   // whether MFA was used
  std::optional<bool> webauthn_platform;     // true if platform authenticator
  std::optional<std::string> credential_id;  // last-used credential id (b64url)
  std::optional<bool> attestation_verified;  // if attestation was verified

  int64_t user_id_or_throw() {
    if (user_id) {
      return user_id.value();
    }
    throw std::runtime_error("user_id is not set");
  }

  bool is_admin() const {
    return std::find(user_roles.begin(), user_roles.end(), "admin") !=
           user_roles.end();
  }

  void add_permissions_from_string(const std::string& json_perms_str) {
    if (json_perms_str.empty() || json_perms_str == "{}") return;
    try {
      auto permissions_jv = boost::json::parse(json_perms_str);
      auto permissions_t =
          json::value_to<std::vector<Permission>>(permissions_jv);
      user_permissions.insert(user_permissions.end(), permissions_t.begin(),
                              permissions_t.end());
    } catch (const std::exception& e) {
      std::cerr << "Failed to parse permissions: " << e.what()
                << ", str: " << json_perms_str << std::endl;
    }
  }

  friend void tag_invoke(const json::value_from_tag&, json::value& jv,
                         const SessionAttributes& sa) {
    json::object jo{};

    if (sa.user_id) {
      jo["user_id"] = sa.user_id.value();
    }
    if (sa.user_name) {
      jo["user_name"] = sa.user_name.value();
    }
    if (sa.user_email) {
      jo["user_email"] = sa.user_email.value();
    }
    if (sa.created_at) {
      jo["created_at"] = sa.created_at.value();
    }
    if (sa.user_quota_id) {
      jo["user_quota_id"] = sa.user_quota_id.value();
    }
    if (!sa.user_roles.empty()) {
      jo["user_roles"] = json::value_from(sa.user_roles);
    }
    if (!sa.user_permissions.empty()) {
      jo["user_permissions"] = json::value_from(sa.user_permissions);
    }
    jo["auth_by"] = static_cast<int>(sa.auth_by);

    if (!sa.amr.empty()) {
      jo["amr"] = json::value_from(sa.amr);
    }
    if (sa.acr) {
      jo["acr"] = *sa.acr;
    }
    if (sa.auth_time) {
      jo["auth_time"] = *sa.auth_time;
    }
    if (sa.mfa) {
      jo["mfa"] = *sa.mfa;
    }
    if (sa.webauthn_platform) {
      jo["webauthn_platform"] = *sa.webauthn_platform;
    }
    if (sa.credential_id) {
      jo["credential_id"] = *sa.credential_id;
    }
    if (sa.attestation_verified) {
      jo["attestation_verified"] = *sa.attestation_verified;
    }
    if (sa.country_of_residence) {
      jo["country_of_residence"] = *sa.country_of_residence;
    }
    if (sa.preferred_market_id) {
      jo["preferred_market_id"] = *sa.preferred_market_id;
    }
    if (sa.preferred_locale) {
      jo["preferred_locale"] = *sa.preferred_locale;
    }
    if (sa.preferred_currency) {
      jo["preferred_currency"] = *sa.preferred_currency;
    }
    if (sa.user_state) {
      jo["user_state"] = *sa.user_state;
    }
    if (sa.email_verified_at) {
      jo["email_verified_at"] = *sa.email_verified_at;
    }
    if (sa.email_verified.has_value()) {
      jo["email_verified"] = *sa.email_verified;
    }
    jv = std::move(jo);
  }

  friend SessionAttributes tag_invoke(
      const json::value_to_tag<SessionAttributes>&, const json::value& jv) {
    SessionAttributes sa;
    if (auto* jo_p = jv.if_object()) {
      if (auto* user_id_p = jo_p->if_contains("user_id")) {
        sa.user_id.emplace(user_id_p->to_number<int64_t>());
      }
      if (auto* user_name_p = jo_p->if_contains("user_name")) {
        sa.user_name.emplace(user_name_p->as_string());
      }
      if (auto* user_email_p = jo_p->if_contains("user_email")) {
        sa.user_email.emplace(user_email_p->as_string());
      }
      if (auto* created_at_p = jo_p->if_contains("created_at")) {
        sa.created_at.emplace(created_at_p->to_number<int64_t>());
      }
      if (auto* user_quota_id_p = jo_p->if_contains("user_quota_id")) {
        sa.user_quota_id.emplace(user_quota_id_p->to_number<int64_t>());
      }
      if (auto* user_roles_p = jo_p->if_contains("user_roles")) {
        sa.user_roles = json::value_to<std::vector<std::string>>(*user_roles_p);
      }
      if (auto* user_permissions_p = jo_p->if_contains("user_permissions")) {
        sa.user_permissions =
            json::value_to<std::vector<Permission>>(*user_permissions_p);
      }
      if (auto* amr_p = jo_p->if_contains("amr")) {
        sa.amr = json::value_to<std::vector<std::string>>(*amr_p);
      }
      if (auto* acr_p = jo_p->if_contains("acr")) {
        sa.acr.emplace(acr_p->as_string());
      }
      if (auto* auth_time_p = jo_p->if_contains("auth_time")) {
        sa.auth_time.emplace(auth_time_p->to_number<int64_t>());
      }
      if (auto* mfa_p = jo_p->if_contains("mfa")) {
        sa.mfa.emplace(mfa_p->as_bool());
      }
      if (auto* platform_p = jo_p->if_contains("webauthn_platform")) {
        sa.webauthn_platform.emplace(platform_p->as_bool());
      }
      if (auto* cred_id_p = jo_p->if_contains("credential_id")) {
        sa.credential_id.emplace(cred_id_p->as_string());
      }
      if (auto* attest_p = jo_p->if_contains("attestation_verified")) {
        sa.attestation_verified.emplace(attest_p->as_bool());
      }
      if (auto* country_p = jo_p->if_contains("country_of_residence")) {
        sa.country_of_residence.emplace(country_p->as_string());
      }
      if (auto* market_p = jo_p->if_contains("preferred_market_id")) {
        sa.preferred_market_id.emplace(market_p->as_string());
      }
      if (auto* locale_p = jo_p->if_contains("preferred_locale")) {
        sa.preferred_locale.emplace(locale_p->as_string());
      }
      if (auto* currency_p = jo_p->if_contains("preferred_currency")) {
        sa.preferred_currency.emplace(currency_p->as_string());
      }
      if (auto* state_p = jo_p->if_contains("user_state")) {
        sa.user_state.emplace(state_p->as_string());
      }
      if (auto* verified_at_p = jo_p->if_contains("email_verified_at")) {
        sa.email_verified_at.emplace(verified_at_p->to_number<int64_t>());
      }
      if (auto* verified_p = jo_p->if_contains("email_verified")) {
        sa.email_verified.emplace(verified_p->as_bool());
      }
    }
    return sa;
  }
};

struct ExitCode {
  int value;
  constexpr operator int() const { return value; }
  static constexpr ExitCode OK() { return ExitCode{0}; };
};

struct StrongInt {
  int value;
  constexpr operator int() const { return value; }
  static constexpr StrongInt ZERO() { return StrongInt{0}; };
  static constexpr StrongInt ONE() { return StrongInt{1}; };

  static constexpr StrongInt PRINT_NONE() { return StrongInt{0}; };
  static constexpr StrongInt PRINT_DEFAULT() { return StrongInt{1}; };
  static constexpr StrongInt PRINT_TABLE() { return StrongInt{2}; };
  static constexpr StrongInt PRINT_JSON() { return StrongInt{3}; };
};

struct HowDetail {
  int value;
  constexpr operator int() const { return value; }
  static constexpr HowDetail Least() {
    return HowDetail{std::numeric_limits<int>::min()};
  }
  static constexpr HowDetail Most() {
    return HowDetail{std::numeric_limits<int>::max()};
  }

  bool is_least() const { return value == std::numeric_limits<int>::min(); }

  bool is_gt(int v) const { return value > v; }
  bool is_lt(int v) const { return value < v; }

  bool is_most() const { return value == std::numeric_limits<int>::max(); }
};

}  // namespace cjj365
