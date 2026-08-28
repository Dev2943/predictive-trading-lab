#include "ptl/pipeline/trading_pipeline.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace ptl::pipeline {
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

}  // namespace

// ---------------------------------------------------------------------------
// StoredPredictionSource
// ---------------------------------------------------------------------------

Result<bool> StoredPredictionSource::add(InstrumentId instrument, Timestamp produced_at,
                                         double value) {
    if (instrument == kInvalidInstrument) return fail(bad("prediction has no instrument"));
    if (!is_set(produced_at)) return fail(bad("prediction has no timestamp"));
    if (!is_finite(value)) return fail(bad("prediction value is not finite"));
    by_instrument_[index_of(instrument)][produced_at] = value;
    ++count_;
    return true;
}

std::optional<IPredictionSource::Prediction> StoredPredictionSource::predict_at(
    InstrumentId instrument, Timestamp as_of) const {
    const auto it = by_instrument_.find(index_of(instrument));
    if (it == by_instrument_.end()) return std::nullopt;

    // THE LATEST PREDICTION AT OR BEFORE as_of -- never the nearest.
    //
    // upper_bound gives the first entry strictly AFTER as_of; stepping back one
    // gives the newest at or before it. Using the temporally nearest entry
    // instead would happily return a prediction from the future whenever one
    // was closer, which is the exact leak this project exists to prevent.
    const auto upper = it->second.upper_bound(as_of);
    if (upper == it->second.begin()) return std::nullopt;

    const auto chosen = std::prev(upper);
    return Prediction{chosen->second, chosen->first};
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------

double PipelineStats::hit_rate() const noexcept {
    if (signals_resolved == 0) return 0.0;
    return static_cast<double>(signal_hits) / static_cast<double>(signals_resolved);
}

Duration PipelineStats::average_holding_period() const noexcept {
    if (closed_positions == 0) return Duration::zero();
    return total_holding_period / static_cast<std::int64_t>(closed_positions);
}

std::string PipelineStats::describe() const {
    std::ostringstream ss;
    ss.precision(4);
    ss << std::fixed;
    ss << "pipeline over " << bars_seen << " bars\n";
    ss << "  predictions consumed " << predictions_consumed << '\n';
    ss << "  signals generated    " << signals_generated << " (" << signals_actionable
       << " actionable, " << signals_filtered << " filtered)\n";
    ss << "  rebalances           " << rebalances << '\n';
    ss << "  orders               " << orders_submitted << " submitted, " << orders_rejected
       << " rejected\n";
    ss << "  fills                " << fills_received << '\n';
    ss << "  gross turnover       " << gross_turnover.get() << '\n';
    ss << "  realized costs       " << realized_costs.get() << '\n';
    ss << "  signal hit rate      " << hit_rate() << " over " << signals_resolved << " resolved\n";
    ss << "  avg holding period   " << average_holding_period().count() / 1'000'000'000 << "s\n";
    ss << "  peak gross leverage  " << peak_gross_leverage << '\n';
    ss << "  peak net leverage    " << peak_net_leverage << '\n';
    return ss.str();
}

// ---------------------------------------------------------------------------
// TradingPipeline
// ---------------------------------------------------------------------------

TradingPipeline::TradingPipeline(signal::ISignalGenerator& generator,
                                 IPredictionSource* predictions, const market::Calendar* calendar,
                                 PipelineConfig cfg)
    : generator_(&generator),
      predictions_(predictions),
      calendar_(calendar),
      cfg_(std::move(cfg)),
      filters_(cfg_.filters),
      sizer_(cfg_.sizing),
      rebalancer_(cfg_.rebalance) {}

Result<bool> TradingPipeline::on_start(const engine::StrategyContext&) {
    if (generator_ == nullptr) return fail(bad("pipeline has no signal generator"));
    stats_ = PipelineStats{};
    state_.clear();
    latest_signals_.clear();
    emitted_.clear();
    bars_since_rebalance_ = 0;
    return true;
}

void TradingPipeline::update_state(const market::Bar& bar) {
    auto& st = state_[index_of(bar.instrument())];

    const double close = bar.close().get();
    if (st.previous_close > 0.0 && close > 0.0) {
        const double r = std::log(close / st.previous_close);
        if (is_finite(r)) {
            // Welford over log returns: numerically stable, and it uses only
            // bars already seen.
            ++st.return_count;
            const double delta = r - st.return_mean;
            st.return_mean += delta / static_cast<double>(st.return_count);
            st.return_m2 += delta * (r - st.return_mean);
            if (st.return_count > 1) {
                st.volatility = std::sqrt(st.return_m2 / static_cast<double>(st.return_count - 1));
            }
        }
    }

    st.previous_close = close;
    st.last_close = bar.close();
    st.last_bar_time = bar.close_time();
    st.interval_volume = bar.volume().get();

    // Running mean volume, for the relative-liquidity filter.
    const double n = static_cast<double>(std::max<std::size_t>(1, st.return_count));
    st.average_volume += (st.interval_volume - st.average_volume) / n;

    // Bar-only mode: no quote, so the spread is synthesised from the bar range.
    // A documented approximation, not a measurement.
    const double range = bar.high().get() - bar.low().get();
    st.spread_bps = close > 0.0 ? Bps{range / close * 1e4 * 0.1} : Bps{0.0};
}

signal::CostEstimate TradingPipeline::estimate_costs(const InstrumentState& st,
                                                     double turnover_fraction) const {
    signal::CostEstimate costs;
    // Half the quoted spread, in return units: what crossing costs one way.
    costs.half_spread = st.spread_bps.get() * 1e-4 * 0.5;
    // Commission as a return fraction, from the configured per-share rate
    // against the current price.
    if (st.last_close.get() > 0.0) {
        costs.commission = 0.0035 / st.last_close.get();
    }
    // Slippage scales with volatility: a violent instrument fills further from
    // the touch.
    costs.slippage = st.volatility * 0.1;
    costs.borrow = 0.0;
    costs.turnover_penalty = std::abs(turnover_fraction) * 1e-4;
    return costs;
}

void TradingPipeline::on_bar(const market::Bar& bar, const engine::StrategyContext& ctx,
                             engine::OrderSink& sink) {
    ++stats_.bars_seen;
    last_bar_time_ = bar.close_time();
    update_state(bar);

    const auto& st = state_[index_of(bar.instrument())];

    // --- prediction ----------------------------------------------------------
    signal::GeneratorInput input;
    // as_of is the BAR CLOSE: the earliest instant this bar's contents exist.
    input.as_of = bar.close_time();
    input.instrument = bar.instrument();
    input.volatility = st.volatility;
    input.reference_price = bar.close();
    input.costs = estimate_costs(st, 0.0);

    if (predictions_ != nullptr) {
        const auto prediction = predictions_->predict_at(bar.instrument(), input.as_of);
        if (!prediction.has_value()) return;  // nothing to act on
        input.prediction = prediction->value;
        input.prediction_time = prediction->produced_at;
        ++stats_.predictions_consumed;
    } else {
        // Pure rule mode: the signal comes from the bar itself.
        input.prediction = st.return_mean;
        input.prediction_time = bar.close_time();
    }

    // --- signal --------------------------------------------------------------
    auto sig = generator_->generate(input);
    if (!sig) return;  // a refused signal is a refused signal, not a silent long
    ++stats_.signals_generated;
    emitted_.push_back(*sig);

    // --- filters -------------------------------------------------------------
    signal::FilterContext fctx;
    fctx.now = bar.close_time();
    fctx.realized_volatility = st.volatility;
    fctx.interval_volume = st.interval_volume;
    fctx.average_volume = st.average_volume;
    fctx.spread_bps = st.spread_bps;
    fctx.feature_age = bar.close_time() - st.last_bar_time;
    fctx.prediction_age =
        is_set(input.prediction_time) ? bar.close_time() - input.prediction_time : Duration::zero();
    fctx.has_market_data = true;

    const auto decision = filters_.evaluate(*sig, fctx, calendar_);
    filters_.record(sig->instrument(), decision, fctx.now);
    if (!decision.passed()) {
        ++stats_.signals_filtered;
        return;
    }
    if (sig->is_actionable()) ++stats_.signals_actionable;

    latest_signals_.insert_or_assign(index_of(sig->instrument()), *sig);

    // --- rebalance cadence ---------------------------------------------------
    ++bars_since_rebalance_;
    if (bars_since_rebalance_ >= cfg_.rebalance_interval_bars) {
        rebalance(bar.close_time(), ctx, sink, false);
        bars_since_rebalance_ = 0;
    }
}

void TradingPipeline::rebalance(Timestamp now, const engine::StrategyContext& ctx,
                                engine::OrderSink& sink, bool flatten) {
    const auto& pf = ctx.portfolio();
    const double equity = pf.equity().get();
    if (!is_finite(equity) || equity <= 0.0) return;

    construction::TargetPortfolio targets{now};

    // Running exposure as targets are built, so each instrument's limit check
    // accounts for what earlier instruments have already claimed. Without this
    // a set of individually-compliant positions can collectively breach the
    // gross limit.
    Notional running_gross{0.0};
    Notional running_net{0.0};
    std::map<std::int32_t, Notional> sector_exposure;

    for (const auto& [key, sig] : latest_signals_) {
        const auto instrument = static_cast<InstrumentId>(key);
        const auto st_it = state_.find(key);
        if (st_it == state_.end()) continue;
        const auto& st = st_it->second;
        if (st.last_close.get() <= 0.0) continue;

        // A signal that has outlived its horizon no longer describes the
        // market it was made in.
        if (is_set(sig.expires_at()) && now > sig.expires_at()) continue;

        sizing::SizingContext sctx;
        sctx.now = now;
        sctx.equity = pf.equity();
        sctx.reference_price = st.last_close;
        sctx.volatility = st.volatility;
        sctx.current_position = ctx.position_of(instrument);
        const auto sector_it = cfg_.sectors.find(key);
        sctx.sector = sector_it == cfg_.sectors.end() ? -1 : sector_it->second;
        sctx.existing_gross = running_gross;
        sctx.existing_net = running_net;
        if (sctx.sector >= 0) sctx.sector_exposure = sector_exposure[sctx.sector];

        auto sized = flatten ? sizing::SizingDecision{} : [&] {
            auto d = sizer_.size(sig, sctx);
            return d.has_value() ? *d : sizing::SizingDecision{};
        }();

        construction::TargetPosition target;
        target.instrument = instrument;
        target.target_quantity = sized.target_position;
        target.reference_price = st.last_close;
        target.target_weight = sized.final_weight;
        target.net_edge = sig.net_edge();
        if (!targets.set(target)) continue;

        const double notional = std::abs(sized.target_notional.get());
        running_gross = running_gross + Notional{notional};
        running_net = running_net + sized.target_notional;
        if (sctx.sector >= 0) {
            sector_exposure[sctx.sector] = sector_exposure[sctx.sector] + Notional{notional};
        }
    }

    auto plan = rebalancer_.plan(targets, pf);
    if (!plan) return;  // a refused plan (e.g. excessive turnover) trades nothing
    if (plan->actionable() == 0) return;

    ++stats_.rebalances;
    stats_.gross_turnover = stats_.gross_turnover + plan->gross_turnover;

    auto orders = rebalancer_.to_orders(*plan, now, [&sink] { return sink.next_order_id(); });
    if (!orders) return;

    for (const auto& order : *orders) {
        // THE ONLY ROUTE TO THE VENUE. OrderSink::submit runs the Phase 3 risk
        // gate on every order; the pipeline holds no broker reference and
        // cannot construct a Fill.
        if (sink.submit(order)) {
            ++stats_.orders_submitted;
        } else {
            ++stats_.orders_rejected;
        }
    }

    const double gross = pf.gross_exposure().get() / equity;
    const double net = std::abs(pf.net_exposure().get()) / equity;
    stats_.peak_gross_leverage = std::max(stats_.peak_gross_leverage, gross);
    stats_.peak_net_leverage = std::max(stats_.peak_net_leverage, net);
}

void TradingPipeline::on_fill(const oms::Fill& fill, const engine::StrategyContext& ctx) {
    ++stats_.fills_received;
    stats_.realized_costs = stats_.realized_costs + fill.total_cost();

    auto& st = state_[index_of(fill.instrument())];
    const Qty position = ctx.position_of(fill.instrument());

    if (position.get() == 0.0) {
        // Closed out: record the holding period.
        if (is_set(st.position_opened)) {
            stats_.total_holding_period =
                stats_.total_holding_period + (fill.fill_time() - st.position_opened);
            ++stats_.closed_positions;
            st.position_opened = kNoTimestamp;
        }
    } else if (!is_set(st.position_opened)) {
        st.position_opened = fill.fill_time();
    }

    // Signal hit accounting: did the realised move agree with the direction the
    // signal predicted? Measured against the ARRIVAL price, so it reflects the
    // decision rather than the fill.
    const auto sig_it = latest_signals_.find(index_of(fill.instrument()));
    if (sig_it != latest_signals_.end() && st.last_close.get() > 0.0 &&
        fill.arrival_price().get() > 0.0) {
        const double move = st.last_close.get() - fill.arrival_price().get();
        const int predicted = signal::sign_of(sig_it->second.direction());
        if (predicted != 0) {
            ++stats_.signals_resolved;
            if ((move > 0.0 && predicted > 0) || (move < 0.0 && predicted < 0)) {
                ++stats_.signal_hits;
            }
        }
    }
}

void TradingPipeline::on_session_open(Timestamp, const engine::StrategyContext&,
                                      engine::OrderSink&) {
    // Signals do not survive an overnight gap: the market they described is
    // gone, and acting on them at the next open would be trading yesterday's
    // view against today's prices.
    latest_signals_.clear();
    bars_since_rebalance_ = 0;
}

void TradingPipeline::on_session_close(Timestamp now, const engine::StrategyContext& ctx,
                                       engine::OrderSink& sink) {
    if (!cfg_.flatten_at_session_close) return;
    // A position held overnight is exposed to a gap the intraday model never
    // described, so the pipeline targets zero everywhere before the close.
    rebalance(now, ctx, sink, true);
    latest_signals_.clear();
}

std::uint64_t TradingPipeline::content_hash() const noexcept {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (const auto& s : emitted_) {
        const std::int64_t ns = s.as_of().time_since_epoch().count();
        hash_bytes(h, &ns, sizeof(ns));
        const std::uint32_t inst = index_of(s.instrument());
        hash_bytes(h, &inst, sizeof(inst));
        const int dir = signal::sign_of(s.direction());
        hash_bytes(h, &dir, sizeof(dir));
        // Bit patterns, so a one-ulp divergence between runs is caught.
        const double edge = s.net_edge();
        hash_bytes(h, &edge, sizeof(edge));
        const double conf = s.confidence();
        hash_bytes(h, &conf, sizeof(conf));
    }
    return h;
}

}  // namespace ptl::pipeline
