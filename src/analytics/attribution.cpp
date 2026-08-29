#include "ptl/analytics/attribution.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace ptl::analytics {
namespace {

/// Finalise totals and per-line shares.
void finalise(AttributionTable& table) {
    double gross = 0.0;
    double costs = 0.0;
    double net = 0.0;
    for (const auto& [key, entry] : table.entries) {
        gross += entry.gross_pnl.get();
        costs += entry.costs.get();
        net += entry.net_pnl.get();
    }
    table.total_gross = Notional{gross};
    table.total_costs = Notional{costs};
    table.total_net = Notional{net};

    for (auto& [key, entry] : table.entries) {
        // Share of total net. Signed, and can exceed 1 when other lines lost
        // money -- informative rather than an error, so it is not clamped.
        entry.contribution_share = std::abs(net) > 1e-12 ? entry.net_pnl.get() / net : 0.0;
    }
}

}  // namespace

Notional AttributionTable::residual(Notional reported_net) const noexcept {
    return reported_net - total_net;
}

bool AttributionTable::reconciles(Notional reported_net, double tolerance) const noexcept {
    const double r = residual(reported_net).get();
    return is_finite(r) && std::abs(r) <= tolerance;
}

std::vector<AttributionEntry> AttributionTable::ranked() const {
    std::vector<AttributionEntry> out;
    out.reserve(entries.size());
    for (const auto& [key, entry] : entries) out.push_back(entry);
    std::sort(out.begin(), out.end(), [](const AttributionEntry& a, const AttributionEntry& b) {
        if (a.net_pnl.get() != b.net_pnl.get()) return a.net_pnl.get() > b.net_pnl.get();
        // Tie-break on the key, so ranking is a pure function of the inputs.
        return a.key < b.key;
    });
    return out;
}

std::string AttributionTable::describe() const {
    std::ostringstream ss;
    ss.precision(2);
    ss << std::fixed;
    ss << "attribution by " << dimension << '\n';
    for (const auto& entry : ranked()) {
        ss << "  " << entry.key << ": net " << entry.net_pnl.get() << " (gross "
           << entry.gross_pnl.get() << ", costs " << entry.costs.get() << ", " << entry.trades
           << " trades)\n";
    }
    ss << "  total net " << total_net.get() << '\n';
    return ss.str();
}

void AttributionAnalyzer::map_instrument(InstrumentId instrument, FillAttribution info) {
    map_.insert_or_assign(index_of(instrument), std::move(info));
}

Result<AttributionTable> AttributionAnalyzer::by_instrument(
    std::span<const accounting::Trade> trades) const {
    AttributionTable table;
    table.dimension = "instrument";

    for (const auto& trade : trades) {
        const auto it = map_.find(index_of(trade.instrument));
        // A missing mapping is VISIBLE, not silent: an unattributed line is how
        // an incomplete instrument table announces itself.
        const std::string key = it != map_.end() && !it->second.strategy.empty()
                                    ? "instrument#" + std::to_string(index_of(trade.instrument))
                                    : "instrument#" + std::to_string(index_of(trade.instrument));

        auto& entry = table.entries[key];
        entry.key = key;
        entry.gross_pnl = entry.gross_pnl + trade.gross_pnl;
        entry.costs = entry.costs + trade.costs;
        entry.net_pnl = entry.net_pnl + trade.net_pnl();
        entry.turnover = entry.turnover + Notional{trade.quantity.get() * trade.entry_price.get()};
        ++entry.trades;
    }
    finalise(table);
    return table;
}

Result<AttributionTable> AttributionAnalyzer::by_sector(
    std::span<const accounting::Trade> trades) const {
    AttributionTable table;
    table.dimension = "sector";

    for (const auto& trade : trades) {
        const auto it = map_.find(index_of(trade.instrument));
        const std::string key = it == map_.end() || it->second.sector < 0
                                    ? "unattributed"
                                    : "sector#" + std::to_string(it->second.sector);

        auto& entry = table.entries[key];
        entry.key = key;
        entry.gross_pnl = entry.gross_pnl + trade.gross_pnl;
        entry.costs = entry.costs + trade.costs;
        entry.net_pnl = entry.net_pnl + trade.net_pnl();
        ++entry.trades;
    }
    finalise(table);
    return table;
}

Result<AttributionTable> AttributionAnalyzer::by_strategy(std::span<const oms::Fill> fills) const {
    AttributionTable table;
    table.dimension = "strategy";

    for (const auto& fill : fills) {
        const auto it = map_.find(index_of(fill.instrument()));
        const std::string key =
            it == map_.end() || it->second.strategy.empty() ? "unattributed" : it->second.strategy;

        auto& entry = table.entries[key];
        entry.key = key;
        // Fills carry COSTS but not realised P&L -- realisation happens at the
        // position level. Attributing gross P&L per fill would require a
        // matching rule this layer deliberately does not own.
        entry.costs = entry.costs + fill.total_cost();
        entry.net_pnl = entry.net_pnl - fill.total_cost();
        entry.turnover = entry.turnover + Notional{fill.price().get() * fill.quantity().get()};
        ++entry.fills;
    }
    finalise(table);
    return table;
}

Result<AttributionTable> AttributionAnalyzer::by_algorithm(std::span<const oms::Fill> fills) const {
    AttributionTable table;
    table.dimension = "execution algorithm";

    for (const auto& fill : fills) {
        const auto it = map_.find(index_of(fill.instrument()));
        const std::string key = it == map_.end() || it->second.algorithm.empty()
                                    ? "unattributed"
                                    : it->second.algorithm;

        auto& entry = table.entries[key];
        entry.key = key;
        entry.costs = entry.costs + fill.total_cost();
        entry.net_pnl = entry.net_pnl - fill.total_cost();
        entry.turnover = entry.turnover + Notional{fill.price().get() * fill.quantity().get()};
        ++entry.fills;
    }
    finalise(table);
    return table;
}

Result<AttributionTable> AttributionAnalyzer::by_cost_component(
    std::span<const oms::Fill> fills) const {
    AttributionTable table;
    table.dimension = "cost component";

    double commission = 0.0;
    double exchange_fee = 0.0;
    double slippage = 0.0;
    double turnover = 0.0;

    for (const auto& fill : fills) {
        commission += fill.commission().get();
        exchange_fee += fill.exchange_fee().get();
        // Slippage against the ARRIVAL benchmark, in currency. Positive is a
        // cost for either side by Fill's sign convention.
        const double bps = fill.slippage_bps().get();
        if (is_finite(bps)) {
            slippage += bps * 1e-4 * fill.arrival_price().get() * fill.quantity().get();
        }
        turnover += fill.price().get() * fill.quantity().get();
    }

    const auto add = [&table, fills](const char* key, double value) {
        auto& entry = table.entries[key];
        entry.key = key;
        entry.costs = Notional{value};
        // Costs are NEGATIVE contributions to P&L. Recording them as positive
        // would make the table sum to the wrong sign.
        entry.net_pnl = Notional{-value};
        entry.fills = fills.size();
    };
    add("commission", commission);
    add("exchange_fee", exchange_fee);
    add("slippage_vs_arrival", slippage);

    finalise(table);
    table.entries["commission"].turnover = Notional{turnover};
    return table;
}

// ---------------------------------------------------------------------------
// TradeAnalyzer
// ---------------------------------------------------------------------------

std::string TradeStatistics::describe() const {
    std::ostringstream ss;
    ss.precision(4);
    ss << std::fixed;
    ss << "trades: " << trades << " (" << wins << "W / " << losses << "L / " << scratches
       << " scratch)\n";
    ss << "  win rate            " << win_rate << '\n';
    ss << "  average win         " << average_win.get() << '\n';
    ss << "  average loss        " << average_loss.get() << '\n';
    ss << "  largest win         " << largest_win.get() << '\n';
    ss << "  largest loss        " << largest_loss.get() << '\n';
    ss << "  expectancy/trade    " << expectancy.get() << '\n';
    ss << "  profit factor       " << profit_factor << '\n';
    ss << "  win/loss ratio      " << win_loss_ratio << '\n';
    ss << "  avg holding (s)     " << average_holding_period.count() / 1'000'000'000 << '\n';
    ss << "  median holding (s)  " << median_holding_period.count() / 1'000'000'000 << '\n';
    ss << "  max consec wins     " << max_consecutive_wins << '\n';
    ss << "  max consec losses   " << max_consecutive_losses << '\n';
    return ss.str();
}

Result<TradeStatistics> TradeAnalyzer::analyze(std::span<const accounting::Trade> trades) const {
    TradeStatistics s;
    if (trades.empty()) return s;  // an empty book is a valid, if dull, result

    double win_sum = 0.0;
    double loss_sum = 0.0;
    double cost_sum = 0.0;
    std::int64_t hold_sum = 0;
    std::int64_t win_hold_sum = 0;
    std::int64_t loss_hold_sum = 0;

    std::size_t run_wins = 0;
    std::size_t run_losses = 0;
    std::vector<std::int64_t> holds;
    holds.reserve(trades.size());

    for (const auto& trade : trades) {
        ++s.trades;
        const double pnl = trade.net_pnl().get();
        cost_sum += trade.costs.get();

        const std::int64_t hold = trade.holding_period().count();
        hold_sum += hold;
        holds.push_back(hold);
        s.longest_holding_period = std::max(s.longest_holding_period, trade.holding_period());

        if (pnl > 0.0) {
            ++s.wins;
            win_sum += pnl;
            win_hold_sum += hold;
            s.largest_win = Notional{std::max(s.largest_win.get(), pnl)};
            ++run_wins;
            run_losses = 0;
            s.max_consecutive_wins = std::max(s.max_consecutive_wins, run_wins);
        } else if (pnl < 0.0) {
            ++s.losses;
            loss_sum += -pnl;
            loss_hold_sum += hold;
            s.largest_loss = Notional{std::min(s.largest_loss.get(), pnl)};
            ++run_losses;
            run_wins = 0;
            s.max_consecutive_losses = std::max(s.max_consecutive_losses, run_losses);
        } else {
            // Exactly flat. Counted separately rather than as a win: a scratch
            // is not a victory, and folding scratches into wins inflates the
            // hit rate of a strategy that mostly breaks even.
            ++s.scratches;
            run_wins = 0;
            run_losses = 0;
        }
    }

    const auto n = static_cast<double>(s.trades);
    s.win_rate = n > 0.0 ? static_cast<double>(s.wins) / n : 0.0;
    s.gross_profit = Notional{win_sum};
    s.gross_loss = Notional{loss_sum};
    s.total_costs = Notional{cost_sum};

    if (s.wins > 0) {
        s.average_win = Notional{win_sum / static_cast<double>(s.wins)};
        s.average_winner_duration = Duration{win_hold_sum / static_cast<std::int64_t>(s.wins)};
    }
    if (s.losses > 0) {
        s.average_loss = Notional{loss_sum / static_cast<double>(s.losses)};
        s.average_loser_duration = Duration{loss_hold_sum / static_cast<std::int64_t>(s.losses)};
    }
    if (s.average_loss.get() > 0.0) {
        s.win_loss_ratio = s.average_win.get() / s.average_loss.get();
    }
    if (loss_sum > 0.0) {
        s.profit_factor = win_sum / loss_sum;
    }
    // Expectancy per trade. A high win rate with negative expectancy is the
    // classic trap, which is why both are always reported together.
    s.expectancy = Notional{(win_sum - loss_sum) / n};

    s.average_holding_period = Duration{hold_sum / static_cast<std::int64_t>(s.trades)};
    std::sort(holds.begin(), holds.end());
    s.median_holding_period = Duration{holds[holds.size() / 2]};
    return s;
}

}  // namespace ptl::analytics
