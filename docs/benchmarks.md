# Benchmarks

```bash
cmake --preset benchmark
cmake --build build/benchmark
./build/benchmark/benchmarks/ptl_bench_core
./build/benchmark/benchmarks/ptl_bench_core --benchmark_filter="Live"
```

## Methodology, and its limits

Google Benchmark, single-threaded, on whatever machine you run it. The numbers
in this repository come from **GCC 13 on x86-64 in a shared container**, which
means:

- They are **not** production latency figures. A container under variable load
  is the wrong place to measure a p99.
- **Relative** costs and **scaling** are the useful signal. That rolling
  volatility is O(n) rather than O(n·w) is a real property; that it took 125 µs
  on that particular afternoon is not.
- Re-running the same benchmark on this repository has produced spreads of
  20–25% between sessions. Treat a change smaller than that as noise.

## Reading a scaling claim

```
BM_RollingVolatility/1000     9.0 µs
BM_RollingVolatility/10000    125 µs
```

Ten times the input, roughly fourteen times the cost: linear, with the constant
you would expect from a larger working set. That is the claim the benchmark
exists to check. Compare against:

```
BM_RollingVaR/10000           307 µs
```

Two and a half times the incremental statistics, because a quantile needs order
statistics and is O(n log w). That cost is unavoidable and is why VaR is a
separate benchmark rather than averaged into the others.

## A benchmark that measures nothing

`BM_StructuredLogging` once reported `0.000 ns`. The compiler had proved the
branch was never taken and deleted the loop — a number that reads as excellent
news and means the benchmark is broken.

If a benchmark reports a suspiciously round zero, or a throughput above what the
memory bus permits, assume it has been optimised away and check for
`benchmark::DoNotOptimize` on the value the loop actually produces.

## Categories

| Group | Path | Typical cost |
|---|---|---|
| Core types, RNG, time | hot | 1–20 ns |
| Features, signals | per event | 10–400 ns |
| Execution algorithms | per order | 25 ns–4 µs |
| Optimization | per rebalance | 5 µs–2 ms |
| Analytics, attribution | per report | 10 µs–2 ms |
| Live and paper adapters | per order/message | 20 ns–2 µs |
| Ops instrumentation | per event | 0.5–110 ns |

The last row is the one to watch: instrumentation sits on the per-event path,
and a counter increment at 5.5 ns is affordable where one at 500 ns would not be.
