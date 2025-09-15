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

// Helper to write a string to a file path (create parent dirs as needed)
static void write_file(const fs::path& p, const std::string& content) {
  fs::create_directories(p.parent_path());
  std::ofstream ofs(p, std::ios::binary);
  ASSERT_TRUE(ofs.is_open()) << "Failed to open file for write: " << p;
  ofs << content;
}

// Create a unique temporary directory under the system temp path
static fs::path make_temp_dir(const std::string& name_hint) {
  fs::path base = fs::temp_directory_path();
  fs::path dir;
  for (int i = 0; i < 1000; ++i) {
    dir = base / (name_hint + "_" + std::to_string(::getpid()) + "_" + std::to_string(i));
    std::error_code ec;
    if (fs::create_directory(dir, ec)) return dir;
  }
  // As a fallback (very unlikely), use a random directory name
  dir = base / (name_hint + "_fallback_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  return dir;
}

TEST(ConfigSourcesTest, JsonOverrideMergingWorks) {
  // Arrange: temp config directory
  fs::path root = make_temp_dir("configsources");

  // application.*.json seed for "svc" section
  write_file(root / "application.json",
             R"({
  "svc": { "a": 1, "nested": { "x": "base" }, "override_me": "base" }
})");

  write_file(root / "application.dev.json",
             R"({
  "svc": { "b": 2, "nested": { "y": "dev" }, "override_me": "dev" }
})");

  write_file(root / "application.override.json",
             R"({
  "svc": { "nested": { "z": "over" }, "override_me": "override" }
})");

  // svc.*.json additional layers
  write_file(root / "svc.json",
             R"({
  "c": 3, "nested": { "s": "file" }, "override_me": "svc"
})");

  write_file(root / "svc.dev.json",
             R"({
  "d": 4, "nested": { "t": "filedev" }, "override_me": "svcdev"
})");

  write_file(root / "svc.override.json",
             R"({
  "e": 5, "nested": { "u": "fileover" }, "override_me": "svcover"
})");

  // Act: build ConfigSources with profile "dev" and query "svc"
  cjj365::ConfigSources sources({root}, {"dev"});
  auto res = sources.json_content("svc");

  // Assert
  ASSERT_TRUE(res.is_ok()) << "Expected Ok, got error: " << res.error().what;
  const json::value& jv = res.value();
  ASSERT_TRUE(jv.is_object());
  const json::object& jo = jv.as_object();

  // From application.json
  EXPECT_EQ(jo.at("a").to_number<int>(), 1);
  // From application.dev.json
  EXPECT_EQ(jo.at("b").to_number<int>(), 2);
  // From svc.json
  EXPECT_EQ(jo.at("c").to_number<int>(), 3);
  // From svc.dev.json
  EXPECT_EQ(jo.at("d").to_number<int>(), 4);
  // From svc.override.json
  EXPECT_EQ(jo.at("e").to_number<int>(), 5);

  // Nested merge collected from all levels
  ASSERT_TRUE(jo.at("nested").is_object());
  const json::object& jn = jo.at("nested").as_object();
  EXPECT_EQ(jn.at("x").as_string(), "base");    // application.json
  EXPECT_EQ(jn.at("y").as_string(), "dev");     // application.dev.json
  EXPECT_EQ(jn.at("z").as_string(), "over");    // application.override.json
  EXPECT_EQ(jn.at("s").as_string(), "file");    // svc.json
  EXPECT_EQ(jn.at("t").as_string(), "filedev"); // svc.dev.json
  EXPECT_EQ(jn.at("u").as_string(), "fileover"); // svc.override.json

  // override_me should be overridden by the last layer: svc.override.json
  EXPECT_EQ(jo.at("override_me").as_string(), "svcover");

  // Cleanup
  std::error_code ec;
  fs::remove_all(root, ec);
}

TEST(ConfigSourcesTest, MissingJsonReturnsError) {
  fs::path root = make_temp_dir("configsources_missing");
  cjj365::ConfigSources sources({root}, {"dev"});
  auto res = sources.json_content("nonexistent");
  ASSERT_TRUE(res.is_err());
  EXPECT_EQ(res.error().code, 5019);
  std::error_code ec;
  fs::remove_all(root, ec);
}

} // namespace
