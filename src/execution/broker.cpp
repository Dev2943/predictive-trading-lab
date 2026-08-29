#include "ptl/execution/broker.hpp"

#include <algorithm>
#include <cmath>

namespace ptl::execution {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

}  // namespace

BrokerSimulator::BrokerSimulator(const IClock& clock, const ICostModel& costs,
                                 const ILatencyModel& latency, DeterministicRng rng,
                                 FillConfig fill_cfg)
    : clock_(&clock),
      costs_(&costs),
      latency_(&latency),
      // Forked streams, so adding a consumer later cannot perturb the fills an
      // existing one produces. A fork is a pure function of (seed, stream id).
      latency_rng_(rng.fork(kStreamLatency)),
      slippage_rng_(rng.fork(kStreamSlippage)),
      fill_cfg_(fill_cfg) {}

Result<Timestamp> BrokerSimulator::submit(const oms::Order& order) {
    const Timestamp now = clock_->now();
    if (!is_set(now)) return fail(bad("broker clock is unset"));

    if (order.decision_time() > now) {
        // An order whose decision lies in the future means the strategy acted on
        // information it could not have had.
        ++stats_.orders_rejected;
        return fail(bad("order decision_time is in the future relative to the venue clock"));
    }

    const Duration flight = latency_->decision_to_arrival(latency_rng_);
    // Strictly after: even a zero-latency configuration must not let an order
    // arrive at the instant of its own decision. This is the no-same-bar rule
    // enforced at the venue boundary, independent of anything upstream.
    const Duration effective = flight > Duration::zero() ? flight : Duration{1};
    const Timestamp arrival = now + effective;

    auto stamped = order.with_arrival(arrival);
    if (!stamped) {
        ++stats_.orders_rejected;
        return fail(stamped.error());
    }

    pending_.push_back(PendingOrder{*stamped, arrival, Qty{}});
    ++stats_.orders_accepted;
    return arrival;
}

Result<bool> BrokerSimulator::cancel(oms::OrderId id) {
    const auto it = std::find_if(pending_.begin(), pending_.end(),
                                 [id](const PendingOrder& p) { return p.order.id() == id; });
    if (it == pending_.end()) return fail(bad("cancel for an unknown or completed order"));
    pending_.erase(it);
    return true;
}

Result<oms::Fill> BrokerSimulator::make_fill(const oms::Order& order, Price price, Qty qty,
                                             Timestamp now, oms::Liquidity liquidity) {
    if (!is_finite(price.get()) || price.get() <= 0.0) {
        return fail(bad("computed fill price is not a positive finite number"));
    }
    if (!is_finite(qty.get()) || qty.get() <= 0.0) {
        return fail(bad("computed fill quantity is not positive"));
    }

    oms::Fill f;
    f.order_id_ = order.id();
    f.parent_id_ = order.parent_id();
    f.instrument_ = order.instrument();
    f.side_ = order.side();
    f.price_ = price;
    f.quantity_ = qty;
    f.commission_ = costs_->commission(qty, price);
    f.exchange_fee_ = Notional{0.0};
    f.liquidity_ = liquidity;
    f.times_ = order.times();
    f.times_.fill_time = now;
    f.arrival_price_ = order.arrival_price().get() > 0.0 ? order.arrival_price() : price;

    if (const auto v = validate_chain(f.times_); v.has_value()) {
        return fail(bad("fill would violate the timestamp chain: " + v->describe()));
    }
    stats_.total_commission = stats_.total_commission + f.commission_;
    return f;
}

Result<std::vector<oms::Fill>> BrokerSimulator::on_market(InstrumentId instrument,
                                                          const MarketState& state, Timestamp now) {
    std::vector<oms::Fill> fills;

    // Participation budget is shared across every order in this interval, so a
    // strategy cannot evade the cap by splitting one parent into many children.
    double remaining_participation = state.interval_volume.get() * fill_cfg_.max_participation_rate;

    for (auto it = pending_.begin(); it != pending_.end();) {
        PendingOrder& p = *it;
        if (p.order.instrument() != instrument) {
            ++it;
            continue;
        }
        // NOT YET ARRIVED. This is where latency actually bites: an order
        // submitted on this event cannot fill on this event.
        if (now < p.arrival_time) {
            ++it;
            continue;
        }

        const Side side = p.order.side();
        const Price touch = state.has_quote ? state.touch(side) : state.bid;
        // --- stop triggering --------------------------------------------------
        //
        // A stop is DORMANT until the market trades through its stop price.
        // is_marketable_against() reports true for a stop unconditionally --
        // correct once triggered, since a triggered stop IS a market order --
        // so without this gate a protective stop would fill the instant it
        // arrived, which is the opposite of its purpose.
        const bool is_stop =
            p.order.type() == oms::OrderType::Stop || p.order.type() == oms::OrderType::StopLimit;
        if (is_stop && !p.stop_triggered) {
            const Price stop = *p.order.stop_price();
            // A buy stop triggers when the market rises TO or through it; a
            // sell stop when it falls to or through it.
            const bool triggered = side == Side::Buy ? touch >= stop : touch <= stop;
            if (!triggered) {
                ++it;
                continue;
            }
            p.stop_triggered = true;
            ++stats_.stops_triggered;
        }

        const bool marketable = p.order.is_marketable_against(touch);

        if (!marketable) {
            // A resting limit does NOT fill because a price was touched. A bar
            // whose low equals your limit says nothing about whether you were
            // ever at the front of a queue you cannot observe.
            ++stats_.passive_no_fill;
            ++it;
            continue;
        }

        Qty want = p.order.quantity() - p.filled;
        if (want.get() <= 0.0) {
            it = pending_.erase(it);
            continue;
        }

        double allowed = want.get();
        bool capped = false;

        // LIQUIDITY EVIDENCE DIFFERS BY PRICE SOURCE. With a real quote we see
        // size AT THE TOUCH; with only a bar we see interval volume and nothing
        // about the book. Both constraints apply when both are available.
        if (state.has_quote) {
            const double displayed =
                side == Side::Buy ? state.ask_size.get() : state.bid_size.get();
            if (displayed > 0.0) {
                if (fill_cfg_.respect_displayed_size && displayed < allowed) {
                    allowed = displayed;
                    capped = true;
                    ++stats_.displayed_size_capped;
                }
            } else if (fill_cfg_.respect_displayed_size) {
                // A quote with no size at the touch offers nothing to take.
                ++it;
                continue;
            }
        }
        if (remaining_participation > 0.0 && remaining_participation < allowed) {
            allowed = remaining_participation;
            capped = true;
            ++stats_.participation_capped;
        } else if (state.volume_known && state.interval_volume.get() <= 0.0) {
            // Bar-only mode with nothing traded in the interval: no liquidity
            // was available, and zero participation means NO FILL rather than
            // an unconstrained one. This does not apply when a quote supplied
            // displayed size, which is the stronger evidence.
            ++it;
            continue;
        }

        if (allowed <= 0.0) {
            ++it;
            continue;
        }

        // --- fill-or-kill -----------------------------------------------------
        //
        // FOK is all-or-nothing IMMEDIATELY. Distinct from IOC, which takes
        // what it can and cancels the rest: an FOK that cannot be filled in
        // full fills NOTHING. Conflating the two understates execution risk on
        // exactly the orders that are too large for the displayed book.
        if (p.order.time_in_force() == oms::TimeInForce::FillOrKill &&
            allowed + 1e-9 < want.get()) {
            ++stats_.fok_killed;
            it = pending_.erase(it);
            continue;
        }

        // --- price -----------------------------------------------------------
        const Price reference = state.has_quote ? touch : state.bid;
        const Bps half = state.has_quote ? Bps{0.0} : costs_->half_spread(state);
        const Bps impact = costs_->market_impact(Qty{allowed}, state);
        const Bps slip = costs_->stochastic_slippage(slippage_rng_);
        const Bps total_bps = half + impact + slip;

        // Direction +1 always pays UP for a buy and DOWN for a sell: costs move
        // the fill against the trader, never for them.
        const Price fill_price = apply_bps(reference, total_bps, sign_of(side));

        auto f = make_fill(p.order, fill_price, Qty{allowed}, now, oms::Liquidity::Taker);
        if (!f) return fail(f.error());

        if (state.has_quote) {
            ++stats_.filled_from_quote;
        } else {
            ++stats_.filled_from_bar;
        }
        stats_.total_impact_cost =
            stats_.total_impact_cost + Notional{impact.get() * 1e-4 * reference.get() * allowed};
        ++stats_.fills;

        p.filled = p.filled + Qty{allowed};
        remaining_participation = std::max(0.0, remaining_participation - allowed);
        fills.push_back(*f);

        if (p.filled.get() + 1e-9 >= p.order.quantity().get()) {
            it = pending_.erase(it);
        } else {
            ++stats_.partial_fills;
            if (p.order.time_in_force() == oms::TimeInForce::ImmediateOrCancel) {
                // IOC: the unfilled remainder is cancelled, not left resting.
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
        (void)capped;
    }
    return fills;
}

Result<std::vector<oms::Fill>> BrokerSimulator::on_quote_update(InstrumentId instrument,
                                                                const RoutedMarket& routed,
                                                                Timestamp now) {
    // "No fills" and "no fills because the instrument was halted" are different
    // facts. Recording WHICH refusal applied is what lets a run explain an
    // unexpectedly small trade count instead of leaving it a mystery.
    if (!routed.executable) {
        switch (routed.quote_state.condition) {
            case QuoteCondition::Stale:
                ++stats_.rejected_stale_quote;
                break;
            case QuoteCondition::Crossed:
                ++stats_.rejected_crossed;
                break;
            default:
                break;
        }
        if (routed.quote_state.trading_state == TradingState::Halted) {
            ++stats_.rejected_halted;
        }
        // Orders REST rather than being cancelled: a halt is temporary, and
        // cancelling on every halt would silently change the strategy.
        return std::vector<oms::Fill>{};
    }
    return on_market(instrument, routed.state, now);
}

void BrokerSimulator::reset() noexcept {
    pending_.clear();
    stats_ = ExecutionStats{};
}

}  // namespace ptl::execution
