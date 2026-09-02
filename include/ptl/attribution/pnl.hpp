#pragma once

/// \file pnl.hpp
/// Economic decomposition of profit and loss.
///
/// WHAT THIS ADDS THAT PHASE 10 DOES NOT. `analytics::AttributionAnalyzer`
/// answers "which instrument/sector/strategy made the money?" -- attribution
/// along an ORGANISATIONAL dimension. This answers a different question:
/// "which ECONOMIC SOURCE did the money come from?" Price movement, financing,
/// borrow, execution cost and cash drag are separate phenomena that a
/// per-instrument table sums together and cannot separate.
///
/// Both are needed and neither subsumes the other. A sector table showing a
/// loss in energy does not say whether the position was wrong or merely
/// expensive to hold; this decomposition does.
///
/// THE IDENTITY IS THE POINT. Every component sums to the reported change in
/// equity, and `residual()` is asserted to be zero. A decomposition that does
/// not close is a set of plausible numbers, not an explanation.

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/accounting/journal.hpp"
#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"
#include "ptl/oms/fill.hpp"
#include "ptl/portfolio/portfolio.hpp"

namespace ptl::attribution {

/// Where a dollar came from, over one period.
///
/// Sign convention: POSITIVE IS A CONTRIBUTION TO P&L for every field. Costs
/// are therefore stored negative. One convention throughout spares every
/// aggregate a special case, and a mixed convention is how a sign error hides
/// inside a table that still adds up.
struct PnlDecomposition {
    Timestamp period_begin{kNoTimestamp};
    Timestamp period_end{kNoTimestamp};

    /// Realised on positions closed during the period.
    Notional realized{};
    /// Change in the mark-to-market value of positions still open. Includes
    /// positions opened during the period and not yet closed.
    Notional unrealized{};

    // --- explicit costs, all negative -------------------------------------
    Notional commission{};
    Notional exchange_fees{};
    /// Execution cost against the arrival benchmark. Distinct from commission:
    /// commission is invoiced, slippage is inferred, and conflating them hides
    /// which of the two a change in broker would actually fix.
    Notional slippage{};
    /// Cost of borrowing to hold a short.
    Notional borrow{};
    /// Financing on leveraged long positions, and interest earned on cash.
    Notional carry{};
    /// Return foregone on uninvested cash. A real cost of an under-invested
    /// book, and invisible in any P&L that only counts what was traded.
    Notional cash_drag{};

    [[nodiscard]] Notional total_costs() const noexcept;
    [[nodiscard]] Notional gross_pnl() const noexcept;
    [[nodiscard]] Notional net_pnl() const noexcept;

    /// Reported equity change minus the sum of the components. MUST be zero.
    [[nodiscard]] Notional residual(Notional reported_equity_change) const noexcept;
    [[nodiscard]] bool closes(Notional reported_equity_change,
                              double tolerance = 1e-6) const noexcept;

    [[nodiscard]] std::string describe() const;
};

/// Systematic versus idiosyncratic return.
///
/// Requires a benchmark. Computing it against an implicit zero series would
/// silently redefine alpha as total return -- the same trap `RiskAnalyzer`
/// avoids, and worth avoiding identically here.
struct FactorContribution {
    /// Return explained by benchmark exposure: beta times benchmark return.
    double beta_contribution = 0.0;
    /// Return not explained by the benchmark.
    double alpha_contribution = 0.0;
    /// Anything left after both. Non-zero only through estimation error, and
    /// reported rather than absorbed into alpha, which is where an
    /// unexplained remainder conventionally goes to hide.
    double residual_contribution = 0.0;

    double beta = 0.0;
    double benchmark_return = 0.0;
    double portfolio_return = 0.0;
    std::size_t periods = 0;

    [[nodiscard]] bool closes(double tolerance = 1e-9) const noexcept;
    [[nodiscard]] std::string describe() const;
};

/// Cost inputs the analyzer cannot infer from fills alone.
///
/// Borrow, carry and cash drag are properties of POSITIONS HELD OVER TIME, not
/// of executions. No fill records them, and inventing them from a fill stream
/// would be fabrication. The caller supplies the rates; this module applies
/// them consistently.
struct FinancingRates {
    /// Annualised borrow rate on short notional.
    double borrow_rate = 0.0;
    /// Annualised financing rate on margin borrowing.
    double margin_rate = 0.0;
    /// Annualised rate earned on idle cash.
    double cash_rate = 0.0;
    /// Benchmark return per period, for the cash-drag opportunity cost. Cash
    /// drag is the return the cash DID NOT earn, so it needs a reference for
    /// what it could have.
    double opportunity_rate = 0.0;
    double periods_per_year = 252.0;
};

/// Decomposes P&L over an equity curve.
///
/// READ-ONLY. Every input is a const reference to completed history; nothing
/// here writes to a portfolio, an OMS or a journal. The layer observes and
/// never trades.
class PnlAttributor {
public:
    explicit PnlAttributor(FinancingRates rates = {}) : rates_(rates) {}

    /// Decompose a single period from two consecutive equity observations.
    [[nodiscard]] Result<PnlDecomposition> decompose_period(
        const portfolio::EquityPoint& begin, const portfolio::EquityPoint& end,
        std::span<const oms::Fill> fills_in_period) const;

    /// Decompose each period of a curve. One entry per interval, so the result
    /// is `curve.size() - 1` long.
    [[nodiscard]] Result<std::vector<PnlDecomposition>> decompose_series(
        std::span<const portfolio::EquityPoint> curve, std::span<const oms::Fill> fills) const;

    /// Sum a series into one decomposition covering the whole span.
    [[nodiscard]] static PnlDecomposition aggregate(std::span<const PnlDecomposition>);

    /// Split returns into beta and alpha contributions.
    [[nodiscard]] static Result<FactorContribution> factor_contribution(
        std::span<const double> portfolio_returns, std::span<const double> benchmark_returns);

    [[nodiscard]] const FinancingRates& rates() const noexcept { return rates_; }

private:
    FinancingRates rates_;
};

}  // namespace ptl::attribution
