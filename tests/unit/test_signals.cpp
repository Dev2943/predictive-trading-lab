#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>

#include "ptl/signal/filter.hpp"
#include "ptl/signal/generator.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::signal;
using namespace std::chrono;

namespace {

Timestamp at(const char* iso) {
    Timestamp ts{};
    REQUIRE(parse_timestamp(iso, ts));
    return ts;
}

constexpr InstrumentId kSpy{0};
constexpr std::uint64_t kModel = 0xABCD;

GeneratorInput base_input(double prediction) {
    GeneratorInput in;
    in.as_of = at("2024-07-02T15:00:00Z");
    in.instrument = kSpy;
    in.prediction = prediction;
    in.prediction_time = at("2024-07-02T15:00:00Z");
    in.volatility = 0.001;
    in.reference_price = Price{500.0};
    return in;
}

const market::Calendar& us() {
    static const market::Calendar cal = [] {
        auto r = market::Calendar::build(market::Calendar::us_equities_spec(), 2024, 2024);
        REQUIRE(r.has_value());
        return std::move(*r);
    }();
    return cal;
}

}  // namespace

TEST_CASE("a signal carries its full provenance", "[signal]") {
    // Without model id and horizon, a disappointing month cannot be attributed
    // to a specific model or a horizon mismatch, and the post-mortem has
    // nowhere to start.
    auto s = Signal::create(at("2024-07-02T15:00:00Z"), kSpy, Direction::Long, 0.002, 0.7,
                            minutes{15}, kModel);
    REQUIRE(s.has_value());
    REQUIRE(s->instrument() == kSpy);
    REQUIRE(s->direction() == Direction::Long);
    REQUIRE(s->confidence() == Catch::Approx(0.7));
    REQUIRE(s->horizon() == minutes{15});
    REQUIRE(s->model_id() == kModel);
    REQUIRE(s->expires_at() == at("2024-07-02T15:15:00Z"));
}

TEST_CASE("net edge subtracts every cost", "[signal][costs]") {
    // THE COST GATE. A signal whose expected move cannot pay for its own round
    // trip is a losing trade however confident the model is.
    CostEstimate costs;
    costs.half_spread = 0.0005;
    costs.commission = 0.0001;
    costs.slippage = 0.0002;
    costs.borrow = 0.0000;
    costs.turnover_penalty = 0.0001;
    REQUIRE(costs.total() == Catch::Approx(0.0009));

    auto profitable = Signal::create(at("2024-07-02T15:00:00Z"), kSpy, Direction::Long, 0.003, 0.7,
                                     minutes{15}, kModel, costs);
    REQUIRE(profitable->net_edge() == Catch::Approx(0.003 - 0.0009));
    REQUIRE(profitable->is_actionable());

    // A move smaller than its costs is not an opportunity.
    auto unprofitable = Signal::create(at("2024-07-02T15:00:00Z"), kSpy, Direction::Long, 0.0005,
                                       0.95, minutes{15}, kModel, costs);
    REQUIRE(unprofitable->net_edge() < 0.0);
    REQUIRE_FALSE(unprofitable->is_actionable());
}

TEST_CASE("costs are charged against the magnitude for either side", "[signal][costs][property]") {
    // A short with a large negative expected return is just as profitable as a
    // long with a large positive one. Charging costs against the signed value
    // would make shorts look better the more they were expected to fall.
    CostEstimate costs;
    costs.half_spread = 0.001;

    auto long_side = Signal::create(at("2024-07-02T15:00:00Z"), kSpy, Direction::Long, 0.005, 0.7,
                                    minutes{15}, kModel, costs);
    auto short_side = Signal::create(at("2024-07-02T15:00:00Z"), kSpy, Direction::Short, -0.005,
                                     0.7, minutes{15}, kModel, costs);
    REQUIRE(long_side->net_edge() == Catch::Approx(short_side->net_edge()));
    REQUIRE(long_side->is_actionable());
    REQUIRE(short_side->is_actionable());
}

TEST_CASE("invalid signals are refused", "[signal][validation]") {
    const Timestamp t = at("2024-07-02T15:00:00Z");
    REQUIRE_FALSE(Signal::create(kNoTimestamp, kSpy, Direction::Long, 0.1, 0.5, minutes{15}, kModel)
                      .has_value());
    REQUIRE_FALSE(
        Signal::create(t, kInvalidInstrument, Direction::Long, 0.1, 0.5, minutes{15}, kModel)
            .has_value());
    // Confidence outside [0, 1] is not a confidence.
    REQUIRE_FALSE(
        Signal::create(t, kSpy, Direction::Long, 0.1, 1.5, minutes{15}, kModel).has_value());
    REQUIRE_FALSE(
        Signal::create(t, kSpy, Direction::Long, 0.1, -0.1, minutes{15}, kModel).has_value());
    // A signal with no horizon cannot expire or be matched to a label.
    REQUIRE_FALSE(
        Signal::create(t, kSpy, Direction::Long, 0.1, 0.5, Duration::zero(), kModel).has_value());
    // Provenance is mandatory.
    REQUIRE_FALSE(Signal::create(t, kSpy, Direction::Long, 0.1, 0.5, minutes{15}, 0).has_value());
}

TEST_CASE("flat is distinct from no signal", "[signal]") {
    // The model spoke and said stay out, which coverage statistics must tell
    // apart from a filter having removed the signal.
    const auto s = Signal::flat(at("2024-07-02T15:00:00Z"), kSpy, kModel);
    REQUIRE(s.is_flat());
    REQUIRE_FALSE(s.is_actionable());
    REQUIRE(s.net_edge() == 0.0);
    REQUIRE(s.model_id() == kModel);
}

// ---------------------------------------------------------------------------
// Generators
// ---------------------------------------------------------------------------

TEST_CASE("a prediction from the future is refused", "[signal][generator][leakage]") {
    // THE CENTRAL LEAK CHECK OF THE SIGNAL LAYER.
    ModelSignalGenerator gen{"model", kModel};
    auto in = base_input(0.005);
    in.prediction_time = in.as_of + minutes{1};

    auto s = gen.generate(in);
    REQUIRE_FALSE(s.has_value());
    REQUIRE(s.error().message.find("stamped after the decision") != std::string::npos);

    // At or before the decision is fine.
    in.prediction_time = in.as_of;
    REQUIRE(gen.generate(in).has_value());
    in.prediction_time = in.as_of - minutes{1};
    REQUIRE(gen.generate(in).has_value());
}

TEST_CASE("a regression prediction becomes a directional signal", "[signal][generator]") {
    ModelSignalGenerator gen{"model", kModel};
    auto up = gen.generate(base_input(0.005));
    REQUIRE(up.has_value());
    REQUIRE(up->direction() == Direction::Long);
    REQUIRE(up->expected_return() == Catch::Approx(0.005));

    auto down = gen.generate(base_input(-0.005));
    REQUIRE(down->direction() == Direction::Short);
}

TEST_CASE("a probability prediction is converted explicitly", "[signal][generator]") {
    // A classifier gives direction, not magnitude. Converting needs an explicit
    // scale rather than pretending a probability is a return.
    GeneratorConfig cfg;
    cfg.kind = PredictionKind::Probability;
    cfg.probability_return_scale = 0.01;
    ModelSignalGenerator gen{"clf", kModel, cfg};

    auto bullish = gen.generate(base_input(0.9));
    REQUIRE(bullish->direction() == Direction::Long);
    // (0.9 - 0.5) * 2 * 0.01
    REQUIRE(bullish->expected_return() == Catch::Approx(0.008));
    REQUIRE(bullish->confidence() == Catch::Approx(0.8));

    auto bearish = gen.generate(base_input(0.1));
    REQUIRE(bearish->direction() == Direction::Short);

    // A coin flip is flat.
    REQUIRE(gen.generate(base_input(0.5))->is_flat());
}

TEST_CASE("the entry threshold produces flat rather than a weak trade", "[signal][generator]") {
    GeneratorConfig cfg;
    cfg.entry_threshold = 0.01;
    ModelSignalGenerator gen{"model", kModel, cfg};
    REQUIRE(gen.generate(base_input(0.005))->is_flat());
    REQUIRE(gen.generate(base_input(0.02))->direction() == Direction::Long);
}

TEST_CASE("disabling shorts yields flat not long", "[signal][generator][edge]") {
    // Flipping the direction would convert a bearish view into a bullish trade.
    GeneratorConfig cfg;
    cfg.allow_short = false;
    ModelSignalGenerator gen{"model", kModel, cfg};
    REQUIRE(gen.generate(base_input(-0.005))->is_flat());
    REQUIRE(gen.generate(base_input(0.005))->direction() == Direction::Long);
}

TEST_CASE("the rule generator inverts with a negative multiplier",
          "[signal][generator][baseline]") {
    // The permanent benchmark: same interface, same causality checks, same cost
    // accounting as a model, which is what makes the comparison meaningful.
    GeneratorConfig cfg;
    cfg.direction_multiplier = -1.0;
    RuleSignalGenerator rule{"reversal", 0x1234, cfg};

    auto s = rule.generate(base_input(0.005));
    REQUIRE(s.has_value());
    REQUIRE(s->direction() == Direction::Short);
    REQUIRE(s->model_id() == 0x1234);
}

TEST_CASE("ensemble members all see the same input", "[signal][ensemble][leakage]") {
    // No member can see another's output or any instant other than as_of, so an
    // ensemble cannot leak where its members do not.
    auto a = std::make_shared<ModelSignalGenerator>("a", 1);
    auto b = std::make_shared<ModelSignalGenerator>("b", 2);

    EnsembleSignalGenerator ens{"ens", 0xFFFF, EnsembleMethod::WeightedAverage};
    REQUIRE(ens.add(a, 1.0).has_value());
    REQUIRE(ens.add(b, 3.0).has_value());
    REQUIRE(ens.size() == 2);

    auto s = ens.generate(base_input(0.004));
    REQUIRE(s.has_value());
    REQUIRE(s->direction() == Direction::Long);
    REQUIRE(s->model_id() == 0xFFFF);

    // A future-stamped prediction is refused for the ensemble too.
    auto leaky = base_input(0.004);
    leaky.prediction_time = leaky.as_of + minutes{1};
    REQUIRE_FALSE(ens.generate(leaky).has_value());
}

TEST_CASE("ensemble voting resolves disagreement", "[signal][ensemble]") {
    GeneratorConfig bullish;
    GeneratorConfig bearish;
    bearish.direction_multiplier = -1.0;

    auto up1 = std::make_shared<ModelSignalGenerator>("up1", 1, bullish);
    auto up2 = std::make_shared<ModelSignalGenerator>("up2", 2, bullish);
    auto down = std::make_shared<ModelSignalGenerator>("down", 3, bearish);

    EnsembleSignalGenerator ens{"vote", 0xFFFF, EnsembleMethod::Voting};
    REQUIRE(ens.add(up1).has_value());
    REQUIRE(ens.add(up2).has_value());
    REQUIRE(ens.add(down).has_value());

    auto s = ens.generate(base_input(0.005));
    REQUIRE(s->direction() == Direction::Long);
    // Two of three agree.
    REQUIRE(s->confidence() == Catch::Approx(2.0 / 3.0));
}

TEST_CASE("an exactly tied vote is flat", "[signal][ensemble][edge]") {
    // A tie is genuinely undecided; picking a side would invent a view.
    GeneratorConfig bullish;
    GeneratorConfig bearish;
    bearish.direction_multiplier = -1.0;
    auto up = std::make_shared<ModelSignalGenerator>("up", 1, bullish);
    auto down = std::make_shared<ModelSignalGenerator>("down", 2, bearish);

    EnsembleSignalGenerator ens{"vote", 0xFFFF, EnsembleMethod::Voting};
    REQUIRE(ens.add(up).has_value());
    REQUIRE(ens.add(down).has_value());
    REQUIRE(ens.generate(base_input(0.005))->is_flat());
}

TEST_CASE("a zero-weight ensemble member is refused", "[signal][ensemble]") {
    // It would contribute nothing while appearing to be part of the ensemble.
    EnsembleSignalGenerator ens{"ens", 0xFFFF};
    auto member = std::make_shared<ModelSignalGenerator>("m", 1);
    REQUIRE_FALSE(ens.add(member, 0.0).has_value());
    REQUIRE_FALSE(ens.add(member, -1.0).has_value());
    REQUIRE_FALSE(ens.add(nullptr, 1.0).has_value());
    REQUIRE_FALSE(ens.generate(base_input(0.005)).has_value());  // empty ensemble
}

TEST_CASE("ensembles are deterministic", "[signal][ensemble][determinism]") {
    const auto build = [] {
        EnsembleSignalGenerator ens{"ens", 0xFFFF, EnsembleMethod::ConfidenceWeighted};
        REQUIRE(ens.add(std::make_shared<ModelSignalGenerator>("a", 1), 1.0).has_value());
        REQUIRE(ens.add(std::make_shared<ModelSignalGenerator>("b", 2), 2.5).has_value());
        auto s = ens.generate(base_input(0.004));
        REQUIRE(s.has_value());
        return std::make_pair(s->expected_return(), s->confidence());
    };
    REQUIRE(build() == build());
}

// ---------------------------------------------------------------------------
// Filters
// ---------------------------------------------------------------------------

namespace {

FilterContext good_context() {
    FilterContext ctx;
    ctx.now = at("2024-07-02T15:00:00Z");
    ctx.realized_volatility = 0.01;
    ctx.interval_volume = 100000.0;
    ctx.average_volume = 100000.0;
    ctx.spread_bps = Bps{2.0};
    ctx.feature_age = seconds{30};
    ctx.prediction_age = seconds{30};
    ctx.has_market_data = true;
    return ctx;
}

Signal actionable_signal() {
    CostEstimate costs;
    costs.half_spread = 0.0001;
    auto s = Signal::create(at("2024-07-02T15:00:00Z"), kSpy, Direction::Long, 0.005, 0.8,
                            minutes{15}, kModel, costs);
    REQUIRE(s.has_value());
    return *s;
}

}  // namespace

TEST_CASE("a clean signal passes every filter", "[signal][filter]") {
    SignalFilterChain chain;
    const auto d = chain.evaluate(actionable_signal(), good_context(), &us());
    INFO(d.describe());
    REQUIRE(d.passed());
}

TEST_CASE("missing market data is refused before anything else", "[signal][filter][leakage]") {
    // Every check below reads market state; a filter evaluated without data
    // reports compliance it cannot support.
    SignalFilterChain chain;
    auto ctx = good_context();
    ctx.has_market_data = false;
    REQUIRE(chain.evaluate(actionable_signal(), ctx, &us()).reason == FilterReason::NoMarketData);
}

TEST_CASE("stale features and predictions are refused", "[signal][filter][leakage]") {
    SignalFilterChain chain;
    auto stale_features = good_context();
    stale_features.feature_age = hours{2};
    REQUIRE(chain.evaluate(actionable_signal(), stale_features, &us()).reason ==
            FilterReason::StaleFeatures);

    auto stale_prediction = good_context();
    stale_prediction.prediction_age = hours{2};
    REQUIRE(chain.evaluate(actionable_signal(), stale_prediction, &us()).reason ==
            FilterReason::StalePrediction);
}

TEST_CASE("low confidence and negative edge are refused", "[signal][filter]") {
    SignalFilterChain chain;
    CostEstimate costs;
    costs.half_spread = 0.01;  // larger than the expected move

    auto weak = Signal::create(at("2024-07-02T15:00:00Z"), kSpy, Direction::Long, 0.005, 0.2,
                               minutes{15}, kModel);
    REQUIRE(chain.evaluate(*weak, good_context(), &us()).reason == FilterReason::LowConfidence);

    auto unprofitable = Signal::create(at("2024-07-02T15:00:00Z"), kSpy, Direction::Long, 0.005,
                                       0.9, minutes{15}, kModel, costs);
    REQUIRE(chain.evaluate(*unprofitable, good_context(), &us()).reason ==
            FilterReason::NegativeNetEdge);
}

TEST_CASE("market condition filters bind", "[signal][filter]") {
    SignalFilterChain chain;

    auto volatile_ctx = good_context();
    volatile_ctx.realized_volatility = 5.0;
    REQUIRE(chain.evaluate(actionable_signal(), volatile_ctx, &us()).reason ==
            FilterReason::VolatilityOutOfRange);

    auto illiquid = good_context();
    illiquid.interval_volume = 100.0;  // 0.1% of average
    REQUIRE(chain.evaluate(actionable_signal(), illiquid, &us()).reason ==
            FilterReason::InsufficientLiquidity);

    auto wide = good_context();
    wide.spread_bps = Bps{100.0};
    REQUIRE(chain.evaluate(actionable_signal(), wide, &us()).reason == FilterReason::SpreadTooWide);
}

TEST_CASE("the open and close buffers are enforced", "[signal][filter][calendar]") {
    // The auctions have different microstructure, and a model trained on
    // continuous trading does not describe them.
    SignalFilterChain chain;
    const auto session = us().session_on(at("2024-07-02"));
    REQUIRE(session.has_value());

    auto at_open = good_context();
    at_open.now = session->open + minutes{1};
    REQUIRE(chain.evaluate(actionable_signal(), at_open, &us()).reason ==
            FilterReason::OutsideTradingHours);

    auto at_close = good_context();
    at_close.now = session->close - minutes{1};
    REQUIRE(chain.evaluate(actionable_signal(), at_close, &us()).reason ==
            FilterReason::OutsideTradingHours);

    auto midday = good_context();
    midday.now = session->open + hours{2};
    REQUIRE(chain.evaluate(actionable_signal(), midday, &us()).passed());
}

TEST_CASE("the cooldown prevents churn", "[signal][filter]") {
    // Stops a noisy model flipping a position on consecutive bars, which costs
    // real money and produces no expected return.
    SignalFilterChain chain;
    auto ctx = good_context();

    const auto first = chain.evaluate(actionable_signal(), ctx, &us());
    REQUIRE(first.passed());
    chain.record(kSpy, first, ctx.now);

    ctx.now = ctx.now + minutes{1};
    REQUIRE(chain.evaluate(actionable_signal(), ctx, &us()).reason == FilterReason::CooldownActive);

    ctx.now = ctx.now + minutes{20};
    REQUIRE(chain.evaluate(actionable_signal(), ctx, &us()).passed());
}

TEST_CASE("a flat signal bypasses the filters", "[signal][filter][edge]") {
    // Passing it through keeps "the model said stay out" distinguishable from
    // "a filter removed the signal".
    SignalFilterChain chain;
    auto ctx = good_context();
    ctx.has_market_data = false;
    REQUIRE(chain.evaluate(Signal::flat(ctx.now, kSpy, kModel), ctx, &us()).passed());
}

TEST_CASE("filter outcomes are counted and reported", "[signal][filter]") {
    // A suppressed signal that vanishes without trace makes a backtest diverge
    // from live trading invisibly.
    SignalFilterChain chain;
    auto ctx = good_context();
    ctx.has_market_data = false;
    for (int i = 0; i < 3; ++i) {
        chain.record(kSpy, chain.evaluate(actionable_signal(), ctx, &us()), ctx.now);
    }
    REQUIRE(chain.rejection_count() == 3);
    REQUIRE(chain.rejection_count(FilterReason::NoMarketData) == 3);
    REQUIRE(chain.summary().find("no_market_data") != std::string::npos);
}

TEST_CASE("filter evaluation is pure", "[signal][filter][determinism]") {
    // Given the same signal and context, the answer is always the same:
    // evaluate() mutates nothing.
    SignalFilterChain chain;
    const auto sig = actionable_signal();
    const auto ctx = good_context();
    REQUIRE(chain.evaluate(sig, ctx, &us()).reason == chain.evaluate(sig, ctx, &us()).reason);
    REQUIRE(chain.rejection_count() == 0);
    REQUIRE(chain.pass_count() == 0);
}
