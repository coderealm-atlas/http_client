#pragma once
#include <boost/asio.hpp>

namespace ioc {

class IIocProvider {
 public:
  virtual ~IIocProvider() = default;
  virtual boost::asio::io_context& get() = 0;
};

class DummyIocProvider : public IIocProvider {
 public:
  boost::asio::io_context& get() override {
    static boost::asio::io_context dummy_ioc;
    return dummy_ioc;
  }
};

}  // namespace ioc