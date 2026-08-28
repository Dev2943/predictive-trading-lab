#pragma once

/// \file sizer.hpp
/// Position sizing: from an intention to a share count.
///
/// Sizing is where a good signal becomes a bad trade. A correct direction sized
/// wrongly loses money, and the sizing rule usually contributes more variance
/// to the outcome than the signal does. Every algorithm here is therefore
/// explicit about its assumptions and bounded by the same exposure limits.
///
/// KELLY IS FRACTIONAL BY DEFAULT AND CAPPED. Full Kelly is optimal only if the
/// edge estimate is exact, which it never is; with an overestimated edge it is
/// spectacularly destructive. The default quarter-Kelly reflects that the edge
/// is estimated, not known.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"
#include "ptl/portfolio/portfolio.hpp"
#include "ptl/signal/signal.hpp"

namespace ptl::sizing {

enum class SizingMethod : std::uint8_t {
    FixedShares,
    FixedDollar,
    PercentCapital,
    VolatilityTarget,
    RiskParity,
    Kelly,
    FractionalKelly,
};

[[nodiscard]] std::string_view to_string(SizingMethod) noexcept;

struct ExposureLimits {
    /// Sum of |position value| over equity.
    double max_gross_leverage = 2.0;
    /// |sum of signed position value| over equity.
    double max_net_leverage = 1.0;
    /// Fraction of equity in any one instrument.
    double max_position_weight = 0.25;
    /// Fraction of equity in any one sector.
    double max_sector_weight = 0.40;
    Notional max_position_notional{500'000.0};
    Notional max_order_notional{250'000.0};
};

struct SizingConfig {
    SizingMethod method{SizingMethod::VolatilityTarget};

    double fixed_shares = 100.0;
    Notional fixed_dollar{10'000.0};
    double percent_capital = 0.02;

    /// Annualised volatility each position is scaled toward.
    double target_volatility = 0.10;
    /// Periods per year, for converting a per-bar volatility to annual.
    double annualization_periods = 252.0 * 390.0;

    /// Multiplier on the full Kelly fraction. 0.25 by default: full Kelly
    /// assumes the edge is known exactly, and it is not.
    double kelly_fraction = 0.25;
    /// Hard ceiling on the Kelly weight regardless of estimated edge. An
    /// overestimated edge would otherwise size a single position at many times
    /// equity.
    double kelly_cap = 0.20;

    /// Round to this lot size. Zero disables rounding.
    double lot_size = 1.0;
    /// Orders below this notional are dropped: the commission floor makes a
    /// tiny trade unprofitable however good the signal.
    Notional min_trade_notional{500.0};

    ExposureLimits limits;
};

/// Everything a sizer may see, at one instant.
struct SizingContext {
    Timestamp now{kNoTimestamp};
    Notional equity{};
    Price reference_price{};
    /// Trailing per-period volatility of the instrument.
    double volatility = 0.0;
    /// Current signed position in shares.
    Qty current_position{};
    /// Sector id for the sector-exposure limit; -1 means ungrouped.
    std::int32_t sector = -1;
    /// Gross and net exposure already committed elsewhere in the book.
    Notional existing_gross{};
    Notional existing_net{};
    /// Notional already held in this instrument's sector.
    Notional sector_exposure{};
};

/// A sizing decision, including why it was capped.
struct SizingDecision {
    Qty target_position{};
    Notional target_notional{};
    double raw_weight = 0.0;
    double final_weight = 0.0;
    bool capped = false;
    std::string cap_reason;

    [[nodiscard]] std::string describe() const;
};

class IPositionSizer {
public:
    IPositionSizer() = default;
    virtual ~IPositionSizer() = default;
    IPositionSizer(const IPositionSizer&) = delete;
    IPositionSizer& operator=(const IPositionSizer&) = delete;

protected:
    IPositionSizer(IPositionSizer&&) = default;
    IPositionSizer& operator=(IPositionSizer&&) = default;

public:
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// \returns the TARGET position in shares, signed. Never a delta: the
    ///          rebalance engine computes deltas, so a sizer cannot
    ///          accidentally double a position by returning an increment.
    [[nodiscard]] virtual Result<SizingDecision> size(const signal::Signal&,
                                                      const SizingContext&) const = 0;
};

/// Every algorithm in one class, selected by configuration.
///
/// One implementation rather than seven, because the LIMIT APPLICATION is
/// identical for all of them and duplicating it seven times is how one copy
/// ends up missing a cap.
class PositionSizer final : public IPositionSizer {
public:
    explicit PositionSizer(SizingConfig cfg = {}) : cfg_(cfg) {}

    [[nodiscard]] std::string_view name() const noexcept override { return to_string(cfg_.method); }
    [[nodiscard]] Result<SizingDecision> size(const signal::Signal&,
                                              const SizingContext&) const override;

    [[nodiscard]] const SizingConfig& config() const noexcept { return cfg_; }

    /// The Kelly fraction for a signal, before any cap. Exposed because the
    /// formula deserves its own test rather than being reachable only through
    /// the full sizing path.
    [[nodiscard]] static double kelly_weight(double edge, double variance) noexcept;

    /// Inverse-volatility weight, normalised by the sizer's target.
    [[nodiscard]] static double risk_parity_weight(double volatility,
                                                   double target_volatility) noexcept;

private:
    /// Raw weight before limits, as a fraction of equity.
    [[nodiscard]] double raw_weight_for(const signal::Signal&, const SizingContext&) const;

    SizingConfig cfg_;
};

}  // namespace ptl::sizing
