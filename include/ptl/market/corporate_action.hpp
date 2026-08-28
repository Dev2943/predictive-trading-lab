#pragma once

/// \file corporate_action.hpp
/// Splits and cash distributions, as first-class events.
///
/// Even the ETF universe pays dividends (SPY, TLT, XLF quarterly). Modelling
/// these as events rather than folding them into adjusted prices keeps the
/// distinction the research insists on: adjusted prices are a research
/// convenience for computing returns, NOT a substitute for explicit accounting.
/// A dividend is a cash credit; a split changes share count and cost basis.

#include <cstdint>
#include <string_view>

#include "ptl/core/result.hpp"
#include "ptl/core/time.hpp"
#include "ptl/core/types.hpp"

namespace ptl::market {

enum class CorporateActionKind : std::uint8_t { Split, CashDividend };

[[nodiscard]] std::string_view to_string(CorporateActionKind k) noexcept;

class CorporateAction {
public:
    /// `ratio` is new shares per old share: a 2-for-1 split is 2.0, a 1-for-10
    /// reverse split is 0.1.
    [[nodiscard]] static Result<CorporateAction> split(InstrumentId instrument, Timestamp effective,
                                                       double ratio);

    /// `amount` is per share, in the instrument's currency.
    [[nodiscard]] static Result<CorporateAction> cash_dividend(InstrumentId instrument,
                                                               Timestamp ex_date, Notional amount);

    [[nodiscard]] InstrumentId instrument() const noexcept { return instrument_; }
    [[nodiscard]] const EventTime& time() const noexcept { return time_; }
    [[nodiscard]] CorporateActionKind kind() const noexcept { return kind_; }
    [[nodiscard]] double split_ratio() const noexcept { return ratio_; }
    [[nodiscard]] Notional dividend_amount() const noexcept { return amount_; }

private:
    CorporateAction() = default;

    InstrumentId instrument_{kInvalidInstrument};
    EventTime time_{};
    CorporateActionKind kind_{CorporateActionKind::Split};
    double ratio_{1.0};
    Notional amount_{};
};

}  // namespace ptl::market
