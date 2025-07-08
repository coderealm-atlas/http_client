
#include <gtest/gtest.h>  // Add this line

#include <boost/json.hpp>  // IWYU pragma: keep
#include <boost/url.hpp>   // IWYU pragma: keep
#include <boost/url/rfc/unreserved_chars.hpp>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <variant>

namespace urls = boost::urls;
namespace json = boost::json;

namespace {

TEST(UrlsTest, autoEncode) {
  // you can't construct a url like this, it's invalid.
  // urls::url u1(
  //     "https://example.com/df/table/list?name=电商&pageSize=10&pageNum=1");

  urls::url u1("https://example.com/df/table/list?name=&pageSize=10&pageNum=1");
  // 会自动encode，除非在encoded_params()上添加。如果添加的内容没有percent
  // sign，两者是一致的。 这个需要注意。
  u1.params().set("name", "电商");
  std::cerr << "u1 created." << std::endl;
  EXPECT_EQ(u1.query(), "name=电商&pageSize=10&pageNum=1")
      << "Query should match the expected format";

  // 注意，即使采用了encoded_params()，效果一致。
  // 所谓，
  // encoded_params()就是不再自动encode。某些特殊场合，比如一些对url加密的算法需要保证url完全在加密后不得变更。即使语义不变。
  u1.encoded_params().set("name", "电商");
  std::cerr << "u1 created." << std::endl;
  EXPECT_EQ(u1.query(), "name=电商&pageSize=10&pageNum=1")
      << "Query should match the expected format";

  // 确实会自动转换。
  EXPECT_EQ(u1.encoded_query(), "name=%E7%94%B5%E5%95%86&pageSize=10&pageNum=1")
      << "Encoded query should match the expected format";
  EXPECT_EQ(u1.buffer(),
            "https://example.com/df/table/"
            "list?name=%E7%94%B5%E5%95%86&pageSize=10&pageNum=1")
      << "Buffer should match the expected format";
  auto name_it = u1.params().find("name");
  EXPECT_NE(name_it, u1.params().end())
      << "Query should contain 'name' parameter";
  EXPECT_EQ(std::string((*name_it).value), "电商");
}

TEST(UrlsTest, manualEncode) {
  urls::url u1("https://example.com/df/table/list?name=&pageSize=10&pageNum=1");
  u1.encoded_params().set("name", "电商");
  EXPECT_EQ(u1.query(), "name=电商&pageSize=10&pageNum=1")
      << "Query should match the expected format";
  // 确实会自动转换。
  EXPECT_EQ(u1.encoded_query(), "name=%E7%94%B5%E5%95%86&pageSize=10&pageNum=1")
      << "Encoded query should match the expected format";
  EXPECT_EQ(u1.buffer(),
            "https://example.com/df/table/"
            "list?name=%E7%94%B5%E5%95%86&pageSize=10&pageNum=1")
      << "Buffer should match the expected format";
  auto name_it = u1.params().find("name");
  EXPECT_NE(name_it, u1.params().end())
      << "Query should contain 'name' parameter";
  EXPECT_EQ(std::string((*name_it).value), "电商");

  // ----------手动encode----------------------

  u1.encoded_params().set("name", urls::encode("电商", urls::unreserved_chars));
  EXPECT_EQ(u1.query(), "name=电商&pageSize=10&pageNum=1")
      << "Query should match the expected format";
  // 确实会自动转换。
  EXPECT_EQ(u1.encoded_query(), "name=%E7%94%B5%E5%95%86&pageSize=10&pageNum=1")
      << "Encoded query should match the expected format";
  EXPECT_EQ(u1.buffer(),
            "https://example.com/df/table/"
            "list?name=%E7%94%B5%E5%95%86&pageSize=10&pageNum=1")
      << "Buffer should match the expected format";
  name_it = u1.params().find("name");
  EXPECT_NE(name_it, u1.params().end())
      << "Query should contain 'name' parameter";
  EXPECT_EQ(std::string((*name_it).value), "电商");
}

}  // namespace
