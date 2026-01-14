#include "io_monad.hpp"

#include "io_monad_poll_with_state.hpp"

#include <gtest/gtest.h>  // Add this line

#include <atomic>
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
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <i_output.hpp>
#include <stdexcept>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include "api_handler_base.hpp"
#include "i_output.hpp"
#include "in_flight_counter.hpp"
#include "io_monad.hpp"  // include your monad definition
#include "json_util.hpp"
#include "result_monad.hpp"

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

TEST(CollectIOTest, CollectsSequentialValues) {
  std::vector<int> visit_order;
  auto make_io = [&](int v) {
    return IO<int>::pure(v).map([&, v](int value) {
      visit_order.push_back(v);
      return value * 10;
    });
  };

  bool completed = false;
  collect_io<int>({make_io(1), make_io(2), make_io(3)})
      .run([&](IO<std::vector<int>>::IOResult result) {
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.value(), std::vector<int>({10, 20, 30}));
        completed = true;
      });

  EXPECT_TRUE(completed);
  EXPECT_EQ(visit_order, std::vector<int>({1, 2, 3}));
}

TEST(CollectIOTest, StopsOnFirstError) {
  int map_calls = 0;
  bool final_ran = false;

  auto first = IO<int>::pure(5).map([&](int v) {
    ++map_calls;
    return v;
  });

  auto failing = IO<int>::fail(Error{77, "boom"});

  auto third = IO<int>::pure(9).map([&](int v) {
    final_ran = true;
    return v;
  });

  collect_io<int>({first, failing, third})
      .run([&](IO<std::vector<int>>::IOResult result) {
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.error().code, 77);
      });

  EXPECT_EQ(map_calls, 1);
  EXPECT_FALSE(final_ran);
}

TEST(CollectResultIOTest, ReturnsAllResults) {
  collect_result_io<int>(
      {IO<int>::pure(1), IO<int>::fail(Error{9, "fail"}), IO<int>::pure(3)})
      .run([](IO<std::vector<Result<int, Error>>>::IOResult result) {
        ASSERT_TRUE(result.is_ok());
        const auto& values = result.value();
        ASSERT_EQ(values.size(), 3u);
        EXPECT_TRUE(values[0].is_ok());
        EXPECT_EQ(values[0].value(), 1);
        EXPECT_TRUE(values[1].is_err());
        EXPECT_EQ(values[1].error().code, 9);
        EXPECT_TRUE(values[2].is_ok());
        EXPECT_EQ(values[2].value(), 3);
      });
}

TEST(CollectIOParallelTest, CollectsValuesInOriginalOrder) {
  boost::asio::io_context ioc;

  auto make_io = [&](int value, std::chrono::milliseconds delay) {
    return IO<int>([&, value, delay](IO<int>::Callback cb) {
      auto timer = std::make_shared<boost::asio::steady_timer>(ioc);
      timer->expires_after(delay);
      timer->async_wait(
          [timer, cb, value](const boost::system::error_code& ec) mutable {
            if (ec) {
              cb(Result<int, Error>::Err(Error{
                  ec.value(), std::string{"timer error: "} + ec.message()}));
              return;
            }
            cb(Result<int, Error>::Ok(value));
          });
    });
  };

  bool completed = false;
  collect_io_parallel<int>({make_io(1, std::chrono::milliseconds(30)),
                            make_io(2, std::chrono::milliseconds(10)),
                            make_io(3, std::chrono::milliseconds(5))})
      .run([&](IO<std::vector<int>>::IOResult result) {
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.value(), std::vector<int>({1, 2, 3}));
        completed = true;
      });

  ioc.run();
  EXPECT_TRUE(completed);
}

TEST(PollWithStateTest, StopsWhenDecideDone) {
  boost::asio::io_context ioc;

  struct State {
    int last_attempt = 0;
  };

  int calls = 0;
  auto job = [&calls](int attempt, State& st) -> IO<int> {
    ++calls;
    st.last_attempt = attempt;
    return IO<int>::pure(attempt);
  };

  auto decide = [](int /*attempt*/, State& /*st*/,
                   const Result<int, Error>& r) -> PollControl {
    if (r.is_ok() && r.value() >= 3) {
      return PollControl::done();
    }
    return PollControl::retry(std::chrono::milliseconds(1));
  };

  std::optional<Result<int, Error>> result_r;
  poll_with_state<int>(5, std::chrono::milliseconds(1), ioc.get_executor(),
                       State{}, job, decide)
      .run([&](auto r) { result_r = std::move(r); });

  ioc.run();
  ASSERT_TRUE(result_r.has_value());
  ASSERT_TRUE(result_r->is_ok()) << result_r->error();
  EXPECT_EQ(result_r->value(), 3);
  EXPECT_EQ(calls, 3);
}

TEST(PollWithStateTest, RetriesAndKeepsStateAcrossAttempts) {
  boost::asio::io_context ioc;

  struct State {
    std::shared_ptr<std::string> last_err_what;
  };

  auto last_err = std::make_shared<std::string>("");
  auto job = [](int attempt, State& /*st*/) -> IO<int> {
    if (attempt <= 2) {
      return IO<int>::fail(Error{9, "transient"});
    }
    return IO<int>::pure(42);
  };

  auto decide = [](int /*attempt*/, State& st,
                   const Result<int, Error>& r) -> PollControl {
    if (r.is_ok()) {
      return PollControl::done();
    }
    *st.last_err_what = r.error().what;
    return PollControl::retry(std::chrono::milliseconds(1));
  };

  std::optional<Result<int, Error>> result_r;
  poll_with_state<int>(5, std::chrono::milliseconds(1), ioc.get_executor(),
                       State{last_err}, job, decide)
      .run([&](auto r) { result_r = std::move(r); });

  ioc.run();
  ASSERT_TRUE(result_r.has_value());
  ASSERT_TRUE(result_r->is_ok()) << result_r->error();
  EXPECT_EQ(result_r->value(), 42);
  EXPECT_EQ(*last_err, "transient");
}

TEST(PollWithStateTest, ExhaustedUsesCustomError) {
  boost::asio::io_context ioc;

  struct State {
    int last_attempt = 0;
  };

  auto job = [](int /*attempt*/, State& /*st*/) -> IO<int> {
    return IO<int>::pure(0);
  };

  auto decide = [](int attempt, State& st,
                   const Result<int, Error>& /*r*/) -> PollControl {
    st.last_attempt = attempt;
    return PollControl::retry(std::chrono::milliseconds(1));
  };

  auto on_exhausted = [](int attempts, State& st,
                         const Result<int, Error>& /*last*/) -> Error {
    return Error{123,
                 "exhausted: attempts=" + std::to_string(attempts) +
                     ", last_attempt=" + std::to_string(st.last_attempt)};
  };

  std::optional<Result<int, Error>> result_r;
  poll_with_state<int>(3, std::chrono::milliseconds(1), ioc.get_executor(),
                       State{}, job, decide, on_exhausted)
      .run([&](auto r) { result_r = std::move(r); });

  ioc.run();
  ASSERT_TRUE(result_r.has_value());
  ASSERT_TRUE(result_r->is_err());
  EXPECT_EQ(result_r->error().code, 123);
  EXPECT_NE(result_r->error().what.find("attempts=3"), std::string::npos);
  EXPECT_NE(result_r->error().what.find("last_attempt=3"), std::string::npos);
}

TEST(CollectIOParallelTest, PropagatesFirstError) {
  boost::asio::io_context ioc;

  auto ok_io = [&](int value, std::chrono::milliseconds delay) {
    return IO<int>([&, value, delay](IO<int>::Callback cb) {
      auto timer = std::make_shared<boost::asio::steady_timer>(ioc);
      timer->expires_after(delay);
      timer->async_wait(
          [timer, cb, value](const boost::system::error_code& ec) mutable {
            if (ec) {
              cb(Result<int, Error>::Err(Error{
                  ec.value(), std::string{"timer error: "} + ec.message()}));
              return;
            }
            cb(Result<int, Error>::Ok(value));
          });
    });
  };

  auto failing_io = [&](std::chrono::milliseconds delay) {
    return IO<int>([&, delay](IO<int>::Callback cb) {
      auto timer = std::make_shared<boost::asio::steady_timer>(ioc);
      timer->expires_after(delay);
      timer->async_wait([timer, cb](const boost::system::error_code&) mutable {
        cb(Result<int, Error>::Err(Error{55, "parallel failure"}));
      });
    });
  };

  bool saw_error = false;
  collect_io_parallel<int>({ok_io(1, std::chrono::milliseconds(20)),
                            failing_io(std::chrono::milliseconds(5)),
                            ok_io(3, std::chrono::milliseconds(10))})
      .run([&](IO<std::vector<int>>::IOResult result) {
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.error().code, 55);
        saw_error = true;
      });

  ioc.run();
  EXPECT_TRUE(saw_error);
}

TEST(CollectIOParallelTest, RespectsConcurrencyLimit) {
  boost::asio::io_context ioc;

  std::atomic<int> active{0};
  std::atomic<int> max_active{0};

  auto make_io = [&](int value, std::chrono::milliseconds delay) {
    return IO<int>([&, value, delay](IO<int>::Callback cb) {
      int current = active.fetch_add(1, std::memory_order_acq_rel) + 1;
      int observed = max_active.load(std::memory_order_relaxed);
      while (current > observed &&
             !max_active.compare_exchange_weak(observed, current,
                                               std::memory_order_release,
                                               std::memory_order_relaxed)) {
      }

      auto timer = std::make_shared<boost::asio::steady_timer>(ioc);
      timer->expires_after(delay);
      timer->async_wait([timer, cb, value,
                         &active](const boost::system::error_code& ec) mutable {
        active.fetch_sub(1, std::memory_order_acq_rel);
        if (ec) {
          cb(Result<int, Error>::Err(
              Error{ec.value(), std::string{"timer error: "} + ec.message()}));
          return;
        }
        cb(Result<int, Error>::Ok(value));
      });
    });
  };

  bool completed = false;
  collect_io_parallel<int>({make_io(1, std::chrono::milliseconds(5)),
                            make_io(2, std::chrono::milliseconds(10)),
                            make_io(3, std::chrono::milliseconds(15)),
                            make_io(4, std::chrono::milliseconds(20)),
                            make_io(5, std::chrono::milliseconds(25))},
                           2)
      .run([&](IO<std::vector<int>>::IOResult result) {
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.value(), std::vector<int>({1, 2, 3, 4, 5}));
        completed = true;
      });

  ioc.run();
  EXPECT_TRUE(completed);
  EXPECT_LE(max_active.load(std::memory_order_relaxed), 2);
}

TEST(CollectResultParallelTest, ReturnsAllResults) {
  boost::asio::io_context ioc;

  auto make_io = [&](Result<int, Error> outcome,
                     std::chrono::milliseconds delay) {
    return IO<int>([&, outcome = std::move(outcome),
                    delay](IO<int>::Callback cb) mutable {
      auto stored = std::make_shared<Result<int, Error>>(std::move(outcome));
      auto timer = std::make_shared<boost::asio::steady_timer>(ioc);
      timer->expires_after(delay);
      timer->async_wait(
          [timer, cb, stored](const boost::system::error_code& ec) mutable {
            if (ec) {
              cb(Result<int, Error>::Err(Error{
                  ec.value(), std::string{"timer error: "} + ec.message()}));
              return;
            }
            cb(std::move(*stored));
          });
    });
  };

  bool completed = false;
  collect_result_parallel<int>(
      {make_io(Result<int, Error>::Ok(1), std::chrono::milliseconds(15)),
       make_io(Result<int, Error>::Err(Error{90, "nope"}),
               std::chrono::milliseconds(5)),
       make_io(Result<int, Error>::Ok(3), std::chrono::milliseconds(10))})
      .run([&](IO<std::vector<Result<int, Error>>>::IOResult result) {
        ASSERT_TRUE(result.is_ok());
        const auto& values = result.value();
        ASSERT_EQ(values.size(), 3u);
        EXPECT_TRUE(values[0].is_ok());
        EXPECT_EQ(values[0].value(), 1);
        EXPECT_TRUE(values[1].is_err());
        EXPECT_EQ(values[1].error().code, 90);
        EXPECT_TRUE(values[2].is_ok());
        EXPECT_EQ(values[2].value(), 3);
        completed = true;
      });

  ioc.run();
  EXPECT_TRUE(completed);
}

TEST(CollectResultIOTest, HandlesVoidIOs) {
  bool done = false;
  collect_result_io<void>(
      {IO<void>::pure(), IO<void>::fail(Error{17, "oops"}), IO<void>::pure()})
      .run([&](IO<std::vector<Result<void, Error>>>::IOResult result) {
        ASSERT_TRUE(result.is_ok());
        const auto& values = result.value();
        ASSERT_EQ(values.size(), 3u);
        EXPECT_TRUE(values[0].is_ok());
        EXPECT_TRUE(values[1].is_err());
        EXPECT_EQ(values[1].error().code, 17);
        EXPECT_TRUE(values[2].is_ok());
        done = true;
      });
  EXPECT_TRUE(done);
}

TEST(ZipIOTest, AggregatesTupleValues) {
  bool done = false;
  zip_io(IO<int>::pure(7), IO<std::string>::pure("zip"), IO<double>::pure(1.5))
      .run([&](IO<std::tuple<int, std::string, double>>::IOResult result) {
        ASSERT_TRUE(result.is_ok());
        const auto& value = result.value();
        EXPECT_EQ(std::get<0>(value), 7);
        EXPECT_EQ(std::get<1>(value), "zip");
        EXPECT_DOUBLE_EQ(std::get<2>(value), 1.5);
        done = true;
      });
  EXPECT_TRUE(done);
}

TEST(ZipIOTest, PropagatesErrors) {
  zip_io(IO<int>::pure(1), IO<int>::fail(Error{42, "tuple failure"}),
         IO<int>::pure(3))
      .run([](IO<std::tuple<int, int, int>>::IOResult result) {
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.error().code, 42);
      });
}

TEST(ZipIOSkipVoidTest, DropsVoidEntries) {
  bool done = false;
  zip_io_skip_void(IO<void>::pure(), IO<int>::pure(9), IO<void>::pure())
      .run([&](IO<std::tuple<int>>::IOResult result) {
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(std::get<0>(result.value()), 9);
        done = true;
      });
  EXPECT_TRUE(done);
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

TEST(IOMonadTest, FromResultValueOk) {
  MyResult<int> r = MyResult<int>::Ok(7);
  IO<int>::from_result(std::move(r)).run([](IO<int>::IOResult result) {
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), 7);
  });
}

TEST(IOMonadTest, FromResultValueErr) {
  Error e{321, "oops"};
  MyResult<int> r = MyResult<int>::Err(e);
  IO<int>::from_result(std::move(r)).run([](IO<int>::IOResult result) {
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, 321);
    EXPECT_EQ(result.error().what, "oops");
  });
}

TEST(IOMonadTest, FromResultVoidOk) {
  MyVoidResult r = MyVoidResult::Ok();
  IO<void>::from_result(std::move(r)).run([](IO<void>::IOResult result) {
    ASSERT_TRUE(result.is_ok());
  });
}

TEST(IOMonadTest, FromResultVoidErr) {
  Error e{654, "bad"};
  MyVoidResult r = MyVoidResult::Err(e);
  IO<void>::from_result(std::move(r)).run([](IO<void>::IOResult result) {
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error().code, 654);
    EXPECT_EQ(result.error().what, "bad");
  });
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
  MyVoidResult result = MyVoidResult::Err(Error{123, "Oops"});
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
    return MyVoidResult::Err(Error{999, "Unexpected"});
  });
  EXPECT_TRUE(recovered.is_ok());
}

TEST(ResultVoidTest, AndThenChainsVoidToValue) {
  MyVoidResult result = MyVoidResult::Ok();
  auto chained = result.and_then([]() { return MyResult<int>::Ok(42); });
  EXPECT_TRUE(chained.is_ok());
  EXPECT_EQ(chained.value(), 42);
}

TEST(ResultVoidTest, AndThenChainsVoidToVoid) {
  MyVoidResult result = MyVoidResult::Ok();
  auto chained = result.and_then([]() { return MyVoidResult::Ok(); });
  EXPECT_TRUE(chained.is_ok());
}

TEST(ResultVoidTest, AndThenPreservesErrorOnVoidResult) {
  MyVoidResult result = MyVoidResult::Err(Error{404, "Not Found"});
  auto chained = result.and_then([]() {
    ADD_FAILURE() << "Should not be called on error";
    return MyResult<std::string>::Ok("Should not reach here");
  });
  EXPECT_TRUE(chained.is_err());
  EXPECT_EQ(chained.error().code, 404);
  EXPECT_EQ(chained.error().what, "Not Found");
}

TEST(ResultVoidTest, AndThenMoveSemantics) {
  MyVoidResult result = MyVoidResult::Ok();
  auto chained = std::move(result).and_then(
      []() { return MyResult<std::string>::Ok("Success"); });
  EXPECT_TRUE(chained.is_ok());
  EXPECT_EQ(chained.value(), "Success");
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

TEST(PollIfTest, ValueSatisfiedEventually) {
  boost::asio::io_context ioc;
  int counter = 0;
  auto io = IO<int>([&counter](IO<int>::Callback cb) {
              cb(IO<int>::IOResult::Ok(counter++));
            }).poll_if(5, std::chrono::milliseconds(10), ioc, [](const int& v) {
    return v >= 3;
  });

  bool called = false;
  io.run([&](IO<int>::IOResult r) {
    ASSERT_TRUE(r.is_ok());
    EXPECT_GE(r.value(), 3);
    called = true;
  });
  ioc.run();
  EXPECT_TRUE(called);
}

TEST(PollIfTest, AttemptsExhausted) {
  boost::asio::io_context ioc;
  auto io = IO<int>::pure(1).poll_if(3, std::chrono::milliseconds(5), ioc,
                                     [](const int& v) { return v > 10; });
  bool called = false;
  io.run([&](IO<int>::IOResult r) {
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.error().code, 3);
    called = true;
  });
  ioc.run();
  EXPECT_TRUE(called);
}

TEST(PollIfTest, ErrorThenSuccessWithRetryPredicate) {
  boost::asio::io_context ioc;
  int attempt = 0;
  auto io = IO<int>([&attempt](IO<int>::Callback cb) {
              if (attempt++ == 0) {
                cb(IO<int>::IOResult::Err(Error{9, "first attempt fails"}));
              } else {
                cb(IO<int>::IOResult::Ok(42));
              }
            })
                .poll_if(
                    3, std::chrono::milliseconds(5), ioc,
                    [](const int& v) { return v == 42; },
                    [](const Error& e) { return e.code == 9; });
  bool called = false;
  io.run([&](IO<int>::IOResult r) {
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 42);
    called = true;
  });
  ioc.run();
  EXPECT_TRUE(called);
}

TEST(PollIfVoidTest, SatisfiedAfterActions) {
  boost::asio::io_context ioc;
  int counter = 0;
  auto action = IO<void>::pure().map([&counter]() { counter++; });
  auto polled =
      std::move(action).poll_if(5, std::chrono::milliseconds(5), ioc,
                                [&counter]() { return counter >= 2; });

  bool called = false;
  polled.run([&](IO<void>::IOResult r) {
    ASSERT_TRUE(r.is_ok());
    called = true;
  });
  ioc.run();
  EXPECT_TRUE(called);
  EXPECT_GE(counter, 2);
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

  auto io =
      IO<int>([nc_ptr](IO<int>::Callback cb) {
        cb(IO<int>::IOResult::Err(make_error(42, "NonCopyable thunk failed")));
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

TEST(IOTest, finally_then) {
  bool finally_called = false;

  auto io =
      IO<int>::pure(42).finally([&finally_called]() { finally_called = true; });

  bool called = false;
  io.run([&called](IO<int>::IOResult result) {
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), 42);
    called = true;
  });

  EXPECT_TRUE(called);
  EXPECT_TRUE(finally_called);
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

TEST(ApiHandlerTest, Download) {
  monad::IO<apihandler::DownloadFile>::pure(
      apihandler::DownloadFile{"file-not-exist", "html/text", "hello"})
      .then(apihandler::http_response_gen_fn)
      .run([](auto r) {
        EXPECT_FALSE(r.is_ok());
        if (r.is_err()) {
          std::cerr << r.error() << std::endl;
        }
      });
}

TEST(IOMonadTest, NonCopyableCapture) {
  NonCopyable nc(10);
  std::shared_ptr<NonCopyable> nc_ptr =
      std::make_shared<NonCopyable>(std::move(nc));
  IO<int>([nc_ptr](auto&& cb) {
    cb(MyResult<int>::Ok(nc_ptr->value));
  }).map([](auto v) {
      return v + 5;
    }).run([](MyResult<int> result) {
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), 15);
  });
}

TEST(ErrorDataTest, with_data_content) {
  Error err{404, "Not Found"};
  json::value alternative_body{{"data", "This is some data content"}};
  err.alternative_body = std::make_optional(alternative_body);

  MyVoidResult result = MyVoidResult::Err(err);
  EXPECT_TRUE(result.is_err());
  EXPECT_EQ(result.error().code, 404);
  EXPECT_EQ(result.error().what, "Not Found");
  EXPECT_TRUE(result.error().alternative_body.has_value());
  EXPECT_EQ(result.error().alternative_body->as_object().at("data").as_string(),
            "This is some data content");

  Error err1{404, "Not Found"};
  std::string response_str = error_to_response(err1);
  json::value response_json = json::parse(response_str);
  std::cerr << response_json << std::endl;
  EXPECT_TRUE(response_json.is_object());
  EXPECT_EQ(
      response_json.as_object().at("error").as_object().at("code").as_int64(),
      404);
  EXPECT_EQ(
      response_json.as_object().at("error").as_object().at("what").as_string(),
      "Not Found");
}
TEST(ApiResponseTest, deleted_construct) {
  using namespace apihandler;
  ApiDataResponse<int> response(42, "text/plain");
  EXPECT_EQ(
      response.data.index(),
      1);  // Should be int, we added a monostate, so the index() became 1.
  EXPECT_EQ(response.content_type, "text/plain");

  ApiDataResponse<int> response1 = response;  // copyable

  int i = 5;
  // ApiDataResponse<int> response3(i, "text/plain");  // Should not compile, as
  // it is deleted ApiDataResponse<int> response3(i);
}

TEST(IOTest, current_dir) {
  std::cerr << std::filesystem::absolute(std::filesystem::current_path())
            << std::endl;
}

TEST(IOTypeAliasTest, CommonTypeAliases) {
  using namespace monad;

  // Test VoidIO
  bool void_called = false;
  VoidIO::pure().run([&void_called](VoidIO::IOResult result) {
    ASSERT_TRUE(result.is_ok());
    void_called = true;
  });
  EXPECT_TRUE(void_called);

  // Test StringIO
  bool string_called = false;
  StringIO::pure("hello world")
      .run([&string_called](StringIO::IOResult result) {
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.value(), "hello world");
        string_called = true;
      });
  EXPECT_TRUE(string_called);

  // Test IntIO and Int64IO
  bool int_called = false;
  IntIO::pure(42).run([&int_called](IntIO::IOResult result) {
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), 42);
    int_called = true;
  });
  EXPECT_TRUE(int_called);

  bool int64_called = false;
  Int64IO::pure(9223372036854775807LL)
      .run([&int64_called](Int64IO::IOResult result) {
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.value(), 9223372036854775807LL);
        int64_called = true;
      });
  EXPECT_TRUE(int64_called);

  // Test BoolIO
  bool bool_called = false;
  BoolIO::pure(true).run([&bool_called](BoolIO::IOResult result) {
    ASSERT_TRUE(result.is_ok());
    EXPECT_TRUE(result.value());
    bool_called = true;
  });
  EXPECT_TRUE(bool_called);
}

TEST(IOTypeAliasTest, ContainerAndJsonTypes) {
  using namespace monad;

  // Test StringVectorIO
  std::vector<std::string> vec = {"hello", "world", "test"};
  bool vector_called = false;
  StringVectorIO::pure(vec).run(
      [&vector_called](StringVectorIO::IOResult result) {
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.value().size(), 3);
        EXPECT_EQ(result.value()[0], "hello");
        EXPECT_EQ(result.value()[1], "world");
        EXPECT_EQ(result.value()[2], "test");
        vector_called = true;
      });
  EXPECT_TRUE(vector_called);

  // Test JsonIO
  boost::json::value json_val =
      boost::json::object{{"key", "value"}, {"number", 42}};
  bool json_called = false;
  JsonIO::pure(json_val).run([&json_called](JsonIO::IOResult result) {
    ASSERT_TRUE(result.is_ok());
    auto obj = result.value().as_object();
    EXPECT_EQ(obj.at("key").as_string(), "value");
    EXPECT_EQ(obj.at("number").as_int64(), 42);
    json_called = true;
  });
  EXPECT_TRUE(json_called);
}

TEST(IOTypeAliasTest, OptionalTypes) {
  using namespace monad;

  // Test OptionalStringIO with value
  std::optional<std::string> opt_with_value = "optional content";
  bool opt_string_called = false;
  OptionalStringIO::pure(opt_with_value)
      .run([&opt_string_called](OptionalStringIO::IOResult result) {
        ASSERT_TRUE(result.is_ok());
        ASSERT_TRUE(result.value().has_value());
        EXPECT_EQ(result.value().value(), "optional content");
        opt_string_called = true;
      });
  EXPECT_TRUE(opt_string_called);

  // Test OptionalIntIO with no value
  std::optional<int> opt_empty;
  bool opt_int_called = false;
  OptionalIntIO::pure(opt_empty).run(
      [&opt_int_called](OptionalIntIO::IOResult result) {
        ASSERT_TRUE(result.is_ok());
        EXPECT_FALSE(result.value().has_value());
        opt_int_called = true;
      });
  EXPECT_TRUE(opt_int_called);
}

TEST(IOVoidTest, MapToProducesValues) {
  using namespace monad;

  // Test map_to converting void to int
  bool int_called = false;
  VoidIO::pure()
      .map_to([]() { return 42; })
      .run([&int_called](IntIO::IOResult result) {
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.value(), 42);
        int_called = true;
      });
  EXPECT_TRUE(int_called);

  // Test map_to converting void to string
  bool string_called = false;
  VoidIO::pure()
      .map_to([]() { return std::string("generated value"); })
      .run([&string_called](StringIO::IOResult result) {
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.value(), "generated value");
        string_called = true;
      });
  EXPECT_TRUE(string_called);

  // Test map_to with error propagation
  bool error_called = false;
  VoidIO::fail(Error{404, "Not Found"})
      .map_to([]() {
        ADD_FAILURE() << "Should not be called on error";
        return 99;
      })
      .run([&error_called](IntIO::IOResult result) {
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.error().code, 404);
        EXPECT_EQ(result.error().what, "Not Found");
        error_called = true;
      });
  EXPECT_TRUE(error_called);

  // Test map_to with exception handling
  bool exception_called = false;
  VoidIO::pure()
      .map_to([]() -> int { throw std::runtime_error("Something went wrong"); })
      .run([&exception_called](IntIO::IOResult result) {
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.error().code, -1);
        EXPECT_EQ(result.error().what, "Something went wrong");
        exception_called = true;
      });
  EXPECT_TRUE(exception_called);
}
}  // namespace
