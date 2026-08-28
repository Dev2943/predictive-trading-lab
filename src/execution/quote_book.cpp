#include "ptl/execution/quote_book.hpp"

#include <algorithm>
#include <cmath>

#include "ptl/execution/quote_router.hpp"

namespace ptl::execution {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

}  // namespace

std::string_view to_string(QuoteCondition c) noexcept {
    switch (c) {
        case QuoteCondition::Normal:
            return "normal";
        case QuoteCondition::Locked:
            return "locked";
        case QuoteCondition::Crossed:
            return "crossed";
        case QuoteCondition::Stale:
            return "stale";
        case QuoteCondition::Absent:
            return "absent";
    }
    return "unknown";
}

std::string_view to_string(TradingState s) noexcept {
    switch (s) {
        case TradingState::Trading:
            return "trading";
        case TradingState::Halted:
            return "halted";
        case TradingState::Closed:
            return "closed";
        case TradingState::Auction:
            return "auction";
    }
    return "unknown";
}

std::string_view to_string(QuoteTier t) noexcept {
    switch (t) {
        case QuoteTier::OneMinute:
            return "cbbo-1m";
        case QuoteTier::OneSecond:
            return "cbbo-1s";
        case QuoteTier::Continuous:
            return "continuous";
    }
    return "unknown";
}

Duration default_staleness_budget(QuoteTier tier) noexcept {
    // Straight from ADR-0001: max_staleness_t2_seconds = 60,
    // max_staleness_t3_seconds = 2. A sampled quote is normally about one
    // sampling interval old, so the budget is the interval plus headroom.
    switch (tier) {
        case QuoteTier::OneMinute:
            return std::chrono::seconds{60};
        case QuoteTier::OneSecond:
            return std::chrono::seconds{2};
        case QuoteTier::Continuous:
            return std::chrono::seconds{5};
    }
    return std::chrono::seconds{60};
}

std::string_view QuoteState::refusal_reason() const noexcept {
    if (!present) return "no quote available";
    switch (trading_state) {
        case TradingState::Halted:
            return "instrument is halted";
        case TradingState::Closed:
            return "market is closed";
        case TradingState::Auction:
            return "auction period, not continuous trading";
        case TradingState::Trading:
            break;
    }
    switch (condition) {
        case QuoteCondition::Crossed:
            return "consolidated quote is crossed, indicating a feed or parsing fault";
        case QuoteCondition::Stale:
            return "quote is older than the tier's staleness budget";
        case QuoteCondition::Absent:
            return "no quote available";
        case QuoteCondition::Normal:
        case QuoteCondition::Locked:
            break;
    }
    return "executable";
}

QuoteBook::QuoteBook(QuoteBookConfig cfg) : cfg_(cfg) {
    budget_ = cfg_.staleness_budget > Duration::zero() ? cfg_.staleness_budget
                                                       : default_staleness_budget(cfg_.tier);
}

Result<bool> QuoteBook::update(const market::Quote& quote) {
    auto& entry = entries_[index_of(quote.instrument())];
    const Timestamp arriving = quote.time().exchange_time;

    if (entry.present && arriving < entry.received) {
        // STRICTLY MONOTONIC. An out-of-order quote means the feed or the merge
        // is broken; accepting it would let a stale price overwrite a current
        // one, and every fill afterwards would be priced from the past.
        ++stats_.updates_rejected_out_of_order;
        return fail(bad("quote precedes the one already held",
                        to_iso8601(arriving) + " < " + to_iso8601(entry.received)));
    }

    // Quote::create already refuses ask < bid, so a crossed quote cannot reach
    // here through the normal path. The check remains because a book can be fed
    // from a decoder that constructs quotes by other means, and a crossed
    // CONSOLIDATED quote is a data fault worth naming loudly.
    if (quote.ask() < quote.bid()) {
        ++stats_.updates_rejected_crossed;
        if (cfg_.reject_crossed) {
            return fail(bad("consolidated quote is crossed", to_iso8601(arriving)));
        }
    }
    if (quote.ask() == quote.bid()) ++stats_.locked_observed;

    entry.quote = quote;
    entry.received = arriving;
    entry.present = true;
    ++stats_.updates_accepted;
    return true;
}

void QuoteBook::set_trading_state(InstrumentId instrument, TradingState state) noexcept {
    // Halts arrive out of band. Inferring one from a quote gap would produce a
    // phantom halt on every quiet minute, because a gap and a halt look
    // identical in sampled data.
    entries_[index_of(instrument)].trading_state = state;
}

QuoteState QuoteBook::state_at(InstrumentId instrument, Timestamp now) const noexcept {
    QuoteState out;
    out.tier = cfg_.tier;

    const auto it = entries_.find(index_of(instrument));
    if (it == entries_.end() || !it->second.present) {
        out.condition = QuoteCondition::Absent;
        out.trading_state = it == entries_.end() ? TradingState::Closed : it->second.trading_state;
        return out;
    }

    const Entry& entry = it->second;
    out.quote = entry.quote;
    out.present = true;
    out.trading_state = entry.trading_state;
    out.age = now - entry.received;

    if (entry.trading_state == TradingState::Halted) ++stats_.halted_reads;

    // A quote from the FUTURE relative to `now` is lookahead, and the only
    // honest response is to treat it as unusable. This is the check that makes
    // replaying a quote stream safe when events interleave.
    if (out.age < Duration::zero()) {
        out.condition = QuoteCondition::Stale;
        ++stats_.stale_reads;
        return out;
    }

    if (out.age > budget_) {
        out.condition = QuoteCondition::Stale;
        ++stats_.stale_reads;
        return out;
    }

    if (entry.quote->ask() < entry.quote->bid()) {
        out.condition = QuoteCondition::Crossed;
    } else if (entry.quote->ask() == entry.quote->bid()) {
        out.condition = QuoteCondition::Locked;
    } else {
        out.condition = QuoteCondition::Normal;
    }
    return out;
}

std::optional<market::Quote> QuoteBook::raw_quote(InstrumentId instrument) const noexcept {
    const auto it = entries_.find(index_of(instrument));
    if (it == entries_.end() || !it->second.present) return std::nullopt;
    return it->second.quote;
}

Timestamp QuoteBook::last_update(InstrumentId instrument) const noexcept {
    const auto it = entries_.find(index_of(instrument));
    return it == entries_.end() ? kNoTimestamp : it->second.received;
}

void QuoteBook::reset() noexcept {
    entries_.clear();
    stats_ = QuoteBookStats{};
}

// ---------------------------------------------------------------------------
// QuoteRouter
// ---------------------------------------------------------------------------

std::string_view to_string(PriceSource s) noexcept {
    switch (s) {
        case PriceSource::Quote:
            return "quote";
        case PriceSource::SyntheticFromBar:
            return "synthetic_from_bar";
        case PriceSource::None:
            return "none";
    }
    return "unknown";
}

RoutedMarket QuoteRouter::route(InstrumentId instrument, Timestamp now) const {
    RoutedMarket out;
    out.quote_state = book_->state_at(instrument, now);

    if (!out.quote_state.executable()) {
        out.source = PriceSource::None;
        out.executable = false;
        return out;
    }

    const auto& q = *out.quote_state.quote;
    out.state.bid = q.bid();
    out.state.ask = q.ask();
    out.state.bid_size = q.bid_size();
    out.state.ask_size = q.ask_size();
    // has_quote tells the cost model to use the REAL spread rather than
    // synthesising one. That single flag is the difference between a measured
    // transaction cost and a modelled one.
    out.state.has_quote = true;
    // A top-of-book snapshot carries no interval volume. Saying so explicitly
    // stops the venue from reading an unset zero as "nothing traded".
    out.state.volume_known = false;
    out.source = PriceSource::Quote;
    out.executable = true;
    return out;
}

RoutedMarket QuoteRouter::route_from_bar(const market::Bar& bar, double intraday_volatility) {
    RoutedMarket out;
    // Bar-only mode: no spread information exists, so bid == ask == close and
    // the cost model synthesises a half-spread. Marked SyntheticFromBar so a
    // report can separate these fills from quote-priced ones.
    out.state.bid = bar.close();
    out.state.ask = bar.close();
    out.state.interval_volume = bar.volume();
    out.state.intraday_volatility = intraday_volatility;
    out.state.has_quote = false;
    out.source = PriceSource::SyntheticFromBar;
    // A bar carries no halt information. Treating it as executable is a
    // documented limitation of bar-only mode, not an assumption that the
    // market was open.
    out.executable = true;
    return out;
}

RoutedMarket QuoteRouter::route_preferring_quote(const market::Bar& bar, double intraday_volatility,
                                                 Timestamp now) const {
    // A real quote always beats a synthesised one. The bar still supplies
    // volume and volatility, which a top-of-book snapshot does not carry.
    RoutedMarket quoted = route(bar.instrument(), now);
    if (quoted.executable) {
        // The bar supplies volume the quote lacks, so it becomes known again.
        quoted.state.interval_volume = bar.volume();
        quoted.state.intraday_volatility = intraday_volatility;
        quoted.state.volume_known = true;
        return quoted;
    }
    // If the instrument is HALTED, fall through to nothing rather than to a
    // synthetic bar price: a halt is a fact about the venue, and a bar cannot
    // override it.
    if (quoted.quote_state.present && quoted.quote_state.trading_state == TradingState::Halted) {
        return quoted;
    }
    return route_from_bar(bar, intraday_volatility);
}

}  // namespace ptl::execution
