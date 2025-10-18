#include "pch.hpp"
#include <gtest/gtest.h>
#include "io_monad.hpp"

using namespace monad;

// Very small test that runs retry_exponential_if and poll_if with short timers
// to ensure cleanup paths don't leak or UAF under ASan.
TEST(RetryPollRegression, RetryExponentialAndPollCleanup) {
  boost::asio::io_context ioc;

  // An IO<int> that fails the first two times and succeeds the third time
  int counter = 0;
  auto flaky = IO<int>([&](auto cb) mutable {
    ++counter;
    if (counter < 3) {
      cb(Result<int, Error>::Err(Error{42, "transient"}));
    } else {
      cb(Result<int, Error>::Ok(123));
    }
  });

  bool done = false;

  // run retry_exponential_if which should eventually succeed
  auto job = std::move(flaky).retry_exponential_if(5, std::chrono::milliseconds(1), ioc,
                                                    [](const Error&){ return true; });

  job.run([&](Result<int, Error> r){
    EXPECT_TRUE(r.is_ok());
    if (r.is_ok()) EXPECT_EQ(r.value(), 123);
    done = true;
  });

  // run the io_context until idle
  ioc.run_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(done);

  // Now a poll_if that becomes satisfied quickly
  boost::asio::io_context ioc2;
  int x = 0;
  auto inc = IO<void>([&](auto cb){ ++x; cb(Result<void, Error>::Ok()); });

  bool poll_done = false;
  auto poll_job = std::move(inc).poll_if(5, std::chrono::milliseconds(1), ioc2,
                                         [&]{ return x >= 2; },
                                         [](const Error&){ return true; });
  poll_job.run([&](Result<void, Error> r){
    EXPECT_TRUE(r.is_ok());
    poll_done = true;
  });

  ioc2.run_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(poll_done);
}
