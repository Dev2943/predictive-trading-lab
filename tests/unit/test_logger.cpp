#include <catch2/catch_test_macros.hpp>

#include "support/ptl_catch.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "ptl/log/logger.hpp"

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
    PTL_INFO(lg, "order rejected", log::kv("symbol", "SPY"),
             log::kv("reason", "stale_quote"), log::kv("qty", 100),
             log::kv("price", 512.25), log::kv("marketable", true));
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
