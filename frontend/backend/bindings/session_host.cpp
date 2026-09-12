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
#include "ptl/analytics/drawdown.hpp"
#include "ptl/core/instrument_table.hpp"
#include "ptl/risk/risk_manager.hpp"

namespace ptl_host {
namespace {

/// Cap on retained samples. A few hundred points is more than any chart
/// renders, and the bound is what keeps a long session from growing forever.
constexpr std::size_t kMaxHistory = 2000;

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
        // MANUAL REQUESTS FIRST, through the same sink the strategy uses. The
        // risk gate and OMS cannot tell a manual order from an automatic one,
        // which is the point: there is one submission path, not two.
        drain(bar, sink);

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
        if (auto submitted = sink.submit(order->with_arrival_price(bar.close()))) {
            submitted_.push_back(ptl::oms::value_of(*submitted));
        }
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

    /// Requests waiting to be drained into the engine at the next event.
    struct Request {
        std::uint64_t         request_id = 0;
        bool                  is_cancel = false;
        std::uint64_t         cancel_target = 0;
        ptl::InstrumentId     instrument{0};
        ptl::Side             side{ptl::Side::Buy};
        ptl::Qty              quantity{};
        ptl::oms::OrderType   type{ptl::oms::OrderType::Market};
        ptl::Price            limit_price{};
        ptl::Price            stop_price{};
        ptl::oms::TimeInForce tif{ptl::oms::TimeInForce::Day};
    };

    void enqueue(Request request) { inbox_.push_back(std::move(request)); }
    [[nodiscard]] std::size_t pending() const noexcept { return inbox_.size(); }

    /// Order ids this session has submitted, manual and automatic alike, in
    /// submission order. The OMS exposes only working orders, so a terminal
    /// order would otherwise vanish from the blotter the moment it filled.
    [[nodiscard]] const std::vector<std::uint64_t>& submitted() const noexcept {
        return submitted_;
    }

    /// Outcome of a manual request, so the UI can report a rejection against
    /// the request the user made rather than silently dropping it.
    struct Outcome {
        std::uint64_t request_id = 0;
        std::uint64_t order_id = 0;
        bool          accepted = false;
        std::string   detail;
    };
    [[nodiscard]] const std::deque<Outcome>& outcomes() const noexcept {
        return outcomes_;
    }

    void drain(const ptl::market::Bar& bar, ptl::engine::OrderSink& sink) {
        while (!inbox_.empty()) {
            const Request request = inbox_.front();
            inbox_.pop_front();

            Outcome outcome;
            outcome.request_id = request.request_id;

            if (request.is_cancel) {
                auto cancelled =
                    sink.cancel(static_cast<ptl::oms::OrderId>(request.cancel_target));
                outcome.accepted = cancelled.has_value();
                outcome.order_id = request.cancel_target;
                if (!cancelled) outcome.detail = cancelled.error().message;
                record(std::move(outcome));
                continue;
            }

            ptl::LifecycleTimes times;
            // From the EVENT. A manual order is decided when the user pressed
            // the button, but it is DECIDED FOR THE ENGINE at the bar it
            // reaches; using a wall clock here would break the timestamp chain.
            times.decision_time = bar.close_time();

            const auto id = sink.next_order_id();
            ptl::Result<ptl::oms::Order> built =
                ptl::fail(ptl::make_error(ptl::ErrorCode::ValidationFailed,
                                          "unsupported order type"));

            switch (request.type) {
                case ptl::oms::OrderType::Market:
                    built = ptl::oms::Order::market(id, request.instrument, request.side,
                                                    request.quantity, times, request.tif);
                    break;
                case ptl::oms::OrderType::Limit:
                    built = ptl::oms::Order::limit(id, request.instrument, request.side,
                                                   request.quantity, request.limit_price,
                                                   times, request.tif);
                    break;
                case ptl::oms::OrderType::Stop:
                    built = ptl::oms::Order::stop(id, request.instrument, request.side,
                                                  request.quantity, request.stop_price,
                                                  times, request.tif);
                    break;
                case ptl::oms::OrderType::StopLimit:
                    built = ptl::oms::Order::stop_limit(
                        id, request.instrument, request.side, request.quantity,
                        request.stop_price, request.limit_price, times, request.tif);
                    break;
            }

            if (!built) {
                outcome.detail = built.error().message;
                record(std::move(outcome));
                continue;
            }

            auto submitted = sink.submit(built->with_arrival_price(bar.close()));
            if (!submitted) {
                // A risk rejection is reported against the request the user
                // made. Dropping it silently would leave them believing the
                // order is live.
                outcome.detail = submitted.error().message;
                record(std::move(outcome));
                continue;
            }

            outcome.accepted = true;
            outcome.order_id = ptl::oms::value_of(*submitted);
            submitted_.push_back(outcome.order_id);
            record(std::move(outcome));
        }
    }

    void record(Outcome outcome) {
        outcomes_.push_back(std::move(outcome));
        if (outcomes_.size() > kMaxOutcomes) outcomes_.pop_front();
    }

private:
    static constexpr std::size_t kMaxFills = 200;
    static constexpr std::size_t kMaxOutcomes = 200;
    std::deque<Request>          inbox_;
    std::vector<std::uint64_t>   submitted_;
    std::deque<Outcome>          outcomes_;
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
    /// Owned here so the host can resolve ids to symbols. The engine takes
    /// instrument ids; only the presentation boundary needs names.
    ptl::InstrumentTable                           instruments;

    /// Host-sampled equity history.
    ///
    /// BOUNDED. A session left running for days would otherwise grow without
    /// limit, and no chart renders more than a few hundred points anyway.
    struct Sample {
        ptl::Timestamp ts{ptl::kNoTimestamp};
        double equity = 0.0;
        double cash = 0.0;
        double realized_pnl = 0.0;
        double unrealized_pnl = 0.0;
        double gross_exposure = 0.0;
        double net_exposure = 0.0;
    };
    std::deque<Sample> history;
    std::size_t        events_at_last_sample = 0;
    /// Monotonic request ids, so a caller can match an outcome to the request
    /// it made. Per-session, not global: two sessions must not share a counter.
    std::uint64_t next_request_id = 1;
    HostStrategy                                   strategy;
    std::unique_ptr<ptl::paper::PaperAccount>      account;
    std::unique_ptr<ptl::paper::PaperBroker>       broker;
    std::unique_ptr<ptl::paper::PaperSession>      session;

    Impl(ptl::market::Calendar cal, ptl::risk::RiskLimits limits,
         ptl::portfolio::PortfolioConfig portfolio_config, std::string artifact_root)
        : calendar(std::move(cal)),
          portfolio(portfolio_config),
          risk(limits),
          artifacts(std::move(artifact_root)) {
        // Instrument 0 is the single synthetic instrument this session trades.
        (void)instruments.intern("SPY");
    }
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
    // The opening balance is a real observation: without it a chart's first
    // point is the equity after the first trades, and the starting capital
    // never appears.
    record_sample();
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

    // Sampled only when the book actually moved. Recording an identical point
    // on every idle poll would fill the series with duplicates and make a
    // stalled session look busy.
    if (*processed > 0) record_sample();
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
        ss << "{\"instrument\": " << key << ", \"symbol\": \""
           << escape(impl_->instruments.symbol(static_cast<ptl::InstrumentId>(key)))
           << "\", \"quantity\": " << num(position.quantity().get())
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
           << ", \"symbol\": \""
           << escape(impl_->instruments.symbol(record->order.instrument())) << '"' 
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
           << ", \"symbol\": \""
           << escape(impl_->instruments.symbol(
                  static_cast<ptl::InstrumentId>(it->instrument)))
           << '"' 
           << ", \"side\": " << it->side << ", \"quantity\": " << num(it->quantity)
           << ", \"price\": " << num(it->price)
           << ", \"commission\": " << num(it->commission) << '}';
    }
    ss << "]}";
    return ss.str();
}

std::string PaperSessionHost::instruments_json() const {
    if (!impl_) return R"({"instruments": []})";

    std::ostringstream ss;
    ss << "{\"instruments\": [";
    // Only the instruments this session actually uses. Listing a global
    // universe would imply the session trades things it does not.
    const auto symbol = impl_->instruments.symbol(kInstrument);
    ss << "{\"instrument\": " << ptl::index_of(kInstrument) << ", \"symbol\": \""
       << escape(symbol) << "\"}";
    ss << "]}";
    return ss.str();
}

namespace {

[[nodiscard]] ptl::Result<ptl::oms::OrderType> parse_type(const std::string& text) {
    if (text == "market") return ptl::oms::OrderType::Market;
    if (text == "limit") return ptl::oms::OrderType::Limit;
    if (text == "stop") return ptl::oms::OrderType::Stop;
    if (text == "stop_limit") return ptl::oms::OrderType::StopLimit;
    return ptl::fail(bad("unknown order type: " + text));
}

[[nodiscard]] ptl::Result<ptl::oms::TimeInForce> parse_tif(const std::string& text) {
    if (text == "day") return ptl::oms::TimeInForce::Day;
    if (text == "ioc") return ptl::oms::TimeInForce::ImmediateOrCancel;
    if (text == "fok") return ptl::oms::TimeInForce::FillOrKill;
    if (text == "gtc") return ptl::oms::TimeInForce::GoodTillCancel;
    return ptl::fail(bad("unknown time in force: " + text));
}

}  // namespace

ptl::Result<std::uint64_t> PaperSessionHost::enqueue_order(const ManualOrder& request) {
    if (state_ != HostState::Running || !impl_) {
        return ptl::fail(bad("no running session to accept an order"));
    }

    const auto instrument = impl_->instruments.find(request.symbol);
    if (!instrument) {
        // REFUSED, never guessed. Interning an unknown symbol would create an
        // instrument the session has no data for, and the order would rest
        // forever against a book that never ticks.
        return ptl::fail(bad("unknown symbol: " + request.symbol));
    }
    if (!(request.quantity > 0.0) || !ptl::is_finite(request.quantity)) {
        return ptl::fail(bad("quantity must be positive"));
    }

    auto type = parse_type(request.type);
    if (!type) return ptl::fail(type.error());
    auto tif = parse_tif(request.time_in_force);
    if (!tif) return ptl::fail(tif.error());

    // Prices are validated HERE, before queuing. A limit order with no limit
    // price would otherwise sit in the inbox and fail at the next bar, long
    // after the user could connect the rejection to what they typed.
    const bool needs_limit = *type == ptl::oms::OrderType::Limit ||
                             *type == ptl::oms::OrderType::StopLimit;
    const bool needs_stop = *type == ptl::oms::OrderType::Stop ||
                            *type == ptl::oms::OrderType::StopLimit;
    if (needs_limit && !(request.limit_price > 0.0)) {
        return ptl::fail(bad("a limit order needs a positive limit price"));
    }
    if (needs_stop && !(request.stop_price > 0.0)) {
        return ptl::fail(bad("a stop order needs a positive stop price"));
    }

    HostStrategy::Request queued;
    queued.request_id = impl_->next_request_id++;
    queued.instrument = *instrument;
    queued.side = request.side >= 0 ? ptl::Side::Buy : ptl::Side::Sell;
    queued.quantity = ptl::Qty{request.quantity};
    queued.type = *type;
    queued.limit_price = ptl::Price{request.limit_price};
    queued.stop_price = ptl::Price{request.stop_price};
    queued.tif = *tif;

    const auto id = queued.request_id;
    impl_->strategy.enqueue(std::move(queued));
    return id;
}

ptl::Result<bool> PaperSessionHost::enqueue_cancel(std::uint64_t order_id) {
    if (state_ != HostState::Running || !impl_) {
        return ptl::fail(bad("no running session to cancel against"));
    }
    const auto* record = impl_->oms.find(static_cast<ptl::oms::OrderId>(order_id));
    if (record == nullptr) {
        return ptl::fail(bad("no such order: " + std::to_string(order_id)));
    }

    HostStrategy::Request queued;
    queued.request_id = impl_->next_request_id++;
    queued.is_cancel = true;
    queued.cancel_target = order_id;
    impl_->strategy.enqueue(std::move(queued));
    return true;
}

ptl::Result<std::size_t> PaperSessionHost::enqueue_cancel_all() {
    if (state_ != HostState::Running || !impl_) {
        return ptl::fail(bad("no running session"));
    }
    std::size_t queued = 0;
    for (const auto id : impl_->oms.working()) {
        if (enqueue_cancel(ptl::oms::value_of(id))) ++queued;
    }
    return queued;
}

ptl::Result<std::size_t> PaperSessionHost::enqueue_flatten() {
    if (state_ != HostState::Running || !impl_) {
        return ptl::fail(bad("no running session to flatten"));
    }

    // Cancel first, then close. Closing while an order still works could leave
    // the book flat and an order live, which would immediately re-open a
    // position the user asked to eliminate.
    (void)enqueue_cancel_all();

    std::size_t queued = 0;
    for (const auto& [key, position] : impl_->portfolio.positions()) {
        if (position.is_flat()) continue;

        ManualOrder closing;
        closing.symbol =
            std::string{impl_->instruments.symbol(static_cast<ptl::InstrumentId>(key))};
        // The OPPOSITE side, for the exact quantity held.
        closing.side = position.quantity().get() > 0.0 ? -1 : 1;
        closing.quantity = std::abs(position.quantity().get());
        closing.type = "market";
        if (enqueue_order(closing)) ++queued;
    }
    return queued;
}

std::string PaperSessionHost::pending_json() const {
    if (!impl_) return R"({"pending": 0, "outcomes": []})";

    std::ostringstream ss;
    ss << "{\"pending\": " << impl_->strategy.pending() << ", \"outcomes\": [";
    bool first = true;
    const auto& outcomes = impl_->strategy.outcomes();
    for (auto it = outcomes.rbegin(); it != outcomes.rend(); ++it) {
        if (!first) ss << ", ";
        first = false;
        ss << "{\"request_id\": " << it->request_id << ", \"order_id\": "
           << it->order_id << ", \"accepted\": " << (it->accepted ? "true" : "false")
           << ", \"detail\": \"" << escape(it->detail) << "\"}";
    }
    ss << "]}";
    return ss.str();
}

std::string PaperSessionHost::order_history_json() const {
    if (!impl_) return R"({"available": false, "orders": []})";

    std::ostringstream ss;
    ss << "{\"available\": true, \"orders\": [";
    bool first = true;
    // Most recent first. The OMS exposes only working orders, so the ids the
    // strategy recorded are the only way a filled or cancelled order stays
    // visible in a blotter.
    const auto& ids = impl_->strategy.submitted();
    for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
        const auto* record = impl_->oms.find(static_cast<ptl::oms::OrderId>(*it));
        if (record == nullptr) continue;
        if (!first) ss << ", ";
        first = false;
        ss << "{\"order_id\": " << *it << ", \"symbol\": \""
           << escape(impl_->instruments.symbol(record->order.instrument()))
           << "\", \"state\": \"" << ptl::oms::to_string(record->state)
           << "\", \"side\": " << (record->order.side() == ptl::Side::Buy ? 1 : -1)
           << ", \"type\": \"" << ptl::oms::to_string(record->order.type())
           << "\", \"quantity\": " << num(record->order.quantity().get())
           << ", \"filled\": " << num(record->filled_quantity.get())
           << ", \"reject_reason\": \"" << escape(record->reject_reason) << "\"}";
    }
    ss << "]}";
    return ss.str();
}

void PaperSessionHost::record_sample() {
    if (!impl_ || !impl_->session) return;

    Impl::Sample sample;
    sample.ts = impl_->clock.now();
    // READ ONLY. Portfolio::snapshot() would append to the engine's own curve,
    // a series the engine believes it controls.
    sample.equity = impl_->portfolio.equity().get();
    sample.cash = impl_->portfolio.cash().get();
    sample.realized_pnl = impl_->portfolio.realized_pnl().get();
    sample.unrealized_pnl = impl_->portfolio.unrealized_pnl().get();
    sample.gross_exposure = impl_->portfolio.gross_exposure().get();
    sample.net_exposure = impl_->portfolio.net_exposure().get();

    impl_->history.push_back(sample);
    if (impl_->history.size() > kMaxHistory) impl_->history.pop_front();
}

std::string PaperSessionHost::history_json(std::size_t max_points) const {
    if (!impl_) return R"({"available": false, "points": []})";

    const auto& curve = impl_->history;
    if (curve.empty()) {
        // A session that has started but processed no events has no history.
        // Reported as empty rather than as a single point at zero, which a
        // chart would draw as a real observation.
        return R"({"available": true, "points": [], "max_drawdown": 0.0, "current_drawdown": 0.0})";
    }

    // Drawdown from the ENGINE's tracker, fed the same curve. Recomputing it
    // here would be a second definition that could disagree with the risk
    // engine that halts on it.
    ptl::analytics::DrawdownTracker drawdown;
    for (const auto& point : curve) {
        (void)drawdown.update(point.ts, ptl::Notional{point.equity});
    }

    // STRIDE, not averaging. An averaged equity curve smooths away the
    // drawdown troughs, which are the points a reader is looking for.
    const std::size_t stride =
        (max_points == 0 || curve.size() <= max_points)
            ? 1
            : (curve.size() + max_points - 1) / max_points;

    std::ostringstream ss;
    ss << "{\"available\": true, \"total_points\": " << curve.size()
       << ", \"stride\": " << stride
       << ", \"max_drawdown\": " << num(drawdown.max_drawdown())
       << ", \"current_drawdown\": " << num(drawdown.current_drawdown())
       << ", \"peak_equity\": " << num(drawdown.peak_equity().get())
       << ", \"points\": [";

    bool first = true;
    const auto emit = [&](const Impl::Sample& point) {
        if (!first) ss << ", ";
        first = false;
        ss << "{\"ts\": \"" << iso_or_empty(point.ts) << "\", \"equity\": "
           << num(point.equity) << ", \"cash\": " << num(point.cash)
           << ", \"realized_pnl\": " << num(point.realized_pnl)
           << ", \"unrealized_pnl\": " << num(point.unrealized_pnl)
           << ", \"gross_exposure\": " << num(point.gross_exposure)
           << ", \"net_exposure\": " << num(point.net_exposure) << '}';
    };

    for (std::size_t i = 0; i < curve.size(); i += stride) emit(curve[i]);
    // The final point is always kept, so the series ends where the data does
    // rather than up to stride-1 observations short of it.
    if ((curve.size() - 1) % stride != 0) emit(curve.back());

    ss << "]}";
    return ss.str();
}

std::string PaperSessionHost::snapshot_json() const {
    // One document, so a dashboard renders a consistent picture from a single
    // read. Assembling it from four separate calls would let the account and
    // the positions come from different instants.
    std::ostringstream ss;
    // History is INCLUDED here rather than offered as a separate call.
    //
    // A reader fetching history on demand would have to call into the session
    // from a request thread, which breaks the single-writer guarantee. Instead
    // the driver publishes it with everything else, so every reader sees one
    // consistent instant and none of them touches the engine.
    ss << "{\"state\": " << state_json() << ", \"portfolio\": " << portfolio_json()
       << ", \"positions\": " << positions_json() << ", \"orders\": " << orders_json()
       << ", \"fills\": " << fills_json() << ", \"history\": " << history_json(240)
       << ", \"order_history\": " << order_history_json()
       << ", \"pending\": " << pending_json()
       << ", \"instruments\": " << instruments_json() << '}';
    return ss.str();
}

}  // namespace ptl_host
