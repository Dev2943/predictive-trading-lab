#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <type_traits>

#include "ptl/core/types.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;

// The strong typedefs exist to catch unit confusion at compile time. That is
// only worth having if they cost nothing at run time -- otherwise the honest
// choice would be bare doubles plus discipline. These assertions are the proof,
// and they are static so a regression is a build failure, not a slow test.
static_assert(sizeof(Price) == sizeof(double));
static_assert(sizeof(Qty) == sizeof(double));
static_assert(sizeof(Notional) == sizeof(double));
static_assert(sizeof(Bps) == sizeof(double));
static_assert(std::is_trivially_copyable_v<Price>);
static_assert(std::is_standard_layout_v<Price>);

// Distinct types, so a Price cannot silently become a Qty.
static_assert(!std::is_same_v<Price, Qty>);
static_assert(!std::is_convertible_v<Price, Qty>);
static_assert(!std::is_convertible_v<double, Price>);  // construction is explicit

// Price * Qty must NOT compile as a bare product; it goes through notional().
template <class A, class B>
concept Multipliable = requires(A a, B b) { a* b; };
static_assert(!Multipliable<Price, Qty>);
static_assert(Multipliable<Price, double>);  // scaling is fine

TEST_CASE("named type arithmetic", "[core][types]") {
    constexpr Price a{100.5};
    constexpr Price b{0.25};

    STATIC_REQUIRE((a + b).get() == 100.75);
    STATIC_REQUIRE((a - b).get() == 100.25);
    STATIC_REQUIRE((a * 2.0).get() == 201.0);
    STATIC_REQUIRE((2.0 * a).get() == 201.0);
    STATIC_REQUIRE((-a).get() == -100.5);
    STATIC_REQUIRE(a > b);
    STATIC_REQUIRE(a != b);

    Price c{10.0};
    c += Price{5.0};
    REQUIRE(c.get() == 15.0);
    c -= Price{5.0};
    REQUIRE(c.get() == 10.0);
}

TEST_CASE("ratio of same-unit values is dimensionless", "[core][types]") {
    STATIC_REQUIRE((Price{200.0} / Price{100.0}) == 2.0);
}

TEST_CASE("notional is the only cross-unit product", "[core][types]") {
    STATIC_REQUIRE(notional(Price{50.0}, Qty{3.0}).get() == 150.0);
}

TEST_CASE("participation relates Qty to Volume explicitly", "[core][types]") {
    // Qty and Volume are both share counts but mean different things. Relating
    // them is allowed only through a named function, so every participation
    // calculation is greppable.
    STATIC_REQUIRE(participation(Qty{500.0}, Volume{10000.0}) == 0.05);
}

TEST_CASE("basis point helpers agree in both directions", "[core][types]") {
    constexpr Price ref{100.0};
    const Price up = apply_bps(ref, Bps{10.0}, +1);
    REQUIRE(up.get() == Catch::Approx(100.10));
    REQUIRE(to_bps(up, ref).get() == Catch::Approx(10.0));

    const Price down = apply_bps(ref, Bps{10.0}, -1);
    REQUIRE(to_bps(down, ref).get() == Catch::Approx(-10.0));
}

TEST_CASE("side helpers", "[core][types]") {
    STATIC_REQUIRE(sign_of(Side::Buy) == 1);
    STATIC_REQUIRE(sign_of(Side::Sell) == -1);
    STATIC_REQUIRE(opposite(Side::Buy) == Side::Sell);
    REQUIRE(to_string(Side::Buy) == "BUY");
}
