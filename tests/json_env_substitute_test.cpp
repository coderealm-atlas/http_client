#include <gtest/gtest.h>

#include <boost/json.hpp>
#include <cstdlib>
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
  std::map<std::string, std::string> extra{
      {"APP_PORT", "9999"}};  // should be ignored
  substitue_envs(v, extra);
  ASSERT_TRUE(v.is_object());
  EXPECT_EQ(v.as_object().at("port").as_string(), "8080");
}

TEST(JSONEnvSubstitutionTest, ExtraMapUsedWhenEnvMissing) {
  // ensure env missing
  ::unsetenv("DB_HOST");
  json::value v = json::parse(R"({"db":"${DB_HOST:-localhost}"})");
  std::map<std::string, std::string> extra{{"DB_HOST", "db.internal"}};
  substitue_envs(v, extra);
  EXPECT_EQ(v.as_object().at("db").as_string(), "db.internal");
}

TEST(JSONEnvSubstitutionTest, DefaultUsedWhenEnvAndExtraMissing) {
  ::unsetenv("CACHE_SIZE");
  json::value v = json::parse(R"({"size":"${CACHE_SIZE:-256}"})");
  std::map<std::string, std::string> extra{};
  substitue_envs(v, extra);
  EXPECT_EQ(v.as_object().at("size").as_string(), "256");
}

TEST(JSONEnvSubstitutionTest, UnresolvedLeftIntactWhenNoDefault) {
  ::unsetenv("UNSET_KEY");
  json::value v = json::parse(R"({"raw":"Value ${UNSET_KEY} stays"})");
  std::map<std::string, std::string> extra{};
  substitue_envs(v, extra);
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
  std::map<std::string, std::string> extra{{"SERVICE_C", "gamma"}};
  substitue_envs(v, extra);
  auto& obj = v.as_object();
  EXPECT_EQ(obj.at("line").as_string(), "A=alpha B=beta C=gamma A2=alpha");
  auto& arr = obj.at("arr").as_array();
  ASSERT_EQ(arr.size(), 3u);
  EXPECT_EQ(arr[0].as_string(), "X=alpha");
  EXPECT_EQ(arr[1].as_string(), "Y=bee");  // default 'bee' used for SERVICE_B
  EXPECT_EQ(arr[2].as_string(), "Z=gamma");
  EXPECT_EQ(obj.at("nested").as_object().at("inner").as_string(), "beta");
}

TEST(JSONEnvSubstitutionTest, NoChangeForNonStringKinds) {
  json::value v =
      json::parse(R"({"n":123, "b":true, "nullv":null, "arr":[1,false,null]})");
  std::map<std::string, std::string> extra{};
  substitue_envs(v, extra);
  auto& o = v.as_object();
  EXPECT_TRUE(o.at("n").is_int64());
  EXPECT_TRUE(o.at("b").is_bool());
  EXPECT_TRUE(o.at("nullv").is_null());
  EXPECT_TRUE(o.at("arr").is_array());
}

}  // namespace
