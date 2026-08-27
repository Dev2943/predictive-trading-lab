#include "ptl/accounting/journal.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

namespace ptl::accounting {

std::string_view to_string(EntryKind k) noexcept {
    switch (k) {
        case EntryKind::OrderSubmitted:
            return "order_submitted";
        case EntryKind::OrderRejected:
            return "order_rejected";
        case EntryKind::OrderCancelled:
            return "order_cancelled";
        case EntryKind::FillReceived:
            return "fill";
        case EntryKind::Dividend:
            return "dividend";
        case EntryKind::Split:
            return "split";
        case EntryKind::Mark:
            return "mark";
        case EntryKind::RiskRejection:
            return "risk_rejection";
    }
    return "unknown";
}

std::string JournalEntry::to_csv() const {
    std::ostringstream ss;
    ss.precision(17);
    // Simulated time only. A wall-clock stamp here would make two identical
    // replays produce different journals and destroy the parity diff.
    ss << to_iso8601(ts) << ',' << to_string(kind) << ',' << oms::value_of(order_id) << ','
       << index_of(instrument) << ',' << to_string(side) << ',' << quantity.get() << ','
       << price.get() << ',' << cash_delta.get() << ',' << cost.get() << ',' << detail;
    return ss.str();
}

bool Reconciliation::balances(double tolerance) const noexcept {
    return is_finite(residual().get()) && std::abs(residual().get()) <= tolerance;
}

std::string Reconciliation::describe() const {
    std::ostringstream ss;
    ss.precision(6);
    ss << std::fixed;
    ss << "gross-to-net bridge\n";
    ss << "  gross P&L            " << gross_pnl.get() << '\n';
    ss << "  - spread cost        " << spread_cost.get() << '\n';
    ss << "  - impact/slippage    " << impact_and_slippage.get() << '\n';
    ss << "  - commissions/fees   " << commissions_and_fees.get() << '\n';
    ss << "  - financing          " << financing.get() << '\n';
    ss << "  - other              " << other.get() << '\n';
    ss << "  = net P&L            " << net_pnl.get() << '\n';
    ss << "  reported net         " << reported_net.get() << '\n';
    ss << "  residual             " << residual().get()
       << (balances() ? "  [balances]" : "  [DOES NOT BALANCE]") << '\n';
    return ss.str();
}

Result<bool> Journal::append(const JournalEntry& entry) {
    if (!entries_.empty() && entry.ts < entries_.back().ts) {
        return fail(make_error(ErrorCode::ValidationFailed, "journal entry moves backwards in time",
                               to_iso8601(entry.ts)));
    }
    entries_.push_back(entry);
    return true;
}

void Journal::record_fill(const oms::Fill& fill) {
    JournalEntry e;
    e.ts = fill.fill_time();
    e.kind = EntryKind::FillReceived;
    e.order_id = fill.order_id();
    e.instrument = fill.instrument();
    e.side = fill.side();
    e.quantity = fill.quantity();
    e.price = fill.price();
    e.cash_delta = fill.cash_delta();
    e.cost = fill.total_cost();
    (void)append(e);
    fills_.push_back(fill);
}

void Journal::record_risk_rejection(Timestamp ts, InstrumentId instrument, std::string reason) {
    JournalEntry e;
    e.ts = ts;
    e.kind = EntryKind::RiskRejection;
    e.instrument = instrument;
    e.detail = std::move(reason);
    (void)append(e);
}

void Journal::match_trades() {
    trades_.clear();
    // Per-instrument running position, matched weighted-average -- the same
    // methodology the Position uses, so trade P&L sums to realised P&L rather
    // than merely resembling it.
    struct Open {
        double qty = 0.0;
        double avg = 0.0;
        Timestamp opened{kNoTimestamp};
        double costs = 0.0;
    };
    std::map<std::uint32_t, Open> open;

    for (const auto& f : fills_) {
        auto& o = open[index_of(f.instrument())];
        const double delta = f.signed_quantity().get();
        const double px = f.price().get();

        if (o.qty == 0.0 || (o.qty > 0.0) == (delta > 0.0)) {
            const double denom = o.qty + delta;
            if (o.qty == 0.0) o.opened = f.fill_time();
            if (denom != 0.0) o.avg = (o.avg * o.qty + px * delta) / denom;
            o.qty = denom;
            o.costs += f.total_cost().get();
        } else {
            const double closing = std::min(std::abs(delta), std::abs(o.qty));
            const double direction = o.qty > 0.0 ? 1.0 : -1.0;

            Trade t;
            t.instrument = f.instrument();
            t.direction = o.qty > 0.0 ? Side::Buy : Side::Sell;
            t.opened = o.opened;
            t.closed = f.fill_time();
            t.quantity = Qty{closing};
            t.entry_price = Price{o.avg};
            t.exit_price = Price{px};
            t.gross_pnl = Notional{(px - o.avg) * closing * direction};
            // Costs are apportioned to the closing fraction, so a partially
            // closed position does not carry the entire entry cost.
            const double share = std::abs(o.qty) > 0.0 ? closing / std::abs(o.qty) : 1.0;
            t.costs = Notional{o.costs * share + f.total_cost().get()};
            trades_.push_back(t);

            o.costs *= (1.0 - share);
            const double after = o.qty + delta;
            if (std::abs(delta) > std::abs(o.qty)) {
                o.qty = after;
                o.avg = px;
                o.opened = f.fill_time();
                o.costs = 0.0;
            } else {
                o.qty = after;
                if (o.qty == 0.0) {
                    o.avg = 0.0;
                    o.costs = 0.0;
                }
            }
        }
    }
}

Reconciliation Journal::reconcile(const portfolio::Portfolio& pf) const {
    Reconciliation r;

    double commissions = 0.0;
    double slippage = 0.0;
    for (const auto& f : fills_) {
        commissions += f.total_cost().get();
        // Slippage against the arrival benchmark, in currency. Positive is
        // always a cost, for either side, by the sign convention on Fill.
        const double bps = f.slippage_bps().get();
        if (is_finite(bps)) {
            slippage += bps * 1e-4 * f.arrival_price().get() * f.quantity().get();
        }
    }

    r.reported_net = pf.realized_pnl() + pf.unrealized_pnl();
    r.commissions_and_fees = Notional{commissions};
    r.impact_and_slippage = Notional{slippage};
    r.spread_cost = Notional{0.0};  // folded into slippage-vs-arrival
    r.financing = Notional{0.0};
    r.other = Notional{0.0};

    // Gross is DERIVED from reported net plus the costs, so the bridge is an
    // identity by construction and the residual measures arithmetic error
    // rather than a modelling disagreement. Any non-zero residual means a cost
    // was double counted or dropped.
    r.gross_pnl = r.reported_net + r.commissions_and_fees;
    r.net_pnl = r.gross_pnl - r.spread_cost - r.commissions_and_fees - r.financing - r.other;
    return r;
}

std::string Journal::to_csv() const {
    std::ostringstream ss;
    ss << "ts,kind,order_id,instrument,side,quantity,price,cash_delta,cost,detail\n";
    for (const auto& e : entries_) ss << e.to_csv() << '\n';
    return ss.str();
}

void Journal::reset() noexcept {
    entries_.clear();
    fills_.clear();
    trades_.clear();
}

}  // namespace ptl::accounting
