#include <benchmark/benchmark.h>

#include <vector>

#include "namespace_aliases.h"

static void STRING_VECTOR(benchmark::State& state) {
  for (auto _ : state) {
    std::string s3 = "^{}";
    std::string s4 = "r";
    std::vector<char> v1(s3.data(), s3.data() + s3.size());
    v1.push_back('\0');
    v1.insert(v1.end(), s4.data(), s4.data() + s4.size());
    std::string line(v1.data(), v1.size());
  }
}

static void STRING_RESIZE(benchmark::State& state) {
  for (auto _ : state) {
    std::string s3 = "^{}";
    std::string s4 = "r";
    s3.resize(s3.size() + 1, '\0');
    s3.append(s4);
    std::string line{s3.data(), s3.size()};
  }
}

// BENCHMARK(PARSE_ONE_LINE);
// BENCHMARK(STRING_VECTOR)->Iterations(1000 * 10000);
// BENCHMARK(STRING_RESIZE)->Iterations(1000 * 10000);
BENCHMARK(STRING_VECTOR);
BENCHMARK(STRING_RESIZE);

BENCHMARK_MAIN();
