#include <catch2/catch_test_macros.hpp>

#include "support/ptl_catch.hpp"

#include <string>
#include <vector>

#include "ptl/core/instrument_table.hpp"

using namespace ptl;

TEST_CASE("interning is idempotent and ids are dense", "[core][instruments]") {
    InstrumentTable t;
    const auto spy = t.intern("SPY");
    const auto qqq = t.intern("QQQ");
    const auto spy2 = t.intern("SPY");

    REQUIRE(spy == spy2);
    REQUIRE(t.size() == 2);

    // Dense from zero, so per-instrument state is a flat vector indexed
    // directly rather than a hash lookup in the hot loop.
    REQUIRE(index_of(spy) == 0);
    REQUIRE(index_of(qqq) == 1);
}

TEST_CASE("symbol lookup round-trips", "[core][instruments]") {
    InstrumentTable t;
    const auto id = t.intern("IWM");
    REQUIRE(t.symbol(id) == "IWM");
    REQUIRE(t.find("IWM").has_value());
    REQUIRE(*t.find("IWM") == id);
    REQUIRE_FALSE(t.find("NOPE").has_value());
    REQUIRE(t.symbol(kInvalidInstrument).empty());
    REQUIRE_FALSE(t.contains(kInvalidInstrument));
}

TEST_CASE("iteration order is insertion order not hash order", "[core][instruments][determinism]") {
    // Load-bearing for reproducibility. Anything in the pipeline that iterates
    // instruments -- feature computation, portfolio marking, report rows --
    // would otherwise produce a machine-dependent ordering, and floating-point
    // summation is not associative.
    const std::vector<std::string> universe{"SPY", "QQQ", "IWM", "DIA", "XLF",
                                            "XLK", "XLE", "TLT", "GLD"};
    InstrumentTable t;
    for (const auto& s : universe) t.intern(s);

    REQUIRE(t.size() == universe.size());
    const auto all = t.all();
    for (std::size_t i = 0; i < universe.size(); ++i) {
        REQUIRE(all[i] == universe[i]);
        REQUIRE(index_of(*t.find(universe[i])) == static_cast<std::uint32_t>(i));
    }
}

TEST_CASE("string views stay valid as the table grows", "[core][instruments]") {
    // The views point into deque storage precisely so that growth does not
    // invalidate them. A vector<string> here would be a dangling-reference bug
    // that only appears past the initial capacity.
    InstrumentTable t;
    const auto first = t.intern("AAA");
    const std::string_view view = t.symbol(first);
    for (int i = 0; i < 2000; ++i) t.intern("SYM" + std::to_string(i));
    REQUIRE(view == "AAA");
    REQUIRE(t.symbol(first) == "AAA");
}

TEST_CASE("clear resets the table", "[core][instruments]") {
    InstrumentTable t;
    t.intern("SPY");
    t.clear();
    REQUIRE(t.empty());
    REQUIRE_FALSE(t.find("SPY").has_value());
    REQUIRE(index_of(t.intern("QQQ")) == 0);
}
