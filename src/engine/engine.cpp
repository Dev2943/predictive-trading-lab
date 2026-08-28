#include "ptl/engine/engine.hpp"

#include <algorithm>
#include <cmath>

namespace ptl::engine {

/// The order sink handed to the strategy. It is the ONLY route from a strategy
/// to the venue, and every order on that route passes through risk.
class Engine::Sink final : public OrderSink {
public:
    // Deliberately holds only the engine. An earlier revision also stored a
    // StrategyContext, but the sink builds a fresh one per callback from the
    // engine's live state -- a cached context would be a stale view of the
    // portfolio by the time an order was submitted.
    explicit Sink(Engine& engine) : engine_(&engine) {}

    [[nodiscard]] oms::OrderId next_order_id() override { return engine_->oms_->next_id(); }

    [[nodiscard]] Result<oms::OrderId> submit(const oms::Order& order) override {
        Engine& e = *engine_;

        // --- risk, before anything else -------------------------------------
        risk::RiskContext rctx;
        rctx.now = e.clock_->now();
        rctx.peak_equity = e.peak_equity_;
        rctx.turnover_today = e.turnover_today_;

        const auto key = index_of(order.instrument());
        const auto st = e.state_.find(key);
        if (st != e.state_.end()) {
            rctx.reference_price = st->second.has_quote ? st->second.mid() : st->second.bid;
            const auto lu = e.last_update_.find(key);
            rctx.data_age = lu == e.last_update_.end() ? Duration::max() : rctx.now - lu->second;
        } else {
            // No market data at all for this instrument. The risk gate will
            // refuse on NoMarkAvailable rather than guessing a price.
            rctx.data_age = Duration::max();
        }

        const risk::RiskDecision decision = e.risk_->check(order, *e.pf_, *e.oms_, rctx);
        e.risk_->record(decision);

        if (!decision.approved()) {
            // Counted and journalled, never silently dropped: a suppressed
            // order that vanishes without trace makes a backtest diverge from
            // paper trading invisibly.
            ++e.summary_.orders_rejected;
            e.journal_->record_risk_rejection(rctx.now, order.instrument(), decision.describe());
            return fail(make_error(ErrorCode::ValidationFailed, decision.describe()));
        }

        // Stamp the decision-time benchmark. Implementation shortfall is
        // measured from here, and only the signal layer may set it.
        const oms::Order priced = rctx.reference_price.get() > 0.0
                                      ? order.with_arrival_price(rctx.reference_price)
                                      : order;

        if (auto submitted = e.oms_->submit(priced); !submitted) {
            return fail(submitted.error());
        }
        if (auto arrival = e.broker_->submit(priced); !arrival) {
            (void)e.oms_->transition(priced.id(), oms::OrderState::Rejected,
                                     arrival.error().message);
            return fail(arrival.error());
        }
        (void)e.oms_->transition(priced.id(), oms::OrderState::PendingNew);
        (void)e.oms_->transition(priced.id(), oms::OrderState::Working);

        accounting::JournalEntry entry;
        entry.ts = rctx.now;
        entry.kind = accounting::EntryKind::OrderSubmitted;
        entry.order_id = priced.id();
        entry.instrument = priced.instrument();
        entry.side = priced.side();
        entry.quantity = priced.quantity();
        entry.price = rctx.reference_price;
        (void)e.journal_->append(entry);

        ++e.summary_.orders_submitted;
        return priced.id();
    }

    [[nodiscard]] Result<bool> cancel(oms::OrderId id) override {
        Engine& e = *engine_;
        if (auto c = e.broker_->cancel(id); !c) return fail(c.error());
        return e.oms_->transition(id, oms::OrderState::Cancelled, "strategy cancel");
    }

private:
    Engine* engine_;
};

Engine::Engine(IClock& clock, market::IMarketDataSource& source, IStrategy& strategy,
               execution::BrokerSimulator& broker, portfolio::Portfolio& pf, oms::OrderManager& oms,
               risk::RiskManager& risk, accounting::Journal& journal,
               const market::Calendar* calendar, EngineConfig cfg)
    : clock_(&clock),
      source_(&source),
      strategy_(&strategy),
      broker_(&broker),
      pf_(&pf),
      oms_(&oms),
      risk_(&risk),
      journal_(&journal),
      calendar_(calendar),
      cfg_(cfg) {}

Result<bool> Engine::dispatch_fills(std::vector<oms::Fill>&& fills) {
    StrategyContext ctx{*clock_, *pf_, *oms_, risk_->limits(), calendar_};
    for (const auto& f : fills) {
        if (auto applied = oms_->apply_fill(f); !applied) return fail(applied.error());
        if (auto applied = pf_->apply(f); !applied) return fail(applied.error());
        journal_->record_fill(f);
        turnover_today_ =
            turnover_today_ + Notional{std::abs(f.price().get() * f.quantity().get())};
        ++summary_.fills;
        strategy_->on_fill(f, ctx);
    }
    return true;
}

Result<bool> Engine::handle_bar(const market::Bar& bar, Sink& sink) {
    const auto key = index_of(bar.instrument());
    auto& st = state_[key];

    // Bar-only mode: no quote exists, so bid == ask == close and the cost model
    // synthesises a spread. has_quote stays false, which is what tells the
    // model to do so rather than reading a spread that is not there.
    if (!st.has_quote) {
        st.bid = bar.close();
        st.ask = bar.close();
    }
    st.interval_volume =
        cfg_.default_interval_volume.get() > 0.0 ? cfg_.default_interval_volume : bar.volume();
    const double range = bar.high().get() - bar.low().get();
    st.intraday_volatility = bar.close().get() > 0.0 ? std::abs(range) / bar.close().get() : 0.0;
    last_update_[key] = bar.close_time();

    pf_->mark_last(bar.instrument(), bar.close());

    // FILLS FIRST, THEN THE STRATEGY.
    //
    // Orders resting from previous events are matched against this bar before
    // the strategy sees it. Reversing the order would let an order submitted on
    // this bar fill on this same bar -- same-bar execution, reintroduced at the
    // loop level even though every lower layer forbids it.
    auto fills = broker_->on_market(bar.instrument(), st, bar.close_time());
    if (!fills) return fail(fills.error());
    if (auto d = dispatch_fills(std::move(*fills)); !d) return fail(d.error());

    StrategyContext ctx{*clock_, *pf_, *oms_, risk_->limits(), calendar_};
    strategy_->on_bar(bar, ctx, sink);

    ++summary_.bars;
    if (cfg_.snapshot_on_bar) {
        if (auto s = pf_->snapshot(bar.close_time()); !s) return fail(s.error());
    }
    return true;
}

Result<bool> Engine::handle_quote(const market::Quote& q, Sink& sink) {
    const auto key = index_of(q.instrument());
    auto& st = state_[key];
    st.bid = q.bid();
    st.ask = q.ask();
    st.bid_size = q.bid_size();
    st.ask_size = q.ask_size();
    st.has_quote = true;
    last_update_[key] = q.time().exchange_time;

    pf_->mark_from_quote(q);

    auto fills = broker_->on_market(q.instrument(), st, q.time().exchange_time);
    if (!fills) return fail(fills.error());
    if (auto d = dispatch_fills(std::move(*fills)); !d) return fail(d.error());

    StrategyContext ctx{*clock_, *pf_, *oms_, risk_->limits(), calendar_};
    strategy_->on_quote(q, ctx, sink);
    ++summary_.quotes;
    return true;
}

Result<RunSummary> Engine::run() {
    reset_chain_violation_count();
    summary_ = RunSummary{};
    peak_equity_ = pf_->equity();

    StrategyContext start_ctx{*clock_, *pf_, *oms_, risk_->limits(), calendar_};
    if (auto s = strategy_->on_start(start_ctx); !s) return fail(s.error());

    Sink sink{*this};

    while (auto event = source_->next()) {
        ++summary_.events_processed;
        const Timestamp ts = market::exchange_time_of(*event);
        if (!is_set(summary_.first_event)) summary_.first_event = ts;
        summary_.last_event = ts;

        // std::visit over the variant. Every alternative is handled; adding a
        // new event type is a compile error here rather than a silent no-op.
        Result<bool> r = true;
        std::visit(
            [&](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                StrategyContext ctx{*clock_, *pf_, *oms_, risk_->limits(), calendar_};
                if constexpr (std::is_same_v<T, market::Bar>) {
                    r = handle_bar(e, sink);
                } else if constexpr (std::is_same_v<T, market::Quote>) {
                    r = handle_quote(e, sink);
                } else if constexpr (std::is_same_v<T, market::Trade>) {
                    strategy_->on_trade(e, ctx, sink);
                } else if constexpr (std::is_same_v<T, market::CorporateAction>) {
                    if (e.kind() == market::CorporateActionKind::Split) {
                        r = pf_->apply_split(e.instrument(), e.split_ratio());
                    } else {
                        r = pf_->apply_dividend(e.instrument(), e.dividend_amount());
                    }
                } else if constexpr (std::is_same_v<T, market::SessionEvent>) {
                    if (e.kind == market::SessionEventKind::Open) {
                        turnover_today_ = Notional{0.0};
                        strategy_->on_session_open(e.time.exchange_time, ctx, sink);
                    } else {
                        strategy_->on_session_close(e.time.exchange_time, ctx, sink);
                        if (cfg_.snapshot_on_session_close) {
                            auto s = pf_->snapshot(e.time.exchange_time);
                            if (!s) r = fail(s.error());
                        }
                    }
                }
            },
            *event);
        if (!r) return fail(r.error());

        const Notional eq = pf_->equity();
        if (eq.get() > peak_equity_.get()) peak_equity_ = eq;
    }

    StrategyContext stop_ctx{*clock_, *pf_, *oms_, risk_->limits(), calendar_};
    strategy_->on_stop(stop_ctx);

    journal_->match_trades();
    const auto recon = journal_->reconcile(*pf_);

    summary_.final_equity = pf_->equity();
    summary_.chain_violations = chain_violation_count();
    summary_.reconciled = recon.balances() && pf_->identity_holds();
    return summary_;
}

}  // namespace ptl::engine
