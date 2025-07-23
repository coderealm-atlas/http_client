#pragma once

#include <unistd.h>  // for isatty

#include <cstdio>  // for fileno
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>

namespace customio {

namespace log_color {

constexpr std::string_view RESET = "\033[0m";
constexpr std::string_view RED = "\033[31m";
constexpr std::string_view YELLOW = "\033[33m";
constexpr std::string_view GREEN = "\033[32m";
constexpr std::string_view CYAN = "\033[36m";
constexpr std::string_view GRAY = "\033[90m";

inline std::string color_prefix(std::string_view prefix,
                                std::string_view color) {
  return std::string(color) + std::string(prefix) + std::string(RESET);
}

}  // namespace log_color

class PrefixedStreamThreadSafe {
 public:
  PrefixedStreamThreadSafe(std::ostream& os, std::string prefix, bool enabled,
                           std::mutex& mutex)
      : os_(os),
        prefix_(std::move(prefix)),
        enabled_(enabled),
        mutex_(mutex),
        first_(true) {}

  template <typename T>
  PrefixedStreamThreadSafe& operator<<(const T& val) {
    if (!enabled_) return *this;
    maybe_prefix();
    buffer_ << val;
    return *this;
  }

  PrefixedStreamThreadSafe& operator<<(std::ostream& (*manip)(std::ostream&)) {
    if (!enabled_) return *this;

    if (manip == static_cast<std::ostream& (*)(std::ostream&)>(std::endl)) {
      flush();
    } else {
      buffer_ << manip;
    }

    return *this;
  }

  [[nodiscard]] bool is_enabled() const noexcept { return enabled_; }

 private:
  void maybe_prefix() {
    if (first_) {
      buffer_ << prefix_;
      first_ = false;
    }
  }

  void flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    os_ << buffer_.str() << std::endl;
    buffer_.str({});
    buffer_.clear();
    first_ = true;
  }

  std::ostream& os_;
  std::string prefix_;
  bool enabled_;
  std::mutex& mutex_;
  std::ostringstream buffer_;
  bool first_;
};

class LogStream {
 public:
  template <typename T>
  LogStream& operator<<(const T& val) {
    stream_->write(val);
    return *this;
  }

  LogStream& operator<<(std::ostream& (*manip)(std::ostream&)) {
    stream_->write(manip);
    return *this;
  }

  [[nodiscard]] bool is_enabled() const noexcept {
    return stream_->is_enabled();
  }

  static LogStream make_enabled(std::ostream& os, std::string prefix,
                                std::mutex& mutex) {
    return LogStream(
        std::make_shared<ImplPrefixed>(os, std::move(prefix), mutex));
  }

  static LogStream make_disabled() {
    return LogStream(std::make_shared<ImplNull>());
  }

 private:
  struct IStreamImpl {
    virtual ~IStreamImpl() = default;
    virtual void write(std::ostream& (*)(std::ostream&)) = 0;
    virtual void write(const std::string&) = 0;
    virtual bool is_enabled() const = 0;
  };

  struct ImplPrefixed : IStreamImpl {
    PrefixedStreamThreadSafe stream;
    ImplPrefixed(std::ostream& os, std::string prefix, std::mutex& m)
        : stream(os, std::move(prefix), true, m) {}

    void write(const std::string& s) override { stream << s; }
    void write(std::ostream& (*manip)(std::ostream&)) override {
      stream << manip;
    }
    bool is_enabled() const override { return stream.is_enabled(); }
  };

  struct ImplNull : IStreamImpl {
    void write(const std::string&) override {}
    void write(std::ostream& (*)(std::ostream&)) override {}
    bool is_enabled() const override { return false; }
  };

  explicit LogStream(std::shared_ptr<IStreamImpl> impl)
      : stream_(std::move(impl)) {}

  std::shared_ptr<IStreamImpl> stream_;
};

class PrefixedStream {
 public:
  PrefixedStream(std::ostream& os, std::string prefix, bool enabled = true)
      : os_(os), prefix_(std::move(prefix)), enabled_(enabled), first_(true) {}

  template <typename T>
  PrefixedStream& operator<<(const T& val) {
    if (!enabled_) return *this;
    maybe_prefix();
    os_ << val;
    return *this;
  }

  PrefixedStream& operator<<(std::ostream& (*manip)(std::ostream&)) {
    if (!enabled_) return *this;
    os_ << manip;
    if (manip == static_cast<std::ostream& (*)(std::ostream&)>(std::endl)) {
      first_ = true;
    }
    return *this;
  }

  [[nodiscard]] bool is_enabled() const noexcept { return enabled_; }

 private:
  void maybe_prefix() {
    if (first_) {
      os_ << prefix_;
      first_ = false;
    }
  }

  std::ostream& os_;
  std::string prefix_;
  bool enabled_;
  bool first_;
};

class NullStreamThreadSafe {
 public:
  template <typename T>
  NullStreamThreadSafe& operator<<(const T&) {
    return *this;
  }

  NullStreamThreadSafe& operator<<(std::ostream& (*)(std::ostream&)) {
    return *this;
  }

  [[nodiscard]] bool is_enabled() const noexcept { return false; }
};

class IOutput {
 public:
  virtual ~IOutput() = default;
  virtual LogStream trace() = 0;
  virtual LogStream debug() = 0;
  virtual LogStream info() = 0;
  virtual LogStream warning() = 0;
  virtual LogStream error() = 0;

  virtual std::ostream& stream() = 0;
  virtual std::ostream& err_stream() = 0;
  virtual std::size_t verbosity() const = 0;
};

class ConsoleOutputWithColor : public IOutput {
 public:
  explicit ConsoleOutputWithColor(std::size_t verbosity = 0,
                                  std::ostream& os = std::cerr)
      : verbosity_(verbosity), os_(os) {}

  LogStream error() override {
    return make_stream(log_color::color_prefix("[error]: ", log_color::RED),
                       verbosity_ >= 1);
  }

  LogStream warning() override {
    return make_stream(
        log_color::color_prefix("[warning]: ", log_color::YELLOW),
        verbosity_ >= 2);
  }

  LogStream info() override {
    return make_stream(log_color::color_prefix("[info]: ", log_color::GREEN),
                       verbosity_ >= 3);
  }

  LogStream debug() override {
    return make_stream(log_color::color_prefix("[debug]: ", log_color::CYAN),
                       verbosity_ >= 4);
  }

  LogStream trace() override {
    return make_stream(log_color::color_prefix("[trace]: ", log_color::GRAY),
                       verbosity_ >= 5);
  }

  std::ostream& stream() override { return std::cout; }
  std::ostream& err_stream() override { return std::cerr; }
  std::size_t verbosity() const override { return verbosity_; }

 private:
  LogStream make_stream(const std::string& prefix, bool enabled) {
    if (enabled && !use_color_) {
      return LogStream::make_enabled(os_, remove_color(prefix), mutex_);
    } else if (enabled) {
      return LogStream::make_enabled(os_, prefix, mutex_);
    } else {
      return LogStream::make_disabled();
    }
  }

  static std::string remove_color(const std::string& s) {
    // naive ANSI escape sequence stripper (optional)
    std::string result;
    bool skip = false;
    for (char c : s) {
      if (c == '\033')
        skip = true;
      else if (skip && c == 'm')
        skip = false;
      else if (!skip)
        result += c;
    }
    return result;
  }

  std::size_t verbosity_;
  std::ostream& os_;
  mutable std::mutex mutex_;
  bool use_color_ =
      isatty(fileno(stderr));  // or fileno(stdout) if using stdout
};

class ConsoleOutput : public IOutput {
 public:
  explicit ConsoleOutput(std::size_t verbosity = 0,
                         std::ostream& os = std::cerr)
      : verbosity_(verbosity), os_(os) {}

  LogStream trace() override {
    return make_stream("[trace]: ", verbosity_ >= 5);
  }

  LogStream debug() override {
    return make_stream("[debug]: ", verbosity_ >= 4);
  }

  LogStream info() override { return make_stream("[info]: ", verbosity_ >= 3); }

  LogStream warning() override {
    return make_stream("[warning]: ", verbosity_ >= 2);
  }

  LogStream error() override {
    return make_stream("[error]: ", verbosity_ >= 1);
  }

  std::ostream& stream() override { return std::cout; }
  std::ostream& err_stream() override { return std::cerr; }

  std::size_t verbosity() const override { return verbosity_; }

 private:
  LogStream make_stream(const std::string& prefix, bool enabled) {
    if (enabled) {
      return LogStream::make_enabled(os_, prefix, mutex_);
    } else {
      return LogStream::make_disabled();
    }
  }

  std::size_t verbosity_;
  std::ostream& os_;
  mutable std::mutex mutex_;
};

/**
 * default verbosity is 1, silent is 0.
 */
class OsstringOutput : public IOutput {
 public:
  explicit OsstringOutput(std::size_t verbosity = 0) : verbosity_(verbosity) {}

  LogStream trace() override {
    return make_stream("[trace]: ", verbosity_ >= 5);
  }

  LogStream debug() override {
    return make_stream("[debug]: ", verbosity_ >= 4);
  }

  LogStream info() override { return make_stream("[info]: ", verbosity_ >= 3); }

  LogStream warning() override {
    return make_stream("[warning]: ", verbosity_ >= 2);
  }

  LogStream error() override {
    return make_stream("[error]: ", verbosity_ >= 1);
  }

  std::ostream& stream() override { return os_; }
  std::ostream& err_stream() override { return os_; }

  std::string str() const { return os_.str(); }

  void clear() {
    os_.str("");
    os_.clear();
  }

  std::size_t verbosity() const override { return verbosity_; }

 private:
  LogStream make_stream(const std::string& prefix, bool enabled) {
    if (enabled) {
      return LogStream::make_enabled(os_, prefix, mutex_);
    } else {
      return LogStream::make_disabled();
    }
  }

  std::ostringstream os_;
  std::size_t verbosity_;
  mutable std::mutex mutex_;
};

class FileOutput : public IOutput {
 public:
  FileOutput(std::size_t verbosity, std::string_view file_path)
      : verbosity_(verbosity) {
    ofs_.open(file_path.data(), std::ios::out | std::ios::app);
    if (!ofs_.is_open()) {
      throw std::runtime_error("Failed to open output file: " +
                               std::string(file_path));
    }
  }

  ~FileOutput() override { ofs_.close(); }

  LogStream trace() override {
    return make_stream("[trace]: ", verbosity_ >= 4);
  }

  LogStream debug() override {
    return make_stream("[debug]: ", verbosity_ >= 3);
  }

  LogStream info() override { return make_stream("[info]: ", verbosity_ >= 2); }

  LogStream warning() override {
    return make_stream("[warning]: ", verbosity_ >= 1);
  }

  LogStream error() override {
    return make_stream("[error]: ", verbosity_ >= 1);
  }

  std::ostream& stream() override { return ofs_; }

  std::ostream& err_stream() override { return ofs_; }

  std::size_t verbosity() const override { return verbosity_; }

 private:
  LogStream make_stream(const std::string& prefix, bool enabled) {
    if (enabled) {
      return LogStream::make_enabled(ofs_, prefix, mutex_);
    } else {
      return LogStream::make_disabled();
    }
  }

  std::ofstream ofs_;
  std::size_t verbosity_;
  mutable std::mutex mutex_;
};

}  // namespace customio