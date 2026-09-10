#include "session_host.hpp"

#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

#include "ptl/market/bar.hpp"
#include "ptl/market/calendar.hpp"
#include "ptl/oms/order.hpp"
#include "ptl/oms/order_manager.hpp"
#include "ptl/paper/account.hpp"
#include "ptl/paper/broker.hpp"
#include "ptl/portfolio/portfolio.hpp"
#include "ptl/risk/risk_manager.hpp"

namespace ptl_host {
namespace {

[[nodiscard]] ptl::Error bad(std::string message) {
    return ptl::make_error(ptl::ErrorCode::ValidationFailed, std::move(message));
}

/// Round-trip exact, matching the engine's own serializers. Two decimals would
/// look tidier and would silently change the numbers a client reads back.
[[nodiscard]] std::string num(double v) {
    if (!ptl::is_finite(v)) return "null";
    std::ostringstream ss;
    ss << std::setprecision(std::numeric_limits<double>::max_digits10) << v;
    return ss.str();
}

[[nodiscard]] std::string iso_or_empty(ptl::Timestamp ts) {
    // to_iso8601 on an unset timestamp overflows int64 days, so an unset value
    // becomes an empty string rather than a date in 1677.
    return ptl::is_set(ts) ? ptl::to_iso8601(ts) : std::string{};
}

[[nodiscard]] std::string escape(std::string_view in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (const char c : in) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    return out;
}

constexpr ptl::InstrumentId kInstrument{0};

}  // namespace

std::string_view to_string(HostState s) noexcept {
    switch (s) {
        case HostState::Stopped:  return "STOPPED";
        case HostState::Starting: return "STARTING";
        case HostState::Running:  return "RUNNING";
        case HostState::Stopping: return "STOPPING";
        case HostState::Error:    return "ERROR";
    }
    return "ERROR";
}

// ---------------------------------------------------------------------------
// The strategy
// ---------------------------------------------------------------------------

namespace {

/// A deterministic demonstration strategy.
///
/// It has no edge and is not meant to. It exists so the session produces
/// orders, fills and positions for the interface to display. Every decision is
/// a pure function of the bar count, so two runs of the same replay are
/// identical.
class HostStrategy final : public ptl::engine::IStrategy {
public:
    struct FillRecord {
        ptl::Timestamp ts{ptl::kNoTimestamp};
        std::uint64_t  order_id = 0;
        std::uint32_t  instrument = 0;
        int            side = 0;
        double         quantity = 0.0;
        double         price = 0.0;
        double         commission = 0.0;
    };

    [[nodiscard]] std::string_view name() const noexcept override {
        return "host_demo";
    }

    [[nodiscard]] ptl::Result<bool> on_start(
        const ptl::engine::StrategyContext&) override {
        bars_ = 0;
        fills_.clear();
        return true;
    }

    void on_bar(const ptl::market::Bar& bar,
                const ptl::engine::StrategyContext& ctx,
                ptl::engine::OrderSink& sink) override {
        ++bars_;
        if (bars_ % 15 != 0) return;

        const bool holding = ctx.position_of(bar.instrument()).get() > 0.0;
        const ptl::Side side = holding ? ptl::Side::Sell : ptl::Side::Buy;

        ptl::LifecycleTimes times;
        // From the EVENT, never a clock. A strategy that reads the wall clock
        // cannot be replayed.
        times.decision_time = bar.close_time();

        auto order = ptl::oms::Order::market(sink.next_order_id(), bar.instrument(),
                                             side, ptl::Qty{25.0}, times);
        if (!order) return;
        (void)sink.submit(order->with_arrival_price(bar.close()));
    }

    void on_fill(const ptl::oms::Fill& fill,
                 const ptl::engine::StrategyContext&) override {
        FillRecord record;
        record.ts = fill.fill_time();
        record.order_id = ptl::oms::value_of(fill.order_id());
        record.instrument = ptl::index_of(fill.instrument());
        record.side = fill.side() == ptl::Side::Buy ? 1 : -1;
        record.quantity = fill.quantity().get();
        record.price = fill.price().get();
        record.commission = fill.commission().get();

        fills_.push_back(record);
        // BOUNDED. A session running for days would otherwise grow without
        // limit; the interface only ever shows the most recent fills.
        if (fills_.size() > kMaxFills) fills_.pop_front();
    }

    [[nodiscard]] const std::deque<FillRecord>& fills() const noexcept {
        return fills_;
    }

private:
    static constexpr std::size_t kMaxFills = 200;
    std::size_t                  bars_ = 0;
    std::deque<FillRecord>       fills_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct PaperSessionHost::Impl {
    ptl::market::Calendar                          calendar;
    std::vector<ptl::market::MarketEvent>          events;
    ptl::SimulatedClock                            clock;
    std::unique_ptr<ptl::market::ReplaySource>     source;
    ptl::execution::StandardCostModel              costs;
    ptl::execution::StandardLatencyModel           latency;
    std::unique_ptr<ptl::execution::BrokerSimulator> simulator;
    ptl::portfolio::Portfolio                      portfolio;
    ptl::oms::OrderManager                         oms;
    ptl::risk::RiskManager                         risk;
    ptl::accounting::Journal                       journal;
    ptl::storage::ArtifactStore                    artifacts;
    HostStrategy                                   strategy;
    std::unique_ptr<ptl::paper::PaperAccount>      account;
    std::unique_ptr<ptl::paper::PaperBroker>       broker;
    std::unique_ptr<ptl::paper::PaperSession>      session;

    Impl(ptl::market::Calendar cal, ptl::risk::RiskLimits limits,
         ptl::portfolio::PortfolioConfig portfolio_config, std::string artifact_root)
        : calendar(std::move(cal)),
          portfolio(portfolio_config),
          risk(limits),
          artifacts(std::move(artifact_root)) {}
};

PaperSessionHost::PaperSessionHost() = default;
PaperSessionHost::~PaperSessionHost() = default;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

ptl::Result<bool> PaperSessionHost::start(const StartOptions& options) {
    if (state_ == HostState::Running || state_ == HostState::Starting) {
        // 409 territory: the caller believes no session is running and is wrong.
        return ptl::fail(bad("a session is already " +
                             std::string{to_string(state_)}));
    }
    if (state_ == HostState::Stopping) {
        return ptl::fail(bad("a session is still stopping"));
    }

    state_ = HostState::Starting;
    options_ = options;
    last_error_.clear();

    auto calendar = ptl::market::Calendar::build(
        ptl::market::Calendar::us_equities_spec(), 2024, 2024);
    if (!calendar) {
        state_ = HostState::Error;
        last_error_ = calendar.error().message;
        return ptl::fail(calendar.error());
    }

    ptl::Timestamp date{};
    (void)ptl::parse_timestamp("2024-07-02", date);
    const auto day = calendar->session_on(date);
    if (!day) {
        state_ = HostState::Error;
        last_error_ = "the reference date is not a trading day";
        return ptl::fail(bad(last_error_));
    }

    ptl::risk::RiskLimits limits;
    limits.max_concentration = 1.0;
    limits.max_daily_turnover = 100.0;

    ptl::portfolio::PortfolioConfig portfolio_config{
        ptl::Notional{options.starting_cash}};

    auto impl = std::make_unique<Impl>(std::move(*calendar), limits, portfolio_config,
                                       options.artifact_root);

    // --- deterministic replay data -----------------------------------------
    // Seeded from the session config so a given seed always produces the same
    // session. Labelled as synthetic wherever it surfaces.
    const double drift = 0.02 / static_cast<double>(options.bars);
    ptl::Timestamp t = day->open;
    for (std::size_t i = 0; i < options.bars; ++i) {
        // The session is half-open, so a bar closing exactly on the close is
        // outside it.
        if (t + std::chrono::minutes{1} >= day->close) break;

        const double phase = static_cast<double>(i) +
                             static_cast<double>(options.seed % 997);
        const double px = 500.0 * (1.0 + drift * static_cast<double>(i)) +
                          std::sin(phase * 0.11) * 3.0;
        auto bar = ptl::market::Bar::from_left_edge(
            kInstrument, t, std::chrono::minutes{1}, ptl::Price{px},
            ptl::Price{px + 0.05}, ptl::Price{px - 0.05}, ptl::Price{px},
            ptl::Volume{50'000.0});
        if (!bar) {
            state_ = HostState::Error;
            last_error_ = bar.error().message;
            return ptl::fail(bar.error());
        }
        impl->events.emplace_back(*bar);
        t += std::chrono::minutes{1};
    }

    auto with_sessions =
        ptl::market::with_session_events(std::move(impl->events), impl->calendar);
    if (!with_sessions) {
        state_ = HostState::Error;
        last_error_ = with_sessions.error().message;
        return ptl::fail(with_sessions.error());
    }
    impl->events = std::move(*with_sessions);

    auto source = ptl::market::ReplaySource::create(impl->events, &impl->clock);
    if (!source) {
        state_ = HostState::Error;
        last_error_ = source.error().message;
        return ptl::fail(source.error());
    }
    impl->source = std::make_unique<ptl::market::ReplaySource>(std::move(*source));

    impl->simulator = std::make_unique<ptl::execution::BrokerSimulator>(
        impl->clock, impl->costs, impl->latency,
        ptl::DeterministicRng{options.seed});

    impl->account = std::make_unique<ptl::paper::PaperAccount>(impl->portfolio);

    ptl::paper::PaperBrokerConfig broker_config;
    // The venue-side buying power screen is off: the risk engine already gates
    // every order, and a second screen here would reject orders for reasons the
    // risk report never sees.
    broker_config.enforce_buying_power = false;
    impl->broker = std::make_unique<ptl::paper::PaperBroker>(
        impl->clock, *impl->simulator, *impl->account, broker_config);

    ptl::paper::SessionConfig session_config;
    session_config.session_id = options.session_id;
    session_config.persist_every_events = 50;

    impl->session = std::make_unique<ptl::paper::PaperSession>(
        session_config, impl->clock, *impl->source, impl->strategy, *impl->simulator,
        *impl->broker, impl->portfolio, impl->oms, impl->risk, impl->journal,
        impl->artifacts, &impl->calendar);

    if (auto started = impl->session->start(); !started) {
        state_ = HostState::Error;
        last_error_ = started.error().message;
        return ptl::fail(started.error());
    }

    impl_ = std::move(impl);
    state_ = HostState::Running;
    return true;
}

ptl::Result<bool> PaperSessionHost::stop() {
    if (state_ != HostState::Running) {
        return ptl::fail(bad("no running session to stop; state is " +
                             std::string{to_string(state_)}));
    }
    state_ = HostState::Stopping;

    if (impl_ && impl_->session) {
        // shutdown() runs on_stop, matches trades and reconciles the journal --
        // the same close-out a backtest performs. Skipping it would leave the
        // final state unreconciled.
        if (auto stopped = impl_->session->shutdown(); !stopped) {
            state_ = HostState::Error;
            last_error_ = stopped.error().message;
            return ptl::fail(stopped.error());
        }
    }

    // The session is destroyed in a defined order: session, then broker, then
    // account, then the rest. Each borrows the ones below it.
    if (impl_) {
        impl_->session.reset();
        impl_->broker.reset();
        impl_->account.reset();
    }
    impl_.reset();
    state_ = HostState::Stopped;
    return true;
}

ptl::Result<std::size_t> PaperSessionHost::step(std::size_t max_events) {
    if (state_ != HostState::Running || !impl_ || !impl_->session) {
        return ptl::fail(bad("no running session to step"));
    }
    auto processed = impl_->session->step(max_events);
    if (!processed) {
        state_ = HostState::Error;
        last_error_ = processed.error().message;
        return ptl::fail(processed.error());
    }
    return *processed;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

std::string PaperSessionHost::state_json() const {
    std::ostringstream ss;
    ss << "{\"state\": \"" << to_string(state_) << "\", \"has_session\": "
       << (impl_ && impl_->session ? "true" : "false") << ", \"session_id\": \""
       << escape(options_.session_id) << "\", \"seed\": " << options_.seed
       << ", \"data_source\": \"synthetic-replay\"";

    if (!last_error_.empty()) {
        ss << ", \"error\": \"" << escape(last_error_) << '"';
    }

    if (impl_ && impl_->session) {
        const auto& stats = impl_->session->stats();
        ss << ", \"phase\": \"" << ptl::paper::to_string(impl_->session->phase())
           << "\", \"trading_permitted\": "
           << (impl_->session->trading_permitted() ? "true" : "false")
           << ", \"events_processed\": " << stats.events_processed
           << ", \"orders_submitted\": " << stats.orders_submitted
           << ", \"orders_rejected\": " << stats.orders_rejected
           << ", \"fills_received\": " << stats.fills_received
           << ", \"persists\": " << stats.persists
           << ", \"started_at\": \"" << iso_or_empty(stats.started_at) << '"';
    }
    ss << '}';
    return ss.str();
}

std::string PaperSessionHost::portfolio_json() const {
    if (!impl_ || !impl_->session) return R"({"available": false})";

    const auto snapshot = impl_->session->account_snapshot();
    std::ostringstream ss;
    ss << "{\"available\": true, \"account\": " << snapshot.to_json() << '}';
    return ss.str();
}

std::string PaperSessionHost::positions_json() const {
    if (!impl_) return R"({"available": false, "positions": []})";

    std::ostringstream ss;
    ss << "{\"available\": true, \"positions\": [";
    bool first = true;
    for (const auto& [key, position] : impl_->portfolio.positions()) {
        if (position.is_flat()) continue;
        if (!first) ss << ", ";
        first = false;
        ss << "{\"instrument\": " << key
           << ", \"quantity\": " << num(position.quantity().get())
           << ", \"average_cost\": " << num(position.average_cost().get())
           << ", \"realized_pnl\": " << num(position.realized_pnl().get()) << '}';
    }
    ss << "]}";
    return ss.str();
}

std::string PaperSessionHost::orders_json() const {
    if (!impl_) return R"({"available": false, "orders": []})";

    std::ostringstream ss;
    ss << "{\"available\": true, \"orders\": [";
    bool first = true;
    for (const auto id : impl_->oms.working()) {
        const auto* record = impl_->oms.find(id);
        if (record == nullptr) continue;
        if (!first) ss << ", ";
        first = false;
        ss << "{\"order_id\": " << ptl::oms::value_of(id)
           << ", \"instrument\": " << ptl::index_of(record->order.instrument())
           << ", \"side\": " << (record->order.side() == ptl::Side::Buy ? 1 : -1)
           << ", \"quantity\": " << num(record->order.quantity().get())
           << ", \"filled\": " << num(record->filled_quantity.get()) << '}';
    }
    ss << "]}";
    return ss.str();
}

std::string PaperSessionHost::fills_json() const {
    if (!impl_) return R"({"available": false, "fills": []})";

    std::ostringstream ss;
    ss << "{\"available\": true, \"fills\": [";
    const auto& fills = impl_->strategy.fills();
    bool        first = true;
    // Most recent first: an operator watching a session cares about the last
    // fill, not the first.
    for (auto it = fills.rbegin(); it != fills.rend(); ++it) {
        if (!first) ss << ", ";
        first = false;
        ss << "{\"ts\": \"" << iso_or_empty(it->ts) << "\", \"order_id\": "
           << it->order_id << ", \"instrument\": " << it->instrument
           << ", \"side\": " << it->side << ", \"quantity\": " << num(it->quantity)
           << ", \"price\": " << num(it->price)
           << ", \"commission\": " << num(it->commission) << '}';
    }
    ss << "]}";
    return ss.str();
}

std::string PaperSessionHost::snapshot_json() const {
    // One document, so a dashboard renders a consistent picture from a single
    // read. Assembling it from four separate calls would let the account and
    // the positions come from different instants.
    std::ostringstream ss;
    ss << "{\"state\": " << state_json() << ", \"portfolio\": " << portfolio_json()
       << ", \"positions\": " << positions_json() << ", \"orders\": " << orders_json()
       << ", \"fills\": " << fills_json() << '}';
    return ss.str();
}

}  // namespace ptl_host
