#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

#include <boost/asio/io_context.hpp>

#include "io_monad_v2.hpp"

using namespace std::chrono_literals;
namespace v2 = monad::v2;
using monad::Error;

TEST(IOV2, Pure_Map_Then) {
  // Pure + map + then to value
  std::optional<int> got;
  v2::IO<int>::pure(10)
      .map([](int x) { return x + 2; })
      .then([](int x) { return v2::IO<std::string>::pure(std::to_string(x)); })
      .run([&](v2::Result<std::string, Error> r) { EXPECT_TRUE(r); got = std::stoi(r.value()); });
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(*got, 12);

  // Map to void
  bool called = false;
  v2::IO<int>::pure(1)
      .map([&](int) { called = true; })
      .run([&](v2::Result<void, Error> r) { EXPECT_TRUE(r.is_ok()); });
  EXPECT_TRUE(called);
}

TEST(IOV2, Map_Then_Exceptions_To_ErrorCodes) {
  // map throws -> code -1
  v2::IO<int>::pure(1)
      .map([](int) -> int { throw std::runtime_error("boom"); })
      .run([](v2::Result<int, Error> r) { EXPECT_TRUE(r.is_err()); EXPECT_EQ(r.error().code, -1); });

  // then throws -> code -2
  v2::IO<int>::pure(1)
      .then([](int) -> v2::IO<int> { throw std::runtime_error("kapow"); })
      .run([](v2::Result<int, Error> r) { EXPECT_TRUE(r.is_err()); EXPECT_EQ(r.error().code, -2); });

  // catch_then throws -> code -3
  v2::IO<int>::fail(Error{9, "x"})
      .catch_then([](const Error&) -> v2::IO<int> { throw std::runtime_error("oops"); })
      .run([](v2::Result<int, Error> r) { EXPECT_TRUE(r.is_err()); EXPECT_EQ(r.error().code, -3); });
}

TEST(IOV2, DelayFor_And_Timeout) {
  boost::asio::io_context ioc;
  auto start = std::chrono::steady_clock::now();
  std::optional<bool> ok;
  v2::delay_for<void>(ioc, 30ms).run([&](v2::Result<void, Error> r) { ok = r.is_ok(); });
  ioc.run();
  ASSERT_TRUE(ok.has_value());
  EXPECT_TRUE(*ok);
  auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_GE(elapsed, 25ms);

  // Timeout path: delay longer than timeout
  boost::asio::io_context ioc2;
  std::optional<Error> err;
  auto io = v2::IO<int>::pure(42).delay(ioc2, 100ms);
  std::move(io).timeout(ioc2, 20ms).run([&](v2::Result<int, Error> r) { if (r.is_err()) err = r.error(); });
  ioc2.run();
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->code, 2);
}

TEST(IOV2, Void_Flows_Then_And_Finally) {
  bool side = false;
  v2::IO<void>::pure()
      .map([&] { side = true; })
      .then([] { return v2::IO<int>::pure(7); })
      .finally([&] { side = side && true; })
      .run([&](v2::Result<int, Error> r) { EXPECT_TRUE(r); EXPECT_EQ(r.value(), 7); });
  EXPECT_TRUE(side);
}

TEST(IOV2, CatchThen_And_MapErr) {
  // catch_then recovers
  v2::IO<int>::fail(Error{1, "fail"})
      .catch_then([](const Error&) { return v2::IO<int>::pure(5); })
      .run([](v2::Result<int, Error> r) { EXPECT_TRUE(r); EXPECT_EQ(r.value(), 5); });

  // map_err transforms error
  v2::IO<int>::fail(Error{2, "e"})
      .map_err([](const Error& e) { return Error{e.code + 1, e.what + "!"}; })
      .run([](v2::Result<int, Error> r) {
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.error().code, 3);
        EXPECT_EQ(r.error().what, "e!");
      });
}

TEST(IOV2, ZipIo_Basic_And_SkipVoid) {
  boost::asio::io_context ioc;
  auto io1 = v2::delay_for<void>(ioc, 5ms).then([] { return v2::IO<int>::pure(1); });
  auto io2 = v2::IO<std::string>::pure("b");
  auto io3 = v2::IO<int>::pure(3);

  std::optional<std::tuple<int, std::string, int>> tup;
  v2::zip_io(std::move(io1), std::move(io2), std::move(io3))
      .run([&](v2::Result<std::tuple<int, std::string, int>, Error> r) {
        if (r) tup = r.value();
      });
  ioc.run();
  ASSERT_TRUE(tup.has_value());
  EXPECT_EQ(std::get<0>(*tup), 1);
  EXPECT_EQ(std::get<1>(*tup), "b");
  EXPECT_EQ(std::get<2>(*tup), 3);

  // Skip void
  boost::asio::io_context ioc2;
  auto j1 = v2::delay_for<void>(ioc2, 5ms);
  auto j2 = v2::IO<int>::pure(5);
  std::optional<std::tuple<int>> tup2;
  v2::zip_io_skip_void(std::move(j1), std::move(j2))
      .run([&](v2::Result<std::tuple<int>, Error> r) {
        if (r) tup2 = r.value();
      });
  ioc2.run();
  ASSERT_TRUE(tup2.has_value());
  EXPECT_EQ(std::get<0>(*tup2), 5);
}

TEST(IOV2, ZipIo_ShortCircuits_On_Error) {
  boost::asio::io_context ioc;
  std::atomic<bool> third_started{false};

  auto a = v2::IO<int>::pure(1);
  auto b = v2::IO<int>::fail(Error{99, "boom"});
  auto c = v2::IO<int>([&](auto cb) {
    third_started = true;
    cb(v2::Result<int, Error>::Ok(7));
  });

  std::optional<Error> err;
  v2::zip_io(std::move(a), std::move(b), std::move(c))
      .run([&](v2::Result<std::tuple<int, int, int>, Error> r) {
        if (r.is_err()) err = r.error();
      });
  // No need to run ioc; pure/fail run synchronously to completion
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->code, 99);
  EXPECT_FALSE(third_started.load());
}

TEST(IOV2, CollectIo_Vector_And_AllOkIo) {
  boost::asio::io_context ioc;
  std::vector<v2::IO<int>> items;
  items.emplace_back(v2::IO<int>::pure(1));
  items.emplace_back(v2::IO<int>::pure(2));
  items.emplace_back(v2::delay_for<void>(ioc, 5ms).then([] { return v2::IO<int>::pure(3); }));

  std::optional<std::vector<int>> got;
  v2::collect_io<int>(std::move(items)).run([&](v2::Result<std::vector<int>, Error> r) {
    if (r) got = std::move(r).value();
  });
  ioc.run();
  ASSERT_TRUE(got.has_value());
  ASSERT_EQ(got->size(), 3u);
  EXPECT_EQ((*got)[0], 1);
  EXPECT_EQ((*got)[1], 2);
  EXPECT_EQ((*got)[2], 3);

  // all_ok_io
  boost::asio::io_context ioc2;
  std::optional<bool> ok;
  v2::all_ok_io(std::vector<v2::IO<void>>{v2::IO<void>::pure(), v2::delay_for<void>(ioc2, 5ms)})
      .run([&](v2::Result<void, Error> r) { ok = r.is_ok(); });
  ioc2.run();
  ASSERT_TRUE(ok.has_value());
  EXPECT_TRUE(*ok);

  // all_ok_io error path
  std::optional<Error> err;
  v2::all_ok_io(std::vector<v2::IO<void>>{v2::IO<void>::pure(), v2::IO<void>::fail(Error{7, "x"})})
      .run([&](v2::Result<void, Error> r) { if (r.is_err()) err = r.error(); });
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->code, 7);
}

TEST(IOV2, RetryExponential_Succeeds_On_Final_Attempt) {
  boost::asio::io_context ioc;
  auto attempts = std::make_shared<int>(0);

  v2::IO<int> op([attempts](v2::IO<int>::Callback cb) mutable {
    (*attempts)++;
    if (*attempts < 3) {
      cb(v2::Result<int, Error>::Err(Error{10, "try"}));
    } else {
      cb(v2::Result<int, Error>::Ok(99));
    }
  });

  std::optional<int> got;
  std::move(op)
      .retry_exponential(5, 5ms, ioc)
      .run([&](v2::Result<int, Error> r) { if (r) got = r.value(); });
  ioc.run();
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(*got, 99);
  EXPECT_EQ(*attempts, 3);
}
