#include "ptl/analytics/exposure.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace ptl::analytics {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

}  // namespace

std::string ExposureSnapshot::describe() const {
    std::ostringstream ss;
    ss.precision(4);
    ss << std::fixed;
    ss << "exposure at " << to_iso8601(ts) << '\n';
    ss << "  long      " << long_exposure.get() << " (" << long_positions << " positions)\n";
    ss << "  short     " << short_exposure.get() << " (" << short_positions << " positions)\n";
    ss << "  gross     " << gross_exposure.get() << "  leverage " << gross_leverage << '\n';
    ss << "  net       " << net_exposure.get() << "  leverage " << net_leverage << '\n';
    ss << "  max conc  " << max_concentration << '\n';
    for (const auto& [sector, notional] : sector_exposure) {
        ss << "  sector " << sector << "  " << notional.get() << '\n';
    }
    for (const auto& [factor, loading] : factor_exposure) {
        ss << "  factor " << factor << "  " << loading << '\n';
    }
    return ss.str();
}

ExposureSnapshot ExposureTracker::snapshot(const portfolio::Portfolio& pf, Timestamp ts) const {
    // READ-ONLY throughout: a const Portfolio& in, a value out. There is no
    // path by which computing an exposure could alter a position.
    ExposureSnapshot out;
    out.ts = ts;
    out.equity = pf.equity();

    const double equity = out.equity.get();
    double longs = 0.0;
    double shorts = 0.0;
    double max_weight = 0.0;

    for (const auto& [key, position] : pf.positions()) {
        if (position.is_flat()) continue;
        const auto mark = pf.mark_price(position.instrument());
        if (!mark.has_value()) continue;

        const double value = position.quantity().get() * mark->get();
        if (!is_finite(value)) continue;

        if (value > 0.0) {
            longs += value;
            ++out.long_positions;
        } else {
            // Stored as a POSITIVE magnitude: "short exposure of 40,000" is
            // clearer than "-40,000", and the sign lives in net_exposure where
            // it belongs.
            shorts += -value;
            ++out.short_positions;
        }

        if (equity > 0.0) {
            max_weight = std::max(max_weight, std::abs(value) / equity);
        }

        const auto sector_it = cfg_.sectors.find(key);
        if (sector_it != cfg_.sectors.end()) {
            // Signed, so a long and a short in one sector net off -- which is
            // the question a sector limit is actually asking.
            out.sector_exposure[sector_it->second] =
                out.sector_exposure[sector_it->second] + Notional{value};
        }

        const auto loadings_it = cfg_.factor_loadings.find(key);
        if (loadings_it != cfg_.factor_loadings.end() && equity > 0.0) {
            for (const auto& [factor, loading] : loadings_it->second) {
                // Weight-times-loading, summed across the book: the book's net
                // exposure to that factor.
                out.factor_exposure[factor] += (value / equity) * loading;
            }
        }
    }

    out.long_exposure = Notional{longs};
    out.short_exposure = Notional{shorts};
    out.gross_exposure = Notional{longs + shorts};
    out.net_exposure = Notional{longs - shorts};
    out.max_concentration = max_weight;

    // Zero equity yields zero leverage, not infinity. An inf here would
    // propagate into every risk report at once.
    if (equity > 0.0 && is_finite(equity)) {
        out.gross_leverage = out.gross_exposure.get() / equity;
        out.net_leverage = out.net_exposure.get() / equity;
    }
    return out;
}

Result<bool> ExposureTracker::record(const ExposureSnapshot& snapshot) {
    if (!history_.empty() && snapshot.ts < history_.back().ts) {
        return fail(bad("exposure history must be non-decreasing in time"));
    }
    history_.push_back(snapshot);
    return true;
}

double ExposureTracker::peak_gross_leverage() const noexcept {
    double peak = 0.0;
    for (const auto& s : history_) peak = std::max(peak, s.gross_leverage);
    return peak;
}

double ExposureTracker::average_gross_leverage() const noexcept {
    if (history_.empty()) return 0.0;
    double sum = 0.0;
    for (const auto& s : history_) sum += s.gross_leverage;
    return sum / static_cast<double>(history_.size());
}

void ExposureTracker::reset() noexcept {
    history_.clear();
}

// ---------------------------------------------------------------------------
// Turnover and capacity
// ---------------------------------------------------------------------------

std::string TurnoverStatistics::describe() const {
    std::ostringstream ss;
    ss.precision(4);
    ss << std::fixed;
    ss << "turnover\n";
    ss << "  total              " << total_turnover.get() << '\n';
    ss << "  average daily      " << average_daily_turnover.get() << '\n';
    ss << "  annualised         " << annualized_turnover << "x equity\n";
    ss << "  implied holding    " << implied_holding_period_days << " days\n";
    return ss.str();
}

Result<TurnoverStatistics> compute_turnover(std::span<const portfolio::EquityPoint> curve,
                                            double periods_per_year) {
    if (curve.size() < 2) {
        return fail(bad("turnover needs at least two equity observations"));
    }
    if (!(periods_per_year > 0.0)) {
        return fail(bad("periods per year must be positive"));
    }

    TurnoverStatistics out;
    // Turnover is CUMULATIVE on the curve, so the total is the difference
    // between endpoints rather than a sum of the column.
    out.total_turnover = curve.back().turnover - curve.front().turnover;

    const double initial_equity = curve.front().equity.get();
    const auto periods = static_cast<double>(curve.size() - 1);

    const Duration elapsed = curve.back().ts - curve.front().ts;
    const double years = static_cast<double>(elapsed.count()) / (365.25 * 24 * 3600 * 1e9);

    if (periods > 0.0) {
        out.average_daily_turnover = Notional{out.total_turnover.get() / periods};
    }
    if (years > 0.0 && initial_equity > 0.0) {
        // Elapsed time drives the annualisation, not the observation count: a
        // curve with gaps would otherwise report a turnover rate it never
        // achieved.
        out.annualized_turnover = out.total_turnover.get() / initial_equity / years;
    }
    if (out.annualized_turnover > 0.0) {
        out.implied_holding_period_days = 365.25 / out.annualized_turnover;
    }
    return out;
}

std::string CapacityEstimate::describe() const {
    std::ostringstream ss;
    ss.precision(2);
    ss << std::fixed;
    ss << "capacity estimate\n";
    ss << "  participation limit  " << participation_limit << '\n';
    ss << "  median daily volume  " << median_daily_dollar_volume.get() << '\n';
    ss << "  annualised turnover  " << annualized_turnover << '\n';
    ss << "  implied capacity     " << implied_capacity.get() << '\n';
    ss << "  " << caveat << '\n';
    return ss.str();
}

Result<CapacityEstimate> estimate_capacity(Notional median_daily_dollar_volume,
                                           double annualized_turnover, double participation_limit) {
    if (median_daily_dollar_volume.get() <= 0.0) {
        return fail(bad("capacity needs a positive median daily dollar volume"));
    }
    if (!(participation_limit > 0.0) || participation_limit > 1.0) {
        return fail(bad("participation limit must lie in (0, 1]"));
    }
    if (annualized_turnover <= 0.0) {
        return fail(bad("capacity needs a positive annualised turnover"));
    }

    CapacityEstimate out;
    out.participation_limit = participation_limit;
    out.median_daily_dollar_volume = median_daily_dollar_volume;
    out.annualized_turnover = annualized_turnover;

    // Daily tradeable notional, divided by the daily turnover rate, gives the
    // book size that could be traded within the participation limit.
    const double daily_capacity = median_daily_dollar_volume.get() * participation_limit;
    const double daily_turnover_rate = annualized_turnover / 252.0;
    out.implied_capacity =
        Notional{daily_turnover_rate > 0.0 ? daily_capacity / daily_turnover_rate : 0.0};

    // Stated plainly on every estimate. A capacity figure without its
    // assumptions is a number people quote back without them.
    out.caveat =
        "CRUDE ESTIMATE: bounds capacity by a participation assumption only. It does not "
        "model impact decay, competition for the same liquidity, or the fact that a larger "
        "book changes the prices it trades at. Treat as an order of magnitude, not a limit.";
    return out;
}

}  // namespace ptl::analytics
