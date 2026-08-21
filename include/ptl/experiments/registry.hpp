#pragma once

/// \file registry.hpp
/// The experiment registry: runs, trials, search budgets and holdout unlocks.
///
/// A RUN registry answers "what produced this result?". That is necessary but
/// not sufficient. A TRIAL registry answers "how many things did you try before
/// this one looked good?" -- which is the input the Deflated Sharpe Ratio needs
/// and the question an interviewer will ask. Both live here.
///
/// SQLite because it is a single file, needs no server, and supports real SQL
/// over the run history. It is deliberately NOT used for bulk time series.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"

struct sqlite3;

namespace ptl::experiments {

namespace fs = std::filesystem;

struct RunRecord {
    std::string   run_id;
    std::string   created_utc;
    std::string   git_sha;
    std::string   git_dirty;
    std::string   config_hash;
    std::string   config_canonical;
    std::string   data_manifest_sha;
    std::uint64_t seed = 0;
    std::string   compiler;
    std::string   compiler_version;
    std::string   build_type;
    std::string   tag;
    std::string   status = "started";  // started | completed | failed | invalidated
    std::uint64_t chain_violations = 0;
};

struct TrialRecord {
    std::int64_t  trial_id = 0;  // assigned on insert
    std::string   run_id;
    std::string   research_question;
    std::string   hypothesis;
    std::string   params_json;
    std::string   status = "planned";  // planned | run | abandoned
};

/// A search budget declared BEFORE evaluation begins. Declaring the number of
/// trials up front is what makes the count meaningful; counting them afterwards
/// measures persistence, not discipline.
struct SearchBudget {
    std::string   research_question;
    std::string   declared_utc;
    std::int64_t  budget = 0;
    std::int64_t  used = 0;
    std::string   rationale;
};

class Registry {
public:
    Registry() = default;
    ~Registry();
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;
    Registry(Registry&&) noexcept;
    Registry& operator=(Registry&&) noexcept;

    /// Opens (creating if absent) and applies the schema migration.
    [[nodiscard]] static Result<Registry> open(const fs::path& db_path);

    [[nodiscard]] Result<bool> insert_run(const RunRecord&);
    [[nodiscard]] Result<bool> finish_run(std::string_view run_id, std::string_view status,
                                          std::uint64_t chain_violations);
    [[nodiscard]] Result<std::optional<RunRecord>> find_run(std::string_view run_id);

    [[nodiscard]] Result<std::int64_t> insert_trial(const TrialRecord&);
    [[nodiscard]] Result<std::int64_t> trial_count(std::string_view research_question);

    [[nodiscard]] Result<bool> declare_budget(const SearchBudget&);
    [[nodiscard]] Result<std::optional<SearchBudget>> get_budget(std::string_view question);

    /// True when trials for `question` have exceeded the declared budget. Every
    /// report that quotes a Sharpe must also quote this.
    [[nodiscard]] Result<bool> budget_exceeded(std::string_view question);

    [[nodiscard]] Result<bool> record_metric(std::string_view run_id, std::int64_t trial_id,
                                             std::string_view name, double value);

    /// Records an indelible unlock of the locked holdout. There is no delete
    /// path: the point of the holdout is that using it is permanent and
    /// visible.
    [[nodiscard]] Result<bool> record_holdout_unlock(std::string_view run_id,
                                                     std::string_view justification);
    [[nodiscard]] Result<std::int64_t> holdout_unlock_count();

private:
    explicit Registry(sqlite3* db) noexcept : db_(db) {}
    sqlite3* db_ = nullptr;
};

}  // namespace ptl::experiments
