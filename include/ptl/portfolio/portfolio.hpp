#pragma once

/// \file portfolio.hpp
/// Cash, positions, marks and the equity curve.
///
/// One identity governs everything here and is asserted after every mutation:
///
///     equity = cash + sum(quantity_i * mark_i)
///
/// Marking policy is LIQUIDATION value by default: longs to bid, shorts to ask.
/// Mid-marking overstates NAV by half a spread per unit of gross exposure on
/// every single bar, which compounds into a material and entirely fictional
/// return. Mid is available separately as an analytic overlay.

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"
#include "ptl/market/quote.hpp"
#include "ptl/portfolio/position.hpp"

namespace ptl::portfolio {

enum class MarkMode : std::uint8_t {
    Liquidation,  ///< longs to bid, shorts to ask -- the reported default
    Mid,          ///< analytic overlay only
    Last,         ///< bar-only fallback, when no quote exists
};

struct PortfolioConfig {
    Notional initial_cash{1'000'000.0};
    MarkMode mark_mode{MarkMode::Liquidation};
    /// Annualised borrow rate applied to short market value. Zero disables it,
    /// but for a market-neutral book this is material, not decorative.
    double annual_borrow_rate = 0.0;
    bool allow_short = true;
};

/// One row of the equity curve.
struct EquityPoint {
    Timestamp ts{kNoTimestamp};
    Notional equity{};
    Notional cash{};
    Notional gross_exposure{};
    Notional net_exposure{};
    Notional realized_pnl{};
    Notional unrealized_pnl{};
    Notional cumulative_costs{};
    Notional turnover{};
};

class Portfolio {
public:
    explicit Portfolio(PortfolioConfig cfg = {});

    [[nodiscard]] Result<bool> apply(const oms::Fill& fill);
    [[nodiscard]] Result<bool> apply_split(InstrumentId instrument, double ratio);
    [[nodiscard]] Result<bool> apply_dividend(InstrumentId instrument, Notional per_share);

    /// Record a tradable mark. Both sides are required so liquidation marking
    /// is possible; use mark_last() when only a bar is available.
    void mark(InstrumentId instrument, Price bid, Price ask) noexcept;
    void mark_last(InstrumentId instrument, Price last) noexcept;
    void mark_from_quote(const market::Quote& q) noexcept;

    /// Append an equity-curve point and assert the accounting identity.
    [[nodiscard]] Result<EquityPoint> snapshot(Timestamp ts);

    [[nodiscard]] Notional cash() const noexcept { return cash_; }
    [[nodiscard]] Notional equity() const noexcept;
    [[nodiscard]] Notional position_value() const noexcept;
    [[nodiscard]] Notional gross_exposure() const noexcept;
    [[nodiscard]] Notional net_exposure() const noexcept;
    [[nodiscard]] Notional realized_pnl() const noexcept { return realized_; }
    [[nodiscard]] Notional unrealized_pnl() const noexcept;
    [[nodiscard]] Notional total_costs() const noexcept { return costs_; }
    [[nodiscard]] Notional turnover() const noexcept { return turnover_; }

    /// gross_exposure / equity. Zero equity yields zero rather than infinity.
    [[nodiscard]] double leverage() const noexcept;

    /// Cash unencumbered by existing positions. Never negative.
    [[nodiscard]] Notional buying_power() const noexcept;

    [[nodiscard]] const Position* position(InstrumentId) const noexcept;
    [[nodiscard]] std::optional<Price> mark_price(InstrumentId) const noexcept;
    [[nodiscard]] const std::map<std::uint32_t, Position>& positions() const noexcept {
        return positions_;
    }
    [[nodiscard]] std::span<const EquityPoint> equity_curve() const noexcept { return curve_; }
    [[nodiscard]] const PortfolioConfig& config() const noexcept { return cfg_; }

    /// The identity that must hold at all times. Exposed so tests and the
    /// reconciliation step can assert it directly.
    [[nodiscard]] bool identity_holds(double tolerance = 1e-6) const noexcept;

private:
    struct MarkPair {
        Price bid{};
        Price ask{};
    };
    [[nodiscard]] Price mark_for(InstrumentId, const Position&) const noexcept;

    PortfolioConfig cfg_;
    Notional cash_{};
    Notional realized_{};
    Notional costs_{};
    Notional turnover_{};
    std::map<std::uint32_t, Position> positions_;
    std::map<std::uint32_t, MarkPair> marks_;
    std::vector<EquityPoint> curve_;
};

}  // namespace ptl::portfolio
