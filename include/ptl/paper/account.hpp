#pragma once

/// \file account.hpp
/// The paper brokerage account.
///
/// A VIEW OVER portfolio::Portfolio, NOT A SECOND LEDGER. The portfolio already
/// owns cash, equity, realised and unrealised P&L, costs and turnover, and it
/// maintains an accounting identity the Phase 3 journal reconciles against. A
/// parallel account holding its own copies would be a second source of truth,
/// and the first time the two disagreed nobody would know which was right.
///
/// What this ADDS is the part a brokerage has and a portfolio does not: a
/// margin model. Maintenance requirements, margin excess and buying power are
/// broker policy, not portfolio arithmetic, and they are the only state this
/// type owns.

#include <cstdint>
#include <string>
#include <string_view>

#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"
#include "ptl/portfolio/portfolio.hpp"

namespace ptl::paper {

/// Broker margin policy.
struct MarginConfig {
    /// Equity fraction required to open a position. Reg-T is 0.50 for equities.
    double initial_margin = 0.50;
    /// Equity fraction below which a margin call is issued. Lower than the
    /// initial requirement, so a position compliant when opened is not
    /// instantly in breach on a small adverse move.
    double maintenance_margin = 0.25;
    /// Multiplier on equity for buying power.
    double buying_power_multiplier = 2.0;
    bool allow_short = true;
    /// Annualised rate charged on borrowed cash.
    double margin_interest_rate = 0.055;
};

enum class AccountStatus : std::uint8_t {
    Active,
    /// Equity below maintenance. New risk is refused, but the book may still be
    /// REDUCED -- halting all trading would trap the account in the very
    /// position that caused the call.
    MarginCall,
    /// Equity exhausted. Nothing may be sent.
    Liquidation,
    Suspended,
};

[[nodiscard]] std::string_view to_string(AccountStatus) noexcept;

/// A point-in-time account statement.
struct AccountSnapshot {
    Timestamp ts{kNoTimestamp};
    Notional cash{};
    Notional equity{};
    Notional position_value{};
    Notional realized_pnl{};
    Notional unrealized_pnl{};
    Notional total_costs{};
    Notional gross_exposure{};
    Notional net_exposure{};

    Notional maintenance_requirement{};
    /// Equity above the requirement. Negative means a margin call.
    Notional margin_excess{};
    Notional available_buying_power{};

    AccountStatus status{AccountStatus::Active};

    [[nodiscard]] std::string describe() const;
    /// Round-trip exact, because the session checksum covers these bits.
    [[nodiscard]] std::string to_json() const;
};

/// Applies broker margin policy to a portfolio.
///
/// Holds a borrowed CONST reference: it cannot modify the portfolio, which is
/// what keeps the account from becoming a second ledger.
class PaperAccount {
public:
    PaperAccount(const portfolio::Portfolio& portfolio, MarginConfig config = {}) noexcept
        : portfolio_(&portfolio), config_(config) {}

    [[nodiscard]] AccountSnapshot snapshot(Timestamp) const;

    /// Whether an order may be accepted.
    ///
    /// \param reduces_risk when true the order shrinks the book and is
    ///        permitted under a margin call.
    [[nodiscard]] Result<bool> can_accept(Notional order_notional, Side, bool reduces_risk) const;

    [[nodiscard]] AccountStatus status() const noexcept;
    [[nodiscard]] Notional maintenance_requirement() const noexcept;
    [[nodiscard]] Notional margin_excess() const noexcept;
    [[nodiscard]] Notional available_buying_power() const noexcept;
    /// Interest accrued over an interval on BORROWED cash only.
    [[nodiscard]] Notional accrue_interest(Duration) const noexcept;

    [[nodiscard]] const MarginConfig& config() const noexcept { return config_; }
    void suspend() noexcept { suspended_ = true; }
    void resume() noexcept { suspended_ = false; }

private:
    const portfolio::Portfolio* portfolio_;
    MarginConfig config_;
    bool suspended_ = false;
};

}  // namespace ptl::paper
