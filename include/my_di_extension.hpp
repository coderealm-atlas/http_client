#pragma once

#include <functional>
#include <memory>

#include "boost/di.hpp"

// return std::make_shared<Impl>(injector.template create<Impl>());
template <class Interface, class Impl>
std::function<std::shared_ptr<Interface>()> always_new_shared_factory(
    const auto& injector) {
  static_assert(std::is_base_of_v<Interface, Impl>,
                "Impl must inherit from Interface");
  return std::function<std::shared_ptr<Interface>()>([&injector]() {
    return injector.template create<std::shared_ptr<Impl>>();
  });
}

template <class Impl>
std::function<std::shared_ptr<Impl>()> always_new_shared_factory(
    const auto& injector) {
  return std::function<std::shared_ptr<Impl>()>([&injector]() {
    return injector.template create<std::shared_ptr<Impl>>();
  });
}

template <typename Impl>
auto bind_shared_factory() {
  return boost::di::make_injector(
      boost::di::bind<Impl>().in(boost::di::unique),
      boost::di::bind<typename Impl::Factory>().to([](const auto& inj) {
        return
            [&inj]() { return inj.template create<std::shared_ptr<Impl>>(); };
      }));
}

template <typename Interface, typename Impl>
auto bind_shared_factory() {
  return boost::di::make_injector(
      // boost::di::bind<Impl>().in(boost::di::unique),
      boost::di::bind<Interface>().template to<Impl>().in(boost::di::unique),
      boost::di::bind<typename Interface::Factory>().to([](const auto& inj) {
        return [&inj]() {
          return inj.template create<std::shared_ptr<Interface>>();
        };
      }));
}

struct always_new_shared_ptr_scope {
  template <class TExpected, class>
  struct scope {
    template <class...>
    using is_referable = std::false_type;

    template <class T, class, class TProvider>
    static auto try_create(const TProvider& provider)
        -> decltype(std::shared_ptr<TExpected>{provider.get()});

    template <class T, class, class TProvider>
    auto create(const TProvider& provider) {
      return std::shared_ptr<TExpected>{provider.get()};
    }
  };
};