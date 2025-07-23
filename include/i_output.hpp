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

#include "log_stream.hpp"

namespace customio {

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