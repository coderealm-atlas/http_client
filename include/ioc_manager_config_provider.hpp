#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/json.hpp>
#include <thread>

#include "json_util.hpp"
#include "simple_data.hpp"

namespace json = boost::json;

namespace cjj365 {
class IocConfig {
  int threads_num = 0;
  std::string name = "net";

 public:
  IocConfig() = default;
  IocConfig(int threads_num, const std::string& name)
      : threads_num(threads_num), name(name) {
    if (threads_num < 0) {
      throw std::invalid_argument("threads_num must be non-negative");
    }
  }
  friend IocConfig tag_invoke(const json::value_to_tag<IocConfig>&,
                              const json::value& jv) {
    IocConfig config;
    config.threads_num = jv.at("threads_num").to_number<int>();
    if (auto* name_p = jv.as_object().if_contains("name")) {
      config.name = name_p->as_string();
    }
    return config;
  }

  int get_threads_num() const {
    int hthreads_num = std::thread::hardware_concurrency();
    if (threads_num == 0) {
      return hthreads_num;
    }
    return (threads_num > hthreads_num) ? hthreads_num : threads_num;
  }
  const std::string& get_name() const { return name; }
};

class IIocConfigProvider {
 public:
  virtual ~IIocConfigProvider() = default;

  virtual const IocConfig& get() const = 0;
};

class IocConfigProviderFile : public IIocConfigProvider {
  IocConfig config_;

 public:
  explicit IocConfigProviderFile(cjj365::AppProperties& app_properties,
                                 cjj365::ConfigSources& config_sources) {
    auto r = config_sources.json_content("ioc_config");
    if (r.is_err()) {
      std::cerr << "Failed to load IOC config: " << r.error()
                << ", fallback to default configuration." << std::endl;
      int cores = std::thread::hardware_concurrency() / 2;
      if (cores < 1) cores = 1;
      config_ = IocConfig(cores, "main");
    } else {
      json::value jv = r.value();
      jsonutil::substitue_envs(jv, config_sources.cli_overrides(),
                               app_properties.properties);
      config_ = json::value_to<IocConfig>(std::move(jv));
    }
  }

  const IocConfig& get() const { return config_; }
};

}  // namespace cjj365