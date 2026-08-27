#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <limits>
#include <vector>

#include "ptl/features/feature_set.hpp"
#include "ptl/features/intraday.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::features;
using namespace std::chrono;

namespace {

Timestamp at(const char* iso) {
    Timestamp ts{};
    REQUIRE(parse_timestamp(iso, ts));
    return ts;
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

TEST_CASE("minute of session is session-relative not clock-relative",
          "[features][intraday][calendar]") {
    // A half day and a regular day both start at minute 0, so a per-minute
    // statistic stays aligned on the ~6 sessions a year that close early
    // (ADR-0001 Addendum A2).
    const auto regular = us().session_on(at("2024-07-02"));
    const auto half = us().session_on(at("2024-07-03"));
    REQUIRE(regular.has_value());
    REQUIRE(half->kind == market::SessionKind::HalfDay);

    REQUIRE(minute_of_session(regular->open, *regular) == 0);
    REQUIRE(minute_of_session(regular->open + minutes{30}, *regular) == 30);
    // Both sessions open at 09:30 local, so minute 30 means the same clock time
    // AND the same session position on each.
    REQUIRE(minute_of_session(half->open + minutes{30}, *half) == 30);

    // Before the open there is no session minute.
    REQUIRE(minute_of_session(regular->open - minutes{1}, *regular) == -1);
}

TEST_CASE("relative volume is flat under a pure U-shape", "[features][intraday][leakage]") {
    // THE NAMED TEST FROM THE RECONCILIATION (row F3).
    //
    // Intraday volume has a strong U-shape. A plain trailing z-score would
    // mostly encode "it is near the open" rather than genuine surprise. Feeding
    // an identical U-curve every session must therefore produce relative volume
    // of exactly 1.0 at every minute -- no surprise anywhere, because nothing
    // was surprising.
    constexpr std::size_t kSlots = 390;
    MinuteOfDayVolumeProfile profile{kSlots, 20};

    const auto u_shape = [](std::size_t minute) {
        const double x = static_cast<double>(minute) / static_cast<double>(kSlots - 1);
        // High at the open and close, low midday.
        return 10000.0 * (1.0 + 4.0 * (x - 0.5) * (x - 0.5));
    };

    for (int session = 0; session < 25; ++session) {
        for (std::size_t m = 0; m < kSlots; ++m) {
            const double v = u_shape(m);
            if (session > 0) {
                INFO("session " << session << " minute " << m);
                REQUIRE(profile.relative_volume(static_cast<std::int32_t>(m), v) ==
                        Catch::Approx(1.0).epsilon(1e-9));
            }
            profile.update(static_cast<std::int32_t>(m), v);
        }
    }

    // And a genuine surprise IS detected: double the usual volume at minute 200.
    REQUIRE(profile.relative_volume(200, u_shape(200) * 2.0) == Catch::Approx(2.0));
}

TEST_CASE("relative volume is neutral when the baseline is empty", "[features][intraday][edge]") {
    // A slot with no history is uninformative, not infinitely surprising. An
    // inf here would flow into a feature vector and poison every aggregate.
    MinuteOfDayVolumeProfile profile{390, 20};
    REQUIRE(profile.relative_volume(5, 1000.0) == Catch::Approx(1.0));
    REQUIRE(is_finite(profile.relative_volume(5, 1000.0)));
    // Zero-volume history likewise.
    for (int i = 0; i < 25; ++i) profile.update(5, 0.0);
    REQUIRE(profile.relative_volume(5, 1000.0) == Catch::Approx(1.0));
}

TEST_CASE("VWAP weights by volume and TWAP does not", "[features][intraday]") {
    // The two answer different questions, and their gap says where volume
    // clustered in the interval.
    RollingVwap vwap{3};
    RollingTwap twap{3};

    vwap.update(100.0, 1.0);
    vwap.update(200.0, 99.0);
    vwap.update(150.0, 0.0);
    twap.update(100.0);
    twap.update(200.0);
    twap.update(150.0);

    // VWAP is dragged toward 200 because that is where the volume traded.
    REQUIRE(vwap.value() == Catch::Approx((100.0 + 200.0 * 99.0) / 100.0));
    REQUIRE(twap.value() == Catch::Approx(150.0));
    REQUIRE(vwap.value() > twap.value());
}

TEST_CASE("VWAP over zero volume returns the last price not a division by zero",
          "[features][intraday][edge]") {
    RollingVwap vwap{5};
    vwap.update(123.45, 0.0);
    REQUIRE(vwap.value() == Catch::Approx(123.45));
    REQUIRE(is_finite(vwap.value()));
}

TEST_CASE("session VWAP resets at the open", "[features][intraday]") {
    // The trading benchmark is the whole session, not a trailing window, and
    // resetting is what makes it comparable to the venue's published figure.
    SessionVwap v;
    v.update(100.0, 100.0);
    v.update(200.0, 100.0);
    REQUIRE(v.value() == Catch::Approx(150.0));

    v.on_session_open();
    REQUIRE_FALSE(v.ready());
    v.update(300.0, 50.0);
    REQUIRE(v.value() == Catch::Approx(300.0));
}

TEST_CASE("spread statistics report level and surprise", "[features][intraday]") {
    SpreadStatistics s{20};
    for (int i = 0; i < 25; ++i) s.update(99.99, 100.01);
    REQUIRE(s.current_bps() == Catch::Approx(2.0));
    REQUIRE(s.ready());
    // A constant spread has zero dispersion, so the z-score is zero rather
    // than infinite.
    REQUIRE(s.zscore() == Catch::Approx(0.0));
    REQUIRE(is_finite(s.zscore()));

    s.update(99.90, 100.10);  // a much wider spread
    REQUIRE(s.zscore() > 0.0);
}

TEST_CASE("illiquidity ignores zero-volume intervals", "[features][intraday][edge]") {
    AmihudIlliquidity a{10};
    a.update(0.01, 0.0);  // no dollar volume: skipped, not infinite
    REQUIRE(is_finite(a.value()));
    for (int i = 0; i < 12; ++i) a.update(0.01, 1e6);
    REQUIRE(a.value() > 0.0);
    REQUIRE(is_finite(a.value()));
}

TEST_CASE("volatility buckets classify regimes", "[features][intraday]") {
    // Regime conditioning distinguishes a model that stopped working from one
    // merely operating in an unfamiliar regime.
    VolatilityBucket b{30};
    for (int i = 0; i < 40; ++i) b.update(0.010);
    REQUIRE(b.ready());
    REQUIRE(b.regime() == VolatilityBucket::Regime::Normal);
}

TEST_CASE("seasonal controls are smooth bounded and periodic", "[features][intraday]") {
    // Raw minute-of-day would force a model to relearn the clock and would
    // extrapolate nonsensically past the session edge.
    SeasonalControls s;
    s.update(0.0);
    REQUIRE(s.sin_component == Catch::Approx(0.0).margin(1e-12));
    REQUIRE(s.cos_component == Catch::Approx(1.0));
    s.update(0.25);
    REQUIRE(s.sin_component == Catch::Approx(1.0));
    s.update(1.0);
    REQUIRE(s.cos_component == Catch::Approx(1.0));
    // Out-of-range progress is clamped, never wrapped into a wrong phase.
    s.update(2.5);
    REQUIRE(s.cos_component == Catch::Approx(1.0));
    REQUIRE(std::abs(s.sin_component) <= 1.0);
}

TEST_CASE("realized volatility annualises by the configured factor", "[features][momentum]") {
    RealizedVolatility per_period{10, 1.0};
    RealizedVolatility annualized{10, 252.0 * 390.0};
    for (int i = 0; i < 12; ++i) {
        const double r = (i % 2 == 0) ? 0.001 : -0.001;
        per_period.update(r);
        annualized.update(r);
    }
    REQUIRE(per_period.value() == Catch::Approx(0.001));
    REQUIRE(annualized.value() == Catch::Approx(0.001 * std::sqrt(252.0 * 390.0)));
}

TEST_CASE("ATR uses Wilder smoothing not a simple average", "[features][momentum]") {
    // Every published ATR level assumes Wilder (alpha = 1/N). An SMA-based ATR
    // is a different indicator wearing the same name.
    AverageTrueRange atr{14};
    atr.update(101.0, 99.0, 100.0);
    REQUIRE(atr.value() == Catch::Approx(2.0));  // seeded with the first TR

    // A gap makes true range exceed the bar's own high-low span.
    atr.update(106.0, 105.0, 105.5);
    REQUIRE(atr.value() > 2.0);
    REQUIRE(atr.value() < 6.0);  // smoothed, not jumped to the raw TR
}

TEST_CASE("rolling stdev is stable where sum-of-squares would not be",
          "[features][momentum][property]") {
    // THE NUMERICAL-STABILITY REQUIREMENT. E[x^2]-E[x]^2 cancels catastrophically
    // when the mean is large relative to the spread -- exactly minute prices
    // around 500 with a spread of 0.05 -- and can return a NEGATIVE variance.
    RollingStdev sd{50};
    const double big_mean = 1e8;
    for (int i = 0; i < 60; ++i) {
        sd.update(big_mean + (i % 2 == 0 ? 1.0 : -1.0));
    }
    REQUIRE(sd.ready());
    REQUIRE(sd.value() > 0.0);
    REQUIRE(is_finite(sd.value()));
    // The true sample stdev of alternating +/-1 about a constant is ~1.0.
    REQUIRE(sd.value() == Catch::Approx(1.0).epsilon(0.02));
}

TEST_CASE("rolling beta and correlation behave on known inputs", "[features][bivariate]") {
    RollingBeta beta{50};
    RollingCorrelation corr{50};
    // asset = 2 * market exactly: beta 2, correlation 1.
    for (int i = 0; i < 60; ++i) {
        const double m = std::sin(static_cast<double>(i) * 0.4) * 0.01;
        beta.update(m, 2.0 * m);
        corr.update(m, 2.0 * m);
    }
    REQUIRE(beta.value() == Catch::Approx(2.0).epsilon(1e-9));
    REQUIRE(corr.value() == Catch::Approx(1.0).epsilon(1e-9));
    // Perfectly explained, so the residual is zero.
    REQUIRE(beta.residual(0.01, 0.02) == Catch::Approx(0.0).margin(1e-12));
}

TEST_CASE("correlation is clamped and finite for degenerate inputs",
          "[features][bivariate][edge]") {
    // A constant series has zero variance and undefined correlation. Returning
    // zero is honest; dividing would give inf, and a correlation of 1.0000000002
    // would produce a NaN in a downstream sqrt(1 - rho^2).
    RollingCorrelation corr{10};
    for (int i = 0; i < 15; ++i) corr.update(5.0, 7.0);
    REQUIRE(corr.value() == Catch::Approx(0.0));
    REQUIRE(is_finite(corr.value()));
    REQUIRE(corr.value() >= -1.0);
    REQUIRE(corr.value() <= 1.0);

    RollingBeta beta{10};
    for (int i = 0; i < 15; ++i) beta.update(5.0, 7.0);
    REQUIRE(beta.value() == Catch::Approx(0.0));
}

TEST_CASE("EW variance stays non-negative", "[features][momentum][edge]") {
    EwVariance ev{10.0};
    for (int i = 0; i < 100; ++i) {
        ev.update(1e6 + (i % 2 == 0 ? 0.5 : -0.5));
    }
    REQUIRE(ev.variance() >= 0.0);
    REQUIRE(is_finite(ev.stdev()));
}

TEST_CASE("rolling extrema track the window", "[features][momentum]") {
    RollingExtrema ex{3};
    ex.update(5.0);
    ex.update(1.0);
    ex.update(9.0);
    REQUIRE(ex.min() == Catch::Approx(1.0));
    REQUIRE(ex.max() == Catch::Approx(9.0));
    ex.update(7.0);  // 5.0 falls out of the window
    REQUIRE(ex.min() == Catch::Approx(1.0));
    ex.update(8.0);  // 1.0 falls out
    REQUIRE(ex.min() == Catch::Approx(7.0));
    REQUIRE(ex.max() == Catch::Approx(9.0));
}
