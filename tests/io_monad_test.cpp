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

TEST(IOMonadTest, MapErrTransformsError) {
  IO<int>::fail(Error{404, "not found"})
      .map_err([](Error e) { return Error{e.code + 1, "handled: " + e.what}; })
      .run([](IO<int>::Result result) {
        ASSERT_TRUE(std::holds_alternative<Error>(result));
        const auto& err = std::get<Error>(result);
        EXPECT_EQ(err.code, 405);
        EXPECT_EQ(err.what, "handled: not found");
      });
}

TEST(IOMonadTest, MapErrDoesNothingOnSuccess) {
  IO<int>::pure(99)
      .map_err([](Error e) {
        ADD_FAILURE() << "map_err should not be called on success";
        return e;
      })
      .run([](IO<int>::Result result) {
        ASSERT_TRUE(std::holds_alternative<int>(result));
        EXPECT_EQ(std::get<int>(result), 99);
      });
}

TEST(IOMonadTest, FinallyCalledOnSuccess) {
  bool called = false;

  IO<std::string>::pure("ok")
      .finally([&]() { called = true; })
      .run([](IO<std::string>::Result result) {
        ASSERT_TRUE(std::holds_alternative<std::string>(result));
        EXPECT_EQ(std::get<std::string>(result), "ok");
      });

  EXPECT_TRUE(called);
}

TEST(IOMonadTest, FinallyCalledOnError) {
  bool called = false;

  IO<int>::fail(Error{123, "failure"})
      .finally([&]() { called = true; })
      .run([](IO<int>::Result result) {
        ASSERT_TRUE(std::holds_alternative<Error>(result));
        EXPECT_EQ(std::get<Error>(result).code, 123);
      });

  EXPECT_TRUE(called);
}

TEST(IOMonadTest, VoidMapErrWorks) {
  IO<void>::fail(Error{888, "bad"})
      .map_err([](Error e) { return Error{e.code + 1, "wrapped: " + e.what}; })
      .run([](IO<void>::Result result) {
        ASSERT_TRUE(std::holds_alternative<Error>(result));
        EXPECT_EQ(std::get<Error>(result).code, 889);
        EXPECT_EQ(std::get<Error>(result).what, "wrapped: bad");
      });
}

TEST(IOMonadTest, VoidFinallyAlwaysCalled) {
  bool called = false;

  IO<void>::fail(Error{2, "err"})
      .finally([&]() { called = true; })
      .run([](IO<void>::Result result) {
        ASSERT_TRUE(std::holds_alternative<Error>(result));
      });

  EXPECT_TRUE(called);
}

}  // namespace
