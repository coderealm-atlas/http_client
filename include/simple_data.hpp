#pragma once
#include <sys/types.h>

#include <atomic>
#include <boost/json/conversion.hpp>
#include <boost/system/detail/error_code.hpp>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <ostream>
#include <vector>

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
  // appplication.json is read first, then application.{profile}.json,
  // value from application.json will be overridden by values from
  // application.{profile}.json.
  std::optional<json::value> application_json;
  std::map<std::string, std::string> cli_overrides_;

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
    if (paths_.empty()) {
      throw std::runtime_error(
          "ConfigSources paths_ cannot be empty, forget to bind the "
          "ConfigSources in DI?");
    }
    auto deep_merge = [](json::value& dst, const json::value& src,
                         const auto& self_ref) -> void {
      if (!dst.is_object() || !src.is_object()) return;
      for (auto const& kv : src.as_object()) {
        auto const& key = kv.key();
        auto const& val = kv.value();
        if (val.is_object()) {
          if (auto* existing = dst.as_object().if_contains(key)) {
            if (existing->is_object()) {
              json::value& sub = dst.as_object()[key];
              self_ref(sub, val, self_ref);
              continue;
            }
          }
          dst.as_object()[key] = val;  // replace non-object with object
        } else {
          // overwrite scalars / arrays
          dst.as_object()[key] = val;
        }
      }
    };
    // try to load application.json
    std::vector<fs::path> ordered_app_json_paths;
    for (const auto& path : this->paths_) {
      // add application.json if exists
      fs::path app_json_path = path / "application.json";
      if (fs::exists(app_json_path)) {
        ordered_app_json_paths.push_back(app_json_path);
      }
      // add application.{profile}.json if exists
      for (const auto& profile : this->profiles) {
        fs::path profile_app_json_path =
            path / ("application." + profile + ".json");
        if (fs::exists(profile_app_json_path)) {
          ordered_app_json_paths.push_back(profile_app_json_path);
        }
      }
      fs::path override_path = path / "application.override.json";
      if (fs::exists(override_path)) {
        ordered_app_json_paths.push_back(override_path);
      }
    }
    // process ordered_app_json_paths
    for (const auto& app_json_path : ordered_app_json_paths) {
      if (fs::exists(app_json_path) && fs::is_regular_file(app_json_path)) {
        std::ifstream ifs(app_json_path);
        if (ifs.is_open()) {
          std::string content((std::istreambuf_iterator<char>(ifs)),
                              std::istreambuf_iterator<char>());
          ifs.close();
          boost::system::error_code ec;
          json::value jv = json::parse(content, ec);
          if (!ec) {
            if (application_json) {
              // deep merge into existing
              deep_merge(*application_json, jv, deep_merge);
            } else {
              application_json = jv;
            }
          } else {
            std::cerr << "Failed to parse " << app_json_path << ": "
                      << ec.message() << std::endl;
          }
        } else {
          std::cerr << "Failed to open " << app_json_path << std::endl;
        }
      } else {
        std::cerr << "File does not exist or is not a regular file: "
                  << app_json_path << std::endl;
      }
    }
  }

  const std::map<std::string, std::string>& cli_overrides() const {
    return cli_overrides_;
  }

  void set_cli_override(std::string key, std::string value) {
    cli_overrides_[std::move(key)] = std::move(value);
  }

  void merge_cli_overrides(std::map<std::string, std::string> overrides) {
    for (auto& [k, v] : overrides) {
      cli_overrides_[std::move(k)] = std::move(v);
    }
  }

  monad::MyResult<json::value> json_content(const std::string& filename) const {
    // std::string content;
    std::vector<fs::path> ordered_paths;
    for (const auto& path : paths_) {
      // DEBUG_PRINT("checking: " << path / (filename + ".json"));
      // check for {filename}.json
      if (fs::exists(path / (filename + ".json"))) {
        ordered_paths.push_back(path / (filename + ".json"));
      }
      // check for {filename}.{profile}.json
      for (const auto& profile : profiles) {
        fs::path full_path = path / (filename + "." + profile + ".json");
        // DEBUG_PRINT("checking: " << full_path);
        if (fs::exists(full_path)) {
          ordered_paths.push_back(full_path);
        }
      }
      auto override_path = path / (filename + ".override.json");
      if (fs::exists(override_path)) {
        ordered_paths.push_back(override_path);
      }
    }
    // process ordered_paths, merge content instead of replacing
    json::value merged_json = json::object{};
    if (application_json) {
      if (auto* a_p = application_json->if_object()) {
        if (auto* f_p = a_p->if_contains(filename)) {
          if (f_p->is_object()) {
            merged_json = *f_p;  // seed with base
          }
        }
      }
    }
    auto deep_merge = [](json::value& dst, const json::value& src,
                         const auto& self_ref) -> void {
      if (!dst.is_object() || !src.is_object()) return;
      for (auto const& kv : src.as_object()) {
        auto const& key = kv.key();
        auto const& val = kv.value();
        if (val.is_object()) {
          if (auto* existing = dst.as_object().if_contains(key)) {
            if (existing->is_object()) {
              json::value& sub = dst.as_object()[key];
              self_ref(sub, val, self_ref);
              continue;
            }
          }
          dst.as_object()[key] = val;
        } else {
          dst.as_object()[key] = val;  // override scalar/array
        }
      }
    };
    for (const auto& path : ordered_paths) {
      if (fs::exists(path) && fs::is_regular_file(path)) {
        std::ifstream ifs(path);
        if (ifs.is_open()) {
          std::string content{(std::istreambuf_iterator<char>(ifs)),
                              std::istreambuf_iterator<char>()};
          ifs.close();
          json::value jv = json::parse(content);
          if (jv.is_object()) {
            if (!merged_json.is_object()) merged_json = json::object{};
            deep_merge(merged_json, jv, deep_merge);
          } else {
            std::cerr << "Failed to open " << path << std::endl;
          }
        } else {
          std::cerr << "File does not exist or is not a regular file: " << path
                    << std::endl;
        }
      }
    }
    if (merged_json.is_object() && !merged_json.as_object().empty()) {
      return monad::MyResult<json::value>::Ok(merged_json);
    }
    std::ostringstream oss;
    for (auto& path : paths_) {
      oss << "Failed to find JSON file in: " << fs::absolute(path) << std::endl;
    }
    return monad::MyResult<json::value>::Err(
        monad::Error{5019, std::format("Failed to find JSON file: {}, in: {}",
                                       filename, oss.str())});
  }

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
};

namespace {
using EnvParseResult = monad::MyResult<std::map<std::string, std::string>>;
using monad::Error;
inline EnvParseResult parse_envrc(const fs::path& envrc) {
  auto ltrim = [](std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i) s.erase(0, i);
  };
  auto rtrim = [](std::string& s) {
    size_t i = s.size();
    while (i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\t')) --i;
    if (i < s.size()) s.erase(i);
  };
  auto trim = [&](std::string& s) {
    rtrim(s);
    ltrim(s);
  };

  std::string line;
  std::map<std::string, std::string> env;
  std::ifstream ifs(envrc);
  if (!ifs) {
    return EnvParseResult::Err(Error{
        5019, std::format("Failed to open envrc file: {}", envrc.c_str())});
  }
  while (std::getline(ifs, line)) {
    // Normalize line endings and trim leading/trailing whitespace
    if (!line.empty() && line.back() == '\r') line.pop_back();
    ltrim(line);
    if (line.empty() || line[0] == '#') continue;

    // Optionally consume leading 'export' keyword (export, export\t,
    // export<spaces>)
    if (line.rfind("export", 0) == 0) {
      // Ensure next char is whitespace or end
      if (line.size() == 6 ||
          line.size() > 6 && (line[6] == ' ' || line[6] == '\t')) {
        line.erase(0, 6);
        ltrim(line);
      }
    }

    if (line.empty() || line[0] == '#') continue;

    // Find key and '='
    // Key is up to '=' or whitespace
    size_t i = 0;
    // Parse key characters (allow [A-Za-z_][A-Za-z0-9_]*), tolerate others but
    // trim
    size_t key_start = 0;
    while (i < line.size() && line[i] != '=' && line[i] != ' ' &&
           line[i] != '\t')
      ++i;
    std::string key = line.substr(key_start, i - key_start);
    rtrim(key);
    if (key.empty()) continue;

    // Skip whitespace before '=' (allow "KEY = value")
    size_t j = i;
    while (j < line.size() && (line[j] == ' ' || line[j] == '\t')) ++j;
    if (j >= line.size() || line[j] != '=') {
      // No '=' present; treat as empty assignment only if the rest is
      // comment/whitespace Otherwise, skip line
      continue;
    }
    ++j;  // skip '='
    // Support '+=' by ignoring '+' before '=' (KEY+=value) — treat same as '='
    // Already handled since we looked for '=' and consumed it; if '+' existed
    // before, the key would contain '+'; sanitize it
    if (!key.empty() && key.back() == '+') key.pop_back();

    // Skip whitespace before value
    while (j < line.size() && (line[j] == ' ' || line[j] == '\t')) ++j;
    std::string value;
    if (j < line.size()) {
      if (line[j] == '"' || line[j] == '\'') {
        // Quoted value; read until matching quote with simple escape handling
        char quote = line[j++];
        bool escape = false;
        for (; j < line.size(); ++j) {
          char c = line[j];
          if (escape) {
            value.push_back(c);
            escape = false;
          } else if (c == '\\') {
            escape = true;
          } else if (c == quote) {
            ++j;  // consume closing quote
            break;
          } else {
            value.push_back(c);
          }
        }
        // Ignore trailing content after closing quote except allow inline
        // comment that starts with #
      } else {
        // Unquoted: read until unquoted '#'
        size_t k = j;
        while (k < line.size() && line[k] != '#') ++k;
        value = line.substr(j, k - j);
        trim(value);
      }
    } else {
      value.clear();
    }

    env[key] = value;
  }
  return EnvParseResult::Ok(env);
}
}  // namespace

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
            if (filename == "application.properties" ||
                filename.starts_with("application.") ||
                !filename.ends_with(".properties") ||
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
              if (filename ==
                      std::format("application.{}.properties", profile) ||
                  !filename.ends_with(std::format(".{}.properties", profile)) ||
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
        auto r = parse_envrc(path).and_then(
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

    for (const auto& [key, value] : config_sources.cli_overrides()) {
      properties[key] = value;
    }
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
