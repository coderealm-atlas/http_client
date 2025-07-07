#include <benchmark/benchmark.h>

static void BM_StringCreation(benchmark::State &state)
{
  for (auto _ : state)
    std::string empty_string;
}
// Register the function as a benchmark
BENCHMARK(BM_StringCreation);

// Define another benchmark
static void BM_StringCopy(benchmark::State &state)
{
  std::string x = "hello";
  for (auto _ : state)
    std::string copy(x);
}
BENCHMARK(BM_StringCopy)
    // ->Range(8, 100000000);
    ->RangeMultiplier(10)
    ->Range(100, 10'0000'0000);
// ->ArgsProduct({{1<<10, 3<<10, 8<<10}, {20, 40, 60, 80}})

// ->ArgsProduct({
//   benchmark::CreateRange(8, 128, /*multi=*/2),
//   benchmark::CreateDenseRange(1, 4, /*step=*/1)
// })

BENCHMARK_MAIN();
