#include "ptl/paper/account.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace ptl::paper {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

/// ROUND-TRIP EXACT, not display-formatted.
///
/// AccountSnapshot::to_json is a STATE SERIALIZER: the session checksum covers
/// these fields' raw bits, so two-decimal currency formatting would make a
/// snapshot fail to verify against its own output. Display formatting belongs
/// in the reporting layer, which has its own precision setting for that.
[[nodiscard]] std::string num(double v) {
    if (!is_finite(v)) return "null";
    std::ostringstream ss;
    ss << std::setprecision(std::numeric_limits<double>::max_digits10) << v;
    return ss.str();
}

}  // namespace

std::string_view to_string(AccountStatus s) noexcept {
    switch (s) {
        case AccountStatus::Active:
            return "active";
        case AccountStatus::MarginCall:
            return "margin_call";
        case AccountStatus::Liquidation:
            return "liquidation";
        case AccountStatus::Suspended:
            return "suspended";
    }
    return "unknown";
}

Notional PaperAccount::maintenance_requirement() const noexcept {
    // Against GROSS exposure, not net. A market-neutral book still requires
    // margin on both legs; netting them would understate the requirement to
    // zero for a pair trade that can very much lose money.
    const double gross = portfolio_->gross_exposure().get();
    if (!is_finite(gross)) return Notional{0.0};
    return Notional{gross * config_.maintenance_margin};
}

Notional PaperAccount::margin_excess() const noexcept {
    const double equity = portfolio_->equity().get();
    const double requirement = maintenance_requirement().get();
    if (!is_finite(equity) || !is_finite(requirement)) return Notional{0.0};
    return Notional{equity - requirement};
}

Notional PaperAccount::available_buying_power() const noexcept {
    const double equity = portfolio_->equity().get();
    if (!is_finite(equity) || equity <= 0.0) return Notional{0.0};
    const double total = equity * config_.buying_power_multiplier;
    const double used = portfolio_->gross_exposure().get();
    const double available = total - used;
    return Notional{std::max(0.0, is_finite(available) ? available : 0.0)};
}

AccountStatus PaperAccount::status() const noexcept {
    if (suspended_) return AccountStatus::Suspended;
    const double equity = portfolio_->equity().get();
    // Non-positive equity is not a margin call but a liquidation: there is
    // nothing left to post as collateral.
    if (!is_finite(equity) || equity <= 0.0) return AccountStatus::Liquidation;
    if (margin_excess().get() < 0.0) return AccountStatus::MarginCall;
    return AccountStatus::Active;
}

Result<bool> PaperAccount::can_accept(Notional order_notional, Side side, bool reduces_risk) const {
    const auto current = status();
    if (current == AccountStatus::Suspended) return fail(bad("account is suspended"));
    if (current == AccountStatus::Liquidation) {
        return fail(bad("account equity is exhausted; no orders may be sent"));
    }
    if (current == AccountStatus::MarginCall && !reduces_risk) {
        // Risk-REDUCING orders remain permitted. Refusing them would trap the
        // account in the position that caused the call, which is the opposite
        // of what a margin call is for.
        return fail(
            bad("account is under a margin call; only risk-reducing orders are "
                "accepted"));
    }
    if (side == Side::Sell && !config_.allow_short && !reduces_risk) {
        return fail(bad("shorting is not permitted on this account"));
    }

    const double notional = std::abs(order_notional.get());
    if (!is_finite(notional)) return fail(bad("order notional is not finite"));

    // A risk-reducing order frees buying power rather than consuming it.
    if (!reduces_risk && notional > available_buying_power().get()) {
        std::ostringstream ss;
        ss.precision(2);
        ss << std::fixed << "order notional " << notional << " exceeds available buying power "
           << available_buying_power().get();
        return fail(bad(ss.str()));
    }
    return true;
}

Notional PaperAccount::accrue_interest(Duration elapsed) const noexcept {
    if (elapsed <= Duration::zero()) return Notional{0.0};
    const double cash = portfolio_->cash().get();
    // Charged on BORROWED cash only. Interest earned on a positive balance
    // belongs to the Phase 12 carry model; computing it here too would
    // double-count it.
    if (!is_finite(cash) || cash >= 0.0) return Notional{0.0};
    const double years = static_cast<double>(elapsed.count()) / (365.25 * 24.0 * 3600.0 * 1e9);
    const double interest = -cash * config_.margin_interest_rate * years;
    return Notional{is_finite(interest) ? -interest : 0.0};
}

AccountSnapshot PaperAccount::snapshot(Timestamp ts) const {
    AccountSnapshot out;
    out.ts = ts;
    // Every figure is READ from the portfolio, never recomputed. A second
    // computation here could disagree with the identity the journal checks.
    out.cash = portfolio_->cash();
    out.equity = portfolio_->equity();
    out.position_value = portfolio_->position_value();
    out.realized_pnl = portfolio_->realized_pnl();
    out.unrealized_pnl = portfolio_->unrealized_pnl();
    out.total_costs = portfolio_->total_costs();
    out.gross_exposure = portfolio_->gross_exposure();
    out.net_exposure = portfolio_->net_exposure();
    out.maintenance_requirement = maintenance_requirement();
    out.margin_excess = margin_excess();
    out.available_buying_power = available_buying_power();
    out.status = status();
    return out;
}

std::string AccountSnapshot::describe() const {
    std::ostringstream ss;
    ss.precision(2);
    ss << std::fixed;
    ss << "account at " << (is_set(ts) ? to_iso8601(ts) : std::string{"unset"}) << " ["
       << to_string(status) << "]\n";
    ss << "  cash              " << cash.get() << '\n';
    ss << "  equity            " << equity.get() << '\n';
    ss << "  positions         " << position_value.get() << '\n';
    ss << "  realized          " << realized_pnl.get() << '\n';
    ss << "  unrealized        " << unrealized_pnl.get() << '\n';
    ss << "  gross exposure    " << gross_exposure.get() << '\n';
    ss << "  maintenance req   " << maintenance_requirement.get() << '\n';
    ss << "  margin excess     " << margin_excess.get() << '\n';
    ss << "  buying power      " << available_buying_power.get() << '\n';
    return ss.str();
}

std::string AccountSnapshot::to_json() const {
    std::ostringstream ss;
    // An unset timestamp serializes as an empty string, never as a date:
    // to_iso8601 on kNoTimestamp overflows int64 days, and rendering "not set"
    // as a date in 1677 would be a lie a reader cannot detect.
    ss << "{\"ts\": \"" << (is_set(ts) ? to_iso8601(ts) : std::string{}) << "\", \"status\": \""
       << to_string(status) << "\", \"cash\": " << num(cash.get())
       << ", \"equity\": " << num(equity.get())
       << ", \"position_value\": " << num(position_value.get())
       << ", \"realized_pnl\": " << num(realized_pnl.get())
       << ", \"unrealized_pnl\": " << num(unrealized_pnl.get())
       << ", \"total_costs\": " << num(total_costs.get())
       << ", \"gross_exposure\": " << num(gross_exposure.get())
       << ", \"net_exposure\": " << num(net_exposure.get())
       << ", \"maintenance_requirement\": " << num(maintenance_requirement.get())
       << ", \"margin_excess\": " << num(margin_excess.get())
       << ", \"available_buying_power\": " << num(available_buying_power.get()) << '}';
    return ss.str();
}

}  // namespace ptl::paper
