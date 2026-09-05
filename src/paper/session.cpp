#include "ptl/paper/session.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace ptl::paper {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

void hash_bytes(std::uint64_t& h, const void* data, std::size_t len) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<std::uint64_t>(p[i]);
        h *= 0x100000001b3ULL;
    }
}

void hash_string(std::uint64_t& h, std::string_view s) {
    hash_bytes(h, s.data(), s.size());
}

[[nodiscard]] std::string json_escape(std::string_view in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (const char c : in) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            default:
                out += c;
        }
    }
    return out;
}

/// An unset timestamp serializes as an empty string, never as a date.
/// to_iso8601 on kNoTimestamp overflows int64 days, and rendering "not set" as
/// a date in 1677 would be a lie a reader cannot detect.
[[nodiscard]] std::string iso_or_empty(Timestamp ts) {
    return is_set(ts) ? to_iso8601(ts) : std::string{};
}

/// ROUND-TRIP EXACT. Persistence must restore the same bits it saved, because
/// the state checksum is computed over the raw double representation. Writing
/// at six decimals looks fine and silently fails recovery for any value not
/// exactly representable at that precision -- which is almost all of them.
[[nodiscard]] std::string num(double v) {
    if (!is_finite(v)) return "null";
    std::ostringstream ss;
    ss << std::setprecision(std::numeric_limits<double>::max_digits10) << v;
    return ss.str();
}

}  // namespace

std::string_view to_string(SessionPhase p) noexcept {
    switch (p) {
        case SessionPhase::Created:
            return "created";
        case SessionPhase::Starting:
            return "starting";
        case SessionPhase::Running:
            return "running";
        case SessionPhase::Paused:
            return "paused";
        case SessionPhase::Stopping:
            return "stopping";
        case SessionPhase::Stopped:
            return "stopped";
        case SessionPhase::Failed:
            return "failed";
    }
    return "unknown";
}

bool is_terminal(SessionPhase p) noexcept {
    return p == SessionPhase::Stopped || p == SessionPhase::Failed;
}

std::string SessionStats::describe() const {
    std::ostringstream ss;
    ss << "paper session over " << events_processed << " events\n";
    ss << "  orders       " << orders_submitted << " submitted, " << orders_rejected
       << " rejected\n";
    ss << "  fills        " << fills_received << '\n';
    ss << "  persists     " << persists << '\n';
    ss << "  pauses       " << pauses << '\n';
    ss << "  daily resets " << daily_resets << '\n';
    return ss.str();
}

std::uint64_t SessionState::checksum() const {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    hash_string(h, session_id);
    hash_bytes(h, &sequence, sizeof(sequence));
    hash_bytes(h, &config_fingerprint, sizeof(config_fingerprint));
    hash_bytes(h, &events_processed, sizeof(events_processed));

    // Bit patterns, so a one-ulp difference on restore is caught rather than
    // rounded away.
    const double cash = account.cash.get();
    hash_bytes(h, &cash, sizeof(cash));
    const double equity = account.equity.get();
    hash_bytes(h, &equity, sizeof(equity));

    for (const auto& p : positions) {
        hash_bytes(h, &p.instrument, sizeof(p.instrument));
        hash_bytes(h, &p.quantity, sizeof(p.quantity));
        hash_bytes(h, &p.average_cost, sizeof(p.average_cost));
    }
    for (const auto& o : working_orders) {
        hash_bytes(h, &o.order_id, sizeof(o.order_id));
        hash_bytes(h, &o.quantity, sizeof(o.quantity));
        hash_bytes(h, &o.filled, sizeof(o.filled));
    }
    return h;
}

std::string SessionState::to_json() const {
    std::ostringstream ss;
    ss << "{\"session_id\": \"" << json_escape(session_id) << "\", \"sequence\": " << sequence
       << ", \"phase\": \"" << to_string(phase) << "\", \"saved_at\": \"" << iso_or_empty(saved_at)
       << "\", \"config_fingerprint\": \"" << std::hex << std::setw(16) << std::setfill('0')
       << config_fingerprint << std::dec << "\", \"events_processed\": " << events_processed
       << ", \"last_event_time\": \"" << iso_or_empty(last_event_time)
       << "\", \"fills_received\": " << fills_received
       << ", \"orders_submitted\": " << orders_submitted << ", \"account\": " << account.to_json()
       << ", \"positions\": [";
    for (std::size_t i = 0; i < positions.size(); ++i) {
        if (i != 0) ss << ", ";
        ss << "{\"instrument\": " << positions[i].instrument
           << ", \"quantity\": " << num(positions[i].quantity)
           << ", \"average_cost\": " << num(positions[i].average_cost)
           << ", \"realized_pnl\": " << num(positions[i].realized_pnl) << '}';
    }
    ss << "], \"working_orders\": [";
    for (std::size_t i = 0; i < working_orders.size(); ++i) {
        if (i != 0) ss << ", ";
        ss << "{\"order_id\": " << working_orders[i].order_id
           << ", \"instrument\": " << working_orders[i].instrument
           << ", \"side\": " << static_cast<int>(working_orders[i].side)
           << ", \"quantity\": " << num(working_orders[i].quantity)
           << ", \"filled\": " << num(working_orders[i].filled) << '}';
    }
    ss << "], \"checksum\": \"" << std::hex << std::setw(16) << std::setfill('0') << checksum()
       << std::dec << "\"}";
    return ss.str();
}

Result<SessionState> SessionState::from_json(std::string_view json) {
    // A narrow reader for a format this module also writes. It extracts by key
    // and verifies the checksum before returning: a corrupt state must be
    // refused, not resumed from.
    const auto field = [json](std::string_view key) -> std::optional<std::string> {
        const std::string needle = "\"" + std::string{key} + "\":";
        const auto pos = json.find(needle);
        if (pos == std::string_view::npos) return std::nullopt;
        auto cursor = json.find_first_not_of(" \t", pos + needle.size());
        if (cursor == std::string_view::npos) return std::nullopt;
        if (json[cursor] == '"') {
            const auto end = json.find('"', cursor + 1);
            if (end == std::string_view::npos) return std::nullopt;
            return std::string{json.substr(cursor + 1, end - cursor - 1)};
        }
        const auto end = json.find_first_of(",}", cursor);
        return std::string{json.substr(cursor, end - cursor)};
    };

    const auto parse_u64 = [](const std::optional<std::string>& text, std::uint64_t& out,
                              int base = 10) -> bool {
        if (!text.has_value() || text->empty()) return false;
        const auto* first = text->data();
        const auto* last = text->data() + text->size();
        return std::from_chars(first, last, out, base).ec == std::errc{};
    };

    SessionState state;
    const auto id = field("session_id");
    if (!id.has_value()) return fail(bad("session state has no session_id"));
    state.session_id = *id;

    if (!parse_u64(field("sequence"), state.sequence)) {
        return fail(bad("session state has an unreadable sequence"));
    }
    if (!parse_u64(field("config_fingerprint"), state.config_fingerprint, 16)) {
        return fail(bad("session state has an unreadable config fingerprint"));
    }
    std::uint64_t events = 0;
    if (!parse_u64(field("events_processed"), events)) {
        return fail(bad("session state has an unreadable event count"));
    }
    state.events_processed = static_cast<std::size_t>(events);

    std::uint64_t fills = 0;
    (void)parse_u64(field("fills_received"), fills);
    state.fills_received = static_cast<std::size_t>(fills);
    std::uint64_t orders = 0;
    (void)parse_u64(field("orders_submitted"), orders);
    state.orders_submitted = static_cast<std::size_t>(orders);

    // An empty string means the timestamp was never set, which is a valid state
    // rather than a parse failure.
    if (const auto saved = field("saved_at"); saved.has_value() && !saved->empty()) {
        (void)parse_timestamp(*saved, state.saved_at);
    }
    if (const auto last = field("last_event_time"); last.has_value() && !last->empty()) {
        (void)parse_timestamp(*last, state.last_event_time);
    }

    const auto read_double = [&field](std::string_view key) -> double {
        const auto text = field(key);
        if (!text.has_value() || text->empty() || *text == "null") return 0.0;
        try {
            return std::stod(*text);
        } catch (...) {
            return 0.0;
        }
    };
    state.account.cash = Notional{read_double("cash")};
    state.account.equity = Notional{read_double("equity")};

    const auto read_array = [json](std::string_view key, auto&& per_object) {
        const std::string needle = "\"" + std::string{key} + "\": [";
        const auto pos = json.find(needle);
        if (pos == std::string_view::npos) return;
        auto cursor = pos + needle.size();
        while (cursor < json.size() && json[cursor] != ']') {
            const auto open = json.find('{', cursor);
            if (open == std::string_view::npos) break;
            const auto close = json.find('}', open);
            if (close == std::string_view::npos) break;
            per_object(json.substr(open, close - open + 1));
            cursor = close + 1;
            const auto next = json.find_first_not_of(" ,", cursor);
            if (next == std::string_view::npos || json[next] == ']') break;
            cursor = next;
        }
    };

    const auto object_field = [](std::string_view object, std::string_view key) -> std::string {
        const std::string needle = "\"" + std::string{key} + "\":";
        const auto pos = object.find(needle);
        if (pos == std::string_view::npos) return {};
        auto cursor = object.find_first_not_of(" \t", pos + needle.size());
        const auto end = object.find_first_of(",}", cursor);
        return std::string{object.substr(cursor, end - cursor)};
    };

    read_array("positions", [&](std::string_view object) {
        SessionState::PersistedPosition p;
        try {
            p.instrument =
                static_cast<std::uint32_t>(std::stoul(object_field(object, "instrument")));
            p.quantity = std::stod(object_field(object, "quantity"));
            p.average_cost = std::stod(object_field(object, "average_cost"));
            p.realized_pnl = std::stod(object_field(object, "realized_pnl"));
        } catch (...) {
            return;
        }
        state.positions.push_back(p);
    });

    read_array("working_orders", [&](std::string_view object) {
        SessionState::PersistedOrder o;
        try {
            o.order_id = std::stoull(object_field(object, "order_id"));
            o.instrument =
                static_cast<std::uint32_t>(std::stoul(object_field(object, "instrument")));
            o.side = static_cast<std::uint8_t>(std::stoi(object_field(object, "side")));
            o.quantity = std::stod(object_field(object, "quantity"));
            o.filled = std::stod(object_field(object, "filled"));
        } catch (...) {
            return;
        }
        state.working_orders.push_back(o);
    });

    std::uint64_t stored = 0;
    if (!parse_u64(field("checksum"), stored, 16)) {
        return fail(bad("session state has no checksum; it may be truncated"));
    }
    if (stored != state.checksum()) {
        // REFUSED, not repaired. Resuming from damaged data is worse than
        // refusing, because the run would look complete.
        return fail(bad("session state checksum mismatch; the file is corrupt", state.session_id));
    }
    return state;
}

PaperSession::PaperSession(SessionConfig config, IClock& clock, market::IMarketDataSource& source,
                           engine::IStrategy& strategy, execution::BrokerSimulator& simulator,
                           PaperBroker& broker, portfolio::Portfolio& portfolio,
                           oms::OrderManager& oms, risk::RiskManager& risk,
                           accounting::Journal& journal, storage::ArtifactStore& artifacts,
                           const market::Calendar* calendar)
    : config_(std::move(config)),
      clock_(&clock),
      source_(&source),
      strategy_(&strategy),
      simulator_(&simulator),
      broker_(&broker),
      portfolio_(&portfolio),
      oms_(&oms),
      risk_(&risk),
      journal_(&journal),
      artifacts_(&artifacts),
      calendar_(calendar),
      account_(portfolio, config_.margin) {}

SessionState PaperSession::capture() const {
    SessionState state;
    state.session_id = config_.session_id;
    state.sequence = sequence_;
    state.saved_at = clock_->now();
    state.phase = phase_;
    state.config_fingerprint = config_.config_fingerprint;
    state.events_processed = stats_.events_processed;
    state.last_event_time = last_event_;
    state.fills_received = stats_.fills_received;
    state.orders_submitted = stats_.orders_submitted;
    state.account = account_.snapshot(clock_->now());

    // READ-ONLY over the portfolio and OMS. Capturing state must never perturb
    // what it captures, or the snapshot describes a book that never existed.
    for (const auto& [key, position] : portfolio_->positions()) {
        if (position.is_flat()) continue;
        SessionState::PersistedPosition p;
        p.instrument = key;
        p.quantity = position.quantity().get();
        p.average_cost = position.average_cost().get();
        p.realized_pnl = position.realized_pnl().get();
        state.positions.push_back(p);
    }

    for (const auto order_id : oms_->working()) {
        const auto* record = oms_->find(order_id);
        if (record == nullptr) continue;
        SessionState::PersistedOrder o;
        o.order_id = oms::value_of(order_id);
        o.instrument = index_of(record->order.instrument());
        o.side = static_cast<std::uint8_t>(record->order.side());
        o.quantity = record->order.quantity().get();
        o.filled = record->filled_quantity.get();
        state.working_orders.push_back(o);
    }
    return state;
}

Result<bool> PaperSession::recover() {
    const std::string key = "paper/" + config_.session_id + "/state";
    if (!artifacts_->contains(key)) return true;  // nothing to recover

    auto json = artifacts_->get(key);
    if (!json) return fail(json.error());
    auto state = SessionState::from_json(*json);
    if (!state) return fail(state.error());

    if (config_.require_matching_config && config_.config_fingerprint != 0 &&
        state->config_fingerprint != config_.config_fingerprint) {
        // Resuming a book under a configuration that did not create it mixes
        // two runs, and every attribution afterwards is wrong.
        std::ostringstream ss;
        ss << std::hex << "refusing to resume: state was written under config "
           << state->config_fingerprint << " but this session is " << config_.config_fingerprint;
        return fail(bad(ss.str()));
    }

    sequence_ = state->sequence;
    stats_.events_processed = state->events_processed;
    stats_.fills_received = state->fills_received;
    stats_.orders_submitted = state->orders_submitted;
    last_event_ = state->last_event_time;
    return true;
}

Result<bool> PaperSession::start() {
    if (phase_ != SessionPhase::Created) {
        return fail(bad("session has already been started", config_.session_id));
    }
    phase_ = SessionPhase::Starting;

    if (auto connected = broker_->connect(); !connected) {
        phase_ = SessionPhase::Failed;
        return fail(connected.error());
    }
    if (auto recovered = recover(); !recovered) {
        phase_ = SessionPhase::Failed;
        return fail(recovered.error());
    }

    // Construct and begin the SAME engine a backtest uses. The session never
    // dispatches events to the strategy itself; doing so would be the second
    // trading implementation this architecture forbids.
    engine_.emplace(*clock_, *source_, *strategy_, *simulator_, *portfolio_, *oms_, *risk_,
                    *journal_, calendar_);
    if (auto begun = engine_->begin(); !begun) {
        phase_ = SessionPhase::Failed;
        return fail(begun.error());
    }

    stats_.started_at = clock_->now();
    current_session_date_ = utc_date_floor(clock_->now());
    phase_ = SessionPhase::Running;
    return true;
}

void PaperSession::pause(std::string reason) {
    if (is_terminal(phase_)) return;
    // Paused, not stopped: data keeps flowing so the book stays marked and the
    // equity curve has no hole. Only NEW ORDERS stop.
    phase_ = SessionPhase::Paused;
    pause_reason_ = std::move(reason);
    ++stats_.pauses;
    broker_->disconnect();
}

Result<bool> PaperSession::resume() {
    if (phase_ != SessionPhase::Paused) {
        return fail(
            bad("only a paused session can resume; this one is " + std::string{to_string(phase_)}));
    }
    if (auto connected = broker_->connect(); !connected) return fail(connected.error());
    pause_reason_.clear();
    phase_ = SessionPhase::Running;
    return true;
}

Result<bool> PaperSession::persist() {
    auto state = capture();
    state.sequence = ++sequence_;
    auto written = artifacts_->put("paper/" + config_.session_id + "/state", state.to_json());
    if (!written) return fail(written.error());
    ++stats_.persists;
    return true;
}

Result<bool> PaperSession::daily_reset(Timestamp now) {
    // Persist BEFORE resetting, so the day's closing state survives whatever
    // the reset clears.
    if (auto saved = persist(); !saved) return saved;
    // RiskManager::reset() rolls the daily budget and also clears its own
    // rejection tallies. That is acceptable here because the session's
    // cumulative counts come from the engine summary, which begin() resets once
    // and nothing else touches.
    risk_->reset();
    current_session_date_ = utc_date_floor(now);
    ++stats_.daily_resets;
    return true;
}

Result<std::size_t> PaperSession::step(std::size_t max_events) {
    if (phase_ != SessionPhase::Running && phase_ != SessionPhase::Paused) {
        return fail(bad("session is not running; it is " + std::string{to_string(phase_)}));
    }
    if (!engine_.has_value()) return fail(bad("session has not been started"));

    std::size_t processed = 0;
    for (std::size_t i = 0; i < max_events && !stop_requested_; ++i) {
        // ONE EVENT AT A TIME THROUGH THE ENGINE, which dispatches to the
        // strategy, runs the risk gate, drives the OMS and matches against the
        // simulator exactly as in a backtest. The session's work happens
        // BETWEEN events, never instead of them.
        auto stepped = engine_->step(1);
        if (!stepped) {
            phase_ = SessionPhase::Failed;
            return fail(stepped.error());
        }
        if (*stepped == 0) break;  // source exhausted
        ++processed;

        const auto& summary = engine_->summary();
        stats_.events_processed = summary.events_processed;
        stats_.fills_received = summary.fills;
        stats_.orders_submitted = summary.orders_submitted;
        stats_.orders_rejected = summary.orders_rejected;
        last_event_ = summary.last_event;

        const std::int64_t ns = last_event_.time_since_epoch().count();
        hash_bytes(content_hash_, &ns, sizeof(ns));

        // Daily boundary detected from the EVENT STREAM, not a wall clock, so a
        // replay of a recorded session resets at the same points.
        if (config_.daily_reset && is_set(last_event_)) {
            const Timestamp day = utc_date_floor(last_event_);
            if (is_set(current_session_date_) && day > current_session_date_) {
                if (auto reset = daily_reset(last_event_); !reset) return fail(reset.error());
            }
        }

        if (config_.persist_every_events > 0 &&
            stats_.events_processed % config_.persist_every_events == 0) {
            if (auto saved = persist(); !saved) return fail(saved.error());
        }
    }
    return processed;
}

Result<SessionStats> PaperSession::run() {
    if (phase_ == SessionPhase::Created) {
        if (auto started = start(); !started) return fail(started.error());
    }
    while (!stop_requested_ && !is_terminal(phase_)) {
        auto processed = step(64);
        if (!processed) return fail(processed.error());
        if (*processed == 0) break;
    }
    if (auto stopped = shutdown(); !stopped) return fail(stopped.error());
    return stats_;
}

Result<bool> PaperSession::shutdown() {
    if (is_terminal(phase_)) return true;
    phase_ = SessionPhase::Stopping;

    // finish() runs on_stop, matches trades and reconciles the journal -- the
    // same close-out a backtest performs.
    if (engine_.has_value()) {
        if (auto finished = engine_->finish(); !finished) {
            phase_ = SessionPhase::Failed;
            return fail(finished.error());
        }
    }
    // Persist AFTER the close-out, so the saved state reflects the final
    // reconciled book rather than one mid-close.
    if (auto saved = persist(); !saved) {
        phase_ = SessionPhase::Failed;
        return fail(saved.error());
    }

    broker_->disconnect();
    stats_.stopped_at = clock_->now();
    phase_ = SessionPhase::Stopped;
    return true;
}

AccountSnapshot PaperSession::account_snapshot() const {
    return account_.snapshot(clock_->now());
}

}  // namespace ptl::paper
