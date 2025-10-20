#pragma once

#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "result_monad.hpp"

namespace fs = std::filesystem;

namespace cjj365 {

inline monad::MyResult<std::map<std::string, std::string>> parse_env_file(
    const fs::path& env_path) {
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

  std::ifstream ifs(env_path);
  if (!ifs) {
    std::string error_msg = "Failed to open env file: " +
                            env_path.generic_string();
    return monad::MyResult<std::map<std::string, std::string>>::Err(
        monad::Error{5019, std::move(error_msg)});
  }

  std::map<std::string, std::string> env;
  std::string line;
  while (std::getline(ifs, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    ltrim(line);
    if (line.empty() || line[0] == '#') continue;

    if (line.rfind("export", 0) == 0) {
      if (line.size() == 6 ||
          (line.size() > 6 && (line[6] == ' ' || line[6] == '\t'))) {
        line.erase(0, 6);
        ltrim(line);
      }
    }

    if (line.empty() || line[0] == '#') continue;

    size_t i = 0;
    while (i < line.size() && line[i] != '=' && line[i] != ' ' &&
           line[i] != '\t') {
      ++i;
    }
    std::string key = line.substr(0, i);
    rtrim(key);
    if (key.empty()) continue;

    size_t j = i;
    while (j < line.size() && (line[j] == ' ' || line[j] == '\t')) ++j;
    if (j >= line.size() || line[j] != '=') {
      continue;
    }
    ++j;
    if (!key.empty() && key.back() == '+') key.pop_back();

    while (j < line.size() && (line[j] == ' ' || line[j] == '\t')) ++j;

    std::string value;
    if (j < line.size()) {
      if (line[j] == '"' || line[j] == '\'') {
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
            ++j;
            break;
          } else {
            value.push_back(c);
          }
        }
      } else {
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

  return monad::MyResult<std::map<std::string, std::string>>::Ok(std::move(env));
}

}  // namespace cjj365
