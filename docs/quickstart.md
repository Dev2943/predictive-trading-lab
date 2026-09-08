# Quick start

## Build

```bash
cmake --preset linux-gcc-release     # or macos-release, linux-clang-debug
cmake --build build/linux-gcc-release
ctest --test-dir build/linux-gcc-release
```

Eigen 3 is required. Without it the configure step **fails loudly** rather than
building a smaller library — a partial build that passes a smaller test suite
looks like success and is not.

```bash
sudo apt install libeigen3-dev        # Debian/Ubuntu
brew install eigen                    # macOS
```

## Confirm your build is reproducible

```bash
./build/linux-gcc-release/apps/ptl_version -c config/base.toml
```

```
config hash            30b44e5972450aad
rng[0..2] from seed    d05ef55272cdfb14 2e2f422341add64e 1c120f3d1ce63170
```

If these differ, stop and find out why before trusting a backtest from this
build. See [reproducibility.md](reproducibility.md).

## Run the examples

```bash
cmake --preset linux-gcc-release -DPTL_BUILD_EXAMPLES=ON
cmake --build build/linux-gcc-release
./build/linux-gcc-release/examples/example_replay
```

| Example | Shows |
|---|---|
| `example_replay` | A complete backtest: strategy, engine, results |
| `example_paper` | The same strategy against a paper account |
| `example_optimization` | Covariance estimation and the nine optimizers |
| `example_walk_forward` | Splits, training, out-of-sample evaluation |
| `example_execution` | TWAP, VWAP, POV, Iceberg schedules |

Each is a single file under `examples/`, compiled by the build and run in CI.

## Write a strategy

```cpp
class MyStrategy final : public ptl::engine::IStrategy {
public:
    std::string_view name() const noexcept override { return "my_strategy"; }

    void on_bar(const ptl::market::Bar& bar,
                const ptl::engine::StrategyContext& ctx,
                ptl::engine::OrderSink& sink) override {
        ptl::LifecycleTimes times;
        times.decision_time = bar.close_time();   // never the wall clock

        auto order = ptl::oms::Order::market(
            sink.next_order_id(), bar.instrument(),
            ptl::Side::Buy, ptl::Qty{10}, times);
        if (!order) return;

        (void)sink.submit(order->with_arrival_price(bar.close()));
    }
};
```

Two things that are not optional:

- **`decision_time` comes from the event, never from a clock.** A strategy that
  reads the wall clock cannot be replayed.
- **`sink.submit` is the only route to the venue.** It runs the risk gate. There
  is no other path, and that is deliberate.

`examples/example_replay.cpp` has the full working version.
