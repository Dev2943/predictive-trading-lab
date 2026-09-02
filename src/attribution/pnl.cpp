#include "ptl/attribution/pnl.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace ptl::attribution {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::InvalidArgument, std::move(message), std::move(context));
}

/// Year fraction between two instants.
[[nodiscard]] double year_fraction(Timestamp begin, Timestamp end) {
    if (!is_set(begin) || !is_set(end) || end <= begin) return 0.0;
    const auto ns = static_cast<double>((end - begin).count());
    return ns / (365.25 * 24.0 * 3600.0 * 1e9);
}

}  // namespace

Notional PnlDecomposition::total_costs() const noexcept {
    // Every cost is already negative, so this sums rather than subtracts.
    return commission + exchange_fees + slippage + borrow + carry + cash_drag;
}

Notional PnlDecomposition::gross_pnl() const noexcept {
    return realized + unrealized;
}

Notional PnlDecomposition::net_pnl() const noexcept {
    return gross_pnl() + total_costs();
}

Notional PnlDecomposition::residual(Notional reported) const noexcept {
    return reported - net_pnl();
}

bool PnlDecomposition::closes(Notional reported, double tolerance) const noexcept {
    const double r = residual(reported).get();
    return is_finite(r) && std::abs(r) <= tolerance;
}

std::string PnlDecomposition::describe() const {
    std::ostringstream ss;
    ss.precision(2);
    ss << std::fixed;
    ss << "P&L " << to_iso8601(period_begin) << " .. " << to_iso8601(period_end) << '\n';
    ss << "  realized        " << realized.get() << '\n';
    ss << "  unrealized      " << unrealized.get() << '\n';
    ss << "  gross           " << gross_pnl().get() << '\n';
    ss << "  commission      " << commission.get() << '\n';
    ss << "  exchange fees   " << exchange_fees.get() << '\n';
    ss << "  slippage        " << slippage.get() << '\n';
    ss << "  borrow          " << borrow.get() << '\n';
    ss << "  carry           " << carry.get() << '\n';
    ss << "  cash drag       " << cash_drag.get() << '\n';
    ss << "  total costs     " << total_costs().get() << '\n';
    ss << "  net             " << net_pnl().get() << '\n';
    return ss.str();
}

bool FactorContribution::closes(double tolerance) const noexcept {
    const double sum = beta_contribution + alpha_contribution + residual_contribution;
    return is_finite(sum) && std::abs(sum - portfolio_return) <= tolerance;
}

std::string FactorContribution::describe() const {
    std::ostringstream ss;
    ss.precision(6);
    ss << std::fixed;
    ss << "return decomposition over " << periods << " periods\n";
    ss << "  portfolio return  " << portfolio_return << '\n';
    ss << "  beta              " << beta << '\n';
    ss << "  beta contribution " << beta_contribution << '\n';
    ss << "  alpha             " << alpha_contribution << '\n';
    ss << "  residual          " << residual_contribution << '\n';
    return ss.str();
}

Result<PnlDecomposition> PnlAttributor::decompose_period(const portfolio::EquityPoint& begin,
                                                         const portfolio::EquityPoint& end,
                                                         std::span<const oms::Fill> fills) const {
    if (!is_set(begin.ts) || !is_set(end.ts)) {
        return fail(bad("equity observations must carry timestamps"));
    }
    if (end.ts < begin.ts) {
        return fail(bad("period end precedes its beginning",
                        to_iso8601(end.ts) + " < " + to_iso8601(begin.ts)));
    }

    PnlDecomposition out;
    out.period_begin = begin.ts;
    out.period_end = end.ts;

    // --- realised and unrealised, from the portfolio's own columns ----------
    // Taken as DIFFERENCES of cumulative figures, not recomputed. The portfolio
    // is the authority on its own P&L; recomputing here would create a second
    // implementation that could disagree with it.
    out.realized = end.realized_pnl - begin.realized_pnl;
    out.unrealized = end.unrealized_pnl - begin.unrealized_pnl;

    // --- explicit execution costs, from fills ------------------------------
    double commission = 0.0;
    double fees = 0.0;
    double slippage = 0.0;
    for (const auto& fill : fills) {
        if (fill.fill_time() < begin.ts || fill.fill_time() > end.ts) continue;
        commission += fill.commission().get();
        fees += fill.exchange_fee().get();

        // Slippage against arrival, converted from basis points to currency.
        // Positive bps is a cost by Fill's convention for either side.
        const double bps = fill.slippage_bps().get();
        if (is_finite(bps) && fill.arrival_price().get() > 0.0) {
            slippage += bps * 1e-4 * fill.arrival_price().get() * fill.quantity().get();
        }
    }
    // Stored NEGATIVE: positive is a contribution to P&L for every field.
    out.commission = Notional{-commission};
    out.exchange_fees = Notional{-fees};
    out.slippage = Notional{-slippage};

    // --- financing ---------------------------------------------------------
    // Borrow, carry and cash drag are properties of positions HELD OVER TIME.
    // No fill records them, and inferring them from a fill stream would be
    // fabrication, so they come from configured rates applied to exposures.
    const double years = year_fraction(begin.ts, end.ts);
    if (years > 0.0) {
        const double gross = begin.gross_exposure.get();
        const double net = begin.net_exposure.get();
        const double equity = begin.equity.get();
        const double cash = begin.cash.get();

        // Short notional is the part of gross not accounted for by the long
        // side: (gross - net) / 2.
        const double short_notional = std::max(0.0, (gross - net) * 0.5);
        out.borrow = Notional{-short_notional * rates_.borrow_rate * years};

        // Margin financing applies only to exposure beyond equity.
        const double borrowed = std::max(0.0, gross - equity);
        const double financing_cost = borrowed * rates_.margin_rate * years;
        const double cash_interest = std::max(0.0, cash) * rates_.cash_rate * years;
        // Carry nets interest earned against financing paid, so a book that is
        // long cash shows a positive carry rather than a suppressed cost.
        out.carry = Notional{cash_interest - financing_cost};

        // Cash drag: the return idle cash did NOT earn. Only a cost when there
        // was somewhere better for it to be.
        if (rates_.opportunity_rate > 0.0 && equity > 0.0) {
            const double idle = std::max(0.0, equity - gross);
            out.cash_drag = Notional{-idle * rates_.opportunity_rate * years};
        }
    }

    for (const double v :
         {out.realized.get(), out.unrealized.get(), out.commission.get(), out.exchange_fees.get(),
          out.slippage.get(), out.borrow.get(), out.carry.get(), out.cash_drag.get()}) {
        if (!is_finite(v)) return fail(bad("P&L decomposition produced a non-finite term"));
    }
    return out;
}

Result<std::vector<PnlDecomposition>> PnlAttributor::decompose_series(
    std::span<const portfolio::EquityPoint> curve, std::span<const oms::Fill> fills) const {
    if (curve.size() < 2) {
        return fail(bad("P&L decomposition needs at least two equity observations"));
    }

    // Fills are bucketed by scanning once in time order rather than filtering
    // the whole span per period, which would be quadratic in the fill count.
    // At institutional volumes that difference is the difference between a
    // report that runs and one that does not.
    std::vector<oms::Fill> sorted(fills.begin(), fills.end());
    std::stable_sort(sorted.begin(), sorted.end(), [](const oms::Fill& a, const oms::Fill& b) {
        return a.fill_time() < b.fill_time();
    });

    std::vector<PnlDecomposition> out;
    out.reserve(curve.size() - 1);

    std::size_t cursor = 0;
    for (std::size_t i = 1; i < curve.size(); ++i) {
        const Timestamp begin = curve[i - 1].ts;
        const Timestamp end = curve[i].ts;

        const std::size_t first = cursor;
        while (cursor < sorted.size() && sorted[cursor].fill_time() <= end) ++cursor;
        const std::span<const oms::Fill> window{sorted.data() + first, cursor - first};

        auto period = decompose_period(curve[i - 1], curve[i], window);
        if (!period) return fail(period.error());
        (void)begin;
        out.push_back(*period);
    }
    return out;
}

PnlDecomposition PnlAttributor::aggregate(std::span<const PnlDecomposition> periods) {
    PnlDecomposition out;
    if (periods.empty()) return out;

    out.period_begin = periods.front().period_begin;
    out.period_end = periods.back().period_end;
    for (const auto& p : periods) {
        out.realized = out.realized + p.realized;
        out.unrealized = out.unrealized + p.unrealized;
        out.commission = out.commission + p.commission;
        out.exchange_fees = out.exchange_fees + p.exchange_fees;
        out.slippage = out.slippage + p.slippage;
        out.borrow = out.borrow + p.borrow;
        out.carry = out.carry + p.carry;
        out.cash_drag = out.cash_drag + p.cash_drag;
    }
    return out;
}

Result<FactorContribution> PnlAttributor::factor_contribution(
    std::span<const double> portfolio_returns, std::span<const double> benchmark_returns) {
    if (portfolio_returns.empty()) {
        return fail(bad("cannot decompose an empty return series"));
    }
    if (portfolio_returns.size() != benchmark_returns.size()) {
        // Refusing beats truncating: a mismatched benchmark pairs each return
        // with the wrong observation and produces a beta that means nothing.
        return fail(bad("benchmark length (" + std::to_string(benchmark_returns.size()) +
                        ") does not match the portfolio series (" +
                        std::to_string(portfolio_returns.size()) + ")"));
    }

    const auto n = static_cast<double>(portfolio_returns.size());
    double mean_p = 0.0;
    double mean_b = 0.0;
    for (std::size_t i = 0; i < portfolio_returns.size(); ++i) {
        if (!is_finite(portfolio_returns[i]) || !is_finite(benchmark_returns[i])) {
            return fail(bad("return series contains a non-finite value"));
        }
        mean_p += portfolio_returns[i];
        mean_b += benchmark_returns[i];
    }
    mean_p /= n;
    mean_b /= n;

    double covariance = 0.0;
    double variance_b = 0.0;
    for (std::size_t i = 0; i < portfolio_returns.size(); ++i) {
        const double db = benchmark_returns[i] - mean_b;
        covariance += (portfolio_returns[i] - mean_p) * db;
        variance_b += db * db;
    }

    FactorContribution out;
    out.periods = portfolio_returns.size();
    // Cumulative, not mean: a contribution is a quantity of return, and a
    // reader comparing it against total P&L expects the total.
    out.portfolio_return = mean_p * n;
    out.benchmark_return = mean_b * n;

    // A benchmark with no variance explains nothing, so beta is zero rather
    // than infinite, and the whole return becomes alpha.
    out.beta = variance_b > 0.0 ? covariance / variance_b : 0.0;
    if (!is_finite(out.beta)) out.beta = 0.0;

    out.beta_contribution = out.beta * out.benchmark_return;
    out.alpha_contribution = out.portfolio_return - out.beta_contribution;
    // Exact by construction here. The field exists so a future model with more
    // than one factor has somewhere honest to put what it cannot explain,
    // rather than folding it into alpha.
    out.residual_contribution = 0.0;
    return out;
}

}  // namespace ptl::attribution
