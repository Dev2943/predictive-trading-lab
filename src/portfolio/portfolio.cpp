#include "ptl/portfolio/portfolio.hpp"

#include <algorithm>
#include <cmath>

namespace ptl::portfolio {
namespace {

[[nodiscard]] Error bad(std::string message) {
    return make_error(ErrorCode::ValidationFailed, std::move(message));
}

}  // namespace

Portfolio::Portfolio(PortfolioConfig cfg) : cfg_(cfg), cash_(cfg.initial_cash) {}

Result<bool> Portfolio::apply(const oms::Fill& fill) {
    if (!is_finite(fill.price().get()) || !is_finite(fill.quantity().get())) {
        return fail(bad("fill carries a non-finite price or quantity"));
    }
    auto& pos = positions_.try_emplace(index_of(fill.instrument()), Position{fill.instrument()})
                    .first->second;

    if (!cfg_.allow_short) {
        const double after = pos.quantity().get() + fill.signed_quantity().get();
        if (after < -1e-9) {
            return fail(bad("short selling is disabled for this portfolio"));
        }
    }

    const Notional realized_now = pos.apply(fill);
    realized_ = realized_ + realized_now;

    // Cash and costs move together, and the cost is charged on the FILLED
    // quantity only -- never the requested quantity, which is the classic
    // double-count.
    cash_ = cash_ + fill.cash_delta();
    costs_ = costs_ + fill.total_cost();
    turnover_ = turnover_ + Notional{std::abs(fill.price().get() * fill.quantity().get())};

    if (!is_finite(cash_.get())) return fail(bad("cash became non-finite"));
    return true;
}

Result<bool> Portfolio::apply_split(InstrumentId instrument, double ratio) {
    if (!is_finite(ratio) || ratio <= 0.0) return fail(bad("split ratio must be positive"));
    const auto it = positions_.find(index_of(instrument));
    if (it != positions_.end()) it->second.apply_split(ratio);
    // Marks are per-share and must scale too, or the position would be marked
    // at the pre-split price and the equity curve would show a fictional jump.
    const auto m = marks_.find(index_of(instrument));
    if (m != marks_.end()) {
        m->second.bid = Price{m->second.bid.get() / ratio};
        m->second.ask = Price{m->second.ask.get() / ratio};
    }
    return true;
}

Result<bool> Portfolio::apply_dividend(InstrumentId instrument, Notional per_share) {
    const auto it = positions_.find(index_of(instrument));
    if (it == positions_.end()) return true;  // nothing held; nothing to credit
    const Notional cash_leg = it->second.dividend_cash(per_share);
    cash_ = cash_ + cash_leg;
    return true;
}

void Portfolio::mark(InstrumentId instrument, Price bid, Price ask) noexcept {
    marks_[index_of(instrument)] = MarkPair{bid, ask};
}

void Portfolio::mark_last(InstrumentId instrument, Price last) noexcept {
    // Bar-only fallback: no spread information exists, so bid == ask == last.
    // Any liquidation haircut is therefore zero here, and the run manifest
    // records that marks came from bars rather than quotes.
    marks_[index_of(instrument)] = MarkPair{last, last};
}

void Portfolio::mark_from_quote(const market::Quote& q) noexcept {
    mark(q.instrument(), q.bid(), q.ask());
}

Price Portfolio::mark_for(InstrumentId instrument, const Position& pos) const noexcept {
    const auto it = marks_.find(index_of(instrument));
    if (it == marks_.end()) return pos.average_cost();

    switch (cfg_.mark_mode) {
        case MarkMode::Mid:
            return Price{(it->second.bid.get() + it->second.ask.get()) * 0.5};
        case MarkMode::Last:
            return it->second.bid;
        case MarkMode::Liquidation:
            // Longs to bid, shorts to ask: what the position would actually
            // fetch if liquidated now. Mid-marking overstates NAV by half a
            // spread per unit of gross exposure on every bar.
            return pos.is_short() ? it->second.ask : it->second.bid;
    }
    return it->second.bid;
}

Notional Portfolio::position_value() const noexcept {
    double total = 0.0;
    for (const auto& [key, pos] : positions_) {
        if (pos.is_flat()) continue;
        total += pos.market_value(mark_for(pos.instrument(), pos)).get();
    }
    return Notional{total};
}

Notional Portfolio::equity() const noexcept {
    return cash_ + position_value();
}

Notional Portfolio::gross_exposure() const noexcept {
    double total = 0.0;
    for (const auto& [key, pos] : positions_) {
        if (pos.is_flat()) continue;
        total += pos.gross_exposure(mark_for(pos.instrument(), pos)).get();
    }
    return Notional{total};
}

Notional Portfolio::net_exposure() const noexcept {
    return position_value();
}

Notional Portfolio::unrealized_pnl() const noexcept {
    double total = 0.0;
    for (const auto& [key, pos] : positions_) {
        if (pos.is_flat()) continue;
        total += pos.unrealized_pnl(mark_for(pos.instrument(), pos)).get();
    }
    return Notional{total};
}

double Portfolio::leverage() const noexcept {
    const double eq = equity().get();
    // Zero equity yields zero, not infinity. An inf leverage would poison every
    // risk check downstream and the failure would surface far from its cause.
    if (!is_finite(eq) || eq == 0.0) return 0.0;
    return gross_exposure().get() / eq;
}

Notional Portfolio::buying_power() const noexcept {
    return Notional{std::max(0.0, cash_.get())};
}

const Position* Portfolio::position(InstrumentId instrument) const noexcept {
    const auto it = positions_.find(index_of(instrument));
    return it == positions_.end() ? nullptr : &it->second;
}

std::optional<Price> Portfolio::mark_price(InstrumentId instrument) const noexcept {
    const auto it = marks_.find(index_of(instrument));
    if (it == marks_.end()) return std::nullopt;
    const auto p = positions_.find(index_of(instrument));
    if (p == positions_.end()) return it->second.bid;
    return mark_for(instrument, p->second);
}

bool Portfolio::identity_holds(double tolerance) const noexcept {
    const double lhs = equity().get();
    const double rhs = cash_.get() + position_value().get();
    return is_finite(lhs) && is_finite(rhs) && std::abs(lhs - rhs) <= tolerance;
}

Result<EquityPoint> Portfolio::snapshot(Timestamp ts) {
    if (!curve_.empty() && ts < curve_.back().ts) {
        // A backwards snapshot would corrupt every time-ordered metric computed
        // from the curve.
        return fail(bad("equity curve snapshot moves backwards in time"));
    }
    EquityPoint p;
    p.ts = ts;
    p.cash = cash_;
    p.equity = equity();
    p.gross_exposure = gross_exposure();
    p.net_exposure = net_exposure();
    p.realized_pnl = realized_;
    p.unrealized_pnl = unrealized_pnl();
    p.cumulative_costs = costs_;
    p.turnover = turnover_;

    if (!is_finite(p.equity.get())) {
        // One non-finite value makes an entire Sharpe NaN, hundreds of lines
        // from its cause. Catching it at the snapshot is the cheapest place.
        return fail(bad("equity is not finite at " + to_iso8601(ts)));
    }
    if (!identity_holds()) {
        return fail(bad("accounting identity violated: equity != cash + position value"));
    }
    curve_.push_back(p);
    return p;
}

}  // namespace ptl::portfolio
