#include <gtest/gtest.h>

#include <boost/json.hpp>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>

#include "json_util.hpp"

using namespace jsonutil;
namespace json = boost::json;

namespace {

struct EnvGuard {
  std::string name;
  std::optional<std::string> old_value;
  EnvGuard(const std::string& n, const std::string& v) : name(n) {
    const char* ov = std::getenv(name.c_str());
    if (ov) old_value = ov;
    ::setenv(name.c_str(), v.c_str(), 1);
  }
  void unset() { ::unsetenv(name.c_str()); }
  ~EnvGuard() {
    if (old_value) {
      ::setenv(name.c_str(), old_value->c_str(), 1);
    } else {
      ::unsetenv(name.c_str());
    }
  }
};

TEST(JSONEnvSubstitutionTest, EnvironmentPrecedenceOverExtraMapAndDefault) {
  EnvGuard g("APP_PORT", "8080");
  json::value v = json::parse(R"({"port":"${APP_PORT:-1234}"})");
  std::map<std::string, std::string> cli{};
  std::map<std::string, std::string> properties{
      {"APP_PORT", "9999"}};  // should be ignored
  substitue_envs(v, cli, properties);
  ASSERT_TRUE(v.is_object());
  EXPECT_EQ(v.as_object().at("port").as_string(), "8080");
}

TEST(JSONEnvSubstitutionTest, ExtraMapUsedWhenEnvMissing) {
  // ensure env missing
  ::unsetenv("DB_HOST");
  json::value v = json::parse(R"({"db":"${DB_HOST:-localhost}"})");
  std::map<std::string, std::string> cli{};
  std::map<std::string, std::string> properties{{"DB_HOST", "db.internal"}};
  substitue_envs(v, cli, properties);
  EXPECT_EQ(v.as_object().at("db").as_string(), "db.internal");
}

TEST(JSONEnvSubstitutionTest, DefaultUsedWhenEnvAndExtraMissing) {
  ::unsetenv("CACHE_SIZE");
  json::value v = json::parse(R"({"size":"${CACHE_SIZE:-256}"})");
  std::map<std::string, std::string> cli{};
  std::map<std::string, std::string> properties{};
  substitue_envs(v, cli, properties);
  EXPECT_EQ(v.as_object().at("size").as_string(), "256");
}

TEST(JSONEnvSubstitutionTest, UnresolvedLeftIntactWhenNoDefault) {
  ::unsetenv("UNSET_KEY");
  json::value v = json::parse(R"({"raw":"Value ${UNSET_KEY} stays"})");
  std::map<std::string, std::string> cli{};
  std::map<std::string, std::string> properties{};
  substitue_envs(v, cli, properties);
  // Implementation leaves pattern intact
  EXPECT_EQ(v.as_object().at("raw").as_string(), "Value ${UNSET_KEY} stays");
}

TEST(JSONEnvSubstitutionTest, MultipleOccurrencesAndMixedSources) {
  EnvGuard g1("SERVICE_A", "alpha");
  ::unsetenv("SERVICE_B");
  json::value v = json::parse(R"({
    "line":"A=${SERVICE_A} B=${SERVICE_B:-beta} C=${SERVICE_C} A2=${SERVICE_A}",
    "arr":["X=${SERVICE_A}", "Y=${SERVICE_B:-bee}", "Z=${SERVICE_C:-zee}"],
    "nested":{"inner":"${SERVICE_B:-beta}"}
  })");
  std::map<std::string, std::string> cli{};
  std::map<std::string, std::string> properties{{"SERVICE_C", "gamma"}};
  substitue_envs(v, cli, properties);
  auto& obj = v.as_object();
  EXPECT_EQ(obj.at("line").as_string(), "A=alpha B=beta C=gamma A2=alpha");
  auto& arr = obj.at("arr").as_array();
  ASSERT_EQ(arr.size(), 3u);
  EXPECT_EQ(arr[0].as_string(), "X=alpha");
  EXPECT_EQ(arr[1].as_string(), "Y=bee");  // default 'bee' used for SERVICE_B
  EXPECT_EQ(arr[2].as_string(), "Z=gamma");
  EXPECT_EQ(obj.at("nested").as_object().at("inner").as_string(), "beta");
}

TEST(JSONEnvSubstitutionTest, SupportsConcatenationAndNoPartialVarMatch) {
  EnvGuard g1("VAR", "/v");
  EnvGuard g2("VAR_NAME", "/base");

  json::value v = json::parse(R"({
    "path":"${VAR_NAME}/cert.pem",
    "both":"a=${VAR_NAME} b=${VAR}",
    "fallback":"${MISSING:-/tmp}/x"
  })");
  std::map<std::string, std::string> cli{};
  std::map<std::string, std::string> properties{};
  substitue_envs(v, cli, properties);

  auto& o = v.as_object();
  EXPECT_EQ(o.at("path").as_string(), "/base/cert.pem");
  EXPECT_EQ(o.at("both").as_string(), "a=/base b=/v");
  EXPECT_EQ(o.at("fallback").as_string(), "/tmp/x");
}

TEST(JSONEnvSubstitutionTest, NoChangeForNonStringKinds) {
  json::value v =
      json::parse(R"({"n":123, "b":true, "nullv":null, "arr":[1,false,null]})");
  std::map<std::string, std::string> cli{};
  std::map<std::string, std::string> properties{};
  substitue_envs(v, cli, properties);
  auto& o = v.as_object();
  EXPECT_TRUE(o.at("n").is_int64());
  EXPECT_TRUE(o.at("b").is_bool());
  EXPECT_TRUE(o.at("nullv").is_null());
  EXPECT_TRUE(o.at("arr").is_array());
}

TEST(JSONEnvSubstitutionTest, CliOverridesHighestPriority) {
  EnvGuard g("CLI_KEY", "env-value");
  json::value v = json::parse(R"({"value":"${CLI_KEY:-default}"})");
  std::map<std::string, std::string> cli{{"CLI_KEY", "cli-value"}};
  std::map<std::string, std::string> properties{{"CLI_KEY", "prop-value"}};
  substitue_envs(v, cli, properties);
  ASSERT_TRUE(v.is_object());
  EXPECT_EQ(v.as_object().at("value").as_string(), "cli-value");
}

TEST(JSONEnvSubstitutionTest, PropertiesUsedWhenCliAndEnvMissing) {
  ::unsetenv("PROP_ONLY");
  json::value v = json::parse(R"({"key":"${PROP_ONLY:-fallback}"})");
  std::map<std::string, std::string> cli{};
  std::map<std::string, std::string> properties{{"PROP_ONLY", "prop"}};
  substitue_envs(v, cli, properties);
  EXPECT_EQ(v.as_object().at("key").as_string(), "prop");
}

}  // namespace
