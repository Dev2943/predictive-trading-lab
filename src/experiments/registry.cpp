#include "ptl/experiments/registry.hpp"

#include <sqlite3.h>

#include <utility>

#include "ptl/core/clock.hpp"

namespace ptl::experiments {
namespace {

constexpr const char* kSchema = R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;

CREATE TABLE IF NOT EXISTS runs (
    run_id            TEXT PRIMARY KEY,
    created_utc       TEXT NOT NULL,
    git_sha           TEXT NOT NULL,
    git_dirty         TEXT NOT NULL,
    config_hash       TEXT NOT NULL,
    config_canonical  TEXT NOT NULL,
    data_manifest_sha TEXT NOT NULL,
    seed              INTEGER NOT NULL,
    compiler          TEXT NOT NULL,
    compiler_version  TEXT NOT NULL,
    build_type        TEXT NOT NULL,
    tag               TEXT NOT NULL,
    status            TEXT NOT NULL,
    chain_violations  INTEGER NOT NULL DEFAULT 0,
    finished_utc      TEXT
);

CREATE TABLE IF NOT EXISTS trials (
    trial_id          INTEGER PRIMARY KEY AUTOINCREMENT,
    run_id            TEXT NOT NULL,
    created_utc       TEXT NOT NULL,
    research_question TEXT NOT NULL,
    hypothesis        TEXT NOT NULL,
    params_json       TEXT NOT NULL,
    status            TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_trials_question ON trials(research_question);

CREATE TABLE IF NOT EXISTS metrics (
    run_id   TEXT NOT NULL,
    trial_id INTEGER NOT NULL,
    name     TEXT NOT NULL,
    value    REAL NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_metrics_run ON metrics(run_id);

CREATE TABLE IF NOT EXISTS search_budgets (
    research_question TEXT PRIMARY KEY,
    declared_utc      TEXT NOT NULL,
    budget            INTEGER NOT NULL,
    rationale         TEXT NOT NULL
);

-- Append-only by design. There is deliberately no delete path: the value of a
-- locked holdout comes entirely from the fact that unlocking it is permanent
-- and visible.
CREATE TABLE IF NOT EXISTS holdout_unlocks (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    unlocked_utc  TEXT NOT NULL,
    run_id        TEXT NOT NULL,
    justification TEXT NOT NULL
);
)SQL";

[[nodiscard]] Error sqlite_error(sqlite3* db, std::string_view what) {
    return make_error(ErrorCode::IoError, std::string{what},
                      db != nullptr ? sqlite3_errmsg(db) : "no database handle");
}

/// RAII for a prepared statement. sqlite3_finalize on every exit path,
/// including the error paths, which is where leaks actually happen.
class Stmt {
public:
    Stmt(sqlite3* db, std::string_view sql) : db_(db) {
        rc_ = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &stmt_, nullptr);
    }
    ~Stmt() { sqlite3_finalize(stmt_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    Stmt(Stmt&&) = delete;
    Stmt& operator=(Stmt&&) = delete;

    [[nodiscard]] bool ok() const noexcept { return rc_ == SQLITE_OK && stmt_ != nullptr; }
    [[nodiscard]] sqlite3* db() const noexcept { return db_; }
    [[nodiscard]] sqlite3_stmt* get() const noexcept { return stmt_; }

    void bind(int i, std::string_view v) {
        sqlite3_bind_text(stmt_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    }
    void bind(int i, std::int64_t v) { sqlite3_bind_int64(stmt_, i, v); }
    void bind(int i, double v) { sqlite3_bind_double(stmt_, i, v); }

    [[nodiscard]] int step() { return sqlite3_step(stmt_); }

    [[nodiscard]] std::string text(int col) const {
        const auto* p = sqlite3_column_text(stmt_, col);
        return p == nullptr ? std::string{} : std::string{reinterpret_cast<const char*>(p)};
    }
    [[nodiscard]] std::int64_t i64(int col) const { return sqlite3_column_int64(stmt_, col); }

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
    int rc_ = SQLITE_ERROR;
};

std::string now_utc() {
    return to_iso8601(WallClock{}.now());
}

}  // namespace

Registry::~Registry() {
    if (db_ != nullptr) sqlite3_close(db_);
}
Registry::Registry(Registry&& other) noexcept : db_(std::exchange(other.db_, nullptr)) {}
Registry& Registry::operator=(Registry&& other) noexcept {
    if (this != &other) {
        if (db_ != nullptr) sqlite3_close(db_);
        db_ = std::exchange(other.db_, nullptr);
    }
    return *this;
}

Result<Registry> Registry::open(const fs::path& db_path) {
    std::error_code ec;
    if (db_path.has_parent_path()) fs::create_directories(db_path.parent_path(), ec);

    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.string().c_str(), &db) != SQLITE_OK) {
        Error e = sqlite_error(db, "cannot open registry");
        sqlite3_close(db);
        return fail(std::move(e));
    }
    char* err = nullptr;
    if (sqlite3_exec(db, kSchema, nullptr, nullptr, &err) != SQLITE_OK) {
        Error e = make_error(ErrorCode::IoError, "registry schema migration failed",
                             err != nullptr ? err : "");
        sqlite3_free(err);
        sqlite3_close(db);
        return fail(std::move(e));
    }
    return Registry{db};
}

Result<bool> Registry::insert_run(const RunRecord& r) {
    Stmt s{db_,
           "INSERT OR REPLACE INTO runs (run_id, created_utc, git_sha, git_dirty, config_hash,"
           " config_canonical, data_manifest_sha, seed, compiler, compiler_version, build_type,"
           " tag, status, chain_violations)"
           " VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14)"};
    if (!s.ok()) return fail(sqlite_error(db_, "prepare insert_run"));
    s.bind(1, r.run_id);
    s.bind(2, r.created_utc.empty() ? now_utc() : r.created_utc);
    s.bind(3, r.git_sha);
    s.bind(4, r.git_dirty);
    s.bind(5, r.config_hash);
    s.bind(6, r.config_canonical);
    s.bind(7, r.data_manifest_sha);
    s.bind(8, static_cast<std::int64_t>(r.seed));
    s.bind(9, r.compiler);
    s.bind(10, r.compiler_version);
    s.bind(11, r.build_type);
    s.bind(12, r.tag);
    s.bind(13, r.status);
    s.bind(14, static_cast<std::int64_t>(r.chain_violations));
    if (s.step() != SQLITE_DONE) return fail(sqlite_error(db_, "insert_run"));
    return true;
}

Result<bool> Registry::finish_run(std::string_view run_id, std::string_view status,
                                  std::uint64_t chain_violations) {
    Stmt s{db_, "UPDATE runs SET status=?1, finished_utc=?2, chain_violations=?3 WHERE run_id=?4"};
    if (!s.ok()) return fail(sqlite_error(db_, "prepare finish_run"));
    s.bind(1, status);
    s.bind(2, now_utc());
    s.bind(3, static_cast<std::int64_t>(chain_violations));
    s.bind(4, run_id);
    if (s.step() != SQLITE_DONE) return fail(sqlite_error(db_, "finish_run"));
    return true;
}

Result<std::optional<RunRecord>> Registry::find_run(std::string_view run_id) {
    Stmt s{db_,
           "SELECT run_id, created_utc, git_sha, git_dirty, config_hash, config_canonical,"
           " data_manifest_sha, seed, compiler, compiler_version, build_type, tag, status,"
           " chain_violations FROM runs WHERE run_id=?1"};
    if (!s.ok()) return fail(sqlite_error(db_, "prepare find_run"));
    s.bind(1, run_id);
    const int rc = s.step();
    if (rc == SQLITE_DONE) return std::optional<RunRecord>{};
    if (rc != SQLITE_ROW) return fail(sqlite_error(db_, "find_run"));

    RunRecord r;
    r.run_id = s.text(0);
    r.created_utc = s.text(1);
    r.git_sha = s.text(2);
    r.git_dirty = s.text(3);
    r.config_hash = s.text(4);
    r.config_canonical = s.text(5);
    r.data_manifest_sha = s.text(6);
    r.seed = static_cast<std::uint64_t>(s.i64(7));
    r.compiler = s.text(8);
    r.compiler_version = s.text(9);
    r.build_type = s.text(10);
    r.tag = s.text(11);
    r.status = s.text(12);
    r.chain_violations = static_cast<std::uint64_t>(s.i64(13));
    return std::optional<RunRecord>{std::move(r)};
}

Result<std::int64_t> Registry::insert_trial(const TrialRecord& t) {
    Stmt s{db_,
           "INSERT INTO trials (run_id, created_utc, research_question, hypothesis, params_json,"
           " status) VALUES (?1,?2,?3,?4,?5,?6)"};
    if (!s.ok()) return fail(sqlite_error(db_, "prepare insert_trial"));
    s.bind(1, t.run_id);
    s.bind(2, now_utc());
    s.bind(3, t.research_question);
    s.bind(4, t.hypothesis);
    s.bind(5, t.params_json);
    s.bind(6, t.status);
    if (s.step() != SQLITE_DONE) return fail(sqlite_error(db_, "insert_trial"));
    return sqlite3_last_insert_rowid(db_);
}

Result<std::int64_t> Registry::trial_count(std::string_view research_question) {
    Stmt s{db_, "SELECT COUNT(*) FROM trials WHERE research_question=?1"};
    if (!s.ok()) return fail(sqlite_error(db_, "prepare trial_count"));
    s.bind(1, research_question);
    if (s.step() != SQLITE_ROW) return fail(sqlite_error(db_, "trial_count"));
    return s.i64(0);
}

Result<bool> Registry::declare_budget(const SearchBudget& b) {
    Stmt s{db_,
           "INSERT OR IGNORE INTO search_budgets (research_question, declared_utc, budget,"
           " rationale) VALUES (?1,?2,?3,?4)"};
    if (!s.ok()) return fail(sqlite_error(db_, "prepare declare_budget"));
    s.bind(1, b.research_question);
    s.bind(2, b.declared_utc.empty() ? now_utc() : b.declared_utc);
    s.bind(3, b.budget);
    s.bind(4, b.rationale);
    if (s.step() != SQLITE_DONE) return fail(sqlite_error(db_, "declare_budget"));
    // INSERT OR IGNORE: a budget is declared once, before evaluation. Silently
    // raising it after the fact would defeat the entire mechanism.
    return sqlite3_changes(db_) > 0;
}

Result<std::optional<SearchBudget>> Registry::get_budget(std::string_view question) {
    Stmt s{db_,
           "SELECT research_question, declared_utc, budget, rationale FROM search_budgets"
           " WHERE research_question=?1"};
    if (!s.ok()) return fail(sqlite_error(db_, "prepare get_budget"));
    s.bind(1, question);
    const int rc = s.step();
    if (rc == SQLITE_DONE) return std::optional<SearchBudget>{};
    if (rc != SQLITE_ROW) return fail(sqlite_error(db_, "get_budget"));

    SearchBudget b;
    b.research_question = s.text(0);
    b.declared_utc = s.text(1);
    b.budget = s.i64(2);
    b.rationale = s.text(3);
    auto used = trial_count(b.research_question);
    if (!used) return fail(used.error());
    b.used = *used;
    return std::optional<SearchBudget>{std::move(b)};
}

Result<bool> Registry::budget_exceeded(std::string_view question) {
    auto b = get_budget(question);
    if (!b) return fail(b.error());
    // No declared budget means no discipline to violate -- but also nothing to
    // report. Callers treat "no budget" as its own reportable state.
    if (!b->has_value()) return false;
    return (*b)->used > (*b)->budget;
}

Result<bool> Registry::record_metric(std::string_view run_id, std::int64_t trial_id,
                                     std::string_view name, double value) {
    Stmt s{db_, "INSERT INTO metrics (run_id, trial_id, name, value) VALUES (?1,?2,?3,?4)"};
    if (!s.ok()) return fail(sqlite_error(db_, "prepare record_metric"));
    s.bind(1, run_id);
    s.bind(2, trial_id);
    s.bind(3, name);
    s.bind(4, value);
    if (s.step() != SQLITE_DONE) return fail(sqlite_error(db_, "record_metric"));
    return true;
}

Result<bool> Registry::record_holdout_unlock(std::string_view run_id,
                                             std::string_view justification) {
    if (justification.empty()) {
        return fail(
            make_error(ErrorCode::InvalidArgument, "holdout unlock requires a justification"));
    }
    Stmt s{db_,
           "INSERT INTO holdout_unlocks (unlocked_utc, run_id, justification)"
           " VALUES (?1,?2,?3)"};
    if (!s.ok()) return fail(sqlite_error(db_, "prepare record_holdout_unlock"));
    s.bind(1, now_utc());
    s.bind(2, run_id);
    s.bind(3, justification);
    if (s.step() != SQLITE_DONE) return fail(sqlite_error(db_, "record_holdout_unlock"));
    return true;
}

Result<std::int64_t> Registry::holdout_unlock_count() {
    Stmt s{db_, "SELECT COUNT(*) FROM holdout_unlocks"};
    if (!s.ok()) return fail(sqlite_error(db_, "prepare holdout_unlock_count"));
    if (s.step() != SQLITE_ROW) return fail(sqlite_error(db_, "holdout_unlock_count"));
    return s.i64(0);
}

}  // namespace ptl::experiments
