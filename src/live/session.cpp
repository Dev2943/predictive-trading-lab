#include "ptl/live/session.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace ptl::live {
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

/// An unset timestamp serializes as an empty string, never as a date:
/// to_iso8601 on kNoTimestamp overflows int64 days.
[[nodiscard]] std::string iso_or_empty(Timestamp ts) {
    return is_set(ts) ? to_iso8601(ts) : std::string{};
}

}  // namespace

// ---------------------------------------------------------------------------
// LiveMarketDataAdapter
// ---------------------------------------------------------------------------

std::optional<market::MarketEvent> LiveMarketDataAdapter::next() {
    if (buffer_.empty()) return std::nullopt;
    auto event = std::move(buffer_.front());
    buffer_.pop_front();
    return event;
}

Timestamp LiveMarketDataAdapter::peek_time() const noexcept {
    if (buffer_.empty()) return kNoTimestamp;
    return market::exchange_time_of(buffer_.front());
}

Result<std::size_t> LiveMarketDataAdapter::pump() {
    std::size_t produced = 0;

    for (const auto& message : connection_->poll()) {
        if (message.kind != MessageKind::Quote && message.kind != MessageKind::Trade) {
            continue;  // order and account traffic belongs to the broker
        }

        const Timestamp ts = is_set(message.venue_time) ? message.venue_time : clock_->now();

        // MONOTONICITY IS ENFORCED HERE. The engine assumes non-decreasing
        // event time, and a venue that delivers a late quote out of order would
        // otherwise walk the clock backwards -- which breaks every rolling
        // feature downstream.
        if (is_set(last_event_time_) && ts < last_event_time_) {
            ++dropped_stale_;
            continue;
        }

        if (message.kind == MessageKind::Quote) {
            // The EXISTING quote model. No new market structure is created, so
            // nothing downstream can tell a live quote from a replayed one.
            // Argument order is (bid, bid_size, ask, ask_size) -- interleaved,
            // not grouped. The named types refused the grouped form at compile
            // time, which is exactly what they exist for.
            auto quote = market::Quote::create(message.instrument, ts, message.bid,
                                               message.bid_size, message.ask, message.ask_size);
            if (!quote) return fail(quote.error());
            buffer_.emplace_back(*quote);
        } else {
            auto trade =
                market::Trade::create(message.instrument, ts, message.price, message.quantity);
            if (!trade) return fail(trade.error());
            buffer_.emplace_back(*trade);
        }
        last_event_time_ = ts;
        ++produced;
    }
    return produced;
}

// ---------------------------------------------------------------------------
// Phase
// ---------------------------------------------------------------------------

std::string_view to_string(LiveSessionPhase p) noexcept {
    switch (p) {
        case LiveSessionPhase::Created:
            return "created";
        case LiveSessionPhase::Connecting:
            return "connecting";
        case LiveSessionPhase::Reconciling:
            return "reconciling";
        case LiveSessionPhase::Running:
            return "running";
        case LiveSessionPhase::Recovering:
            return "recovering";
        case LiveSessionPhase::Halted:
            return "halted";
        case LiveSessionPhase::Stopping:
            return "stopping";
        case LiveSessionPhase::Stopped:
            return "stopped";
        case LiveSessionPhase::Failed:
            return "failed";
    }
    return "unknown";
}

bool is_terminal(LiveSessionPhase p) noexcept {
    return p == LiveSessionPhase::Stopped || p == LiveSessionPhase::Failed;
}

std::string LiveSessionStats::describe() const {
    std::ostringstream ss;
    ss << "live session over " << events_processed << " events\n";
    ss << "  orders          " << orders_submitted << '\n';
    ss << "  fills           " << fills_received << '\n';
    ss << "  reconnects      " << reconnects << '\n';
    ss << "  reconciliations " << reconciliations << " (" << reconciliation_failures
       << " failed)\n";
    ss << "  halts           " << halts << '\n';
    ss << "  persists        " << persists << '\n';
    return ss.str();
}

// ---------------------------------------------------------------------------
// LiveSessionState
// ---------------------------------------------------------------------------

std::uint64_t LiveSessionState::checksum() const {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    hash_string(h, session_id);
    hash_bytes(h, &sequence, sizeof(sequence));
    hash_bytes(h, &config_fingerprint, sizeof(config_fingerprint));
    hash_bytes(h, &events_processed, sizeof(events_processed));
    hash_bytes(h, &last_venue_sequence, sizeof(last_venue_sequence));

    const double cash = account.cash.get();
    hash_bytes(h, &cash, sizeof(cash));
    const double equity = account.equity.get();
    hash_bytes(h, &equity, sizeof(equity));

    for (const auto& [client_id, order_id] : tracked_orders) {
        hash_string(h, client_id);
        hash_bytes(h, &order_id, sizeof(order_id));
    }
    return h;
}

std::string LiveSessionState::to_json() const {
    std::ostringstream ss;
    ss << "{\"session_id\": \"" << json_escape(session_id) << "\", \"sequence\": " << sequence
       << ", \"phase\": \"" << to_string(phase) << "\", \"saved_at\": \"" << iso_or_empty(saved_at)
       << "\", \"config_fingerprint\": \"" << std::hex << std::setw(16) << std::setfill('0')
       << config_fingerprint << std::dec << "\", \"events_processed\": " << events_processed
       << ", \"last_venue_sequence\": " << last_venue_sequence << ", \"last_sync_point\": \""
       << iso_or_empty(last_sync_point) << "\", \"account\": " << account.to_json()
       << ", \"tracked_orders\": [";
    for (std::size_t i = 0; i < tracked_orders.size(); ++i) {
        if (i != 0) ss << ", ";
        ss << "{\"client_order_id\": \"" << json_escape(tracked_orders[i].first)
           << "\", \"order_id\": " << tracked_orders[i].second << '}';
    }
    ss << "], \"checksum\": \"" << std::hex << std::setw(16) << std::setfill('0') << checksum()
       << std::dec << "\"}";
    return ss.str();
}

Result<LiveSessionState> LiveSessionState::from_json(std::string_view json) {
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

    LiveSessionState state;
    const auto id = field("session_id");
    if (!id.has_value()) return fail(bad("live state has no session_id"));
    state.session_id = *id;

    if (!parse_u64(field("sequence"), state.sequence)) {
        return fail(bad("live state has an unreadable sequence"));
    }
    if (!parse_u64(field("config_fingerprint"), state.config_fingerprint, 16)) {
        return fail(bad("live state has an unreadable config fingerprint"));
    }
    std::uint64_t events = 0;
    if (!parse_u64(field("events_processed"), events)) {
        return fail(bad("live state has an unreadable event count"));
    }
    state.events_processed = static_cast<std::size_t>(events);
    (void)parse_u64(field("last_venue_sequence"), state.last_venue_sequence);

    if (const auto saved = field("saved_at"); saved.has_value() && !saved->empty()) {
        (void)parse_timestamp(*saved, state.saved_at);
    }
    if (const auto sync = field("last_sync_point"); sync.has_value() && !sync->empty()) {
        (void)parse_timestamp(*sync, state.last_sync_point);
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

    // Tracked orders must be restored exactly, or a venue reply after a restart
    // cannot be matched to the order it belongs to.
    const std::string needle = "\"tracked_orders\": [";
    if (const auto pos = json.find(needle); pos != std::string_view::npos) {
        auto cursor = pos + needle.size();
        while (cursor < json.size() && json[cursor] != ']') {
            const auto open = json.find('{', cursor);
            if (open == std::string_view::npos) break;
            const auto close = json.find('}', open);
            if (close == std::string_view::npos) break;
            const auto object = json.substr(open, close - open + 1);

            const auto id_start = object.find("\"client_order_id\": \"");
            const auto order_start = object.find("\"order_id\": ");
            if (id_start != std::string_view::npos && order_start != std::string_view::npos) {
                const auto value_start = id_start + 20;
                const auto value_end = object.find('"', value_start);
                std::uint64_t order_id = 0;
                const auto* first = object.data() + order_start + 12;
                const auto* last = object.data() + object.size() - 1;
                if (value_end != std::string_view::npos &&
                    std::from_chars(first, last, order_id).ec == std::errc{}) {
                    state.tracked_orders.emplace_back(
                        std::string{object.substr(value_start, value_end - value_start)}, order_id);
                }
            }
            cursor = close + 1;
            const auto next = json.find_first_not_of(" ,", cursor);
            if (next == std::string_view::npos || json[next] == ']') break;
            cursor = next;
        }
    }

    std::uint64_t stored = 0;
    if (!parse_u64(field("checksum"), stored, 16)) {
        return fail(bad("live state has no checksum; it may be truncated"));
    }
    if (stored != state.checksum()) {
        return fail(bad("live state checksum mismatch; the file is corrupt", state.session_id));
    }
    return state;
}

// ---------------------------------------------------------------------------
// LiveSession
// ---------------------------------------------------------------------------

LiveSession::LiveSession(LiveSessionConfig config, IClock& clock, LiveConnection& connection,
                         LiveMarketDataAdapter& market_data, LiveBroker& broker,
                         engine::IStrategy& strategy, execution::BrokerSimulator& simulator,
                         portfolio::Portfolio& portfolio, oms::OrderManager& oms,
                         risk::RiskManager& risk, accounting::Journal& journal,
                         storage::ArtifactStore& artifacts, const market::Calendar* calendar)
    : config_(std::move(config)),
      clock_(&clock),
      connection_(&connection),
      market_data_(&market_data),
      broker_(&broker),
      strategy_(&strategy),
      simulator_(&simulator),
      portfolio_(&portfolio),
      oms_(&oms),
      risk_(&risk),
      journal_(&journal),
      artifacts_(&artifacts),
      calendar_(calendar) {}

bool LiveSession::trading_permitted() const noexcept {
    // BOTH must agree. A running session on a degraded connection must not
    // trade, and a healthy connection under a halt must not either.
    return phase_ == LiveSessionPhase::Running && connection_->trading_permitted();
}

LiveSessionState LiveSession::capture() const {
    LiveSessionState state;
    state.session_id = config_.session_id;
    state.sequence = sequence_;
    state.saved_at = clock_->now();
    state.phase = phase_;
    state.config_fingerprint = config_.config_fingerprint;
    state.events_processed = stats_.events_processed;
    state.last_venue_sequence = connection_->last_sequence();
    state.last_sync_point = last_sync_;
    state.account = broker_->last_account();
    state.tracked_orders = broker_->listener().snapshot();
    return state;
}

Result<bool> LiveSession::recover() {
    const std::string key = "live/" + config_.session_id + "/state";
    if (!artifacts_->contains(key)) return true;

    auto json = artifacts_->get(key);
    if (!json) return fail(json.error());
    auto state = LiveSessionState::from_json(*json);
    if (!state) return fail(state.error());

    if (config_.config_fingerprint != 0 &&
        state->config_fingerprint != config_.config_fingerprint) {
        return fail(
            bad("refusing to resume a live session under a different "
                "configuration",
                config_.session_id));
    }

    sequence_ = state->sequence;
    stats_.events_processed = state->events_processed;
    last_sync_ = state->last_sync_point;
    // Sequence and tracked orders are restored so redelivered venue messages
    // are still recognised as duplicates, and a reply to an order placed before
    // the restart can still be matched.
    connection_->restore_sequence(state->last_venue_sequence);
    if (auto restored = broker_->listener().restore(state->tracked_orders); !restored) {
        return fail(restored.error());
    }
    return true;
}

Result<bool> LiveSession::start() {
    if (phase_ != LiveSessionPhase::Created) {
        return fail(bad("session has already been started", config_.session_id));
    }
    phase_ = LiveSessionPhase::Connecting;

    if (auto connected = connection_->connect(); !connected) {
        phase_ = LiveSessionPhase::Failed;
        return fail(connected.error());
    }
    if (auto recovered = recover(); !recovered) {
        phase_ = LiveSessionPhase::Failed;
        return fail(recovered.error());
    }

    // Synchronise BEFORE trading. The connection stays out of Ready until this
    // completes, so no order can be sent against a stale book.
    phase_ = LiveSessionPhase::Reconciling;
    if (auto synced = connection_->mark_synchronized(); !synced) {
        phase_ = LiveSessionPhase::Failed;
        return fail(synced.error());
    }
    last_sync_ = clock_->now();

    // The SAME engine a backtest and a paper session use.
    engine_.emplace(*clock_, *market_data_, *strategy_, *simulator_, *portfolio_, *oms_, *risk_,
                    *journal_, calendar_);
    if (auto begun = engine_->begin(); !begun) {
        phase_ = LiveSessionPhase::Failed;
        return fail(begun.error());
    }

    stats_.started_at = clock_->now();
    phase_ = LiveSessionPhase::Running;
    return true;
}

void LiveSession::halt(std::string reason) {
    if (is_terminal(phase_)) return;
    // Halted, not disconnected: data keeps flowing so the book stays marked and
    // the equity curve has no hole. Only ORDERS stop.
    phase_ = LiveSessionPhase::Halted;
    halt_reason_ = std::move(reason);
    ++stats_.halts;
}

Result<bool> LiveSession::resume() {
    if (phase_ != LiveSessionPhase::Halted && phase_ != LiveSessionPhase::Recovering) {
        return fail(bad("only a halted or recovering session can resume; this one is " +
                        std::string{to_string(phase_)}));
    }
    if (!connection_->trading_permitted()) {
        return fail(bad("connection is not ready; cannot resume trading"));
    }
    halt_reason_.clear();
    phase_ = LiveSessionPhase::Running;
    return true;
}

Result<ReconciliationReport> LiveSession::reconcile() {
    ++stats_.reconciliations;
    auto report = broker_->reconcile(broker_->last_account());
    last_sync_ = clock_->now();

    if (!report.clean()) {
        ++stats_.reconciliation_failures;
        if (config_.halt_on_reconciliation_failure) {
            // A book that disagrees with the venue is exactly when NOT to send
            // more orders. Halting is the conservative response and it is the
            // default.
            halt("reconciliation drift: " + report.describe());
        }
    }
    return report;
}

Result<bool> LiveSession::persist() {
    auto state = capture();
    state.sequence = ++sequence_;
    auto written = artifacts_->put("live/" + config_.session_id + "/state", state.to_json());
    if (!written) return fail(written.error());
    ++stats_.persists;
    return true;
}

Result<std::size_t> LiveSession::step(std::size_t max_events) {
    if (is_terminal(phase_)) {
        return fail(bad("session is " + std::string{to_string(phase_)}));
    }
    if (!engine_.has_value()) return fail(bad("session has not been started"));

    // Connection health is re-evaluated against the CLOCK every iteration, so a
    // silent venue is noticed even when no message arrives.
    connection_->tick();

    if (!connection_->trading_permitted() && phase_ == LiveSessionPhase::Running) {
        phase_ = LiveSessionPhase::Recovering;
    }
    if (phase_ == LiveSessionPhase::Recovering) {
        auto reconnected = connection_->maybe_reconnect();
        if (!reconnected) {
            phase_ = LiveSessionPhase::Failed;
            return fail(reconnected.error());
        }
        if (*reconnected) {
            ++stats_.reconnects;
            // Back to Reconciling, never straight to Running: the venue's book
            // may have moved while we were away.
            if (auto synced = connection_->mark_synchronized(); synced) {
                phase_ = LiveSessionPhase::Running;
                last_sync_ = clock_->now();
            }
        }
    }

    // Fills are collected BEFORE new events are dispatched, so the portfolio
    // reflects everything the venue has told us before the strategy sees the
    // next quote.
    auto fills = broker_->poll();
    if (!fills) return fail(fills.error());
    stats_.fills_received += fills->size();

    if (auto pumped = market_data_->pump(); !pumped) return fail(pumped.error());

    std::size_t processed = 0;
    for (std::size_t i = 0; i < max_events && !stop_requested_; ++i) {
        auto stepped = engine_->step(1);
        if (!stepped) {
            phase_ = LiveSessionPhase::Failed;
            return fail(stepped.error());
        }
        if (*stepped == 0) break;  // nothing buffered right now
        ++processed;

        const auto& summary = engine_->summary();
        stats_.events_processed = summary.events_processed;
        stats_.orders_submitted = summary.orders_submitted;
        last_event_ = summary.last_event;

        const std::int64_t ns = last_event_.time_since_epoch().count();
        hash_bytes(content_hash_, &ns, sizeof(ns));

        if (config_.reconcile_every_events > 0 &&
            stats_.events_processed % config_.reconcile_every_events == 0) {
            if (auto reconciled = reconcile(); !reconciled) {
                return fail(reconciled.error());
            }
        }
        if (config_.persist_every_events > 0 &&
            stats_.events_processed % config_.persist_every_events == 0) {
            if (auto saved = persist(); !saved) return fail(saved.error());
        }
    }
    return processed;
}

Result<LiveSessionStats> LiveSession::run(std::size_t max_iterations) {
    if (phase_ == LiveSessionPhase::Created) {
        if (auto started = start(); !started) return fail(started.error());
    }

    std::size_t iterations = 0;
    while (!stop_requested_ && !is_terminal(phase_)) {
        auto processed = step(64);
        if (!processed) return fail(processed.error());
        ++iterations;
        // A live loop has no natural end: an empty poll means "nothing yet",
        // not "the stream ended". The iteration cap is how a caller bounds a
        // run without inventing an end-of-stream that does not exist.
        if (max_iterations > 0 && iterations >= max_iterations) break;
        if (max_iterations == 0 && *processed == 0) break;
    }
    if (auto stopped = shutdown(); !stopped) return fail(stopped.error());
    return stats_;
}

Result<bool> LiveSession::shutdown() {
    if (is_terminal(phase_)) return true;
    phase_ = LiveSessionPhase::Stopping;

    // finish() runs on_stop, matches trades and reconciles the journal -- the
    // same close-out a backtest performs.
    if (engine_.has_value()) {
        if (auto finished = engine_->finish(); !finished) {
            phase_ = LiveSessionPhase::Failed;
            return fail(finished.error());
        }
    }
    if (auto saved = persist(); !saved) {
        phase_ = LiveSessionPhase::Failed;
        return fail(saved.error());
    }

    connection_->disconnect();
    stats_.stopped_at = clock_->now();
    phase_ = LiveSessionPhase::Stopped;
    return true;
}

}  // namespace ptl::live
