#pragma once

/// \file strategy.hpp
/// The strategy interface and its registry.
///
/// One interface, both modes. A strategy has no idea whether it is being
/// replayed or traded live, because everything it can reach -- the clock, the
/// portfolio, the order sink -- is an interface the engine supplies.

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/engine/context.hpp"
#include "ptl/market/event.hpp"
#include "ptl/oms/fill.hpp"

namespace ptl::engine {

class IStrategy {
public:
    IStrategy() = default;
    virtual ~IStrategy() = default;
    IStrategy(const IStrategy&) = delete;
    IStrategy& operator=(const IStrategy&) = delete;

protected:
    IStrategy(IStrategy&&) = default;
    IStrategy& operator=(IStrategy&&) = default;

public:
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// Called once before any event. Failing here aborts the run, which is the
    /// correct response to a strategy that cannot configure itself.
    [[nodiscard]] virtual Result<bool> on_start(const StrategyContext&) { return true; }

    virtual void on_bar(const market::Bar&, const StrategyContext&, OrderSink&) {}
    virtual void on_quote(const market::Quote&, const StrategyContext&, OrderSink&) {}
    virtual void on_trade(const market::Trade&, const StrategyContext&, OrderSink&) {}
    virtual void on_fill(const oms::Fill&, const StrategyContext&) {}
    virtual void on_session_open(Timestamp, const StrategyContext&, OrderSink&) {}
    virtual void on_session_close(Timestamp, const StrategyContext&, OrderSink&) {}
    virtual void on_stop(const StrategyContext&) {}
};

/// Constructs a strategy from its TOML configuration section.
using StrategyFactory =
    std::function<Result<std::unique_ptr<IStrategy>>(const std::string& config_toml)>;

/// Name-to-factory registry.
///
/// An explicit object rather than a static singleton with self-registering
/// globals. Static initialisation order is unspecified, and a registry that
/// fills itself during static init makes the set of available strategies
/// depend on link order -- which is exactly the kind of hidden non-determinism
/// this project exists to avoid.
class StrategyRegistry {
public:
    [[nodiscard]] Result<bool> register_strategy(std::string name, StrategyFactory factory);
    [[nodiscard]] Result<std::unique_ptr<IStrategy>> create(
        std::string_view name, const std::string& config_toml = {}) const;

    [[nodiscard]] bool contains(std::string_view name) const noexcept;
    [[nodiscard]] std::vector<std::string_view> names() const;
    [[nodiscard]] std::size_t size() const noexcept { return factories_.size(); }

private:
    // std::map: names() must be deterministic, and a report listing available
    // strategies in hash order would differ between runs.
    std::map<std::string, StrategyFactory, std::less<>> factories_;
};

}  // namespace ptl::engine
