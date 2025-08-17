#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/beast/core.hpp>
#include <thread>

#include "ioc_manager_config_provider.hpp"
#include "log_stream.hpp"

namespace asio = boost::asio;

namespace cjj365 {

class IoContextManager {
  int threads_num_;
  std::string name_;
  std::vector<std::thread> threads_;
  asio::io_context ioc_;
  asio::executor_work_guard<asio::io_context::executor_type> work_guard;
  std::atomic<bool> stopped_{false};
  customio::IOutput& output_;

 public:
  IoContextManager(cjj365::IIocConfigProvider& ioc_config_provider,
                   customio::IOutput& output)
      : threads_num_(ioc_config_provider.get().get_threads_num()),
        ioc_(threads_num_),
        output_(output),
        name_(ioc_config_provider.get().get_name()),
        work_guard(asio::make_work_guard(ioc_)) {
    threads_.reserve(threads_num_);
    for (int i = 0; i < threads_num_; ++i) {
      threads_.emplace_back([this, i] {
        try {
          asio::io_context::count_type ct = ioc_.run();
          output_.info() << "io_context run count: " << ct << std::endl;
        } catch (const std::exception& e) {
          output_.error() << "io_context run exception: " << e.what();
        }
      });
    }
  }

  asio::io_context& ioc() { return ioc_; }

  void stop() {
    if (stopped_.exchange(true)) {
      return;
    }
    work_guard.reset();
    ioc_.stop();

    auto current_id = std::this_thread::get_id();
    std::size_t joined = 0;
    std::size_t detached = 0;

    for (std::size_t i = 0; i < threads_.size(); ++i) {
      auto& t = threads_[i];
      if (!t.joinable()) continue;
      if (t.get_id() == current_id) {
        t.detach();
        ++detached;
        continue;
      }
      t.join();
      ++joined;
    }
    threads_.clear();
  }
  ~IoContextManager() {
    output_.debug() << "IoContextManager destructor called." << std::endl;
    stop();
  }
};
}  // namespace cjj365
