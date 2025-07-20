#pragma once

#include <fstream>
#include <iostream>
#include <ostream>
#include <sstream>
#include <string>

namespace customio {
class PrefixedStream {
 public:
  PrefixedStream(std::ostream& os, std::string prefix, bool do_stream)
      : os_(os),
        do_stream_(do_stream),
        prefix_(std::move(prefix)),
        first_(true) {}

  template <typename T>
  PrefixedStream& operator<<(const T& val) {
    if (!do_stream_) {
      return *this;
    }
    if (first_) {
      os_ << prefix_;
      first_ = false;
    }
    os_ << val;
    return *this;
  }

  PrefixedStream& operator<<(std::ostream& (*manip)(std::ostream&)) {
    if (!do_stream_) {
      return *this;
    }
    os_ << manip;
    first_ = true;  // reset after newline or flush
    return *this;
  }

 private:
  std::ostream& os_;
  bool do_stream_;
  std::string prefix_;
  bool first_;
};

class NullOpStream {
 public:
  template <typename T>
  NullOpStream& operator<<(const T&) {
    return *this;
  }

  NullOpStream& operator<<(std::ostream& (*)(std::ostream&)) { return *this; }
};

class IOutput {
 public:
  virtual PrefixedStream trace() = 0;
  virtual PrefixedStream debug() = 0;
  virtual PrefixedStream info() = 0;
  virtual PrefixedStream warning() = 0;
  virtual PrefixedStream error() = 0;

  virtual std::ostream& stream() = 0;      // For direct access if needed
  virtual std::ostream& err_stream() = 0;  // For direct access if needed
  virtual ~IOutput() = default;
};

/**
 * default verbosity is 1, silent is 0.
 */
class ConsoleOutput : public IOutput {
  size_t verbosity_{0};
  std::ostream& os_ = std::cerr;  // Default output stream

 public:
  ConsoleOutput(size_t verbosity) : verbosity_(verbosity) {}

  PrefixedStream trace() override {
    return PrefixedStream(std::cerr, "[trace]: ",
                          verbosity_ > 4);  // if false, will generate output.
  }
  PrefixedStream debug() override {
    return PrefixedStream(std::cerr, "[debug]: ", verbosity_ > 3);
  }

  PrefixedStream info() override {
    return PrefixedStream(std::cerr, "[info]: ", verbosity_ > 2);
  }

  PrefixedStream warning() override {
    return PrefixedStream(std::cerr, "[warning]: ", verbosity_ > 1);
  }

  PrefixedStream error() override {
    return PrefixedStream(std::cerr, "[error]: ", verbosity_ > 0);
  }
  std::ostream& stream() override { return std::cout; }
  std::ostream& err_stream() override { return std::cerr; }
};

/**
 * default verbosity is 1, silent is 0.
 */
class OsstringOutput : public IOutput {
  std::ostringstream os_;
  size_t verbosity_{0};

 public:
  OsstringOutput(size_t verbosity) : verbosity_(verbosity) {}

  PrefixedStream trace() override {
    return PrefixedStream(os_, "[trace]: ", verbosity_ > 4);
  }
  PrefixedStream debug() override {
    return PrefixedStream(os_, "[debug]: ", verbosity_ > 3);
  }

  PrefixedStream info() override {
    return PrefixedStream(os_, "[info]: ", verbosity_ > 2);
  }

  PrefixedStream warning() override {
    return PrefixedStream(os_, "[warning]: ", verbosity_ > 1);
  }

  PrefixedStream error() override {
    return PrefixedStream(os_, "[error]: ", verbosity_ > 0);
  }

  std::ostream& stream() override { return os_; }
  std::ostream& err_stream() override { return os_; }

  std::string str() const { return os_.str(); }

  void clear() {
    os_.str("");
    os_.clear();
  }  // Clear the stream
};

class FileOutput : public IOutput {
  std::ofstream ofs_;
  size_t verbosity_{0};

 public:
  FileOutput(size_t verbosity, std::string_view file_path)
      : verbosity_(verbosity) {
    ofs_.open(file_path.data(), std::ios::out | std::ios::app);
    if (!ofs_.is_open()) {
      throw std::runtime_error("Failed to open output file: " +
                               std::string(file_path));
    }
  }

  ~FileOutput() { ofs_.close(); }
  // valve == 5, means all logs are enabled.
  // valve == 0, means all logs are disabled.
  PrefixedStream trace() override {
    return PrefixedStream(ofs_,
                          "[trace]: ", verbosity_ > 4);  // when valve_ is 0,
  }
  PrefixedStream debug() override {
    return PrefixedStream(ofs_, "[debug]: ", verbosity_ > 3);
  }

  PrefixedStream info() override {
    return PrefixedStream(ofs_, "[info]: ", verbosity_ > 2);
  }

  PrefixedStream warning() override {
    return PrefixedStream(ofs_, "[warning]: ", verbosity_ > 1);
  }

  PrefixedStream error() override {
    return PrefixedStream(ofs_, "[error]: ", verbosity_ > 0);
  }

  std::ostream& stream() override { return ofs_; }
  std::ostream& err_stream() override { return ofs_; }
};

}  // namespace customio