#include "ptl/market/source.hpp"

#include <algorithm>
#include <utility>

namespace ptl::market {

Result<ReplaySource> ReplaySource::create(std::vector<MarketEvent> events, SimulatedClock* clock) {
    if (clock == nullptr) {
        return fail(make_error(ErrorCode::InvalidArgument, "replay source requires a clock"));
    }
    // Verify chronology rather than sorting. Sorting would HIDE the defect:
    // an unordered feed means the merge is broken or the file is corrupt, and
    // quietly repairing it produces a backtest built on data we do not
    // understand.
    for (std::size_t i = 1; i < events.size(); ++i) {
        const Timestamp prev = exchange_time_of(events[i - 1]);
        const Timestamp cur = exchange_time_of(events[i]);
        if (cur < prev) {
            return fail(
                make_error(ErrorCode::ValidationFailed,
                           "replay events are not chronological at index " + std::to_string(i),
                           to_iso8601(prev) + " followed by " + to_iso8601(cur)));
        }
    }
    // Receive time must not precede exchange time on any event, or the source
    // is claiming knowledge before the venue acted.
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (receive_time_of(events[i]) < exchange_time_of(events[i])) {
            return fail(make_error(
                ErrorCode::ValidationFailed,
                "event " + std::to_string(i) + " has receive_time before exchange_time"));
        }
    }

    ReplaySource s;
    s.events_ = std::move(events);
    s.clock_ = clock;
    s.first_ = s.events_.empty() ? kNoTimestamp : receive_time_of(s.events_.front());
    if (is_set(s.first_)) clock->reset(s.first_);
    return s;
}

std::optional<MarketEvent> ReplaySource::next() {
    if (exhausted()) return std::nullopt;
    MarketEvent e = events_[next_++];

    // Advance to RECEIVE time, not exchange time. A strategy asking the clock
    // what time it is must be told the earliest instant it could have known
    // about this event -- never the instant the venue acted, which it could not
    // have observed yet.
    clock_->advance_to(receive_time_of(e));
    return e;
}

Timestamp ReplaySource::peek_time() const noexcept {
    // kMaxTimestamp when exhausted, so a k-way merge orders an empty source
    // last without a special case.
    if (exhausted()) return kMaxTimestamp;
    return exchange_time_of(events_[next_]);
}

void ReplaySource::reset() {
    next_ = 0;
    if (clock_ != nullptr && is_set(first_)) clock_->reset(first_);
}

Result<std::vector<MarketEvent>> with_session_events(std::vector<MarketEvent> events,
                                                     const Calendar& calendar) {
    if (events.empty()) return events;

    std::vector<MarketEvent> out;
    out.reserve(events.size() + 32);

    Timestamp current_session_date = kNoTimestamp;
    Session current{};

    for (auto& e : events) {
        const Timestamp ts = exchange_time_of(e);
        const auto s = calendar.session_containing(ts);
        if (!s.has_value()) {
            return fail(make_error(ErrorCode::ValidationFailed,
                                   "event falls outside any trading session",
                                   to_iso8601(ts) + " " + std::string{kind_name(e)}));
        }
        if (s->date != current_session_date) {
            if (is_set(current_session_date)) {
                SessionEvent close_ev;
                close_ev.time.exchange_time = current.close;
                close_ev.time.receive_time = current.close;
                close_ev.kind = SessionEventKind::Close;
                out.emplace_back(close_ev);
            }
            SessionEvent open_ev;
            open_ev.time.exchange_time = s->open;
            open_ev.time.receive_time = s->open;
            open_ev.kind = SessionEventKind::Open;
            out.emplace_back(open_ev);
            current_session_date = s->date;
            current = *s;
        }
        out.push_back(std::move(e));
    }

    if (is_set(current_session_date)) {
        SessionEvent close_ev;
        close_ev.time.exchange_time = current.close;
        close_ev.time.receive_time = current.close;
        close_ev.kind = SessionEventKind::Close;
        out.emplace_back(close_ev);
    }
    return out;
}

Result<std::vector<MarketEvent>> merge_sorted(std::vector<std::vector<MarketEvent>> streams) {
    std::size_t total = 0;
    for (const auto& s : streams) total += s.size();

    std::vector<MarketEvent> out;
    out.reserve(total);
    std::vector<std::size_t> cursor(streams.size(), 0);

    for (std::size_t n = 0; n < total; ++n) {
        std::size_t best = streams.size();
        for (std::size_t i = 0; i < streams.size(); ++i) {
            if (cursor[i] >= streams[i].size()) continue;
            if (best == streams.size()) {
                best = i;
                continue;
            }
            const Timestamp a = exchange_time_of(streams[i][cursor[i]]);
            const Timestamp b = exchange_time_of(streams[best][cursor[best]]);
            if (a < b) {
                best = i;
            } else if (a == b) {
                // Deterministic tie-break. Without one, two runs could order
                // simultaneous events differently and floating-point summation
                // downstream is not associative -- the equity curves would
                // diverge in the last digits and the determinism test would
                // fail for reasons nobody could locate.
                const InstrumentId ia = instrument_of(streams[i][cursor[i]]);
                const InstrumentId ib = instrument_of(streams[best][cursor[best]]);
                if (index_of(ia) < index_of(ib)) best = i;
            }
        }
        if (best == streams.size()) break;
        out.push_back(std::move(streams[best][cursor[best]]));
        ++cursor[best];
    }

    for (std::size_t i = 1; i < out.size(); ++i) {
        if (exchange_time_of(out[i]) < exchange_time_of(out[i - 1])) {
            return fail(make_error(ErrorCode::ValidationFailed,
                                   "merge produced an unordered stream; an input was unsorted"));
        }
    }
    return out;
}

}  // namespace ptl::market
