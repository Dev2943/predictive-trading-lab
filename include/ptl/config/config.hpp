#pragma once

/// \file config.hpp
/// Typed, immutable, hashable, strictly-validated configuration.
///
/// Loaded once in main(), validated at load, then passed by const reference and
/// never mutated. The canonical serialisation feeds the RunId, which is what
/// makes "which settings produced results/a3f9.../?" answerable months later.
///
/// Validation is STRICT: an unrecognised key is an error, not a warning. A
/// mistyped config key that silently does nothing is a reproducibility bug --
/// the run records a setting it never applied.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"
#include "ptl/log/logger.hpp"

namespace ptl::config {

namespace fs = std::filesystem;

struct RunSection {
    std::uint64_t seed = 20240101;
    fs::path      results_dir = "results";
    std::string   tag;
    bool          fail_on_validation_warning = false;
};

struct DataSection {
    fs::path raw_dir = "data/raw";
    fs::path normalized_dir = "data/normalized";
    fs::path reference_dir = "data/reference";
    fs::path cache_dir = "data/cache";
};

/// Per-tier provenance (ADR-0001). Persisted into the run manifest verbatim so
/// that a result set records exactly which feed and schema produced it.
struct DataTier {
    std::string provider;
    std::string feed;
    std::string schema;
    std::string timestamp_semantics;
    std::string coverage;
    int         historical_end_delay_minutes = 0;
};

struct DatabentoSection {
    double max_spend_usd = 25.0;
    bool   require_explicit_paid_override = true;
};

/// Session boundaries.
///
/// NOTE: `timezone` is PROVENANCE METADATA ONLY. It is consumed by the offline
/// calendar generator, which emits data/reference/calendars/*.csv as pairs of
/// UTC instants. The engine never performs a zone conversion -- libc++ lacks
/// the C++20 tzdb, and a runtime dependency on the host zone database would
/// break bit-reproducibility across machines. See ADR-0001 Addendum A1; the CI
/// job enforces this by rejecting tzdb symbols in src/ and include/.
struct SessionSection {
    std::string timezone = "America/New_York";
    std::string regular_open = "09:30:00";
    std::string regular_close = "16:00:00";
    bool        exclude_opening_auction = true;
    bool        exclude_closing_auction = true;

    /// Left-edge minute bars over [09:30, 16:00) are stamped 09:30..15:59 =
    /// 390. Excluding the opening auction drops the 09:30 bar, leaving 389.
    /// Any feature written against a hardcoded 390 silently reaches across the
    /// overnight gap. See ADR-0001 Addendum A2.
    [[nodiscard]] int tradable_bars_per_session() const noexcept {
        return 390 - (exclude_opening_auction ? 1 : 0);
    }
};

struct HoldoutSection {
    /// Fixed BEFORE any data is examined; enters the config hash so it cannot
    /// be quietly moved after seeing results.
    std::string boundary_date;  // "YYYY-MM-DD"; empty = not yet ingested
    int         months = 4;
    bool        unlocked = false;
    std::string unlock_justification;
};

struct LogSection {
    log::Level  level = log::Level::Info;
    std::string file;
    bool        console = true;
    bool        json = true;
};

struct Config {
    RunSection       run;
    DataSection      data;
    DataTier         t1, t2, t3;
    DatabentoSection databento;
    SessionSection   session;
    HoldoutSection   holdout;
    LogSection       log;

    std::string source_path;

    /// Stable, comment-free, sorted, fully-dotted serialisation. Independent of
    /// TOML formatting, key order and whitespace, so two configs that mean the
    /// same thing hash the same.
    std::string canonical;

    [[nodiscard]] std::uint64_t hash() const noexcept;
};

/// RunId = hash(config_canonical || data_manifest_sha || git_sha || seed).
///
/// The four inputs are exactly the four things that can change a result.
struct RunId {
    std::uint64_t value = 0;
    [[nodiscard]] std::string hex() const;
};

[[nodiscard]] RunId make_run_id(std::string_view config_canonical,
                                std::string_view data_manifest_sha,
                                std::string_view git_sha,
                                std::uint64_t    seed) noexcept;

/// Load `path`, apply dotted overrides ("run.seed=42", "log.level=debug"),
/// then validate strictly.
///
/// Overrides exist so a parameter sweep drives one config file from the command
/// line rather than generating dozens of near-identical files that then drift.
[[nodiscard]] Result<Config> load(const fs::path&              path,
                                  std::span<const std::string> overrides = {});

/// Parse a TOML string directly. Used by tests and by the sweep runner.
[[nodiscard]] Result<Config> load_from_string(std::string_view             toml_text,
                                              std::span<const std::string> overrides = {},
                                              std::string_view             origin = "<string>");

}  // namespace ptl::config
