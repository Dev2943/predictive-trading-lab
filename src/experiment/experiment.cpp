#include "ptl/experiment/experiment.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace ptl::experiment {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

void hash_bytes(std::uint64_t& h, const void* data, std::size_t len) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<std::uint64_t>(p[i]);
        h *= 0x100000001b3ULL;
    }
}

void hash_string(std::uint64_t& h, std::string_view s) {
    hash_bytes(h, s.data(), s.size());
}

[[nodiscard]] std::string json_escape(std::string_view in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (const char c : in) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
        }
    }
    return out;
}

/// An UNSET timestamp is serialized as an empty string, never as a date.
///
/// to_iso8601 on kNoTimestamp overflows: converting int64-min nanoseconds to
/// days and back cannot be represented, which UBSan catches as a signed integer
/// overflow. Beyond the UB, rendering "not set" as a date in 1677 would be a
/// lie a reader has no way to detect.
[[nodiscard]] std::string iso_or_empty(Timestamp ts) {
    return is_set(ts) ? to_iso8601(ts) : std::string{};
}

[[nodiscard]] std::string num(double v, int precision = 10) {
    // JSON has no NaN literal; null is the honest representation of a metric
    // that could not be computed.
    if (!is_finite(v)) return "null";
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(precision) << v;
    return ss.str();
}

}  // namespace

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

std::uint64_t ExperimentConfig::fingerprint() const {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    hash_string(h, strategy_id.value());
    hash_bytes(h, &strategy_version.major, sizeof(strategy_version.major));
    hash_bytes(h, &strategy_version.minor, sizeof(strategy_version.minor));
    hash_bytes(h, &strategy_version.patch, sizeof(strategy_version.patch));
    hash_string(h, dataset_id);
    hash_bytes(h, &dataset_version, sizeof(dataset_version));
    hash_bytes(h, &seed, sizeof(seed));
    hash_bytes(h, &config_hash, sizeof(config_hash));
    // Parameters in key order (std::map), so the fingerprint does not depend on
    // the order the caller inserted them.
    for (const auto& [name, value] : parameters) {
        hash_string(h, name);
        hash_string(h, value);
    }
    // Deliberately EXCLUDED: experiment_id, description, tags. Renaming an
    // experiment does not change what it computes, and a fingerprint that moved
    // on a rename would make two identical runs look different.
    return h;
}

Result<bool> ExperimentConfig::validate() const {
    if (experiment_id.empty()) return fail(bad("experiment has no id"));
    if (strategy_id.empty()) return fail(bad("experiment names no strategy", experiment_id));
    if (dataset_id.empty() || dataset_version == 0) {
        return fail(
            bad("experiment names no dataset version; it would not be "
                "reproducible",
                experiment_id));
    }
    if (seed == 0) {
        // A zero seed is almost always an unset field rather than a deliberate
        // choice, and a run whose seed is unknown cannot be reproduced.
        return fail(
            bad("experiment has no seed; set one explicitly so the run can be "
                "reproduced",
                experiment_id));
    }
    return true;
}

std::string ExperimentConfig::to_json() const {
    std::ostringstream ss;
    ss << "{\"experiment_id\": \"" << json_escape(experiment_id) << "\", \"description\": \""
       << json_escape(description) << "\", \"strategy\": \"" << json_escape(strategy_id.value())
       << '@' << strategy_version.to_string() << "\", \"dataset\": \"" << json_escape(dataset_id)
       << "@v" << dataset_version << "\", \"seed\": " << seed << ", \"config_hash\": \"" << std::hex
       << std::setw(16) << std::setfill('0') << config_hash << std::dec << "\", \"fingerprint\": \""
       << std::hex << std::setw(16) << std::setfill('0') << fingerprint() << std::dec
       << "\", \"parameters\": {";
    bool first = true;
    for (const auto& [name, value] : parameters) {
        if (!first) ss << ", ";
        first = false;
        ss << '"' << json_escape(name) << "\": \"" << json_escape(value) << '"';
    }
    ss << "}, \"tags\": [";
    for (std::size_t i = 0; i < tags.size(); ++i) {
        if (i != 0) ss << ", ";
        ss << '"' << json_escape(tags[i]) << '"';
    }
    ss << "]}";
    return ss.str();
}

// ---------------------------------------------------------------------------
// Result
// ---------------------------------------------------------------------------

std::string_view to_string(ExperimentStatus s) noexcept {
    switch (s) {
        case ExperimentStatus::Defined:
            return "defined";
        case ExperimentStatus::Running:
            return "running";
        case ExperimentStatus::Completed:
            return "completed";
        case ExperimentStatus::Failed:
            return "failed";
        case ExperimentStatus::Cancelled:
            return "cancelled";
    }
    return "unknown";
}

Duration ExperimentResult::duration() const noexcept {
    if (!is_set(started_at) || !is_set(finished_at)) return Duration::zero();
    return finished_at - started_at;
}

std::optional<double> ExperimentResult::metric(std::string_view name) const {
    // Custom metrics take precedence: a caller who explicitly recorded a value
    // under a name meant that one.
    if (const auto it = custom_metrics.find(name); it != custom_metrics.end()) {
        return it->second;
    }

    // NOTHING IS RECOMPUTED HERE. Every value is read from the analytics types
    // that already own its definition, so there is exactly one "Sharpe" in the
    // system.
    if (performance.has_value()) {
        const auto& p = *performance;
        if (name == "sharpe") return p.metrics.sharpe;
        if (name == "sortino") return p.metrics.sortino;
        if (name == "calmar") return p.metrics.calmar;
        if (name == "cumulative_return") return p.metrics.cumulative_return;
        if (name == "cagr") return p.metrics.cagr;
        if (name == "max_drawdown") return p.max_drawdown;
        if (name == "information_ratio") return p.risk.information_ratio;
        if (name == "annualized_volatility") return p.risk.annualized_volatility;
        if (name == "beta") return p.risk.beta;
        if (name == "alpha") return p.risk.alpha;
        if (name == "value_at_risk_95") return p.risk.value_at_risk_95;
        if (name == "expected_shortfall_95") return p.risk.expected_shortfall_95;
        if (name == "turnover") return p.turnover.annualized_turnover;
        if (name == "win_rate") return p.trades.win_rate;
        if (name == "profit_factor") return p.trades.profit_factor;
        if (name == "trades") return static_cast<double>(p.trades.trades);
        if (name == "peak_gross_leverage") return p.peak_gross_leverage;
        if (name == "final_equity") return p.final_equity.get();
        if (name == "total_costs") return p.metrics.total_costs.get();
    }
    if (pnl.has_value()) {
        if (name == "net_pnl") return pnl->net_pnl().get();
        if (name == "gross_pnl") return pnl->gross_pnl().get();
        if (name == "slippage") return pnl->slippage.get();
    }
    if (execution.has_value()) {
        if (name == "implementation_shortfall_bps") {
            return execution->average_implementation_shortfall.get();
        }
        if (name == "fill_efficiency") return execution->average_fill_efficiency;
        if (name == "participation_rate") return execution->average_participation_rate;
    }
    if (factors.has_value()) {
        if (name == "alpha_contribution") return factors->alpha_contribution;
        if (name == "beta_contribution") return factors->beta_contribution;
    }
    return std::nullopt;
}

std::vector<std::string> ExperimentResult::available_metrics() const {
    static const std::vector<std::string> candidates{"alpha",
                                                     "alpha_contribution",
                                                     "annualized_volatility",
                                                     "beta",
                                                     "beta_contribution",
                                                     "cagr",
                                                     "calmar",
                                                     "cumulative_return",
                                                     "expected_shortfall_95",
                                                     "fill_efficiency",
                                                     "final_equity",
                                                     "gross_pnl",
                                                     "implementation_shortfall_bps",
                                                     "information_ratio",
                                                     "max_drawdown",
                                                     "net_pnl",
                                                     "participation_rate",
                                                     "peak_gross_leverage",
                                                     "profit_factor",
                                                     "sharpe",
                                                     "slippage",
                                                     "sortino",
                                                     "total_costs",
                                                     "trades",
                                                     "turnover",
                                                     "value_at_risk_95",
                                                     "win_rate"};

    std::vector<std::string> out;
    for (const auto& name : candidates) {
        if (metric(name).has_value()) out.push_back(name);
    }
    for (const auto& [name, value] : custom_metrics) {
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string ExperimentResult::to_json() const {
    std::ostringstream ss;
    ss << "{\"experiment_id\": \"" << json_escape(experiment_id) << "\", \"status\": \""
       << to_string(status) << "\", \"config_fingerprint\": \"" << std::hex << std::setw(16)
       << std::setfill('0') << config_fingerprint << std::dec << "\", \"started_at\": \""
       << iso_or_empty(started_at) << "\", \"finished_at\": \"" << iso_or_empty(finished_at)
       << "\", \"metrics\": {";
    bool first = true;
    for (const auto& name : available_metrics()) {
        const auto value = metric(name);
        if (!value.has_value()) continue;
        if (!first) ss << ", ";
        first = false;
        ss << '"' << json_escape(name) << "\": " << num(*value);
    }
    ss << '}';
    if (!error_message.empty()) {
        ss << ", \"error\": \"" << json_escape(error_message) << '"';
    }
    ss << '}';
    return ss.str();
}

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------

std::vector<ComparisonMetric> default_comparison_metrics() {
    // Direction is stated per metric. A leaderboard assuming "higher is
    // better" would rank the worst drawdown first, and the ranking would look
    // perfectly plausible.
    return {
        {"sharpe", Direction::HigherIsBetter, 2.0},
        {"sortino", Direction::HigherIsBetter, 1.0},
        {"calmar", Direction::HigherIsBetter, 1.0},
        {"information_ratio", Direction::HigherIsBetter, 1.5},
        {"max_drawdown", Direction::LowerIsBetter, 1.5},
        {"turnover", Direction::LowerIsBetter, 0.5},
        {"net_pnl", Direction::HigherIsBetter, 1.0},
        {"implementation_shortfall_bps", Direction::LowerIsBetter, 1.0},
        {"total_costs", Direction::LowerIsBetter, 0.5},
    };
}

const ComparisonRow* ComparisonReport::best() const noexcept {
    return rows.empty() ? nullptr : &rows.front();
}

std::string ComparisonReport::to_json() const {
    std::ostringstream ss;
    ss << "{\"metrics\": [";
    for (std::size_t i = 0; i < metrics.size(); ++i) {
        if (i != 0) ss << ", ";
        ss << "{\"name\": \"" << json_escape(metrics[i].name) << "\", \"direction\": \""
           << (metrics[i].direction == Direction::HigherIsBetter ? "higher_is_better"
                                                                 : "lower_is_better")
           << "\", \"weight\": " << num(metrics[i].weight, 4) << '}';
    }
    ss << "], \"rows\": [";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (i != 0) ss << ", ";
        ss << "{\"experiment_id\": \"" << json_escape(rows[i].experiment_id)
           << "\", \"overall_rank\": " << rows[i].overall_rank
           << ", \"composite\": " << num(rows[i].composite_score, 6) << ", \"values\": {";
        bool first = true;
        for (const auto& [name, value] : rows[i].values) {
            if (!first) ss << ", ";
            first = false;
            ss << '"' << json_escape(name) << "\": " << (value.has_value() ? num(*value) : "null");
        }
        ss << "}}";
    }
    ss << "], \"unavailable_metrics\": [";
    for (std::size_t i = 0; i < unavailable_metrics.size(); ++i) {
        if (i != 0) ss << ", ";
        ss << '"' << json_escape(unavailable_metrics[i]) << '"';
    }
    ss << "]}";
    return ss.str();
}

std::string ComparisonReport::describe() const {
    std::ostringstream ss;
    ss.precision(4);
    ss << std::fixed << "comparison of " << rows.size() << " experiments\n";
    for (const auto& row : rows) {
        ss << "  " << row.overall_rank << ". " << row.experiment_id << " (composite "
           << row.composite_score << ")\n";
    }
    if (!unavailable_metrics.empty()) {
        // Reported rather than dropped: a silently missing column looks like
        // agreement between experiments that simply never measured it.
        ss << "  unavailable: ";
        for (std::size_t i = 0; i < unavailable_metrics.size(); ++i) {
            if (i != 0) ss << ", ";
            ss << unavailable_metrics[i];
        }
        ss << '\n';
    }
    return ss.str();
}

Result<ComparisonReport> ExperimentComparison::compare(
    std::span<const ExperimentResult> results) const {
    if (results.empty()) return fail(bad("cannot compare an empty set of experiments"));

    ComparisonReport report;
    report.metrics = metrics_;
    report.rows.reserve(results.size());

    for (const auto& result : results) {
        ComparisonRow row;
        row.experiment_id = result.experiment_id;
        for (const auto& metric : metrics_) {
            row.values[metric.name] = result.metric(metric.name);
            row.ranks[metric.name] = std::nullopt;
        }
        report.rows.push_back(std::move(row));
    }

    for (const auto& metric : metrics_) {
        // Rank each metric independently over the experiments that can answer
        // for it. An experiment missing a metric is not penalised as though it
        // scored badly -- it simply does not participate in that ranking.
        std::vector<std::size_t> participants;
        for (std::size_t i = 0; i < report.rows.size(); ++i) {
            const auto& value = report.rows[i].values[metric.name];
            if (value.has_value() && is_finite(*value)) participants.push_back(i);
        }

        if (participants.empty()) {
            report.unavailable_metrics.push_back(metric.name);
            continue;
        }
        std::sort(participants.begin(), participants.end(),
                  [&report, &metric](std::size_t a, std::size_t b) {
                      const double va = *report.rows[a].values[metric.name];
                      const double vb = *report.rows[b].values[metric.name];
                      if (va != vb) {
                          return metric.direction == Direction::HigherIsBetter ? va > vb : va < vb;
                      }
                      // Tie-break on id, so the ordering is a pure function of
                      // the inputs rather than of sort stability.
                      return report.rows[a].experiment_id < report.rows[b].experiment_id;
                  });

        for (std::size_t rank = 0; rank < participants.size(); ++rank) {
            report.rows[participants[rank]].ranks[metric.name] = rank + 1;
        }
    }

    // Composite: weighted mean normalised rank, divided by the weight this row
    // ACTUALLY PARTICIPATED IN -- not by the global weight.
    //
    // Dividing by the global weight would mean an experiment that answered for
    // more metrics scored higher purely for having measured more, which is a
    // penalty on the incomplete one wearing a neutral face. Per-row
    // normalisation makes a missing metric neither help nor hurt.
    //
    // The residual risk is the opposite one: an experiment answering for a
    // single easy metric could score perfectly on it. That is why
    // `unavailable_metrics` and the per-metric ranks are both reported -- a
    // composite alone was never meant to be read without them.
    for (auto& row : report.rows) {
        double weighted = 0.0;
        double participating_weight = 0.0;
        for (const auto& metric : metrics_) {
            if (metric.weight <= 0.0) continue;
            const auto rank = row.ranks[metric.name];
            if (!rank.has_value()) continue;
            participating_weight += metric.weight;
            const double normalised =
                1.0 - static_cast<double>(*rank - 1) /
                          std::max(1.0, static_cast<double>(report.rows.size()));
            weighted += metric.weight * normalised;
        }
        row.composite_score = participating_weight > 0.0 ? weighted / participating_weight : 0.0;
    }

    std::sort(report.rows.begin(), report.rows.end(),
              [](const ComparisonRow& a, const ComparisonRow& b) {
                  if (a.composite_score != b.composite_score) {
                      return a.composite_score > b.composite_score;
                  }
                  return a.experiment_id < b.experiment_id;
              });
    for (std::size_t i = 0; i < report.rows.size(); ++i) {
        report.rows[i].overall_rank = i + 1;
    }
    std::sort(report.unavailable_metrics.begin(), report.unavailable_metrics.end());
    return report;
}

// ---------------------------------------------------------------------------
// Leaderboard
// ---------------------------------------------------------------------------

const LeaderboardEntry* Leaderboard::leader() const noexcept {
    return entries.empty() ? nullptr : &entries.front();
}

std::string Leaderboard::to_json() const {
    std::ostringstream ss;
    ss << "{\"name\": \"" << json_escape(name) << "\", \"primary_metric\": \""
       << json_escape(primary_metric) << "\", \"entries\": [";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i != 0) ss << ", ";
        ss << "{\"rank\": " << entries[i].rank << ", \"experiment_id\": \""
           << json_escape(entries[i].experiment_id) << "\", \"strategy_id\": \""
           << json_escape(entries[i].strategy_id) << "\", \"score\": " << num(entries[i].score)
           << ", \"metrics\": {";
        bool first = true;
        for (const auto& [key, value] : entries[i].metrics) {
            if (!first) ss << ", ";
            first = false;
            ss << '"' << json_escape(key) << "\": " << num(value);
        }
        ss << "}}";
    }
    ss << "]}";
    return ss.str();
}

Result<Leaderboard> LeaderboardBuilder::build(std::span<const Experiment> experiments,
                                              std::string_view metric, Direction direction,
                                              std::size_t limit) {
    if (metric.empty()) return fail(bad("leaderboard needs a metric to rank on"));

    Leaderboard board;
    board.name = "leaderboard_" + std::string{metric};
    board.primary_metric = std::string{metric};

    for (const auto& experiment : experiments) {
        // Only COMPLETED experiments are ranked. Including a failed or running
        // one would put a partial result on a board that reads as final.
        if (!experiment.complete()) continue;
        const auto value = experiment.result->metric(metric);
        if (!value.has_value() || !is_finite(*value)) continue;

        LeaderboardEntry entry;
        entry.experiment_id = experiment.config.experiment_id;
        entry.strategy_id = experiment.config.strategy_id.value();
        entry.score = *value;
        for (const auto& name : experiment.result->available_metrics()) {
            if (const auto v = experiment.result->metric(name); v.has_value()) {
                entry.metrics[name] = *v;
            }
        }
        board.entries.push_back(std::move(entry));
    }

    std::sort(board.entries.begin(), board.entries.end(),
              [direction](const LeaderboardEntry& a, const LeaderboardEntry& b) {
                  if (a.score != b.score) {
                      return direction == Direction::HigherIsBetter ? a.score > b.score
                                                                    : a.score < b.score;
                  }
                  return a.experiment_id < b.experiment_id;
              });

    if (limit > 0 && board.entries.size() > limit) board.entries.resize(limit);
    for (std::size_t i = 0; i < board.entries.size(); ++i) {
        board.entries[i].rank = i + 1;
    }
    return board;
}

// ---------------------------------------------------------------------------
// Snapshot and store
// ---------------------------------------------------------------------------

std::uint64_t ExperimentSnapshot::checksum() const {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    hash_string(h, experiment_id);
    hash_bytes(h, &sequence, sizeof(sequence));
    hash_bytes(h, &events_processed, sizeof(events_processed));
    hash_bytes(h, &config_fingerprint, sizeof(config_fingerprint));
    const std::int64_t ns = last_event_time.time_since_epoch().count();
    hash_bytes(h, &ns, sizeof(ns));
    return h;
}

std::string ExperimentSnapshot::to_json() const {
    std::ostringstream ss;
    ss << "{\"experiment_id\": \"" << json_escape(experiment_id) << "\", \"sequence\": " << sequence
       << ", \"status\": \"" << to_string(status) << "\", \"taken_at\": \""
       << iso_or_empty(taken_at) << "\", \"events_processed\": " << events_processed
       << ", \"last_event_time\": \"" << iso_or_empty(last_event_time)
       << "\", \"config_fingerprint\": \"" << std::hex << std::setw(16) << std::setfill('0')
       << config_fingerprint << "\", \"checksum\": \"" << std::setw(16) << std::setfill('0')
       << checksum() << std::dec << "\"}";
    return ss.str();
}

Result<ExperimentSnapshot> ExperimentSnapshot::from_json(std::string_view json) {
    // A deliberately narrow reader for a format this module also writes. It
    // extracts by key rather than parsing generally, and verifies the checksum
    // before returning -- a corrupt snapshot must be refused, not resumed from.
    const auto field = [json](std::string_view key) -> std::optional<std::string> {
        const std::string needle = "\"" + std::string{key} + "\":";
        const auto pos = json.find(needle);
        if (pos == std::string_view::npos) return std::nullopt;
        auto cursor = json.find_first_not_of(" \t", pos + needle.size());
        if (cursor == std::string_view::npos) return std::nullopt;

        if (json[cursor] == '"') {
            const auto end = json.find('"', cursor + 1);
            if (end == std::string_view::npos) return std::nullopt;
            return std::string{json.substr(cursor + 1, end - cursor - 1)};
        }
        const auto end = json.find_first_of(",}", cursor);
        return std::string{json.substr(cursor, end - cursor)};
    };

    ExperimentSnapshot snapshot;
    const auto id = field("experiment_id");
    if (!id.has_value()) return fail(bad("snapshot has no experiment_id"));
    snapshot.experiment_id = *id;

    const auto parse_u64 = [](const std::optional<std::string>& text, std::uint64_t& out,
                              int base = 10) -> bool {
        if (!text.has_value() || text->empty()) return false;
        const auto* first = text->data();
        const auto* last = text->data() + text->size();
        return std::from_chars(first, last, out, base).ec == std::errc{};
    };

    if (!parse_u64(field("sequence"), snapshot.sequence)) {
        return fail(bad("snapshot has an unreadable sequence"));
    }
    std::uint64_t events = 0;
    if (!parse_u64(field("events_processed"), events)) {
        return fail(bad("snapshot has an unreadable event count"));
    }
    snapshot.events_processed = static_cast<std::size_t>(events);

    if (!parse_u64(field("config_fingerprint"), snapshot.config_fingerprint, 16)) {
        return fail(bad("snapshot has an unreadable config fingerprint"));
    }
    // An empty string means the timestamp was never set, which is a valid
    // state rather than a parse failure.
    if (const auto taken = field("taken_at"); taken.has_value() && !taken->empty()) {
        (void)parse_timestamp(*taken, snapshot.taken_at);
    }
    if (const auto last = field("last_event_time"); last.has_value() && !last->empty()) {
        (void)parse_timestamp(*last, snapshot.last_event_time);
    }

    std::uint64_t stored = 0;
    if (!parse_u64(field("checksum"), stored, 16)) {
        return fail(bad("snapshot has no checksum; it may be truncated"));
    }
    if (stored != snapshot.checksum()) {
        // Refused, not repaired. Resuming from a damaged snapshot is worse than
        // starting again, because the run would look complete.
        return fail(bad("snapshot checksum mismatch; it is corrupt", snapshot.experiment_id));
    }
    return snapshot;
}

Result<bool> ExperimentStore::save_config(const ExperimentConfig& config) {
    if (auto ok = config.validate(); !ok) return ok;
    auto written =
        artifacts_->put("experiments/" + config.experiment_id + "/config", config.to_json());
    if (!written) return fail(written.error());
    return true;
}

Result<bool> ExperimentStore::save_result(const ExperimentResult& result) {
    if (result.experiment_id.empty()) return fail(bad("result has no experiment id"));
    auto written =
        artifacts_->put("experiments/" + result.experiment_id + "/result", result.to_json());
    if (!written) return fail(written.error());
    return true;
}

Result<bool> ExperimentStore::save_snapshot(const ExperimentSnapshot& snapshot) {
    if (snapshot.experiment_id.empty()) return fail(bad("snapshot has no experiment id"));
    auto written =
        artifacts_->put("experiments/" + snapshot.experiment_id + "/snapshot", snapshot.to_json());
    if (!written) return fail(written.error());
    return true;
}

Result<std::optional<ExperimentSnapshot>> ExperimentStore::load_snapshot(
    std::string_view experiment_id) {
    const std::string key = "experiments/" + std::string{experiment_id} + "/snapshot";
    if (!artifacts_->contains(key)) return std::optional<ExperimentSnapshot>{};

    auto json = artifacts_->get(key);
    if (!json) return fail(json.error());
    auto snapshot = ExperimentSnapshot::from_json(*json);
    if (!snapshot) return fail(snapshot.error());
    return std::optional<ExperimentSnapshot>{std::move(*snapshot)};
}

Result<std::vector<std::string>> ExperimentStore::list_experiments() const {
    auto keys = artifacts_->list("experiments");
    if (!keys) return fail(keys.error());

    std::vector<std::string> out;
    for (const auto& key : *keys) {
        // "experiments/<id>/config" -> "<id>".
        const auto first = key.find('/');
        if (first == std::string::npos) continue;
        const auto second = key.find('/', first + 1);
        if (second == std::string::npos) continue;
        std::string id = key.substr(first + 1, second - first - 1);
        if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(std::move(id));
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------

Result<bool> ExperimentRunner::validate(const ExperimentConfig& config) const {
    if (auto ok = config.validate(); !ok) return ok;

    const auto* descriptor = strategies_->find(config.strategy_id, config.strategy_version);
    if (descriptor == nullptr) {
        return fail(bad("strategy is not registered at that version",
                        config.strategy_id.value() + "@" + config.strategy_version.to_string()));
    }
    if (!strategy::accepts_new_experiments(descriptor->state)) {
        // A deprecated strategy may still be reproduced, but no new research
        // should start against it.
        return fail(bad("strategy state '" + std::string{to_string(descriptor->state)} +
                            "' does not accept new experiments",
                        descriptor->id.value()));
    }
    if (auto ok = strategy::validate_parameters(*descriptor, config.parameters); !ok) {
        return ok;
    }
    if (!datasets_->contains(config.dataset_id, config.dataset_version)) {
        return fail(
            bad("dataset version is not registered; the experiment would not be "
                "reproducible",
                config.dataset_id + "@v" + std::to_string(config.dataset_version)));
    }
    return true;
}

Result<Experiment> ExperimentRunner::prepare(ExperimentConfig config) const {
    if (auto ok = validate(config); !ok) return fail(ok.error());
    Experiment experiment;
    experiment.config = std::move(config);
    experiment.status = ExperimentStatus::Defined;
    return experiment;
}

Result<bool> ExperimentRunner::can_resume(const ExperimentConfig& config,
                                          const ExperimentSnapshot& snapshot) {
    if (snapshot.experiment_id != config.experiment_id) {
        return fail(bad("snapshot belongs to a different experiment",
                        snapshot.experiment_id + " != " + config.experiment_id));
    }
    if (snapshot.config_fingerprint != config.fingerprint()) {
        // Resuming into a different configuration silently mixes two runs, and
        // every result afterwards is attributed to the wrong definition.
        return fail(
            bad("snapshot was taken under a different configuration; refusing to "
                "resume",
                config.experiment_id));
    }
    return true;
}

}  // namespace ptl::experiment
