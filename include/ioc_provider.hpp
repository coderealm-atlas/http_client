#pragma once
#include <boost/asio.hpp>

namespace ioc {

class IIocProvider {
 public:
  virtual ~IIocProvider() = default;
  virtual boost::asio::io_context& get(const char* name) = 0;
};
}  // namespace ioc