#pragma once

#include <boost/asio.hpp>
#include <memory>
#include <mutex>
#include <thread>

namespace monad {

inline boost::asio::io_context& retry_io_context() {
  static boost::asio::io_context ioc;
  static std::once_flag once;
  static std::unique_ptr<boost::asio::executor_work_guard<
      boost::asio::io_context::executor_type>> guard;
  static std::shared_ptr<std::thread> runner;

  std::call_once(once, [] {
    guard = std::make_unique<
        boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
        ioc.get_executor());
    runner = std::make_shared<std::thread>([] {
      ioc.run();
    });
  });

  return ioc;
}

inline boost::asio::any_io_executor retry_executor() {
  return retry_io_context().get_executor();
}

}  // namespace monad
