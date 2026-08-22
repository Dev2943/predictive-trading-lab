#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "ptl/core/clock.hpp"
#include "ptl/core/types.hpp"
#include "ptl/log/logger.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;

TEST_CASE("levels round-trip", "[log]") {
    log::Level l{};
    REQUIRE(log::parse_level("debug", l));
    REQUIRE(l == log::Level::Debug);
    REQUIRE(log::parse_level("critical", l));
    REQUIRE(l == log::Level::Critical);
    REQUIRE_FALSE(log::parse_level("verbose", l));
    REQUIRE(log::to_string(log::Level::Warn) == "warn");
}

TEST_CASE("structured records are valid JSON lines", "[log]") {
    const auto path = std::filesystem::temp_directory_path() / "ptl_log_test.jsonl";
    std::filesystem::remove(path);

    log::Config cfg;
    cfg.level = log::Level::Trace;
    cfg.console = false;  // keep the test output clean
    cfg.json = true;
    cfg.file = path.string();
    log::init(cfg);

    auto& lg = log::get("test.subsystem");
    lg.set_level(log::Level::Trace);
    PTL_INFO(lg, "order rejected", log::kv("symbol", "SPY"), log::kv("reason", "stale_quote"),
             log::kv("qty", 100), log::kv("price", 512.25), log::kv("marketable", true));
    log::shutdown();

    std::ifstream in{path};
    std::string line;
    REQUIRE(std::getline(in, line));

    REQUIRE(line.front() == '{');
    REQUIRE(line.back() == '}');
    REQUIRE(line.find(R"("level":"info")") != std::string::npos);
    REQUIRE(line.find(R"("subsystem":"test.subsystem")") != std::string::npos);
    REQUIRE(line.find(R"("msg":"order rejected")") != std::string::npos);
    REQUIRE(line.find(R"("symbol":"SPY")") != std::string::npos);
    REQUIRE(line.find(R"("qty":100)") != std::string::npos);
    REQUIRE(line.find(R"("marketable":true)") != std::string::npos);
    REQUIRE(line.find(R"("reason":"stale_quote")") != std::string::npos);

    std::filesystem::remove(path);
}

TEST_CASE("json escaping survives hostile field content", "[log]") {
    // Error messages and symbols end up in log fields. One unescaped quote
    // corrupts the whole line and silently breaks the audit trail.
    const auto path = std::filesystem::temp_directory_path() / "ptl_log_escape.jsonl";
    std::filesystem::remove(path);

    log::Config cfg;
    cfg.console = false;
    cfg.json = true;
    cfg.file = path.string();
    log::init(cfg);

    auto& lg = log::get("escape.test");
    PTL_ERROR(lg, "parse failed", log::kv("detail", std::string{"he said \"no\"\n\tand left"}));
    log::shutdown();

    std::ifstream in{path};
    std::string line;
    REQUIRE(std::getline(in, line));
    REQUIRE(line.find(R"(\"no\")") != std::string::npos);
    REQUIRE(line.find(R"(\n\tand left)") != std::string::npos);
    // The escaped content must not terminate the JSON object early.
    REQUIRE(line.back() == '}');

    std::filesystem::remove(path);
}

TEST_CASE("logger references stay valid as subsystems are added", "[log]") {
    auto& first = log::get("alpha");
    for (int i = 0; i < 200; ++i) (void)log::get("sub" + std::to_string(i));
    REQUIRE(first.subsystem() == "alpha");
    REQUIRE(&first == &log::get("alpha"));
}

TEST_CASE("levels below the compiled threshold are erased", "[log]") {
    // Trace statements vanish entirely unless PTL_ENABLE_TRACE is set. This is
    // what makes it safe to instrument the per-event simulation loop: in a
    // normal build the call is not merely skipped, it does not exist.
#if defined(PTL_ENABLE_TRACE) && PTL_ENABLE_TRACE
    STATIC_REQUIRE(log::kCompiledMinLevel == log::Level::Trace);
#else
    STATIC_REQUIRE(log::kCompiledMinLevel == log::Level::Debug);
    STATIC_REQUIRE(log::Level::Trace < log::kCompiledMinLevel);
#endif
}

namespace {

/// Strip the one field that is permitted to vary between two identical runs.
std::string strip_wall_time(std::string line) {
    const auto pos = line.find(R"(,"wall_time":")");
    if (pos == std::string::npos) return line;
    const auto end = line.find('"', pos + 14);
    if (end == std::string::npos) return line;
    return line.substr(0, pos) + line.substr(end + 1);
}

std::vector<std::string> run_and_capture(const std::filesystem::path& path) {
    std::filesystem::remove(path);

    // A fixed simulated clock: this is what a backtest replay looks like.
    ptl::Timestamp t0{};
    REQUIRE(ptl::parse_timestamp("2024-01-02T14:52:00Z", t0));
    ptl::SimulatedClock clock{t0};

    log::Config cfg;
    cfg.console = false;
    cfg.json = true;
    cfg.file = path.string();
    cfg.sim_clock = &clock;
    log::init(cfg);

    auto& lg = log::get("replay");
    PTL_INFO(lg, "bar closed", log::kv("symbol", "SPY"));
    clock.advance_by(std::chrono::seconds{60});
    PTL_INFO(lg, "order submitted", log::kv("qty", 100));
    clock.advance_by(std::chrono::milliseconds{1500});
    PTL_WARN(lg, "order rejected", log::kv("reason", "stale_quote"));
    log::shutdown();

    std::ifstream in{path};
    std::vector<std::string> lines;
    for (std::string l; std::getline(in, l);) lines.push_back(std::move(l));
    return lines;
}

}  // namespace

TEST_CASE("records carry simulation time when a clock is installed", "[log][determinism]") {
    const auto path = std::filesystem::temp_directory_path() / "ptl_log_simtime.jsonl";
    const auto lines = run_and_capture(path);

    REQUIRE(lines.size() == 3);
    // sim_time advances with the simulated clock, not with how long the test took.
    REQUIRE(lines[0].find(R"("sim_time":"2024-01-02T14:52:00.000000000Z")") != std::string::npos);
    REQUIRE(lines[1].find(R"("sim_time":"2024-01-02T14:53:00.000000000Z")") != std::string::npos);
    REQUIRE(lines[2].find(R"("sim_time":"2024-01-02T14:53:01.500000000Z")") != std::string::npos);
    // wall_time is present for operational forensics, and is the last field.
    REQUIRE(lines[0].find(R"("wall_time":")") != std::string::npos);

    std::filesystem::remove(path);
}

TEST_CASE("two identical replays produce identical logs modulo wall time", "[log][determinism]") {
    // THE POINT OF THE CLOCK INJECTION (review finding C-1).
    //
    // Phase 12 proves paper-trading parity by diffing a live journal against a
    // replayed one. That comparison is impossible if every line differs. With
    // wall_time removed, two runs of the same deterministic replay must be
    // byte-identical -- which is what makes the journal usable as evidence
    // rather than as decoration.
    const auto a = std::filesystem::temp_directory_path() / "ptl_log_det_a.jsonl";
    const auto b = std::filesystem::temp_directory_path() / "ptl_log_det_b.jsonl";

    const auto first = run_and_capture(a);
    const auto second = run_and_capture(b);

    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        INFO("line " << i);
        REQUIRE(strip_wall_time(first[i]) == strip_wall_time(second[i]));
    }

    // And prove the test would actually catch a regression: the raw lines DO
    // differ, because wall_time differs. If they did not, stripping would be
    // proving nothing.
    bool any_raw_difference = false;
    for (std::size_t i = 0; i < first.size(); ++i) {
        if (first[i] != second[i]) any_raw_difference = true;
    }
    REQUIRE(any_raw_difference);

    std::filesystem::remove(a);
    std::filesystem::remove(b);
}

TEST_CASE("without a clock records carry wall time only", "[log]") {
    const auto path = std::filesystem::temp_directory_path() / "ptl_log_noclock.jsonl";
    std::filesystem::remove(path);

    log::Config cfg;
    cfg.console = false;
    cfg.json = true;
    cfg.file = path.string();
    cfg.sim_clock = nullptr;  // the default: correct for tools with no simulation
    log::init(cfg);
    auto& lg = log::get("noclock");
    PTL_INFO(lg, "hello");
    log::shutdown();

    std::ifstream in{path};
    std::string line;
    REQUIRE(std::getline(in, line));
    REQUIRE(line.find(R"("sim_time")") == std::string::npos);
    REQUIRE(line.find(R"("wall_time":")") != std::string::npos);

    std::filesystem::remove(path);
}
