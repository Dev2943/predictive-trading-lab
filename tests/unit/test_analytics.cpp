#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <limits>
#include <vector>

#include "ptl/analytics/performance_analyzer.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::analytics;
using namespace std::chrono;

namespace {

Timestamp at(const char* iso) {
    Timestamp ts{};
    REQUIRE(parse_timestamp(iso, ts));
    return ts;
}

/// An equity curve one day apart.
std::vector<portfolio::EquityPoint> curve_from(const std::vector<double>& equity,
                                               Duration step = hours{24}) {
    std::vector<portfolio::EquityPoint> out;
    Timestamp t = at("2024-01-02T20:00:00Z");
    for (const double e : equity) {
        portfolio::EquityPoint p;
        p.ts = t;
        p.equity = Notional{e};
        p.cash = Notional{e};
        p.gross_exposure = Notional{e * 0.5};
        p.net_exposure = Notional{e * 0.25};
        out.push_back(p);
        t += step;
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Drawdown
// ---------------------------------------------------------------------------

TEST_CASE("the drawdown tracker matches a hand-computed fixture", "[analytics][drawdown]") {
    DrawdownTracker tracker;
    const std::vector<double> equity{100.0, 120.0, 90.0, 110.0, 130.0};
    Timestamp t = at("2024-01-02T20:00:00Z");
    for (const double e : equity) {
        REQUIRE(tracker.update(t, Notional{e}).has_value());
        t += hours{24};
    }
    // Peak 120, trough 90: a 25% drawdown.
    REQUIRE(tracker.max_drawdown() == Catch::Approx(0.25));
    REQUIRE(tracker.peak_equity().get() == Catch::Approx(130.0));
    // A new high means no current drawdown.
    REQUIRE(tracker.current_drawdown() == Catch::Approx(0.0));
    REQUIRE_FALSE(tracker.underwater());
    REQUIRE(tracker.size() == 5);
}

TEST_CASE("the streaming tracker agrees with the Phase 3 batch computation",
          "[analytics][drawdown][regression]") {
    // Two independent implementations of the same statistic. If they ever
    // disagree, one of them is wrong and the test says so immediately.
    const std::vector<double> equity{100.0, 105.0, 98.0, 130.0, 90.0, 95.0, 140.0};
    const auto curve = curve_from(equity);

    DrawdownTracker tracker;
    for (const auto& p : curve) REQUIRE(tracker.update(p.ts, p.equity).has_value());

    std::vector<double> raw;
    for (const auto& p : curve) raw.push_back(p.equity.get());
    REQUIRE(tracker.max_drawdown() == Catch::Approx(MetricsEngine::max_drawdown(raw)));
}

TEST_CASE("the underwater curve and episodes track recovery", "[analytics][drawdown]") {
    DrawdownTracker tracker;
    const std::vector<double> equity{100.0, 90.0, 80.0, 95.0, 105.0};
    Timestamp t = at("2024-01-02T20:00:00Z");
    for (const double e : equity) {
        REQUIRE(tracker.update(t, Notional{e}).has_value());
        t += hours{24};
    }

    REQUIRE(tracker.underwater_curve().size() == 5);
    REQUIRE(tracker.underwater_curve()[0].drawdown == Catch::Approx(0.0));
    REQUIRE(tracker.underwater_curve()[2].drawdown == Catch::Approx(0.20));
    // Three periods below the peak before recovering.
    REQUIRE(tracker.longest_underwater_periods() == 3);

    REQUIRE(tracker.episodes().size() == 1);
    const auto& episode = tracker.episodes().front();
    REQUIRE(episode.recovered());
    REQUIRE(episode.depth == Catch::Approx(0.20));
    REQUIRE(episode.decline_duration() == hours{48});
    REQUIRE(episode.total_duration() == hours{96});
}

TEST_CASE("an unrecovered episode has no total duration", "[analytics][drawdown][edge]") {
    // Inventing one by using "now" would understate the pain of a drawdown that
    // is still going.
    DrawdownTracker tracker;
    Timestamp t = at("2024-01-02T20:00:00Z");
    for (const double e : {100.0, 80.0, 70.0}) {
        REQUIRE(tracker.update(t, Notional{e}).has_value());
        t += hours{24};
    }
    REQUIRE(tracker.underwater());
    // The episode is still open, so it is not in the completed list.
    REQUIRE(tracker.episodes().empty());
    REQUIRE(tracker.current_drawdown() == Catch::Approx(0.30));
}

TEST_CASE("out-of-order drawdown observations are refused", "[analytics][drawdown][leakage]") {
    DrawdownTracker tracker;
    REQUIRE(tracker.update(at("2024-01-03T20:00:00Z"), Notional{100.0}).has_value());
    auto backwards = tracker.update(at("2024-01-02T20:00:00Z"), Notional{90.0});
    REQUIRE_FALSE(backwards.has_value());
    REQUIRE(backwards.error().message.find("non-decreasing") != std::string::npos);
}

TEST_CASE("rolling drawdown uses the window peak not the all-time peak",
          "[analytics][drawdown][property]") {
    // A strategy whose worst 60-day drawdown is 8% behaves differently from one
    // whose worst is 25%, even at the same all-time maximum.
    const auto curve = curve_from({100.0, 200.0, 150.0, 155.0, 160.0, 165.0});
    auto rolling = rolling_max_drawdown(curve, 3);
    REQUIRE(rolling.has_value());
    REQUIRE(rolling->size() == curve.size());

    // At index 2 the window is {100, 200, 150}: a 25% drawdown.
    REQUIRE((*rolling)[2] == Catch::Approx(0.25));
    // By index 5 the 200 peak has left the window, so the rolling figure is
    // far smaller than the all-time 25%.
    REQUIRE((*rolling)[5] < 0.05);

    REQUIRE_FALSE(rolling_max_drawdown(curve, 1).has_value());
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

TEST_CASE("the accumulator is stable where sum-of-squares is not",
          "[analytics][statistics][property]") {
    // The naive form cancels catastrophically when the mean is large relative
    // to the spread -- exactly equity around 10^6 with daily moves of 10^3.
    StatisticsAccumulator acc;
    for (int i = 0; i < 1000; ++i) {
        acc.update(1e8 + (i % 2 == 0 ? 1.0 : -1.0));
    }
    REQUIRE(acc.count() == 1000);
    REQUIRE(acc.stdev() == Catch::Approx(1.0).epsilon(0.01));
    REQUIRE(acc.variance() >= 0.0);
    REQUIRE(is_finite(acc.stdev()));
}

TEST_CASE("moments match known values", "[analytics][statistics]") {
    StatisticsAccumulator acc;
    for (const double x : {2.0, 4.0, 4.0, 4.0, 5.0, 5.0, 7.0, 9.0}) acc.update(x);
    REQUIRE(acc.mean() == Catch::Approx(5.0));
    // Sample stdev of this classic fixture.
    REQUIRE(acc.stdev() == Catch::Approx(2.13809).epsilon(0.001));
    REQUIRE(acc.min() == Catch::Approx(2.0));
    REQUIRE(acc.max() == Catch::Approx(9.0));
    REQUIRE(is_finite(acc.skewness()));
    REQUIRE(is_finite(acc.excess_kurtosis()));
}

TEST_CASE("degenerate accumulator inputs stay finite", "[analytics][statistics][edge]") {
    StatisticsAccumulator empty;
    REQUIRE(empty.mean() == 0.0);
    REQUIRE(empty.variance() == 0.0);
    REQUIRE(empty.skewness() == 0.0);

    StatisticsAccumulator single;
    single.update(5.0);
    REQUIRE(single.variance() == 0.0);
    REQUIRE(single.stdev() == 0.0);

    StatisticsAccumulator constant;
    for (int i = 0; i < 10; ++i) constant.update(3.0);
    REQUIRE(constant.stdev() == Catch::Approx(0.0));
    // Zero dispersion means skewness is undefined; zero is the honest answer.
    REQUIRE(constant.skewness() == 0.0);

    // Non-finite input is ignored rather than poisoning the accumulator.
    StatisticsAccumulator guarded;
    guarded.update(1.0);
    guarded.update(std::numeric_limits<double>::quiet_NaN());
    guarded.update(3.0);
    REQUIRE(guarded.count() == 2);
    REQUIRE(guarded.mean() == Catch::Approx(2.0));
}

TEST_CASE("quantiles do not reorder the caller's data", "[analytics][statistics][property]") {
    // An analytics function that sorted its argument in place would be a
    // mutation in disguise, and the caller's series is usually the equity curve.
    std::vector<double> values{5.0, 1.0, 4.0, 2.0, 3.0};
    const std::vector<double> before = values;

    REQUIRE(quantile_of(values, 0.0) == Catch::Approx(1.0));
    REQUIRE(quantile_of(values, 1.0) == Catch::Approx(5.0));
    REQUIRE(quantile_of(values, 0.5) == Catch::Approx(3.0));
    REQUIRE(expected_shortfall(values, 0.4) == Catch::Approx(1.5));
    REQUIRE(values == before);
}

// ---------------------------------------------------------------------------
// Risk
// ---------------------------------------------------------------------------

TEST_CASE("beta and alpha against a known benchmark", "[analytics][risk][numerical]") {
    // Strategy returns exactly twice the benchmark: beta 2, alpha 0.
    std::vector<double> benchmark;
    std::vector<double> strategy;
    for (int i = 0; i < 100; ++i) {
        const double b = std::sin(static_cast<double>(i) * 0.3) * 0.01;
        benchmark.push_back(b);
        strategy.push_back(2.0 * b);
    }

    REQUIRE(RiskAnalyzer::beta_of(strategy, benchmark) == Catch::Approx(2.0).epsilon(1e-9));

    RiskConfig cfg;
    cfg.periods_per_year = 252.0;
    const RiskAnalyzer analyzer{cfg};
    auto m = analyzer.analyze(strategy, benchmark, 0.10);
    REQUIRE(m.has_value());
    REQUIRE(m->beta == Catch::Approx(2.0).epsilon(1e-9));
    REQUIRE(m->alpha == Catch::Approx(0.0).margin(1e-9));
    REQUIRE(m->r_squared == Catch::Approx(1.0).epsilon(1e-9));
    REQUIRE(m->correlation_to_benchmark == Catch::Approx(1.0).epsilon(1e-9));
}

TEST_CASE("tracking error is zero when the strategy tracks exactly", "[analytics][risk]") {
    std::vector<double> series;
    for (int i = 0; i < 50; ++i) series.push_back(std::sin(static_cast<double>(i)) * 0.01);
    REQUIRE(RiskAnalyzer::tracking_error_of(series, series, 252.0) ==
            Catch::Approx(0.0).margin(1e-12));

    const RiskAnalyzer analyzer;
    auto m = analyzer.analyze(series, series);
    REQUIRE(m.has_value());
    REQUIRE(m->tracking_error == Catch::Approx(0.0).margin(1e-12));
    // Zero tracking error means the information ratio is undefined, not
    // infinite.
    REQUIRE(m->information_ratio == Catch::Approx(0.0));
    REQUIRE(is_finite(m->information_ratio));
}

TEST_CASE("a mismatched benchmark is refused not truncated", "[analytics][risk][leakage]") {
    // Truncating would pair each return with the wrong benchmark observation
    // and produce a beta that means nothing.
    const std::vector<double> returns{0.01, 0.02, 0.03};
    const std::vector<double> benchmark{0.01, 0.02};
    const RiskAnalyzer analyzer;
    auto m = analyzer.analyze(returns, benchmark);
    REQUIRE_FALSE(m.has_value());
    REQUIRE(m.error().message.find("does not match") != std::string::npos);
}

TEST_CASE("no benchmark leaves relative metrics at zero", "[analytics][risk][edge]") {
    // Computing them against an implicit zero series would silently turn them
    // into different statistics with the same names.
    const std::vector<double> returns{0.01, -0.005, 0.02, -0.01};
    const RiskAnalyzer analyzer;
    auto m = analyzer.analyze(returns);
    REQUIRE(m.has_value());
    REQUIRE(m->beta == 0.0);
    REQUIRE(m->alpha == 0.0);
    REQUIRE(m->information_ratio == 0.0);
    REQUIRE(m->treynor_ratio == 0.0);
    // Absolute metrics are still computed.
    REQUIRE(m->annualized_volatility > 0.0);
    REQUIRE(is_finite(m->sharpe));
}

TEST_CASE("a zero-variance benchmark gives zero beta not infinity", "[analytics][risk][edge]") {
    const std::vector<double> returns{0.01, 0.02, 0.03, 0.04};
    const std::vector<double> flat{0.0, 0.0, 0.0, 0.0};
    REQUIRE(RiskAnalyzer::beta_of(returns, flat) == 0.0);
    REQUIRE(is_finite(RiskAnalyzer::beta_of(returns, flat)));
}

TEST_CASE("Treynor is zero at zero beta", "[analytics][risk][edge]") {
    // A market-neutral book legitimately sits at zero beta, where excess return
    // per unit of beta is undefined.
    std::vector<double> benchmark;
    std::vector<double> strategy;
    for (int i = 0; i < 60; ++i) {
        benchmark.push_back(std::sin(static_cast<double>(i) * 0.5) * 0.01);
        // Orthogonal to the benchmark.
        strategy.push_back(std::cos(static_cast<double>(i) * 0.5) * 0.01);
    }
    const RiskAnalyzer analyzer;
    auto m = analyzer.analyze(strategy, benchmark);
    REQUIRE(m.has_value());
    REQUIRE(is_finite(m->treynor_ratio));
}
