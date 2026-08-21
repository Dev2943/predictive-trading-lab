#include "ptl/config/config.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdint>
#include <set>
#include <sstream>

#define TOML_EXCEPTIONS 0
#include <toml++/toml.hpp>

namespace ptl::config {
namespace {

/// Every key the schema knows about. An unrecognised key is an ERROR.
///
/// A typo like `run.sed = 42` under a permissive loader silently leaves the
/// seed at its default while the manifest records a config that appears to set
/// it. That is a reproducibility failure disguised as a typo, so the loader
/// refuses. Each phase appends its own keys here as it introduces them.
const std::set<std::string, std::less<>>& known_keys() {
    static const std::set<std::string, std::less<>> keys{
        "run.seed", "run.results_dir", "run.tag", "run.fail_on_validation_warning",
        "data.raw_dir", "data.normalized_dir", "data.reference_dir", "data.cache_dir",
        "data.t1.provider", "data.t1.feed", "data.t1.schema",
        "data.t1.timestamp_semantics", "data.t1.coverage",
        "data.t1.historical_end_delay_minutes",
        "data.t2.provider", "data.t2.feed", "data.t2.schema",
        "data.t2.timestamp_semantics", "data.t2.coverage",
        "data.t2.historical_end_delay_minutes",
        "data.t3.provider", "data.t3.feed", "data.t3.schema",
        "data.t3.timestamp_semantics", "data.t3.coverage",
        "data.t3.historical_end_delay_minutes",
        "data.databento.max_spend_usd", "data.databento.require_explicit_paid_override",
        "market_session.timezone", "market_session.regular_open",
        "market_session.regular_close", "market_session.exclude_opening_auction",
        "market_session.exclude_closing_auction",
        "holdout.boundary_date", "holdout.months", "holdout.unlocked",
        "holdout.unlock_justification",
        "log.level", "log.file", "log.console", "log.json",
    };
    return keys;
}

/// Canonical text for a scalar node.
///
/// Switching on the node type rather than streaming it keeps the output
/// independent of toml++ formatting decisions, which are not part of our
/// stability contract. Doubles use 17 significant digits so the canonical form
/// round-trips an IEEE double exactly: a config hash that changes when a value
/// is merely reprinted would be worse than no hash at all.
std::string canonical_scalar(const toml::node& v) {
    switch (v.type()) {
        case toml::node_type::string:
            return v.value_or(std::string{});
        case toml::node_type::integer:
            return std::to_string(v.value_or(std::int64_t{0}));
        case toml::node_type::floating_point: {
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%.17g", v.value_or(0.0));
            return std::string{buf};
        }
        case toml::node_type::boolean:
            return v.value_or(false) ? "true" : "false";
        case toml::node_type::date:
            if (const auto d = v.value<toml::date>()) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u", d->year, d->month, d->day);
                return std::string{buf};
            }
            return "<bad-date>";
        case toml::node_type::time:
        case toml::node_type::date_time:
            // Dates and times in config are represented as strings by policy
            // (see [market_session] and [holdout]), precisely so that their
            // canonical form is unambiguous.
            return "<use-string-form>";
        default:
            return "<unsupported>";
    }
}

void collect_keys(const toml::table& tbl, const std::string& prefix,
                  std::vector<std::pair<std::string, std::string>>& out) {
    for (const auto& [k, v] : tbl) {
        std::string key =
            prefix.empty() ? std::string{k.str()} : prefix + "." + std::string{k.str()};
        if (const auto* sub = v.as_table()) {
            collect_keys(*sub, key, out);
        } else if (const auto* arr = v.as_array()) {
            // Index-suffixed, so a reordered array is a different config. For a
            // universe list that is the correct semantics: order determines
            // InstrumentId assignment, which determines iteration order, which
            // determines floating-point summation order.
            std::size_t i = 0;
            for (const auto& el : *arr) {
                std::string ekey = key + "[" + std::to_string(i++) + "]";
                if (const auto* esub = el.as_table()) {
                    collect_keys(*esub, ekey, out);
                } else {
                    out.emplace_back(std::move(ekey), canonical_scalar(el));
                }
            }
        } else {
            out.emplace_back(std::move(key), canonical_scalar(v));
        }
    }
}

/// Insert or replace a dotted path, creating intermediate tables.
/// Values are typed by inspection so that `--set run.seed=42` yields an integer
/// rather than the string "42".
bool set_dotted(toml::table& root, std::string_view dotted, std::string_view value) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= dotted.size()) {
        const std::size_t dot = dotted.find('.', start);
        if (dot == std::string_view::npos) {
            parts.emplace_back(dotted.substr(start));
            break;
        }
        parts.emplace_back(dotted.substr(start, dot - start));
        start = dot + 1;
    }
    if (parts.empty() || parts.back().empty()) return false;

    toml::table* cur = &root;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        auto it = cur->find(parts[i]);
        if (it == cur->end()) {
            cur->insert(parts[i], toml::table{});
            it = cur->find(parts[i]);
        }
        toml::table* next = it->second.as_table();
        if (next == nullptr) return false;
        cur = next;
    }

    const std::string& leaf = parts.back();
    if (value == "true" || value == "false") {
        cur->insert_or_assign(leaf, value == "true");
        return true;
    }
    std::int64_t i64 = 0;
    const auto* first = value.data();
    const auto* last  = value.data() + value.size();
    if (auto [p, ec] = std::from_chars(first, last, i64); ec == std::errc{} && p == last) {
        cur->insert_or_assign(leaf, i64);
        return true;
    }
    try {
        std::size_t consumed = 0;
        const double d = std::stod(std::string{value}, &consumed);
        if (consumed == value.size()) {
            cur->insert_or_assign(leaf, d);
            return true;
        }
    } catch (...) {  // NOLINT(bugprone-empty-catch) -- fall through to string
    }
    cur->insert_or_assign(leaf, std::string{value});
    return true;
}

std::string get_str(const toml::table& t, std::string_view path, std::string fallback) {
    if (const auto v = t.at_path(path).value<std::string>()) return *v;
    return fallback;
}
bool get_bool(const toml::table& t, std::string_view path, bool fallback) {
    return t.at_path(path).value_or(fallback);
}
std::int64_t get_int(const toml::table& t, std::string_view path, std::int64_t fallback) {
    return t.at_path(path).value_or(fallback);
}
double get_dbl(const toml::table& t, std::string_view path, double fallback) {
    if (const auto d = t.at_path(path).value<double>()) return *d;
    if (const auto i = t.at_path(path).value<std::int64_t>()) return static_cast<double>(*i);
    return fallback;
}

void read_tier(const toml::table& t, std::string_view prefix, DataTier& tier) {
    const std::string p{prefix};
    tier.provider            = get_str(t, p + ".provider", "");
    tier.feed                = get_str(t, p + ".feed", "");
    tier.schema              = get_str(t, p + ".schema", "");
    tier.timestamp_semantics = get_str(t, p + ".timestamp_semantics", "");
    tier.coverage            = get_str(t, p + ".coverage", "");
    tier.historical_end_delay_minutes =
        static_cast<int>(get_int(t, p + ".historical_end_delay_minutes", 0));
}

Result<Config> finish(toml::table& root, std::string_view origin) {
    // --- strict schema check -------------------------------------------------
    std::vector<std::pair<std::string, std::string>> flat;
    collect_keys(root, "", flat);

    std::vector<std::string> unknown;
    for (const auto& [k, v] : flat) {
        if (!known_keys().contains(k)) unknown.push_back(k);
    }
    if (!unknown.empty()) {
        std::string msg = "unrecognised configuration key(s): ";
        for (std::size_t i = 0; i < unknown.size(); ++i) {
            if (i != 0) msg += ", ";
            msg += unknown[i];
        }
        return fail(make_error(ErrorCode::ConfigError, std::move(msg), std::string{origin}));
    }

    Config cfg;
    cfg.source_path = origin;

    cfg.run.seed = static_cast<std::uint64_t>(get_int(root, "run.seed", 20240101));
    cfg.run.results_dir = get_str(root, "run.results_dir", "results");
    cfg.run.tag = get_str(root, "run.tag", "");
    cfg.run.fail_on_validation_warning =
        get_bool(root, "run.fail_on_validation_warning", false);

    cfg.data.raw_dir        = get_str(root, "data.raw_dir", "data/raw");
    cfg.data.normalized_dir = get_str(root, "data.normalized_dir", "data/normalized");
    cfg.data.reference_dir  = get_str(root, "data.reference_dir", "data/reference");
    cfg.data.cache_dir      = get_str(root, "data.cache_dir", "data/cache");

    read_tier(root, "data.t1", cfg.t1);
    read_tier(root, "data.t2", cfg.t2);
    read_tier(root, "data.t3", cfg.t3);

    cfg.databento.max_spend_usd = get_dbl(root, "data.databento.max_spend_usd", 25.0);
    cfg.databento.require_explicit_paid_override =
        get_bool(root, "data.databento.require_explicit_paid_override", true);

    cfg.session.timezone      = get_str(root, "market_session.timezone", "America/New_York");
    cfg.session.regular_open  = get_str(root, "market_session.regular_open", "09:30:00");
    cfg.session.regular_close = get_str(root, "market_session.regular_close", "16:00:00");
    cfg.session.exclude_opening_auction =
        get_bool(root, "market_session.exclude_opening_auction", true);
    cfg.session.exclude_closing_auction =
        get_bool(root, "market_session.exclude_closing_auction", true);

    cfg.holdout.boundary_date = get_str(root, "holdout.boundary_date", "");
    cfg.holdout.months        = static_cast<int>(get_int(root, "holdout.months", 4));
    cfg.holdout.unlocked      = get_bool(root, "holdout.unlocked", false);
    cfg.holdout.unlock_justification = get_str(root, "holdout.unlock_justification", "");

    const std::string level_text = get_str(root, "log.level", "info");
    if (!log::parse_level(level_text, cfg.log.level)) {
        return fail(make_error(ErrorCode::ConfigError, "invalid log.level: " + level_text,
                               std::string{origin}));
    }
    cfg.log.file    = get_str(root, "log.file", "");
    cfg.log.console = get_bool(root, "log.console", true);
    cfg.log.json    = get_bool(root, "log.json", true);

    // --- semantic validation -------------------------------------------------
    if (cfg.holdout.months < 1) {
        return fail(make_error(ErrorCode::ConfigError, "holdout.months must be >= 1",
                               std::string{origin}));
    }
    if (cfg.databento.max_spend_usd < 0.0) {
        return fail(make_error(ErrorCode::ConfigError,
                               "data.databento.max_spend_usd must be >= 0",
                               std::string{origin}));
    }
    if (cfg.holdout.unlocked && cfg.holdout.unlock_justification.empty()) {
        return fail(make_error(ErrorCode::ConfigError,
                               "holdout.unlocked requires holdout.unlock_justification",
                               std::string{origin}));
    }

    // --- canonical form ------------------------------------------------------
    // Sorted, fully dotted, one key per line. Independent of TOML formatting,
    // key order, comments and whitespace, so two configs that mean the same
    // thing hash identically.
    std::sort(flat.begin(), flat.end());
    std::string canon;
    canon.reserve(flat.size() * 48);
    for (const auto& [k, v] : flat) {
        canon += k;
        canon += '=';
        canon += v;
        canon += '\n';
    }
    cfg.canonical = std::move(canon);
    return cfg;
}

}  // namespace

std::uint64_t Config::hash() const noexcept { return fnv1a64(canonical); }

std::string RunId::hex() const {
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(value));
    return std::string{buf};
}

RunId make_run_id(std::string_view config_canonical, std::string_view data_manifest_sha,
                  std::string_view git_sha, std::uint64_t seed) noexcept {
    // Concatenate with a separator that cannot appear in the parts, so that
    // moving a character across a boundary cannot collide.
    std::string material;
    material.reserve(config_canonical.size() + data_manifest_sha.size() + git_sha.size() + 64);
    material.append(config_canonical);
    material.push_back('\x1f');
    material.append(data_manifest_sha);
    material.push_back('\x1f');
    material.append(git_sha);
    material.push_back('\x1f');
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(seed));
    material.append(buf);
    return RunId{fnv1a64(material)};
}

Result<Config> load_from_string(std::string_view toml_text, std::span<const std::string> overrides,
                                std::string_view origin) {
    auto parsed = toml::parse(toml_text, origin);
    if (!parsed) {
        const auto& err = parsed.error();
        std::string msg{err.description()};
        msg += " at line " + std::to_string(err.source().begin.line);
        return fail(make_error(ErrorCode::ParseError, std::move(msg), std::string{origin}));
    }
    toml::table root = std::move(parsed).table();

    for (const auto& ov : overrides) {
        const std::size_t eq = ov.find('=');
        if (eq == std::string::npos || eq == 0) {
            return fail(make_error(ErrorCode::InvalidArgument,
                                   "override must be key=value, got: " + ov));
        }
        if (!set_dotted(root, std::string_view{ov}.substr(0, eq),
                        std::string_view{ov}.substr(eq + 1))) {
            return fail(make_error(ErrorCode::InvalidArgument,
                                   "cannot apply override: " + ov));
        }
    }
    return finish(root, origin);
}

Result<Config> load(const fs::path& path, std::span<const std::string> overrides) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return fail(make_error(ErrorCode::NotFound, "config file not found", path.string()));
    }
    std::ifstream in{path};
    if (!in) {
        return fail(make_error(ErrorCode::IoError, "cannot open config file", path.string()));
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return load_from_string(ss.str(), overrides, path.string());
}

}  // namespace ptl::config
