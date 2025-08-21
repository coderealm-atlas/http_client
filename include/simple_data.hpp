#pragma once
#include <boost/json.hpp>
#include <boost/json/value_from.hpp>
#include <boost/system/detail/error_code.hpp>
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

struct ConfigSources {
  std::vector<fs::path> paths;
  std::vector<std::string> profiles;
  // application_json is the final fallback of configuration.
  // appplication.json is read first, then application.{profile}.json,
  // value from application.json will be overridden by values from
  // application.{profile}.json.
  std::optional<json::value> application_json;

  ConfigSources(std::vector<fs::path> paths, std::vector<std::string> profiles)
      : paths(std::move(paths)), profiles(std::move(profiles)) {
    // helper: deep merge two json values (objects only)
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
    for (const auto& path : this->paths) {
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

  monad::MyResult<json::value> json_content(const std::string& filename) const {
    // std::string content;
    std::vector<fs::path> ordered_paths;
    for (const auto& path : paths) {
      // check for {filename}.json
      if (fs::exists(path / (filename + ".json"))) {
        ordered_paths.push_back(path / (filename + ".json"));
      }
      // check for {filename}.{profile}.json
      for (const auto& profile : profiles) {
        fs::path full_path = path / (filename + "." + profile + ".json");
        if (fs::exists(full_path)) {
          ordered_paths.push_back(full_path);
        }
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
    return monad::MyResult<json::value>::Err(monad::Error{
        5019, std::format("Failed to find JSON file: {}", filename)});
  }
};

namespace {
using EnvParseResult = monad::MyResult<std::map<std::string, std::string>>;
using monad::Error;
inline EnvParseResult parse_envrc(const fs::path& envrc) {
  std::string line;
  std::map<std::string, std::string> env;
  std::ifstream ifs(envrc);
  if (!ifs) {
    return EnvParseResult::Err(Error{
        5019, std::format("Failed to open envrc file: {}", envrc.c_str())});
  }
  while (std::getline(ifs, line)) {
    size_t pos = line.find_first_not_of(' ');
    if (pos == std::string::npos || line[pos] == '#') continue;
    pos = line.find("export ", pos);
    if (pos == std::string::npos) {
      continue;
    }
    pos = line.find_first_not_of(' ', pos + 7);
    if (pos == std::string::npos) {
      continue;
    }
    size_t eq_pos = line.find('=', pos);
    if (eq_pos == std::string::npos) {
      continue;
    }
    std::string key = line.substr(pos, eq_pos - pos);
    pos = line.find_first_not_of(' ', eq_pos + 1);
    if (pos == std::string::npos) {
      continue;
    }
    std::string value = line.substr(pos);
    env[key] = value;
  }
  return EnvParseResult::Ok(env);
}
}  // namespace

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
    for (const auto& path : config_sources.paths) {
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
    // Process ordered paths
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
          failed_files.push_back(path);
        } else {
          processed_files.push_back(path);
        }
      }
    }
  }
};
// message SessionAttributes{
//     optional uint64 user_id = 1;
//     optional string user_name = 2;
//     optional string user_email = 3;
//     optional uint64 created_at = 4;
//     optional uint64 user_quota_id = 5;
//     repeated string user_roles = 6;
// }

enum class AuthBy { USERNAME_PASSWORD, TOKEN, OAUTH };

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

  friend Permission tag_invoke(const json::value_from_tag&,
                               const json::value& jv) {
    Permission p;
    p.obtype = jv.at("obtype").as_string();
    p.obid = jv.at("obid").as_string();
    p.actions = json::value_to<std::vector<std::string>>(jv.at("actions"));
    return p;
  }
};

struct SessionAttributes {
  std::optional<uint64_t> user_id;
  std::optional<std::string> user_name;
  std::optional<std::string> user_email;
  std::optional<uint64_t> created_at;
  std::optional<uint64_t> user_quota_id;
  std::vector<std::string> user_roles;
  std::vector<Permission> user_permissions;
  AuthBy auth_by;

  friend void tag_invoke(const json::value_from_tag&, json::value& jv,
                         const SessionAttributes& sa) {
    jv = json::object{
        {"user_id", sa.user_id.value_or(0)},
        {"user_name", sa.user_name.value_or("")},
        {"user_email", sa.user_email.value_or("")},
        {"created_at", sa.created_at.value_or(0)},
        {"user_quota_id", sa.user_quota_id.value_or(0)},
        {"user_roles", json::value_from(sa.user_roles)},
        {"user_permissions", json::value_from(sa.user_permissions)},
        {"auth_by", static_cast<int>(sa.auth_by)}};
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

  bool is_least() { return value == std::numeric_limits<int>::min(); }

  bool is_gt(int v) { return value > v; }
  bool is_lt(int v) { return value < v; }

  bool is_most() { return value == std::numeric_limits<int>::max(); }
};

}  // namespace cjj365
