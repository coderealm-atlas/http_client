#include <gtest/gtest.h>

#include <boost/json.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "simple_data.hpp"

namespace fs = std::filesystem;
namespace json = boost::json;

namespace {

static void write_file(const fs::path& p, const std::string& content) {
  fs::create_directories(p.parent_path());
  std::ofstream ofs(p, std::ios::binary);
  ASSERT_TRUE(ofs.is_open()) << "Failed to open file for write: " << p;
  ofs << content;
}

static fs::path make_temp_dir(const std::string& name_hint) {
  fs::path base = fs::temp_directory_path();
  fs::path dir;
  for (int i = 0; i < 1000; ++i) {
    dir = base / (name_hint + "_" + std::to_string(::getpid()) + "_" + std::to_string(i));
    std::error_code ec;
    if (fs::create_directory(dir, ec)) return dir;
  }
  dir = base / (name_hint + "_fallback_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  return dir;
}

TEST(ConfigSourcesYamlTest, YamlJsonMixedMergeAndEnvExpansion) {
  fs::path root = make_temp_dir("configsources_yaml");
  // application base YAML
  write_file(root / "application.yaml",
             R"(
svc:
  a: 1
  nested:
    x: base
  override_me: base
)");
  // profile YAML
  write_file(root / "application.dev.yaml",
             R"(
svc:
  b: 2
  nested:
    y: dev
  override_me: dev
)");
  // profile JSON (to ensure mixed formats are merged)
  write_file(root / "application.dev.json",
             R"({ "svc": { "c": 3, "nested": { "z": "devjson" }, "override_me": "devjson" } })");
  // per-module base JSON
  write_file(root / "svc.json",
             R"({ "d": 4, "nested": { "s": "file" }, "override_me": "svcjson" })");
  // per-module profile YAML
  write_file(root / "svc.dev.yaml",
             R"(
d: 5  # should override svc.json d
nested:
  t: filedev
override_me: svcyaml
host: "${TEST_HOST_PLACEHOLDER}"
)");
  // override YAML
  write_file(root / "svc.override.yaml",
             R"(
e: 6
nested:
  u: over
override_me: final
)");

  // Ensure env placeholder is left intact when missing
  ::unsetenv("TEST_HOST_PLACEHOLDER");

  cjj365::ConfigSources sources({root}, {"dev"});
  auto res = sources.json_content("svc");
  ASSERT_TRUE(res.is_ok()) << "Expected Ok, got error: " << res.error().what;
  auto jv = res.value();
  ASSERT_TRUE(jv.is_object());
  auto& jo = jv.as_object();

  EXPECT_EQ(jo.at("a").to_number<int>(), 1);
  EXPECT_EQ(jo.at("b").to_number<int>(), 2);
  EXPECT_EQ(jo.at("c").to_number<int>(), 3);
  EXPECT_EQ(jo.at("d").to_number<int>(), 5);  // overridden by svc.dev.yaml
  EXPECT_EQ(jo.at("e").to_number<int>(), 6);
  EXPECT_EQ(jo.at("override_me").as_string(), "final");

  ASSERT_TRUE(jo.at("nested").is_object());
  auto& nested = jo.at("nested").as_object();
  EXPECT_EQ(nested.at("x").as_string(), "base");
  EXPECT_EQ(nested.at("y").as_string(), "dev");
  EXPECT_EQ(nested.at("z").as_string(), "devjson");
  EXPECT_EQ(nested.at("s").as_string(), "file");
  EXPECT_EQ(nested.at("t").as_string(), "filedev");
  EXPECT_EQ(nested.at("u").as_string(), "over");

  // Placeholder should remain unchanged if env missing
  ASSERT_TRUE(jo.at("host").is_string());
  EXPECT_EQ(jo.at("host").as_string(), "${TEST_HOST_PLACEHOLDER}");

  std::error_code ec;
  fs::remove_all(root, ec);
}

TEST(ConfigSourcesYamlTest, YamlMergeKeyAndQuotedScalar) {
  fs::path root = make_temp_dir("configsources_yaml_merge");

  // per-module YAML using anchors + merge key
  write_file(root / "redis_config.yaml",
             R"(
default: &base
  host: localhost
  port: "6379"
  username: ""
  password: ""
  auth_enabled: false
  use_ssl: false
  ca_str: ""
  cert_str: ""
  cert_key_str: ""
  unix_socket: ""
  username_socket: ""
  password_socket: ""
  exec_timeout: 5
  conn_timeout: 10
  health_check_interval: 2
  reconnect_wait_interval: 1
  logging_level: info
  logging_prefix: bbserver

presence:
  <<: *base
  logging_prefix: presence
)");

  cjj365::ConfigSources sources({root}, {});
  auto res = sources.json_content("redis_config");
  ASSERT_TRUE(res.is_ok()) << "Expected Ok, got error: " << res.error().what;

  auto jv = res.value();
  ASSERT_TRUE(jv.is_object());
  auto& jo = jv.as_object();
  ASSERT_TRUE(jo.at("default").is_object());
  ASSERT_TRUE(jo.at("presence").is_object());

  auto& def = jo.at("default").as_object();
  auto& pres = jo.at("presence").as_object();

  // quoted scalar should remain a string
  ASSERT_TRUE(def.at("port").is_string());
  EXPECT_EQ(def.at("port").as_string(), "6379");

  // merge key should bring base keys into the derived map
  EXPECT_EQ(pres.at("host").as_string(), "localhost");
  ASSERT_TRUE(pres.at("port").is_string());
  EXPECT_EQ(pres.at("port").as_string(), "6379");

  // and overrides should still override
  EXPECT_EQ(pres.at("logging_prefix").as_string(), "presence");

  std::error_code ec;
  fs::remove_all(root, ec);
}

}  // namespace

