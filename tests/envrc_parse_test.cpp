#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "simple_data.hpp"
#include "env_file_parser.hpp"

namespace fs = std::filesystem;

namespace {

// Create a temporary file with given content and return its path.
fs::path write_temp_file(const std::string& content,
                         const std::string& name_hint = "envrc_test") {
  fs::path dir = fs::temp_directory_path() / "http_client_envrc_tests";
  std::error_code ec;
  fs::create_directories(dir, ec);
  fs::path file =
      dir / (name_hint + std::to_string(::getpid()) + ".properties");
  std::ofstream ofs(file);
  ofs << content;
  ofs.close();
  return file;
}

}  // namespace

// Historically the parser lived as parse_envrc inside simple_data.hpp. The
// logic now lives in env_file_parser.hpp as parse_env_file, so these tests use
// the public helper directly.

TEST(ParseEnvrcTest, ParsesExportAndNonExport) {
  const std::string content = R"(
    # comment and blank
    
    export FOO=bar
    BAR=baz
  )";
  auto path = write_temp_file(content, "basic");
  auto res = cjj365::parse_env_file(path);
  ASSERT_TRUE(res.is_ok()) << res.error().what;
  const auto& env = res.value();
  EXPECT_EQ(env.at("FOO"), "bar");
  EXPECT_EQ(env.at("BAR"), "baz");
}

TEST(ParseEnvrcTest, HandlesWhitespaceAroundEquals) {
  const std::string content = R"(
    KEY1 = value1
    export KEY2=   value2
    KEY3   =   value3
  )";
  auto path = write_temp_file(content, "ws");
  auto res = cjj365::parse_env_file(path);
  ASSERT_TRUE(res.is_ok()) << res.error().what;
  const auto& env = res.value();
  EXPECT_EQ(env.at("KEY1"), "value1");
  EXPECT_EQ(env.at("KEY2"), "value2");
  EXPECT_EQ(env.at("KEY3"), "value3");
}

TEST(ParseEnvrcTest, ParsesQuotedValuesAndIgnoresInlineComments) {
  const std::string content = R"(
    export Q1="hello world # not a comment"
    Q2=' spaced value with # still inside '
    Q3=unquoted  # trailing comment
  )";
  auto path = write_temp_file(content, "quotes");
  auto res = cjj365::parse_env_file(path);
  ASSERT_TRUE(res.is_ok()) << res.error().what;
  const auto& env = res.value();
  EXPECT_EQ(env.at("Q1"), "hello world # not a comment");
  EXPECT_EQ(env.at("Q2"), " spaced value with # still inside ");
  EXPECT_EQ(env.at("Q3"), "unquoted");
}

TEST(ParseEnvrcTest, SupportsPlusEqualAsAssignment) {
  const std::string content = R"(
    PATH+=/opt/bin
  )";
  auto path = write_temp_file(content, "pluseq");
  auto res = cjj365::parse_env_file(path);
  ASSERT_TRUE(res.is_ok()) << res.error().what;
  const auto& env = res.value();
  // Parser treats KEY+=VALUE as plain assignment to KEY
  EXPECT_EQ(env.at("PATH"), "/opt/bin");
}

TEST(ParseEnvrcTest, HandlesEmptyAndMissingValues) {
  const std::string content = R"(
    EMPTY=
    QUOTED_EMPTY=""
    SP_ONLY=   
  )";
  auto path = write_temp_file(content, "empty");
  auto res = cjj365::parse_env_file(path);
  ASSERT_TRUE(res.is_ok()) << res.error().what;
  const auto& env = res.value();
  EXPECT_EQ(env.at("EMPTY"), "");
  EXPECT_EQ(env.at("QUOTED_EMPTY"), "");
  EXPECT_EQ(env.at("SP_ONLY"), "");
}

TEST(ParseEnvrcTest, IgnoresGarbageLinesAndComments) {
  const std::string content = R"(
    # full line comment
    NOT_AN_ASSIGNMENT something
    export VALID=1 # ok
  )";
  auto path = write_temp_file(content, "garbage");
  auto res = cjj365::parse_env_file(path);
  ASSERT_TRUE(res.is_ok()) << res.error().what;
  const auto& env = res.value();
  ASSERT_EQ(env.size(), 1u);
  EXPECT_EQ(env.at("VALID"), "1");
}
