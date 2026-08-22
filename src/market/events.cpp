#include <cmath>

#include "ptl/market/bar.hpp"
#include "ptl/market/corporate_action.hpp"
#include "ptl/market/event.hpp"
#include "ptl/market/quote.hpp"
#include "ptl/market/trade.hpp"

namespace ptl::market {
namespace {

/// Every price field must be strictly positive and finite. A zero or negative
/// price is not a market condition for any instrument in scope; it is a parse
/// error or a vendor defect, and admitting one lets inf and NaN into the P&L.
[[nodiscard]] bool sane_price(Price p) noexcept {
    return is_finite(p.get()) && p.get() > 0.0;
}

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

}  // namespace

// ---------------------------------------------------------------------------
// Bar
// ---------------------------------------------------------------------------

Result<Bar> Bar::make(InstrumentId instrument, Timestamp open_time, Timestamp close_time,
                      Price open, Price high, Price low, Price close, Volume volume,
                      Duration feed_latency) {
    if (instrument == kInvalidInstrument) return fail(bad("bar has no instrument"));
    if (!is_set(open_time) || !is_set(close_time)) return fail(bad("bar timestamp unset"));
    if (close_time <= open_time) {
        return fail(bad("bar close_time must be after open_time",
                        to_iso8601(open_time) + " .. " + to_iso8601(close_time)));
    }
    if (feed_latency < Duration::zero()) {
        return fail(bad("bar feed latency cannot be negative"));
    }
    if (!sane_price(open) || !sane_price(high) || !sane_price(low) || !sane_price(close)) {
        return fail(bad("bar has a non-positive or non-finite price"));
    }
    if (!is_finite(volume.get()) || volume.get() < 0.0) {
        return fail(bad("bar has a negative or non-finite volume"));
    }
    // OHLC consistency. A vendor row failing this is corrupt, and silently
    // accepting it would let a fill price be sampled outside the traded range.
    if (low > high) return fail(bad("bar low exceeds high"));
    if (open < low || open > high) return fail(bad("bar open outside the low-high range"));
    if (close < low || close > high) return fail(bad("bar close outside the low-high range"));

    Bar b;
    b.instrument_ = instrument;
    b.open_time_ = open_time;
    b.close_time_ = close_time;
    // The bar comes into existence at its CLOSE, not its open. This single
    // assignment is what prevents the one-interval lookahead.
    b.time_.exchange_time = close_time;
    b.time_.receive_time = close_time + feed_latency;
    b.open_ = open;
    b.high_ = high;
    b.low_ = low;
    b.close_ = close;
    b.volume_ = volume;
    return b;
}

Result<Bar> Bar::from_left_edge(InstrumentId instrument, Timestamp open_time, Duration timeframe,
                                Price open, Price high, Price low, Price close, Volume volume,
                                Duration feed_latency) {
    if (timeframe <= Duration::zero()) return fail(bad("bar timeframe must be positive"));
    return make(instrument, open_time, open_time + timeframe, open, high, low, close, volume,
                feed_latency);
}

Result<Bar> Bar::from_right_edge(InstrumentId instrument, Timestamp close_time, Duration timeframe,
                                 Price open, Price high, Price low, Price close, Volume volume,
                                 Duration feed_latency) {
    if (timeframe <= Duration::zero()) return fail(bad("bar timeframe must be positive"));
    return make(instrument, close_time - timeframe, close_time, open, high, low, close, volume,
                feed_latency);
}

// ---------------------------------------------------------------------------
// Quote
// ---------------------------------------------------------------------------

Result<Quote> Quote::create(InstrumentId instrument, Timestamp exchange_time, Price bid,
                            Qty bid_size, Price ask, Qty ask_size, Duration feed_latency) {
    if (instrument == kInvalidInstrument) return fail(bad("quote has no instrument"));
    if (!is_set(exchange_time)) return fail(bad("quote timestamp unset"));
    if (feed_latency < Duration::zero()) return fail(bad("quote feed latency cannot be negative"));
    if (!sane_price(bid) || !sane_price(ask)) {
        return fail(bad("quote has a non-positive or non-finite price"));
    }
    if (ask < bid) {
        // A crossed book is real for microseconds across venues, but a
        // CONSOLIDATED top-of-book snapshot should never be crossed. If one is,
        // the feed or our parsing is wrong, and a negative spread would flow
        // straight into a negative transaction cost -- a strategy that appears
        // to be paid for trading.
        return fail(bad("quote is crossed: ask below bid", to_iso8601(exchange_time)));
    }
    if (!is_finite(bid_size.get()) || !is_finite(ask_size.get()) || bid_size.get() < 0.0 ||
        ask_size.get() < 0.0) {
        return fail(bad("quote has a negative or non-finite size"));
    }

    Quote q;
    q.instrument_ = instrument;
    q.time_.exchange_time = exchange_time;
    q.time_.receive_time = exchange_time + feed_latency;
    q.bid_ = bid;
    q.ask_ = ask;
    q.bid_size_ = bid_size;
    q.ask_size_ = ask_size;
    return q;
}

// ---------------------------------------------------------------------------
// Trade
// ---------------------------------------------------------------------------

Result<Trade> Trade::create(InstrumentId instrument, Timestamp exchange_time, Price price, Qty size,
                            std::optional<Side> aggressor, Duration feed_latency) {
    if (instrument == kInvalidInstrument) return fail(bad("trade has no instrument"));
    if (!is_set(exchange_time)) return fail(bad("trade timestamp unset"));
    if (feed_latency < Duration::zero()) return fail(bad("trade feed latency cannot be negative"));
    if (!sane_price(price)) return fail(bad("trade has a non-positive or non-finite price"));
    if (!is_finite(size.get()) || size.get() <= 0.0) {
        return fail(bad("trade size must be positive and finite"));
    }

    Trade t;
    t.instrument_ = instrument;
    t.time_.exchange_time = exchange_time;
    t.time_.receive_time = exchange_time + feed_latency;
    t.price_ = price;
    t.size_ = size;
    t.aggressor_ = aggressor;
    return t;
}

// ---------------------------------------------------------------------------
// CorporateAction
// ---------------------------------------------------------------------------

std::string_view to_string(CorporateActionKind k) noexcept {
    switch (k) {
        case CorporateActionKind::Split:
            return "split";
        case CorporateActionKind::CashDividend:
            return "cash_dividend";
    }
    return "unknown";
}

Result<CorporateAction> CorporateAction::split(InstrumentId instrument, Timestamp effective,
                                               double ratio) {
    if (instrument == kInvalidInstrument) return fail(bad("split has no instrument"));
    if (!is_set(effective)) return fail(bad("split timestamp unset"));
    if (!is_finite(ratio) || ratio <= 0.0) {
        return fail(bad("split ratio must be positive and finite"));
    }
    CorporateAction c;
    c.instrument_ = instrument;
    c.time_.exchange_time = effective;
    c.time_.receive_time = effective;
    c.kind_ = CorporateActionKind::Split;
    c.ratio_ = ratio;
    return c;
}

Result<CorporateAction> CorporateAction::cash_dividend(InstrumentId instrument, Timestamp ex_date,
                                                       Notional amount) {
    if (instrument == kInvalidInstrument) return fail(bad("dividend has no instrument"));
    if (!is_set(ex_date)) return fail(bad("dividend timestamp unset"));
    if (!is_finite(amount.get()) || amount.get() <= 0.0) {
        return fail(bad("dividend amount must be positive and finite"));
    }
    CorporateAction c;
    c.instrument_ = instrument;
    c.time_.exchange_time = ex_date;
    c.time_.receive_time = ex_date;
    c.kind_ = CorporateActionKind::CashDividend;
    c.amount_ = amount;
    return c;
}

// ---------------------------------------------------------------------------
// MarketEvent accessors
// ---------------------------------------------------------------------------

Timestamp exchange_time_of(const MarketEvent& e) noexcept {
    return std::visit(
        [](const auto& x) -> Timestamp {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, SessionEvent> || std::is_same_v<T, TimerEvent>) {
                return x.time.exchange_time;
            } else {
                return x.time().exchange_time;
            }
        },
        e);
}

Timestamp receive_time_of(const MarketEvent& e) noexcept {
    return std::visit(
        [](const auto& x) -> Timestamp {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, SessionEvent> || std::is_same_v<T, TimerEvent>) {
                return x.time.receive_time;
            } else {
                return x.time().receive_time;
            }
        },
        e);
}

InstrumentId instrument_of(const MarketEvent& e) noexcept {
    return std::visit(
        [](const auto& x) -> InstrumentId {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, SessionEvent> || std::is_same_v<T, TimerEvent>) {
                return kInvalidInstrument;
            } else {
                return x.instrument();
            }
        },
        e);
}

std::string_view kind_name(const MarketEvent& e) noexcept {
    return std::visit(
        [](const auto& x) -> std::string_view {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, Bar>)
                return "bar";
            else if constexpr (std::is_same_v<T, Quote>)
                return "quote";
            else if constexpr (std::is_same_v<T, Trade>)
                return "trade";
            else if constexpr (std::is_same_v<T, CorporateAction>)
                return to_string(x.kind());
            else if constexpr (std::is_same_v<T, SessionEvent>)
                return x.kind == SessionEventKind::Open ? "session_open" : "session_close";
            else
                return "timer";
        },
        e);
}

}  // namespace ptl::market
