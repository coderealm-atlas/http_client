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
#include <cmath>
#include <cstdlib>
#include <i_output.hpp>
#include <stdexcept>
#include <string>
#include <variant>

#include "i_output.hpp"
#include "in_flight_counter.hpp"
#include "io_monad.hpp"  // include your monad definition
#include "json_util.hpp"

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
  IO<int>::pure(42).run([](IO<int>::IOResult result) {
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), 42);
  });
}

TEST(IOMonadTest, FailError) {
  IO<int>::fail(Error{1, "fail"}).run([](IO<int>::IOResult result) {
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, 1);
  });
}

TEST(IOMonadTest, MapSuccess) {
  IO<int>::pure(3)
      .map([](int x) { return x + 4; })
      .run([](IO<int>::IOResult result) {
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.value(), 7);
      });
}

TEST(IOMonadTest, MapThrows) {
  IO<int>::pure(1)
      .map([](int) -> int { throw std::runtime_error("map failed"); })
      .run([](IO<int>::IOResult result) {
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.error().code, -1);
      });
}

TEST(IOMonadTest, ThenSuccess) {
  IO<std::string>::pure("abc")
      .then([](std::string s) { return IO<int>::pure((int)s.size()); })
      .run([](IO<int>::IOResult result) {
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.value(), 3);
      });
}

TEST(IOMonadTest, CatchThenRecover) {
  IO<std::string>::fail(Error{404, "not found"})
      .catch_then(
          [](const Error& e) { return IO<std::string>::pure("recovered"); })
      .run([](IO<std::string>::IOResult result) {
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.value(), "recovered");
      });
}

TEST(IOMonadTest, VoidPureAndMap) {
  IO<void>::pure().map([]() { SUCCEED(); }).run([](IO<void>::IOResult result) {
    ASSERT_TRUE(result.is_ok());
  });
}

TEST(IOMonadTest, VoidThenChain) {
  IO<void>::pure()
      .then([]() { return IO<void>::pure(); })
      .run([](IO<void>::IOResult result) { ASSERT_TRUE(result.is_ok()); });
}

TEST(IOMonadTest, VoidCatchThen) {
  IO<void>::fail(Error{100, "void fail"})
      .catch_then([](const Error&) { return IO<void>::pure(); })
      .run([](IO<void>::IOResult result) { ASSERT_TRUE(result.is_ok()); });
}

TEST(IOMonadTest, NonCopyableSupport) {
  IO<NonCopyable>::pure(NonCopyable{10})
      .map([](NonCopyable nc) { return nc.value + 5; })
      .run([](IO<int>::IOResult result) {
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.value(), 15);
      });
}

TEST(IOMonadTest, MapErrTransformsError) {
  IO<int>::fail(Error{404, "not found"})
      .map_err([](Error e) { return Error{e.code + 1, "handled: " + e.what}; })
      .run([](IO<int>::IOResult result) {
        ASSERT_TRUE(result.is_err());
        const auto& err = result.error();
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
      .run([](IO<int>::IOResult result) {
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.value(), 99);
      });
}

TEST(IOMonadTest, FinallyCalledOnSuccess) {
  bool called = false;

  IO<std::string>::pure("ok")
      .finally([&]() { called = true; })
      .run([](IO<std::string>::IOResult result) {
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.value(), "ok");
      });

  EXPECT_TRUE(called);
}

TEST(IOMonadTest, FinallyCalledOnError) {
  bool called = false;

  IO<int>::fail(Error{123, "failure"})
      .finally([&]() { called = true; })
      .run([](IO<int>::IOResult result) {
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.error().code, 123);
      });

  EXPECT_TRUE(called);
}

TEST(IOMonadTest, VoidMapErrWorks) {
  IO<void>::fail(Error{888, "bad"})
      .map_err([](Error e) { return Error{e.code + 1, "wrapped: " + e.what}; })
      .run([](IO<void>::IOResult result) {
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.error().code, 889);
        EXPECT_EQ(result.error().what, "wrapped: bad");
      });
}

TEST(IOMonadTest, VoidFinallyAlwaysCalled) {
  bool called = false;

  IO<void>::fail(Error{2, "err"})
      .finally([&]() { called = true; })
      .run([](IO<void>::IOResult result) { ASSERT_TRUE(result.is_err()); });

  EXPECT_TRUE(called);
}

using monad::Error;
using monad::MyResult;
using monad::Result;

TEST(ResultTest, OkConstructionAndAccess) {
  MyResult<int> res = MyResult<int>::Ok(42);
  EXPECT_TRUE(res.is_ok());
  EXPECT_FALSE(res.is_err());
  EXPECT_EQ(res.value(), 42);
}

TEST(ResultTest, ErrConstructionAndAccess) {
  Error err{1, "error"};
  MyResult<int> res = MyResult<int>::Err(err);
  EXPECT_TRUE(res.is_err());
  EXPECT_FALSE(res.is_ok());
  EXPECT_EQ(res.error().code, 1);
  EXPECT_EQ(res.error().what, "error");
}

TEST(ResultTest, MapTransformsValue) {
  MyResult<int> res = MyResult<int>::Ok(5);
  auto mapped = res.map([](int x) { return x * 2; });
  EXPECT_TRUE(mapped.is_ok());
  EXPECT_EQ(mapped.value(), 10);
}

TEST(ResultTest, MapPreservesError) {
  Error err{404, "not found"};
  MyResult<int> res = MyResult<int>::Err(err);
  auto mapped = res.map([](int x) { return x * 2; });
  EXPECT_TRUE(mapped.is_err());
  EXPECT_EQ(mapped.error().code, 404);
}

TEST(ResultTest, AndThenChainsOk) {
  MyResult<int> res = MyResult<int>::Ok(3);
  auto chained = res.and_then([](int x) {
    return MyResult<std::string>::Ok(std::string(x, 'a'));  // "aaa"
  });
  EXPECT_TRUE(chained.is_ok());
  EXPECT_EQ(chained.value(), "aaa");
}

TEST(ResultTest, AndThenPreservesError) {
  Error err{2, "fail"};
  MyResult<int> res = MyResult<int>::Err(err);
  auto chained = res.and_then(
      [](int x) { return MyResult<std::string>::Ok(std::to_string(x)); });
  EXPECT_TRUE(chained.is_err());
  EXPECT_EQ(chained.error().code, 2);
}

TEST(ResultTest, CatchThenRecoversFromError) {
  MyResult<int> res = MyResult<int>::Err(Error{999, "boom"});
  auto recovered =
      res.catch_then([](const Error& e) { return MyResult<int>::Ok(100); });
  EXPECT_TRUE(recovered.is_ok());
  EXPECT_EQ(recovered.value(), 100);
}

TEST(ResultTest, CatchThenPassesThroughOk) {
  MyResult<int> res = MyResult<int>::Ok(123);
  auto recovered =
      res.catch_then([](const Error& e) { return MyResult<int>::Ok(0); });
  EXPECT_TRUE(recovered.is_ok());
  EXPECT_EQ(recovered.value(), 123);
}

TEST(ResultVoidTest, OkResult) {
  MyVoidResult result = MyVoidResult::Ok();
  EXPECT_TRUE(result.is_ok());
  EXPECT_FALSE(result.is_err());
}

TEST(ResultVoidTest, ErrResult) {
  Error err = {404, "Not Found"};
  MyVoidResult result = MyVoidResult::Err(err);
  EXPECT_TRUE(result.is_err());
  EXPECT_FALSE(result.is_ok());
  EXPECT_EQ(result.error().code, err.code);
}

TEST(ResultVoidTest, CatchThenOnError) {
  MyVoidResult result = MyVoidResult::Err({123, "Oops"});
  auto recovered = result.catch_then([](const Error& e) {
    EXPECT_EQ(e.code, 123);
    return MyVoidResult::Ok();
  });
  EXPECT_TRUE(recovered.is_ok());
}

TEST(ResultVoidTest, CatchThenOnSuccessSkipsHandler) {
  MyVoidResult result = MyVoidResult::Ok();
  auto recovered = result.catch_then([](const Error&) {
    ADD_FAILURE() << "Should not be called on Ok";
    return MyVoidResult::Err({999, "Unexpected"});
  });
  EXPECT_TRUE(recovered.is_ok());
}

TEST(JsonTest, expect_true) {
  using namespace jsonutil;
  json::value jv = {
      {"key1", true}, {"key2", false}, {"key3", 123}, {"key4", "value"}};

  ASSERT_TRUE(expect_true_at(jv, "key1").is_ok()) << "Expected true at key1";
  ASSERT_FALSE(expect_true_at(jv, "key2").is_ok())
      << "Expected false at key2, but got true";
  ASSERT_FALSE(expect_true_at(jv, "key3").is_ok())
      << "Expected false at key3, but got true";
  ASSERT_FALSE(expect_true_at(jv, "key4").is_ok())
      << "Expected false at key4, but got true";
}

TEST(DelayTest, delay_for) {
  boost::asio::io_context ioc;
  auto io = delay_for<>(ioc, std::chrono::milliseconds(100));

  bool called = false;
  io.run([&called](IO<void>::IOResult result) {
    ASSERT_TRUE(result.is_ok());
    called = true;
  });

  ioc.run();
  EXPECT_TRUE(called);
}

TEST(DelayTest, delay_then) {
  boost::asio::io_context ioc;
  auto io = IO<void>::pure().then([&ioc]() {
    return delay_then(ioc, std::chrono::milliseconds(2000),
                      std::string("hello"));
  });
  bool called = false;
  io.run([&called](auto result) {
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), "hello");
    called = true;
  });
  ioc.run();
  EXPECT_TRUE(called);
}
TEST(DelayTest, retry) {
  boost::asio::io_context ioc;
  auto io = IO<int>::fail(Error{1, "initial failure"})
                .retry_exponential(3, std::chrono::milliseconds(500), ioc);

  bool called = false;
  io.run([&called](IO<int>::IOResult result) {
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, 1);
    called = true;
  });

  ioc.run();
  EXPECT_TRUE(called);
}

TEST(DelayTest, retry_if) {
  boost::asio::io_context ioc;
  int count = 0;
  auto io =
      IO<int>::fail(Error{1, "initial failure"})
          .map_err([&count](Error e) {
            count++;
            return Error{e.code, "retry #" + std::to_string(count)};
          })
          .retry_exponential_if(3, std::chrono::milliseconds(500), ioc,
                                [](const Error& e) { return e.code == 1; });

  bool called = false;
  io.run([&called](IO<int>::IOResult result) {
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, 1);
    called = true;
  });

  ioc.run();
  EXPECT_EQ(count, 3);  // Should retry 3 times
  EXPECT_TRUE(called);
}

TEST(DelayTest, retry_if_1) {
  boost::asio::io_context ioc;
  int count = 0;
  auto io =
      IO<int>::fail(Error{1, "initial failure"})
          .map_err([&count](Error e) {
            count++;
            return Error{e.code, "retry #" + std::to_string(count)};
          })
          .retry_exponential_if(3, std::chrono::milliseconds(500), ioc,
                                [](const Error& e) { return e.code == 2; });

  bool called = false;
  io.run([&called](IO<int>::IOResult result) {
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, 1);
    called = true;
  });

  ioc.run();
  EXPECT_EQ(count, 1);  // Should retry 3 times
  EXPECT_TRUE(called);
}

TEST(IOTest, NonCopyableThunkFailsToClone) {
  struct NonCopyable {
    NonCopyable() = default;
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;

    NonCopyable(NonCopyable&&) noexcept = default;
    NonCopyable& operator=(NonCopyable&&) noexcept = default;
  };

  NonCopyable nc;

  boost::asio::io_context ioc;
  auto nc_ptr =
      std::make_shared<NonCopyable>(std::move(nc));  // move once to heap

  auto io = IO<int>([nc_ptr](IO<int>::Callback cb) {
              cb(IO<int>::IOResult::Err(
                  {.code = 42, .what = "NonCopyable thunk failed"}));
            }).retry_exponential(3, std::chrono::milliseconds(500), ioc);

  // ✅ this will compile and run, but now the lambda is copyable due to
  // shared_ptr

  // ❌ If we do this instead:
  /*
  auto io = IO<int>([nc = std::move(nc)](IO<int>::Callback cb) {
    cb(IO<int>::IOResult::Ok(42));
  });
  */
  // It won't compile because `nc` can't be moved in capture from local var

  bool called = false;
  io.run([&](IO<int>::IOResult result) {
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, 42);
    EXPECT_EQ(result.error().what, "NonCopyable thunk failed");
    called = true;
  });

  ioc.run();

  EXPECT_TRUE(called);
}
TEST(IoutputTest, outputConsole) {
  using namespace customio;
  std::string s = "hello";
  std::string_view sv = s;
  ConsoleOutput console_output(5);
  console_output.trace() << "This is a trace message" << sv << std::endl;
}

TEST(IoutputTest, output) {
  using namespace customio;

  OsstringOutput silent_output(0);
  silent_output.trace() << "This is a trace message" << std::endl;
  EXPECT_EQ(silent_output.str(), "");
  silent_output.clear();
  silent_output.debug() << "This is a debug message" << std::endl;
  EXPECT_EQ(silent_output.str(), "");
  silent_output.clear();
  silent_output.info() << "This is an info message" << std::endl;
  EXPECT_EQ(silent_output.str(), "");
  silent_output.clear();
  silent_output.warning() << "This is a warning message" << std::endl;
  EXPECT_EQ(silent_output.str(), "");
  silent_output.clear();
  silent_output.error() << "This is an error message" << std::endl;
  EXPECT_EQ(silent_output.str(), "");

  OsstringOutput error_output(1);
  error_output.trace() << "This is a trace message" << std::endl;
  EXPECT_EQ(error_output.str(), "");
  error_output.clear();
  error_output.debug() << "This is a debug message" << std::endl;
  EXPECT_EQ(error_output.str(), "");
  error_output.clear();
  error_output.info() << "This is an info message" << std::endl;
  EXPECT_EQ(error_output.str(), "");
  error_output.clear();
  error_output.warning() << "This is a warning message" << std::endl;
  EXPECT_EQ(error_output.str(), "");
  error_output.clear();
  error_output.error() << "This is an error message" << std::endl;
  EXPECT_EQ(error_output.str(), "[error]: This is an error message\n");

  OsstringOutput warning_output(2);
  warning_output.trace() << "This is a trace message" << std::endl;
  EXPECT_EQ(warning_output.str(), "");
  warning_output.clear();
  warning_output.debug() << "This is a debug message" << std::endl;
  EXPECT_EQ(warning_output.str(), "");
  warning_output.clear();
  warning_output.info() << "This is an info message" << std::endl;
  EXPECT_EQ(warning_output.str(), "");
  warning_output.clear();
  warning_output.warning() << "This is a warning message" << std::endl;
  EXPECT_EQ(warning_output.str(), "[warning]: This is a warning message\n");
  warning_output.clear();
  warning_output.error() << "This is an error message" << std::endl;
  EXPECT_EQ(warning_output.str(), "[error]: This is an error message\n");

  OsstringOutput info_output(3);
  info_output.trace() << "This is a trace message" << std::endl;
  EXPECT_EQ(info_output.str(), "");
  info_output.clear();
  info_output.debug() << "This is a debug message" << std::endl;
  EXPECT_EQ(info_output.str(), "");
  info_output.clear();
  info_output.info() << "This is an info message" << std::endl;
  EXPECT_EQ(info_output.str(), "[info]: This is an info message\n");
  info_output.clear();
  info_output.warning() << "This is a warning message" << std::endl;
  EXPECT_EQ(info_output.str(), "[warning]: This is a warning message\n");
  info_output.clear();
  info_output.error() << "This is an error message" << std::endl;
  EXPECT_EQ(info_output.str(), "[error]: This is an error message\n");

  OsstringOutput debug_output(4);
  debug_output.trace() << "This is a trace message" << std::endl;
  EXPECT_EQ(debug_output.str(), "");
  debug_output.clear();
  debug_output.debug() << "This is a debug message" << std::endl;
  EXPECT_EQ(debug_output.str(), "[debug]: This is a debug message\n");
  debug_output.clear();
  debug_output.info() << "This is an info message" << std::endl;
  EXPECT_EQ(debug_output.str(), "[info]: This is an info message\n");
  debug_output.clear();
  debug_output.warning() << "This is a warning message" << std::endl;
  EXPECT_EQ(debug_output.str(), "[warning]: This is a warning message\n");
  debug_output.clear();
  debug_output.error() << "This is an error message" << std::endl;
  EXPECT_EQ(debug_output.str(), "[error]: This is an error message\n");

  OsstringOutput trace_output(5);
  trace_output.trace() << "This is a trace message" << std::endl;
  EXPECT_EQ(trace_output.str(), "[trace]: This is a trace message\n");
  trace_output.clear();
  trace_output.debug() << "This is a debug message" << std::endl;
  EXPECT_EQ(trace_output.str(), "[debug]: This is a debug message\n");
  trace_output.clear();
  trace_output.info() << "This is an info message" << std::endl;
  EXPECT_EQ(trace_output.str(), "[info]: This is an info message\n");
  trace_output.clear();
  trace_output.warning() << "This is a warning message" << std::endl;
  EXPECT_EQ(trace_output.str(), "[warning]: This is a warning message\n");
  trace_output.clear();
  trace_output.error() << "This is an error message" << std::endl;
  EXPECT_EQ(trace_output.str(), "[error]: This is an error message\n");
}

TEST(InflightTest, to_zero) {
  cjj365::InFlightCounter counter;
  EXPECT_EQ(counter.value(), 0);
  {
    cjj365::InFlightCounter::Guard guard(counter);
    EXPECT_EQ(counter.value(), 1);
  }
  EXPECT_EQ(counter.value(), 0);
}
TEST(StopIndicatorTest, to_stop) {
  cjj365::StopIndicator stop_indicator;
  EXPECT_FALSE(stop_indicator.is_stopped());
  stop_indicator.stop();
  EXPECT_TRUE(stop_indicator.is_stopped());
  stop_indicator.stop();  // Should be idempotent
  EXPECT_TRUE(stop_indicator.is_stopped());
}
}  // namespace
