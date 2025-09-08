#pragma once

#include <functional>
#include <memory>
#include <tuple>
#include <type_traits>

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

// Utilities to bind a Factory that safely captures only concrete dependencies
// (not the injector). This avoids dangling injector references when the
// returned factory outlives the injector's scope.
//
// Usage example for an implementation Impl with
//   using Factory = std::function<std::shared_ptr<Impl>()>;
// and constructor Impl(Dep1&, Dep2&):
//   di_utils::safe_factory_binding<Impl, Dep1, Dep2>()
// can be passed directly into di::make_injector(...).
//
// If Interface::Factory is std::function<std::shared_ptr<Interface>()> and
// Impl : Interface, use:
//   di_utils::safe_factory_binding_for<Interface, Impl, Dep1, Dep2>()
// which returns a binding for Interface::Factory.
namespace di_utils {

template <typename Impl, typename... Deps>
auto safe_factory_binding() {
  using Factory = typename Impl::Factory;
  return boost::di::bind<Factory>().to([](const auto& inj) {
    // Capture required dependencies by reference wrappers
    auto deps =
        std::tuple<std::reference_wrapper<std::remove_reference_t<Deps>>...>{
            std::ref(inj.template create<Deps&>())...};
    return Factory{[deps]() mutable {
      return std::apply(
          [](auto&... drefs) { return std::make_shared<Impl>(drefs.get()...); },
          deps);
    }};
  });
}

template <typename Interface, typename Impl, typename... Deps>
auto safe_factory_binding_for() {
  static_assert(std::is_base_of_v<Interface, Impl>,
                "Impl must inherit from Interface");
  using Factory = typename Interface::Factory;
  return boost::di::bind<Factory>().to([](const auto& inj) {
    auto deps =
        std::tuple<std::reference_wrapper<std::remove_reference_t<Deps>>...>{
            std::ref(inj.template create<Deps&>())...};
    return Factory{[deps]() mutable {
      return std::apply(
          [](auto&... drefs) {
            return std::static_pointer_cast<Interface>(
                std::make_shared<Impl>(drefs.get()...));
          },
          deps);
    }};
  });
}

}  // namespace di_utils