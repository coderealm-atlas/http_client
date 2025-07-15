#pragma once

#include <chrono>
#include <iostream>
#include <thread>

// ---------------------------------------------
// Define the macro to print messages conditionally
#ifdef DEBUG_BUILD
#define DEBUG_PRINT(...)                   \
  do {                                     \
    std::cout << __VA_ARGS__ << std::endl; \
  } while (0)
// std::cout << "File: " << __FILE__ << " Line: " << __LINE__ << std::endl;

#define PRINT_SEGMENTS(sv)                                               \
  do {                                                                   \
    auto sit = sv.begin();                                               \
    std::vector<std::string> ssit(sv.size());                            \
    std::transform(sit, sv.end(), ssit.begin(),                          \
                   [](std::string_view sv) { return std::string{sv}; }); \
    std::string s = std::reduce(ssit.begin(), ssit.end(), std::string{}, \
                                [](std::string& acc, std::string& sv_) { \
                                  return acc + sv_.data() + ", ";        \
                                });                                      \
    std::cout << "sv: " << s << std::endl;                               \
  } while (0)

#else
#define DEBUG_PRINT(...)     // No operation
#define PRINT_SEGMENTS(...)  // No operation
#endif

// ---------------------------------------------
#ifdef DEBUG_BUILD_1
#define DEBUG_PRINT_1(...)                 \
  do {                                     \
    std::cout << __VA_ARGS__ << std::endl; \
  } while (0)
#else
#define DEBUG_PRINT_1(...)  // No operation
#endif

// -------------------------------------------------

inline bool isDfVerboseEnabled() {
#ifdef _WIN32
  char* buffer = nullptr;
  size_t len = 0;
  bool result = false;
  if (_dupenv_s(&buffer, &len, "DF_VERBOSE") == 0 && buffer != nullptr) {
    result = true;
    free(buffer);
  }
  return result;
#else
  return std::getenv("DF_VERBOSE") != nullptr;
#endif
}

#define DF_VERBOSE_LOG(...)                              \
  do {                                                   \
    if (isDfVerboseEnabled()) {                          \
      std::cout __VA_OPT__(<<) __VA_ARGS__ << std::endl; \
    }                                                    \
  } while (0)

#define DECLARE_CLASS_NAME(class_name) \
  virtual std::string getClassName() const { return #class_name; }
