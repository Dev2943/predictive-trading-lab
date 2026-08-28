#include "ptl/market/validator.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace ptl::market {

std::string_view to_string(IssueCode c) noexcept {
    switch (c) {
        case IssueCode::NonMonotonicTimestamp:
            return "non_monotonic_timestamp";
        case IssueCode::DuplicateTimestamp:
            return "duplicate_timestamp";
        case IssueCode::GapInSession:
            return "gap_in_session";
        case IssueCode::OutsideSession:
            return "outside_session";
        case IssueCode::UnknownSession:
            return "unknown_session";
        case IssueCode::SuspiciousPriceJump:
            return "suspicious_price_jump";
        case IssueCode::ZeroVolume:
            return "zero_volume";
        case IssueCode::StaleQuote:
            return "stale_quote";
        case IssueCode::CrossedQuote:
            return "crossed_quote";
        case IssueCode::TimeframeMismatch:
            return "timeframe_mismatch";
    }
    return "unknown";
}

std::string ValidationIssue::describe() const {
    std::string out{to_string(code)};
    out += severity == Severity::Fatal ? " [FATAL] " : " [warn] ";
    out += to_iso8601(at);
    if (instrument != kInvalidInstrument) {
        out += " instrument#" + std::to_string(index_of(instrument));
    }
    if (!detail.empty()) out += ": " + detail;
    return out;
}

std::size_t ValidationReport::fatal_count() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(issues.begin(), issues.end(),
                      [](const ValidationIssue& i) { return i.severity == Severity::Fatal; }));
}
std::size_t ValidationReport::warning_count() const noexcept {
    return issues.size() - fatal_count();
}

std::string ValidationReport::summary() const {
    std::string out = "validated " + std::to_string(stats.events) + " events (" +
                      std::to_string(stats.bars) + " bars, " + std::to_string(stats.quotes) +
                      " quotes, " + std::to_string(stats.trades) + " trades) across " +
                      std::to_string(stats.sessions) + " sessions";
    if (is_set(stats.first)) {
        out += "\n  range: " + to_iso8601(stats.first) + " .. " + to_iso8601(stats.last);
    }
    out += "\n  " + std::to_string(fatal_count()) + " fatal, " + std::to_string(warning_count()) +
           " warnings";
    if (stats.zero_volume_bars != 0) {
        out += "\n  " + std::to_string(stats.zero_volume_bars) + " zero-volume bars";
    }
    return out;
}

namespace {

/// Per-instrument running state, so checks that need history do not need a
/// second pass over the stream.
struct InstrumentState {
    Timestamp last_bar_close{kNoTimestamp};
    Price last_close{};
    std::size_t consecutive_zero_volume = 0;
};

}  // namespace

ValidationReport DataValidator::validate(std::span<const MarketEvent> events,
                                         const Calendar* calendar) const {
    ValidationReport report;
    const Severity warn = cfg_.strict ? Severity::Fatal : Severity::Warning;

    const auto add = [&report](IssueCode code, Severity sev, Timestamp at, InstrumentId id,
                               std::string detail) {
        report.issues.push_back({code, sev, at, id, std::move(detail)});
    };

    std::map<std::uint32_t, InstrumentState> state;
    Timestamp prev_ts = kNoTimestamp;
    Timestamp current_session = kNoTimestamp;

    if (calendar == nullptr) {
        add(IssueCode::UnknownSession, warn, kNoTimestamp, kInvalidInstrument,
            "no calendar supplied; session boundary and gap checks were skipped");
    }

    for (const auto& e : events) {
        ++report.stats.events;
        const Timestamp ts = exchange_time_of(e);
        const InstrumentId id = instrument_of(e);

        if (!is_set(report.stats.first)) report.stats.first = ts;
        report.stats.last = ts;

        // --- stream-level ordering ------------------------------------------
        if (is_set(prev_ts)) {
            if (ts < prev_ts) {
                add(IssueCode::NonMonotonicTimestamp, Severity::Fatal, ts, id,
                    "preceded by " + to_iso8601(prev_ts));
            }
        }
        prev_ts = ts;

        // --- session membership ---------------------------------------------
        if (calendar != nullptr && !std::holds_alternative<SessionEvent>(e) &&
            !std::holds_alternative<TimerEvent>(e) && !std::holds_alternative<CorporateAction>(e)) {
            const auto s = calendar->session_containing(ts);
            if (!s.has_value()) {
                // A bar outside any session is either extended-hours data we
                // did not ask for, or a timestamp-convention error. Both are
                // fatal: silently including an after-hours bar would put an
                // untradeable price into the return series.
                add(IssueCode::OutsideSession, Severity::Fatal, ts, id,
                    "no trading session contains this timestamp");
            } else if (s->date != current_session) {
                current_session = s->date;
                ++report.stats.sessions;
            }
        }

        std::visit(
            [&](const auto& x) {
                using T = std::decay_t<decltype(x)>;

                if constexpr (std::is_same_v<T, Bar>) {
                    ++report.stats.bars;
                    auto& st = state[index_of(x.instrument())];

                    if (cfg_.expected_bar_timeframe > Duration::zero() &&
                        x.timeframe() != cfg_.expected_bar_timeframe) {
                        add(IssueCode::TimeframeMismatch, Severity::Fatal, ts, x.instrument(),
                            "bar spans " + std::to_string(x.timeframe().count()) + "ns, expected " +
                                std::to_string(cfg_.expected_bar_timeframe.count()));
                    }

                    if (is_set(st.last_bar_close)) {
                        if (x.close_time() == st.last_bar_close) {
                            add(IssueCode::DuplicateTimestamp, Severity::Fatal, ts, x.instrument(),
                                "repeated bar close");
                        } else if (cfg_.require_complete_sessions && calendar != nullptr) {
                            // A gap WITHIN a session is missing data. A gap
                            // across the overnight boundary is not -- checking
                            // the calendar is what distinguishes them, and is
                            // why this cannot be done with arithmetic alone.
                            const auto prev_s =
                                calendar->session_containing(st.last_bar_close - Duration{1});
                            const auto cur_s = calendar->session_containing(ts);
                            const bool same_session = prev_s.has_value() && cur_s.has_value() &&
                                                      prev_s->date == cur_s->date;
                            const Duration delta = x.close_time() - st.last_bar_close;
                            if (same_session && delta > cfg_.expected_bar_timeframe) {
                                add(IssueCode::GapInSession, warn, ts, x.instrument(),
                                    "missing " +
                                        std::to_string(delta / cfg_.expected_bar_timeframe - 1) +
                                        " bar(s) since " + to_iso8601(st.last_bar_close));
                            }
                        }

                        const double r = std::log(x.close().get() / st.last_close.get());
                        if (std::abs(r) > cfg_.max_abs_log_return) {
                            // Almost always a missing split rather than a real
                            // move. Admitting it puts a fictional double-digit
                            // return into the series.
                            add(IssueCode::SuspiciousPriceJump, Severity::Fatal, ts, x.instrument(),
                                "log return " + std::to_string(r) +
                                    " exceeds the corporate-action threshold; a split or "
                                    "dividend adjustment is probably missing");
                        }
                    }

                    if (x.volume().get() == 0.0) {
                        ++report.stats.zero_volume_bars;
                        ++st.consecutive_zero_volume;
                        if (st.consecutive_zero_volume == cfg_.max_consecutive_zero_volume) {
                            add(IssueCode::ZeroVolume, warn, ts, x.instrument(),
                                std::to_string(st.consecutive_zero_volume) +
                                    " consecutive zero-volume bars: the feed may have "
                                    "stopped rather than the market being quiet");
                        }
                    } else {
                        st.consecutive_zero_volume = 0;
                    }

                    st.last_bar_close = x.close_time();
                    st.last_close = x.close();

                } else if constexpr (std::is_same_v<T, Quote>) {
                    ++report.stats.quotes;
                    if (x.ask() < x.bid()) {
                        add(IssueCode::CrossedQuote, Severity::Fatal, ts, x.instrument(),
                            "ask below bid");
                    }
                } else if constexpr (std::is_same_v<T, Trade>) {
                    ++report.stats.trades;
                }
            },
            e);
    }
    return report;
}

}  // namespace ptl::market
