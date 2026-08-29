#include "ptl/validation/walk_forward.hpp"

#include <algorithm>
#include <set>
#include <sstream>

namespace ptl::validation {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

}  // namespace

std::string_view to_string(WindowMode m) noexcept {
    return m == WindowMode::Rolling ? "rolling" : "expanding";
}

std::string_view to_string(SetKind k) noexcept {
    switch (k) {
        case SetKind::Train:
            return "train";
        case SetKind::Validation:
            return "validation";
        case SetKind::Test:
            return "test";
        case SetKind::Purged:
            return "purged";
        case SetKind::Embargoed:
            return "embargoed";
        case SetKind::Unused:
            return "unused";
    }
    return "unknown";
}

bool Fold::disjoint() const {
    std::set<std::size_t> seen;
    for (const auto* v : {&train_rows, &validation_rows, &test_rows}) {
        for (const auto i : *v) {
            if (!seen.insert(i).second) return false;
        }
    }
    return true;
}

std::string Fold::describe() const {
    std::ostringstream ss;
    ss << "fold " << fold_id << " [" << to_string(mode) << "] train=" << train_rows.size()
       << " val=" << validation_rows.size() << " test=" << test_rows.size()
       << " purged=" << purged_rows.size() << " embargoed=" << embargoed_rows.size();
    if (is_set(train_begin)) {
        ss << "\n  train " << to_iso8601(train_begin) << " .. " << to_iso8601(train_end);
        ss << "\n  val   " << to_iso8601(validation_begin) << " .. " << to_iso8601(validation_end);
        ss << "\n  test  " << to_iso8601(test_begin) << " .. " << to_iso8601(test_end);
    }
    return ss.str();
}

std::string WalkForwardConfig::signature() const {
    std::ostringstream ss;
    ss << to_string(mode) << "|train=" << train_size << "|val=" << validation_size
       << "|test=" << test_size << "|step=" << step << "|warmup=" << warmup
       << "|embargo=" << embargo << "|min_train=" << min_train_rows << "|purge=" << (purge ? 1 : 0);
    return ss.str();
}

std::uint64_t WalkForwardConfig::hash() const {
    return fnv1a64(signature());
}

Result<std::vector<Fold>> WalkForwardValidator::split(
    std::span<const ObservationInterval> intervals) const {
    if (intervals.empty()) return std::vector<Fold>{};
    if (cfg_.train_size == 0 || cfg_.test_size == 0 || cfg_.step == 0) {
        return fail(bad("walk-forward window sizes must be non-zero"));
    }

    for (std::size_t i = 1; i < intervals.size(); ++i) {
        if (intervals[i].feature_end_time < intervals[i - 1].feature_end_time) {
            // NEVER SHUFFLE. An unordered input would make a "training" window
            // contain observations from the future.
            return fail(
                bad("observations are not in chronological order at index " + std::to_string(i)));
        }
    }

    std::vector<Fold> folds;
    const std::size_t n = intervals.size();
    int fold_id = 0;

    std::size_t train_end_idx = cfg_.warmup + cfg_.train_size;

    while (true) {
        const std::size_t val_begin = train_end_idx;
        const std::size_t val_end = val_begin + cfg_.validation_size;
        const std::size_t test_begin = val_end;
        const std::size_t test_end = test_begin + cfg_.test_size;
        if (test_end > n) break;

        Fold f;
        f.fold_id = fold_id++;
        f.mode = cfg_.mode;

        // Rolling trains on a fixed trailing window; expanding starts at the
        // warmup point. Both are reported, per the research: choosing by which
        // backtests better is itself selection bias.
        const std::size_t train_begin_idx =
            cfg_.mode == WindowMode::Rolling
                ? (train_end_idx >= cfg_.train_size ? train_end_idx - cfg_.train_size : 0)
                : cfg_.warmup;

        const Timestamp test_window_begin = intervals[test_begin].feature_end_time;
        const Timestamp test_window_end = intervals[test_end - 1].label_end_time;
        const Timestamp val_window_begin = intervals[val_begin].feature_end_time;
        const Timestamp val_window_end = intervals[val_end - 1].label_end_time;

        // --- training set, with purging -------------------------------------
        for (std::size_t i = train_begin_idx; i < train_end_idx; ++i) {
            if (cfg_.purge) {
                // INTERVAL OVERLAP, not endpoint comparison. With a horizon
                // longer than the step, a training row whose feature_end_time
                // sits comfortably before the test window can still have a
                // LABEL that reaches into it -- and that label encodes test
                // period outcomes.
                const bool leaks_into_val =
                    intervals[i].label_overlaps(val_window_begin, val_window_end);
                const bool leaks_into_test =
                    intervals[i].label_overlaps(test_window_begin, test_window_end);
                if (leaks_into_val || leaks_into_test) {
                    f.purged_rows.push_back(i);
                    continue;
                }
            }
            f.train_rows.push_back(i);
        }

        for (std::size_t i = val_begin; i < val_end; ++i) f.validation_rows.push_back(i);
        for (std::size_t i = test_begin; i < test_end; ++i) f.test_rows.push_back(i);

        // --- embargo ---------------------------------------------------------
        // Observations immediately after the test window are dropped from the
        // NEXT fold's training set. Features are autocorrelated across the
        // boundary, so a row starting moments after the test ends still shares
        // information with it.
        for (std::size_t i = test_end; i < std::min(n, test_end + cfg_.embargo); ++i) {
            f.embargoed_rows.push_back(i);
        }

        if (f.train_rows.size() < cfg_.min_train_rows) {
            // A fold with too little training data still produces a model, and
            // that model still produces a number. Skipping is safer than
            // emitting a result nothing downstream would flag.
            train_end_idx += cfg_.step;
            continue;
        }

        f.train_begin = intervals[train_begin_idx].feature_end_time;
        f.train_end = intervals[train_end_idx - 1].feature_end_time;
        f.validation_begin = val_window_begin;
        f.validation_end = intervals[val_end - 1].feature_end_time;
        f.test_begin = test_window_begin;
        f.test_end = intervals[test_end - 1].feature_end_time;

        folds.push_back(std::move(f));
        train_end_idx += cfg_.step;
    }

    return folds;
}

Result<std::pair<std::vector<Fold>, std::vector<Fold>>> WalkForwardValidator::split_both_modes(
    std::span<const ObservationInterval> intervals) const {
    WalkForwardConfig rolling = cfg_;
    rolling.mode = WindowMode::Rolling;
    WalkForwardConfig expanding = cfg_;
    expanding.mode = WindowMode::Expanding;

    auto r = WalkForwardValidator{rolling}.split(intervals);
    if (!r) return fail(r.error());
    auto e = WalkForwardValidator{expanding}.split(intervals);
    if (!e) return fail(e.error());
    return std::make_pair(std::move(*r), std::move(*e));
}

// ---------------------------------------------------------------------------
// HoldoutGuard
// ---------------------------------------------------------------------------

Result<bool> HoldoutGuard::unlock(std::string justification) {
    if (justification.empty()) {
        // An unlock nobody had to explain is an unlock nobody will remember.
        return fail(bad("unlocking the holdout requires a written justification"));
    }
    unlocked_ = true;
    justification_ = std::move(justification);
    return true;
}

Result<bool> HoldoutGuard::check(Timestamp ts) const {
    if (!is_holdout(ts) || unlocked_) return true;
    return fail(
        make_error(ErrorCode::ValidationFailed,
                   "access to the locked holdout period was refused. The boundary is " +
                       to_iso8601(boundary_) +
                       "; unlock it deliberately, with a justification, only after the research "
                       "design is frozen.",
                   to_iso8601(ts)));
}

HoldoutGuard::FilterResult HoldoutGuard::filter(
    std::span<const ObservationInterval> intervals) const {
    FilterResult out;
    out.allowed_rows.reserve(intervals.size());
    for (std::size_t i = 0; i < intervals.size(); ++i) {
        // The LABEL END is checked, not the decision time. A label whose
        // horizon extends past the boundary reads holdout prices even though
        // the decision itself was made before it.
        const bool touches_holdout =
            is_holdout(intervals[i].feature_end_time) || is_holdout(intervals[i].label_end_time);
        if (touches_holdout && !unlocked_) {
            ++out.withheld;
            continue;
        }
        out.allowed_rows.push_back(i);
    }
    return out;
}

Result<bool> HoldoutGuard::check_fold(const Fold& fold,
                                      std::span<const ObservationInterval> intervals) const {
    if (unlocked_ || !is_set(boundary_)) return true;

    const auto scan = [&](const std::vector<std::size_t>& rows,
                          std::string_view set_name) -> Result<bool> {
        for (const auto i : rows) {
            if (i >= intervals.size()) continue;
            if (is_holdout(intervals[i].feature_end_time) ||
                is_holdout(intervals[i].label_end_time)) {
                return fail(bad("fold " + std::to_string(fold.fold_id) + " " +
                                    std::string{set_name} + " set reaches into the locked holdout",
                                to_iso8601(intervals[i].label_end_time)));
            }
        }
        return true;
    };

    if (auto r = scan(fold.train_rows, "train"); !r) return r;
    if (auto r = scan(fold.validation_rows, "validation"); !r) return r;
    if (auto r = scan(fold.test_rows, "test"); !r) return r;
    return true;
}

}  // namespace ptl::validation
