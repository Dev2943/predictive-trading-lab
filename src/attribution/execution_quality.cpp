#include "ptl/attribution/execution_quality.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace ptl::attribution {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::InvalidArgument, std::move(message), std::move(context));
}

}  // namespace

Bps ExecutionQualityAnalyzer::cost_bps(Price fill, Price benchmark, Side side) noexcept {
    if (benchmark.get() <= 0.0 || !is_finite(fill.get()) || !is_finite(benchmark.get())) {
        return Bps{0.0};
    }
    // POSITIVE IS ALWAYS A COST. A buy filling above its benchmark and a sell
    // filling below both hurt; multiplying by the side's sign makes one
    // convention serve both and spares every aggregate a special case.
    const double raw = (fill.get() / benchmark.get() - 1.0) * 1e4;
    const double signed_cost = raw * static_cast<double>(sign_of(side));
    return Bps{is_finite(signed_cost) ? signed_cost : 0.0};
}

std::string TradeExecutionQuality::describe() const {
    std::ostringstream ss;
    ss.precision(2);
    ss << std::fixed << "instrument#" << index_of(instrument) << ' ' << to_string(side) << ' '
       << quantity.get() << " @ " << average_fill_price.get();
    if (implementation_shortfall.has_value()) {
        ss << ", IS " << implementation_shortfall->get() << " bps";
    }
    if (participation_rate.has_value()) {
        ss << ", participation " << *participation_rate;
    }
    ss << ", fill efficiency " << fill_efficiency;
    return ss.str();
}

std::string ExecutionQualitySummary::describe() const {
    std::ostringstream ss;
    ss.precision(3);
    ss << std::fixed;
    ss << "execution quality over " << trades << " trades (" << fills << " fills)\n";
    ss << "  implementation shortfall  " << average_implementation_shortfall.get()
       << " bps (median " << median_implementation_shortfall.get() << ", worst "
       << worst_implementation_shortfall.get() << ")\n";
    ss << "  delay cost                " << average_delay_cost.get() << " bps\n";
    ss << "  execution cost            " << average_execution_cost.get() << " bps\n";
    ss << "  versus VWAP               " << average_vs_vwap.get() << " bps\n";
    ss << "  participation rate        " << average_participation_rate << '\n';
    ss << "  fill efficiency           " << average_fill_efficiency << '\n';
    ss << "  below expected edge       " << trades_below_expected_edge << '\n';
    return ss.str();
}

std::string SignalDecayProfile::describe() const {
    std::ostringstream ss;
    ss.precision(3);
    ss << std::fixed << "signal decay\n";
    for (std::size_t i = 0; i < horizons.size() && i < mean_edge_bps.size(); ++i) {
        ss << "  <= " << horizons[i].count() / 1'000'000'000 << "s: " << mean_edge_bps[i]
           << " bps (" << sample_counts[i] << " trades)\n";
    }
    if (half_life.has_value()) {
        ss << "  half-life " << half_life->count() / 1'000'000'000 << "s\n";
    } else {
        // Worth stating: a profile that never halves within the observed range
        // means the window was too short to find the decay, not that there is
        // none.
        ss << "  half-life not reached within the observed horizons\n";
    }
    return ss.str();
}

Result<TradeExecutionQuality> ExecutionQualityAnalyzer::analyze_trade(
    std::span<const oms::Fill> fills, const ExecutionBenchmarks& benchmarks,
    Qty quantity_sought) const {
    if (fills.empty()) return fail(bad("cannot analyse a trade with no fills"));

    TradeExecutionQuality out;
    out.instrument = fills.front().instrument();
    out.side = fills.front().side();
    out.fill_count = fills.size();

    double filled = 0.0;
    double notional = 0.0;
    double commission = 0.0;
    double fees = 0.0;
    Timestamp first = kMaxTimestamp;
    Timestamp last = kNoTimestamp;

    for (const auto& fill : fills) {
        if (fill.instrument() != out.instrument) {
            // Mixing instruments in one trade would produce an average price
            // with no meaning.
            return fail(bad("fills for one trade span more than one instrument"));
        }
        if (fill.side() != out.side) {
            return fail(bad("fills for one trade span both sides"));
        }
        const double q = fill.quantity().get();
        filled += q;
        notional += q * fill.price().get();
        commission += fill.commission().get();
        fees += fill.exchange_fee().get();
        first = std::min(first, fill.fill_time());
        last = std::max(last, fill.fill_time());
    }

    if (filled <= 0.0) return fail(bad("trade has no filled quantity"));

    out.quantity = Qty{filled};
    out.average_fill_price = Price{notional / filled};
    out.first_fill_time = first;
    out.last_fill_time = last;
    out.commission = Notional{commission};
    out.fees = Notional{fees};
    // The decision instant comes from the fill's LIFECYCLE CHAIN, not from a
    // field on the fill itself. Fill carries the full seven-stage chain from
    // Phase 1 precisely so a question like this can be answered without the
    // caller having to remember what it asked for.
    out.decision_time = fills.front().times().decision_time;

    if (is_set(out.decision_time) && is_set(last)) {
        out.holding_period = last - out.decision_time;
    }

    // Filled over sought. Below one means the algorithm ran out of window or
    // liquidity, which is a distinct failure from filling badly.
    if (quantity_sought.get() > 0.0) {
        out.fill_efficiency = std::min(1.0, filled / quantity_sought.get());
    }

    // --- the decomposition that matters ------------------------------------
    // Delay cost belongs to the STRATEGY, execution cost to the ALGORITHM.
    // Reporting only the total makes it impossible to know which to fix.
    if (benchmarks.decision_price.has_value() && benchmarks.arrival_price.has_value()) {
        out.delay_cost = cost_bps(*benchmarks.arrival_price, *benchmarks.decision_price, out.side);
    }
    if (benchmarks.arrival_price.has_value()) {
        out.execution_cost = cost_bps(out.average_fill_price, *benchmarks.arrival_price, out.side);
    }
    if (benchmarks.decision_price.has_value()) {
        out.implementation_shortfall =
            cost_bps(out.average_fill_price, *benchmarks.decision_price, out.side);
    }
    if (benchmarks.interval_vwap.has_value()) {
        out.vs_vwap = cost_bps(out.average_fill_price, *benchmarks.interval_vwap, out.side);
    }
    if (benchmarks.interval_twap.has_value()) {
        out.vs_twap = cost_bps(out.average_fill_price, *benchmarks.interval_twap, out.side);
    }

    if (benchmarks.interval_volume.has_value() && benchmarks.interval_volume->get() > 0.0) {
        out.participation_rate = filled / benchmarks.interval_volume->get();
    }

    // --- excursions ---------------------------------------------------------
    // Measured from the AVERAGE FILL, not the decision price: excursion is
    // about the position actually held, and a position not yet filled cannot
    // have suffered from a move.
    const Price entry = out.average_fill_price;
    // SIGN CARE. cost_bps measures the cost of FILLING at a price. For a
    // position already HELD the same arithmetic reads the other way: a long
    // that fills at 100 and sees 110 has gained, and cost_bps(110, 100, Buy)
    // is +1000 -- which is favourable here, not a cost. A short works out the
    // same way because cost_bps already carries the side's sign, so one
    // expression serves both directions.
    if (benchmarks.best_price.has_value() && entry.get() > 0.0) {
        const double bps = cost_bps(*benchmarks.best_price, entry, out.side).get();
        out.max_favorable_excursion = Bps{std::max(0.0, bps)};
    }
    if (benchmarks.worst_price.has_value() && entry.get() > 0.0) {
        const double bps = -cost_bps(*benchmarks.worst_price, entry, out.side).get();
        out.max_adverse_excursion = Bps{std::max(0.0, bps)};
    }
    return out;
}

Result<ExecutionQualitySummary> ExecutionQualityAnalyzer::summarize(
    std::span<const TradeExecutionQuality> trades) const {
    ExecutionQualitySummary out;
    if (trades.empty()) return out;  // an empty book is a valid, if dull, result

    double weight_total = 0.0;
    double is_weighted = 0.0;
    double delay_weighted = 0.0;
    double execution_weighted = 0.0;
    double vwap_weighted = 0.0;
    double vwap_weight = 0.0;
    double delay_weight = 0.0;
    double execution_weight = 0.0;

    double participation_sum = 0.0;
    std::size_t participation_count = 0;
    double efficiency_sum = 0.0;
    std::int64_t holding_sum = 0;

    std::vector<double> shortfalls;
    shortfalls.reserve(trades.size());

    for (const auto& trade : trades) {
        ++out.trades;
        out.fills += trade.fill_count;
        out.total_commission = out.total_commission + trade.commission;
        out.total_fees = out.total_fees + trade.fees;

        // QUANTITY-WEIGHTED. A hundred-share trade and a hundred-thousand-share
        // trade do not deserve equal say in an average execution cost.
        const double w = trade.quantity.get();
        weight_total += w;

        if (trade.implementation_shortfall.has_value()) {
            is_weighted += trade.implementation_shortfall->get() * w;
            shortfalls.push_back(trade.implementation_shortfall->get());
        }
        if (trade.delay_cost.has_value()) {
            delay_weighted += trade.delay_cost->get() * w;
            delay_weight += w;
        }
        if (trade.execution_cost.has_value()) {
            execution_weighted += trade.execution_cost->get() * w;
            execution_weight += w;
        }
        if (trade.vs_vwap.has_value()) {
            vwap_weighted += trade.vs_vwap->get() * w;
            vwap_weight += w;
        }
        if (trade.participation_rate.has_value()) {
            participation_sum += *trade.participation_rate;
            ++participation_count;
        }
        efficiency_sum += trade.fill_efficiency;
        holding_sum += trade.holding_period.count();

        if (trade.realized_edge.has_value() && trade.expected_edge.has_value() &&
            trade.realized_edge->get() < trade.expected_edge->get()) {
            ++out.trades_below_expected_edge;
        }
    }

    if (weight_total > 0.0) {
        out.average_implementation_shortfall = Bps{is_weighted / weight_total};
    }
    if (delay_weight > 0.0) out.average_delay_cost = Bps{delay_weighted / delay_weight};
    if (execution_weight > 0.0) {
        out.average_execution_cost = Bps{execution_weighted / execution_weight};
    }
    if (vwap_weight > 0.0) out.average_vs_vwap = Bps{vwap_weighted / vwap_weight};

    if (participation_count > 0) {
        out.average_participation_rate =
            participation_sum / static_cast<double>(participation_count);
    }
    out.average_fill_efficiency = efficiency_sum / static_cast<double>(out.trades);
    out.average_holding_period = Duration{holding_sum / static_cast<std::int64_t>(out.trades)};

    if (!shortfalls.empty()) {
        // Copies before sorting: the caller's data is not ours to reorder.
        std::sort(shortfalls.begin(), shortfalls.end());
        out.median_implementation_shortfall = Bps{shortfalls[shortfalls.size() / 2]};
        out.worst_implementation_shortfall = Bps{shortfalls.back()};
    }
    return out;
}

Result<SignalDecayProfile> ExecutionQualityAnalyzer::signal_decay(
    std::span<const TradeExecutionQuality> trades, std::span<const Duration> horizons) const {
    if (horizons.empty()) return fail(bad("signal decay needs at least one horizon"));
    for (std::size_t i = 1; i < horizons.size(); ++i) {
        if (horizons[i] <= horizons[i - 1]) {
            return fail(bad("signal decay horizons must be strictly ascending"));
        }
    }

    SignalDecayProfile out;
    out.horizons.assign(horizons.begin(), horizons.end());
    out.mean_edge_bps.assign(horizons.size(), 0.0);
    out.sample_counts.assign(horizons.size(), 0);

    std::vector<double> sums(horizons.size(), 0.0);

    for (const auto& trade : trades) {
        if (!trade.realized_edge.has_value()) continue;
        // Bucket by DELAY, not holding period: the question is how much edge
        // survives the wait between decision and execution.
        const Duration delay = is_set(trade.decision_time) && is_set(trade.first_fill_time)
                                   ? trade.first_fill_time - trade.decision_time
                                   : Duration::zero();

        for (std::size_t i = 0; i < horizons.size(); ++i) {
            if (delay <= horizons[i]) {
                sums[i] += trade.realized_edge->get();
                ++out.sample_counts[i];
                break;
            }
        }
    }

    for (std::size_t i = 0; i < horizons.size(); ++i) {
        if (out.sample_counts[i] > 0) {
            out.mean_edge_bps[i] = sums[i] / static_cast<double>(out.sample_counts[i]);
        }
    }

    // Half-life: the first horizon whose mean edge has fallen below half the
    // first bucket's. Only meaningful when the first bucket has a positive edge
    // to decay from.
    if (!out.mean_edge_bps.empty() && out.mean_edge_bps.front() > 0.0 &&
        out.sample_counts.front() > 0) {
        const double half = out.mean_edge_bps.front() * 0.5;
        for (std::size_t i = 1; i < out.mean_edge_bps.size(); ++i) {
            if (out.sample_counts[i] == 0) continue;
            if (out.mean_edge_bps[i] < half) {
                out.half_life = horizons[i];
                break;
            }
        }
    }
    return out;
}

}  // namespace ptl::attribution
