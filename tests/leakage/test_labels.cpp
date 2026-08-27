#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <vector>

#include "ptl/labels/label.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::labels;
using namespace std::chrono;

namespace {

Timestamp at(const char* iso) {
    Timestamp ts{};
    REQUIRE(parse_timestamp(iso, ts));
    return ts;
}

constexpr InstrumentId kSpy{0};

/// A deterministic midprice series.
std::vector<PricePoint> series(std::size_t n, double start = 100.0, double step = 0.10) {
    std::vector<PricePoint> out;
    out.reserve(n);
    Timestamp t = at("2024-07-02T14:00:00Z");
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back({t, kSpy, Price{start + static_cast<double>(i) * step}});
        t += minutes{1};
    }
    return out;
}

}  // namespace

TEST_CASE("labels carry all four interval stamps", "[labels][leakage]") {
    // Purging tests interval OVERLAP. With a horizon longer than the decision
    // step the labels overlap, and an endpoint comparison would leave
    // contaminated rows in the training set.
    LabelConfig cfg;
    cfg.kind = LabelKind::ForwardLogReturn;
    cfg.horizon = 15;
    cfg.winsorize = false;

    auto set = LabelBuilder{cfg}.build(series(100));
    REQUIRE(set.has_value());

    const auto& l = set->labels[10];
    REQUIRE(l.valid);
    REQUIRE(l.interval.ok());
    REQUIRE(l.interval.feature_end_time == at("2024-07-02T14:10:00Z"));
    REQUIRE(l.interval.label_start_time == l.interval.feature_end_time);
    // The label ends fifteen bars later: that is the interval purging must see.
    REQUIRE(l.interval.label_end_time == at("2024-07-02T14:25:00Z"));
}

TEST_CASE("rows without a full horizon are invalid not silently shortened", "[labels][leakage]") {
    // Giving the last h rows a shorter horizon would make the target mean
    // something different for them, and nothing downstream would notice.
    LabelConfig cfg;
    cfg.horizon = 10;
    cfg.winsorize = false;

    auto set = LabelBuilder{cfg}.build(series(50));
    REQUIRE(set.has_value());
    REQUIRE(set->size() == 50);
    REQUIRE(set->valid_count() == 40);
    for (std::size_t i = 40; i < 50; ++i) {
        REQUIRE_FALSE(set->labels[i].valid);
    }
    REQUIRE(set->valid_rows().size() == 40);
}

TEST_CASE("the label is independent of the execution price", "[labels][leakage]") {
    // THE NAMED TEST (test_label_price_independence). The builder consumes a
    // MIDPRICE series and has no access to a fill model at all -- coupling the
    // target to the execution price would mean a change to the simulator
    // silently changed what the model was trained to predict.
    LabelConfig cfg;
    cfg.horizon = 5;
    cfg.winsorize = false;

    const auto prices = series(60);
    auto a = LabelBuilder{cfg}.build(prices);
    auto b = LabelBuilder{cfg}.build(prices);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());

    // Bit-identical across builds: nothing in the label path carries state.
    for (std::size_t i = 0; i < a->size(); ++i) {
        REQUIRE(a->labels[i].value == b->labels[i].value);
    }

    // The interface only accepts PricePoint{ts, instrument, mid}. There is no
    // overload taking a Bar, so a close price cannot be passed by accident.
    const double expected = std::log(prices[5].mid.get() / prices[0].mid.get());
    REQUIRE(a->labels[0].value == Catch::Approx(expected));
}

TEST_CASE("an unsorted price series is refused", "[labels][leakage]") {
    // An unsorted series would look forward by accident: index i+h would not be
    // h bars in the future.
    auto prices = series(20);
    std::swap(prices[5], prices[10]);
    auto r = LabelBuilder{LabelConfig{}}.build(prices);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.find("chronological") != std::string::npos);
}

TEST_CASE("vol-normalised labels use only trailing data", "[labels][leakage]") {
    // The normaliser must be knowable at the decision instant. If it used the
    // full-sample volatility, every label would encode information from the
    // future -- a subtle leak that survives most reviews.
    LabelConfig cfg;
    cfg.kind = LabelKind::VolNormalisedReturn;
    cfg.horizon = 5;
    cfg.vol_window = 20;
    cfg.winsorize = false;

    const auto full = series(200);
    auto whole = LabelBuilder{cfg}.build(full);
    REQUIRE(whole.has_value());

    // Truncating the series AFTER row 100 must not change row 50's label: its
    // normaliser only ever looked backwards.
    std::vector<PricePoint> truncated(full.begin(), full.begin() + 100);
    auto partial = LabelBuilder{cfg}.build(truncated);
    REQUIRE(partial.has_value());
    REQUIRE(whole->labels[50].value == partial->labels[50].value);
}

TEST_CASE("direction labels threshold the forward return", "[labels]") {
    LabelConfig cfg;
    cfg.kind = LabelKind::Direction;
    cfg.horizon = 1;

    auto up = LabelBuilder{cfg}.build(series(10, 100.0, 0.5));
    REQUIRE(up.has_value());
    REQUIRE(up->labels[0].value == Catch::Approx(1.0));

    auto down = LabelBuilder{cfg}.build(series(10, 100.0, -0.5));
    REQUIRE(down.has_value());
    REQUIRE(down->labels[0].value == Catch::Approx(0.0));
}

TEST_CASE("cost-aware labels zero out moves that do not pay for the trade", "[labels]") {
    // A move smaller than the round trip is not an opportunity, and labelling
    // it positive teaches the model to trade unprofitably.
    LabelConfig cfg;
    cfg.kind = LabelKind::CostAwareReturn;
    cfg.horizon = 1;
    cfg.round_trip_cost_bps = 50.0;  // 0.5%
    cfg.winsorize = false;

    // A 0.1% move: smaller than the cost.
    auto small = LabelBuilder{cfg}.build(series(5, 100.0, 0.1));
    REQUIRE(small->labels[0].value == Catch::Approx(0.0));

    // A 2% move: larger, and the cost is deducted from the magnitude.
    auto large = LabelBuilder{cfg}.build(series(5, 100.0, 2.0));
    REQUIRE(large->labels[0].value > 0.0);
    REQUIRE(large->labels[0].value < std::log(102.0 / 100.0));
}

TEST_CASE("triple-barrier labels report which barrier was touched first", "[labels]") {
    LabelConfig cfg;
    cfg.kind = LabelKind::TripleBarrier;
    cfg.horizon = 20;
    cfg.vol_window = 20;
    cfg.barrier_upper_sigma = 2.0;
    cfg.barrier_lower_sigma = 2.0;

    // A steadily rising series touches the upper barrier.
    auto rising = LabelBuilder{cfg}.build(series(60, 100.0, 0.05));
    REQUIRE(rising.has_value());
    bool saw_upper = false;
    for (const auto& l : rising->labels) {
        if (l.valid && l.barrier_touched == 1) saw_upper = true;
    }
    REQUIRE(saw_upper);

    // A flat series touches neither: a genuine third outcome, not a missing
    // value.
    auto flat = LabelBuilder{cfg}.build(series(60, 100.0, 0.0));
    REQUIRE(flat.has_value());
    for (const auto& l : flat->labels) {
        if (l.valid) REQUIRE(l.barrier_touched == 0);
    }
}

TEST_CASE("winsorization clips continuous labels but not categorical ones", "[labels][edge]") {
    // Clipping a categorical label would collapse its classes.
    std::vector<PricePoint> spiky = series(100);
    spiky[50].mid = Price{1000.0};  // an extreme outlier

    LabelConfig cont;
    cont.kind = LabelKind::ForwardLogReturn;
    cont.horizon = 1;
    cont.winsorize = true;
    cont.winsor_sigma = 2.0;
    auto clipped = LabelBuilder{cont}.build(spiky);
    REQUIRE(clipped.has_value());

    LabelConfig raw = cont;
    raw.winsorize = false;
    auto unclipped = LabelBuilder{raw}.build(spiky);
    REQUIRE(std::abs(clipped->labels[49].value) < std::abs(unclipped->labels[49].value));

    LabelConfig dir;
    dir.kind = LabelKind::Direction;
    dir.horizon = 1;
    dir.winsorize = true;
    auto categorical = LabelBuilder{dir}.build(spiky);
    for (const auto& l : categorical->labels) {
        if (l.valid) REQUIRE((l.value == 0.0 || l.value == 1.0));
    }
}

TEST_CASE("overlapping labels are down-weighted", "[labels]") {
    // A horizon longer than the step means consecutive labels share
    // information. Uniform weights would silently multiply the effective
    // sample size and make every standard error too small.
    LabelConfig cfg;
    cfg.horizon = 10;
    auto set = LabelBuilder{cfg}.build(series(50));
    REQUIRE(set->labels[0].weight == Catch::Approx(0.1));
}

TEST_CASE("the config hash covers every parameter", "[labels][determinism]") {
    // The horizon is declared up front and hashed, so it cannot be tuned after
    // seeing results.
    LabelConfig a;
    LabelConfig b = a;
    REQUIRE(a.hash() == b.hash());

    b.horizon = a.horizon + 1;
    REQUIRE(a.hash() != b.hash());

    LabelConfig c = a;
    c.winsor_sigma = 3.0;
    REQUIRE(a.hash() != c.hash());

    LabelConfig d = a;
    d.kind = LabelKind::TripleBarrier;
    REQUIRE(a.hash() != d.hash());
}

TEST_CASE("a panel is ordered deterministically", "[labels][determinism]") {
    // Sorted by decision time then instrument. Without the tie-break, two runs
    // could order simultaneous observations differently, and float summation is
    // not associative.
    std::vector<std::vector<PricePoint>> panel;
    for (std::uint32_t inst = 0; inst < 3; ++inst) {
        auto s = series(20);
        for (auto& p : s) p.instrument = static_cast<InstrumentId>(inst);
        panel.push_back(std::move(s));
    }

    LabelConfig cfg;
    cfg.horizon = 5;
    auto a = LabelBuilder{cfg}.build_panel(panel);
    REQUIRE(a.has_value());

    std::reverse(panel.begin(), panel.end());
    auto b = LabelBuilder{cfg}.build_panel(panel);
    REQUIRE(b.has_value());

    REQUIRE(a->size() == b->size());
    for (std::size_t i = 0; i < a->size(); ++i) {
        REQUIRE(a->labels[i].instrument == b->labels[i].instrument);
        REQUIRE(a->labels[i].interval.feature_end_time == b->labels[i].interval.feature_end_time);
        REQUIRE(a->labels[i].value == b->labels[i].value);
    }
}
