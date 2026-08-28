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
#include <string_view>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"
#include "ptl/log/logger.hpp"

namespace ptl::config {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Schema
// ---------------------------------------------------------------------------
//
// Every section declares its own keys immediately beside the struct they
// populate. The loader composes them; it does not maintain a list of its own.
//
// The previous arrangement kept one hardcoded std::set in config.cpp, far from
// the structs it described. Adding a field meant editing two files, and
// forgetting the second produced "unrecognised configuration key" for a key you
// had just added -- a confusing failure that costs ten minutes every time. It
// was also going to be the single worst merge-conflict site in the repository,
// since every phase appends to it.
//
// This is still two edits, but they are adjacent lines in one struct, visible
// in the same diff hunk. Genuinely eliminating the second edit would need
// reflection or a field-generating macro, and a config DSL is more complexity
// than this problem earns. test_config.cpp closes the gap instead, by asserting
// that every declared key is actually accepted by the loader.

using KeyList = std::span<const std::string_view>;

/// Concatenation of every section's keys, in declaration order.
[[nodiscard]] const std::vector<std::string_view>& all_known_keys();

struct RunSection {
    static constexpr std::string_view kKeys[] = {"run.seed", "run.results_dir", "run.tag",
                                                 "run.fail_on_validation_warning"};

    std::uint64_t seed = 20240101;
    fs::path results_dir = "results";
    std::string tag;
    bool fail_on_validation_warning = false;
};

struct DataSection {
    static constexpr std::string_view kKeys[] = {"data.raw_dir", "data.normalized_dir",
                                                 "data.reference_dir", "data.cache_dir"};

    fs::path raw_dir = "data/raw";
    fs::path normalized_dir = "data/normalized";
    fs::path reference_dir = "data/reference";
    fs::path cache_dir = "data/cache";
};

/// Per-tier provenance (ADR-0001). Persisted into the run manifest verbatim so
/// that a result set records exactly which feed and schema produced it.
struct DataTier {
    /// Tier keys are the same shape under three prefixes, so they are generated
    /// rather than listed three times.
    static constexpr std::string_view kSuffixes[] = {".provider", ".feed",
                                                     ".schema",   ".timestamp_semantics",
                                                     ".coverage", ".historical_end_delay_minutes"};

    std::string provider;
    std::string feed;
    std::string schema;
    std::string timestamp_semantics;
    std::string coverage;
    int historical_end_delay_minutes = 0;
};

struct DatabentoSection {
    static constexpr std::string_view kKeys[] = {"data.databento.max_spend_usd",
                                                 "data.databento.require_explicit_paid_override"};

    double max_spend_usd = 25.0;
    bool require_explicit_paid_override = true;
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
    static constexpr std::string_view kKeys[] = {
        "market_session.timezone", "market_session.regular_open", "market_session.regular_close",
        "market_session.exclude_opening_auction", "market_session.exclude_closing_auction"};

    std::string timezone = "America/New_York";
    std::string regular_open = "09:30:00";
    std::string regular_close = "16:00:00";
    bool exclude_opening_auction = true;
    bool exclude_closing_auction = true;

    /// Parsed at load, so nothing downstream re-parses these strings and
    /// nothing can parse them differently. Offsets from midnight in the
    /// exchange's local day -- NOT instants. Converting one to an instant
    /// needs a date and a calendar, which is Phase 2.
    Duration open_offset{};
    Duration close_offset{};

    // NO tradable_bars_per_session() HERE, DELIBERATELY.
    //
    // A previous revision computed 390 minus the opening auction. That is wrong
    // on every half-day -- the day after Thanksgiving, Christmas Eve and July 3
    // close at 13:00, giving 210 minutes -- and it silently ignored
    // exclude_closing_auction. A feature written against it reaches across the
    // overnight gap on roughly six sessions a year and produces plausible
    // numbers, which is the worst failure mode available.
    //
    // Session length is a property of a DATE, not a compile-time constant. Ask
    // ptl::market::Calendar (Phase 2), which loads session boundaries as UTC
    // instants from data/reference/calendars/. This struct supplies the default
    // schedule; the calendar supplies the exceptions. See ADR-0001 Addendum A2
    // and the Phase 1 review, finding H-4.
};

struct HoldoutSection {
    static constexpr std::string_view kKeys[] = {"holdout.boundary_date", "holdout.months",
                                                 "holdout.unlocked",
                                                 "holdout.unlock_justification"};

    /// Fixed BEFORE any data is examined; enters the config hash so it cannot
    /// be quietly moved after seeing results.
    std::string boundary_date;  // "YYYY-MM-DD"; empty = not yet ingested
    int months = 4;
    bool unlocked = false;
    std::string unlock_justification;

    /// Parsed form; kNoTimestamp when boundary_date is empty.
    ///
    /// This is the most consequential string in the configuration. A typo such
    /// as "2024-13-01" surviving to Phase 5 would define a holdout that does not
    /// exist, and the failure would surface as a confusing fold-generation error
    /// long after anyone remembers what the value was meant to be.
    Timestamp boundary{kNoTimestamp};
};

struct LogSection {
    static constexpr std::string_view kKeys[] = {"log.level", "log.file", "log.console",
                                                 "log.json"};

    log::Level level = log::Level::Info;
    std::string file;
    bool console = true;
    bool json = true;
};

struct Config {
    RunSection run;
    DataSection data;
    DataTier t1, t2, t3;
    DatabentoSection databento;
    SessionSection session;
    HoldoutSection holdout;
    LogSection log;

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
                                std::string_view data_manifest_sha, std::string_view git_sha,
                                std::uint64_t seed) noexcept;

/// Load `path`, apply dotted overrides ("run.seed=42", "log.level=debug"),
/// then validate strictly.
///
/// Overrides exist so a parameter sweep drives one config file from the command
/// line rather than generating dozens of near-identical files that then drift.
/// \param accessed When non-null, receives every schema key the loader actually
///        read. Used by the schema drift test; production callers pass nullptr.
[[nodiscard]] Result<Config> load(const fs::path& path, std::span<const std::string> overrides = {},
                                  std::vector<std::string>* accessed = nullptr);

/// Parse a TOML string directly. Used by tests and by the sweep runner.
[[nodiscard]] Result<Config> load_from_string(std::string_view toml_text,
                                              std::span<const std::string> overrides = {},
                                              std::string_view origin = "<string>",
                                              std::vector<std::string>* accessed = nullptr);

}  // namespace ptl::config
