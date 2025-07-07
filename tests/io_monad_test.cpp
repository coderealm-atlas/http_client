#include "io_monad.hpp"

#include <gtest/gtest.h>  // Add this line

#include <boost/beast.hpp>  // IWYU pragma: keep
#include <boost/intrusive/detail/algo_type.hpp>
#include <boost/intrusive/detail/list_iterator.hpp>
#include <boost/intrusive/detail/tree_iterator.hpp>
#include <boost/intrusive/link_mode.hpp>
#include <boost/iostreams/detail/ios.hpp>
#include <boost/json/object.hpp>
#include <boost/process/v2/detail/config.hpp>
#include <boost/smart_ptr/intrusive_ref_counter.hpp>
#include <boost/url/detail/config.hpp>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <variant>

#include "io_monad.hpp"  // include your monad definition

using namespace monad;

namespace {

struct NonCopyable {
  int value;
  explicit NonCopyable(int v) : value(v) {}
  NonCopyable(const NonCopyable&) = delete;
  NonCopyable& operator=(const NonCopyable&) = delete;
  NonCopyable(NonCopyable&&) = default;
  NonCopyable& operator=(NonCopyable&&) = default;
};

TEST(IOMonadTest, PureSuccess) {
  IO<int>::pure(42).run([](IO<int>::Result result) {
    ASSERT_TRUE(std::holds_alternative<int>(result));
    EXPECT_EQ(std::get<int>(result), 42);
  });
}

TEST(IOMonadTest, FailError) {
  IO<int>::fail(Error{1, "fail"}).run([](IO<int>::Result result) {
    ASSERT_TRUE(std::holds_alternative<Error>(result));
    EXPECT_EQ(std::get<Error>(result).code, 1);
  });
}

TEST(IOMonadTest, MapSuccess) {
  IO<int>::pure(3)
      .map([](int x) { return x + 4; })
      .run([](IO<int>::Result result) {
        ASSERT_TRUE(std::holds_alternative<int>(result));
        EXPECT_EQ(std::get<int>(result), 7);
      });
}

TEST(IOMonadTest, MapThrows) {
  IO<int>::pure(1)
      .map([](int) -> int { throw std::runtime_error("map failed"); })
      .run([](IO<int>::Result result) {
        ASSERT_TRUE(std::holds_alternative<Error>(result));
        EXPECT_EQ(std::get<Error>(result).code, -1);
      });
}

TEST(IOMonadTest, ThenSuccess) {
  IO<std::string>::pure("abc")
      .then([](std::string s) { return IO<int>::pure((int)s.size()); })
      .run([](IO<int>::Result result) {
        ASSERT_TRUE(std::holds_alternative<int>(result));
        EXPECT_EQ(std::get<int>(result), 3);
      });
}

TEST(IOMonadTest, CatchThenRecover) {
  IO<std::string>::fail(Error{404, "not found"})
      .catch_then(
          [](const Error& e) { return IO<std::string>::pure("recovered"); })
      .run([](IO<std::string>::Result result) {
        ASSERT_TRUE(std::holds_alternative<std::string>(result));
        EXPECT_EQ(std::get<std::string>(result), "recovered");
      });
}

TEST(IOMonadTest, VoidPureAndMap) {
  IO<void>::pure().map([]() { SUCCEED(); }).run([](IO<void>::Result result) {
    ASSERT_TRUE(std::holds_alternative<std::monostate>(result));
  });
}

TEST(IOMonadTest, VoidThenChain) {
  IO<void>::pure()
      .then([]() { return IO<void>::pure(); })
      .run([](IO<void>::Result result) {
        ASSERT_TRUE(std::holds_alternative<std::monostate>(result));
      });
}

TEST(IOMonadTest, VoidCatchThen) {
  IO<void>::fail(Error{100, "void fail"})
      .catch_then([](const Error&) { return IO<void>::pure(); })
      .run([](IO<void>::Result result) {
        ASSERT_TRUE(std::holds_alternative<std::monostate>(result));
      });
}

TEST(IOMonadTest, NonCopyableSupport) {
  IO<NonCopyable>::pure(NonCopyable{10})
      .map([](NonCopyable nc) { return nc.value + 5; })
      .run([](IO<int>::Result result) {
        ASSERT_TRUE(std::holds_alternative<int>(result));
        EXPECT_EQ(std::get<int>(result), 15);
      });
}

}  // namespace
