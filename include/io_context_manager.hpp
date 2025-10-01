#pragma once

#include <boost/asio/io_context.hpp>
#include <thread>

#include "ioc_manager_config_provider.hpp"
#include "log_stream.hpp"

namespace asio = boost::asio;

namespace cjj365 {

class IIoContextManager {
 public:
  virtual asio::io_context& ioc() = 0;
  virtual void stop() = 0;
  virtual ~IIoContextManager() = default;
};

class IoContextManager : public IIoContextManager {
  int threads_num_;
  std::string name_;
  std::vector<std::thread> threads_;
  asio::io_context ioc_;
  asio::executor_work_guard<asio::io_context::executor_type> work_guard;
  std::atomic<bool> stopped_{false};
  customio::IOutput& output_;
  std::atomic<int> instance_count_{0};

 public:
  IoContextManager(cjj365::IIocConfigProvider& ioc_config_provider,
                   customio::IOutput& output)
      : threads_num_(ioc_config_provider.get().get_threads_num()),
        ioc_(threads_num_),
        output_(output),
        name_(ioc_config_provider.get().get_name()),
        work_guard(asio::make_work_guard(ioc_)) {
    if (instance_count_.fetch_add(1) > 0) {
      throw std::runtime_error(
          "Only one instance of IoContextManager is allowed.");
    }
    output_.info() << "[IoContextManager] constructing name=" << name_ << " threads_num=" << threads_num_ << std::endl;
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
      output_.debug() << "[IoContextManager] started worker thread index=" << i << std::endl;
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
  ~IoContextManager() { stop(); }
};
}  // namespace cjj365
