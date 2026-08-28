#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <limits>
#include <vector>

#include "ptl/features/cross_sectional.hpp"
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

constexpr InstrumentId kSpy{0};
constexpr InstrumentId kXlf{1};
constexpr InstrumentId kXle{2};
constexpr InstrumentId kTlt{3};

CrossSectionalConfig four_name_universe(bool sector_neutral = false) {
    CrossSectionalConfig cfg;
    cfg.universe = {{kSpy, 0, true}, {kXlf, 1, false}, {kXle, 1, false}, {kTlt, 2, false}};
    cfg.sector_neutral = sector_neutral;
    cfg.winsorize_inputs = false;
    return cfg;
}

}  // namespace

// ---------------------------------------------------------------------------
// The barrier
// ---------------------------------------------------------------------------

TEST_CASE("the barrier refuses contributions from a different bar",
          "[features][crosssectional][leakage]") {
    // THE BARRIER'S CENTRAL CHECK (reconciliation: test_cross_sectional_barrier).
    //
    // Mixing one instrument's state with another's from a DIFFERENT instant is
    // lookahead if the newcomer is later, and a stale value presented as
    // current if earlier. Both are refused rather than tolerated.
    CrossSectionalStage stage{four_name_universe()};
    const Timestamp t = at("2024-07-02T14:53:00Z");

    REQUIRE(stage.contribute(kSpy, t, 0.001, 1e6).has_value());

    auto later = stage.contribute(kXlf, t + minutes{1}, 0.002, 1e6);
    REQUIRE_FALSE(later.has_value());
    REQUIRE(later.error().message.find("not from the current bar") != std::string::npos);

    auto earlier = stage.contribute(kXle, t - minutes{1}, 0.003, 1e6);
    REQUIRE_FALSE(earlier.has_value());
}

TEST_CASE("an incomplete cross-section is refused", "[features][crosssectional][leakage]") {
    // A partial cross-section silently changes what a rank MEANS: top-decile
    // among two names is not top-decile among nine.
    CrossSectionalStage stage{four_name_universe()};
    const Timestamp t = at("2024-07-02T14:53:00Z");
    REQUIRE(stage.contribute(kSpy, t, 0.001, 1e6).has_value());
    REQUIRE(stage.contribute(kXlf, t, 0.002, 1e6).has_value());

    auto r = stage.compute();
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.find("incomplete cross-section") != std::string::npos);
}

TEST_CASE("a duplicate contribution is refused", "[features][crosssectional]") {
    CrossSectionalStage stage{four_name_universe()};
    const Timestamp t = at("2024-07-02T14:53:00Z");
    REQUIRE(stage.contribute(kSpy, t, 0.001, 1e6).has_value());
    REQUIRE_FALSE(stage.contribute(kSpy, t, 0.002, 1e6).has_value());
}

TEST_CASE("an instrument outside the universe is refused", "[features][crosssectional]") {
    CrossSectionalStage stage{four_name_universe()};
    REQUIRE_FALSE(
        stage.contribute(InstrumentId{99}, at("2024-07-02T14:53:00Z"), 0.001, 1e6).has_value());
}

TEST_CASE("market-relative return subtracts the proxy", "[features][crosssectional]") {
    CrossSectionalStage stage{four_name_universe()};
    const Timestamp t = at("2024-07-02T14:53:00Z");
    REQUIRE(stage.contribute(kSpy, t, 0.010, 1e6).has_value());  // proxy up 1%
    REQUIRE(stage.contribute(kXlf, t, 0.015, 1e6).has_value());
    REQUIRE(stage.contribute(kXle, t, 0.005, 1e6).has_value());
    REQUIRE(stage.contribute(kTlt, t, -0.002, 1e6).has_value());

    auto rows = stage.compute();
    REQUIRE(rows.has_value());
    REQUIRE(rows->size() == 4);

    // Rows come back in instrument-index order, deterministically.
    REQUIRE((*rows)[0].instrument == kSpy);
    REQUIRE((*rows)[0].market_relative_return == Catch::Approx(0.0));
    REQUIRE((*rows)[1].market_relative_return == Catch::Approx(0.005));
    REQUIRE((*rows)[3].market_relative_return == Catch::Approx(-0.012));
    REQUIRE(stage.contributed() == 0);  // compute() clears for the next bar
}

TEST_CASE("sector-neutral demeans within the group not across it", "[features][crosssectional]") {
    // A signal that merely says "energy outperformed" is a sector bet wearing
    // an alpha costume. Demeaning within sector is what separates the two.
    CrossSectionalStage stage{four_name_universe(true)};
    const Timestamp t = at("2024-07-02T14:53:00Z");
    REQUIRE(stage.contribute(kSpy, t, 0.010, 1e6).has_value());
    REQUIRE(stage.contribute(kXlf, t, 0.020, 1e6).has_value());   // sector 1
    REQUIRE(stage.contribute(kXle, t, 0.010, 1e6).has_value());   // sector 1
    REQUIRE(stage.contribute(kTlt, t, -0.005, 1e6).has_value());  // sector 2

    auto rows = stage.compute();
    REQUIRE(rows.has_value());
    // Sector 1 mean is 0.015, so XLF is +0.005 and XLE is -0.005 within sector.
    REQUIRE((*rows)[1].sector_relative_return == Catch::Approx(0.005));
    REQUIRE((*rows)[2].sector_relative_return == Catch::Approx(-0.005));
    // TLT is alone in sector 2, so it is neutral by construction.
    REQUIRE((*rows)[3].sector_relative_return == Catch::Approx(0.0));
}

TEST_CASE("the barrier is deterministic regardless of arrival order",
          "[features][crosssectional][determinism]") {
    // Contributions can arrive in any order; the output must not depend on it,
    // because floating-point summation is not associative and a differing
    // order would eventually diverge the equity curve.
    const Timestamp t = at("2024-07-02T14:53:00Z");
    const auto run = [t](bool reversed) {
        CrossSectionalStage stage{four_name_universe()};
        const std::vector<std::pair<InstrumentId, double>> input{
            {kSpy, 0.010}, {kXlf, 0.015}, {kXle, 0.005}, {kTlt, -0.002}};
        if (reversed) {
            for (auto it = input.rbegin(); it != input.rend(); ++it) {
                REQUIRE(stage.contribute(it->first, t, it->second, 1e6).has_value());
            }
        } else {
            for (const auto& [id, r] : input) {
                REQUIRE(stage.contribute(id, t, r, 1e6).has_value());
            }
        }
        auto rows = stage.compute();
        REQUIRE(rows.has_value());
        std::vector<double> out;
        for (const auto& r : *rows) out.push_back(r.market_relative_return);
        return out;
    };
    // Exact equality, not approximate.
    REQUIRE(run(false) == run(true));
}

// ---------------------------------------------------------------------------
// Transforms
// ---------------------------------------------------------------------------

TEST_CASE("percentile ranks average ties", "[features][crosssectional]") {
    // Ordinal ranking would impose an arbitrary order on equal values -- common
    // before warmup, when many features are still zero -- and make the output
    // depend on input order.
    const std::vector<double> v{10.0, 20.0, 20.0, 30.0};
    const auto r = percentile_ranks(v);
    REQUIRE(r[0] == Catch::Approx(0.0));
    REQUIRE(r[1] == Catch::Approx(r[2]));  // tied values share a rank
    REQUIRE(r[1] == Catch::Approx(0.5));   // average of ranks 1 and 2 of 0..3
    REQUIRE(r[3] == Catch::Approx(1.0));

    REQUIRE(percentile_ranks({}).empty());
    REQUIRE(percentile_ranks(std::vector<double>{5.0}).front() == Catch::Approx(0.5));
}

TEST_CASE("cross-sectional z-score handles zero dispersion", "[features][crosssectional][edge]") {
    // A cross-section where every value is identical has no dispersion.
    // Dividing would give infinities across the entire universe at once.
    const auto z = cross_sectional_zscore(std::vector<double>{7.0, 7.0, 7.0});
    for (const double v : z) {
        REQUIRE(v == Catch::Approx(0.0));
        REQUIRE(is_finite(v));
    }

    const auto real = cross_sectional_zscore(std::vector<double>{1.0, 2.0, 3.0});
    REQUIRE(real[0] < 0.0);
    REQUIRE(real[1] == Catch::Approx(0.0));
    REQUIRE(real[2] > 0.0);
}

TEST_CASE("winsorization clips rather than drops", "[features][crosssectional]") {
    // Clipped to the boundary so the cross-section keeps its size and every
    // instrument still receives a value.
    const std::vector<double> v{-1000.0, 1.0, 2.0, 3.0, 4.0, 5.0, 1000.0};
    const auto w = winsorize(v, 0.15, 0.85);
    REQUIRE(w.size() == v.size());
    REQUIRE(w.front() > -1000.0);
    REQUIRE(w.back() < 1000.0);
    // Interior values are untouched.
    REQUIRE(w[3] == Catch::Approx(3.0));
}

TEST_CASE("winsorization replaces non-finite values", "[features][crosssectional][edge]") {
    const std::vector<double> v{1.0, std::numeric_limits<double>::quiet_NaN(), 3.0,
                                std::numeric_limits<double>::infinity(), 5.0};
    const auto w = winsorize(v, 0.0, 1.0);
    for (const double x : w) REQUIRE(is_finite(x));
}

TEST_CASE("demean and group_demean", "[features][crosssectional]") {
    const auto d = demean(std::vector<double>{1.0, 2.0, 3.0});
    REQUIRE(d[0] == Catch::Approx(-1.0));
    REQUIRE(d[2] == Catch::Approx(1.0));

    const std::vector<double> v{1.0, 3.0, 10.0, 20.0};
    const std::vector<std::int32_t> g{0, 0, 1, 1};
    const auto gd = group_demean(v, g);
    REQUIRE(gd[0] == Catch::Approx(-1.0));
    REQUIRE(gd[1] == Catch::Approx(1.0));
    REQUIRE(gd[2] == Catch::Approx(-5.0));
    REQUIRE(gd[3] == Catch::Approx(5.0));

    // Mismatched lengths return the input untouched rather than reading out of
    // bounds.
    REQUIRE(group_demean(v, std::vector<std::int32_t>{0}).size() == v.size());
}

TEST_CASE("median handles even and odd counts", "[features][crosssectional]") {
    REQUIRE(median_of(std::vector<double>{3.0, 1.0, 2.0}) == Catch::Approx(2.0));
    REQUIRE(median_of(std::vector<double>{4.0, 1.0, 3.0, 2.0}) == Catch::Approx(2.5));
    REQUIRE(median_of({}) == Catch::Approx(0.0));
}
