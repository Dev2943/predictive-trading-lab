#include "ptl/features/validation.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace ptl::features {

std::string_view to_string(FeatureIssueCode c) noexcept {
    switch (c) {
        case FeatureIssueCode::LookaheadDetected:
            return "lookahead_detected";
        case FeatureIssueCode::NonMonotonicRows:
            return "non_monotonic_rows";
        case FeatureIssueCode::NaNValue:
            return "nan_value";
        case FeatureIssueCode::InfiniteValue:
            return "infinite_value";
        case FeatureIssueCode::StaleFeature:
            return "stale_feature";
        case FeatureIssueCode::ConstantFeature:
            return "constant_feature";
        case FeatureIssueCode::MissingData:
            return "missing_data";
        case FeatureIssueCode::ReadyBeforeWarmup:
            return "ready_before_warmup";
    }
    return "unknown";
}

std::string FeatureIssue::describe() const {
    std::string out{to_string(code)};
    out += " [row " + std::to_string(row);
    if (!feature_name.empty()) out += ", feature '" + feature_name + "'";
    out += "]";
    if (!detail.empty()) out += ": " + detail;
    return out;
}

std::size_t FeatureValidationReport::count(FeatureIssueCode c) const noexcept {
    return static_cast<std::size_t>(std::count_if(
        issues.begin(), issues.end(), [c](const FeatureIssue& i) { return i.code == c; }));
}

std::string FeatureValidationReport::summary() const {
    std::ostringstream ss;
    ss << "checked " << rows_checked << " rows (" << ready_rows << " ready), " << issues.size()
       << " issues";
    if (!issues.empty()) {
        ss << '\n';
        // Cap the listing: a systematic fault produces one issue per row, and
        // a million-line report is no more useful than the first few.
        const std::size_t shown = std::min<std::size_t>(issues.size(), 10);
        for (std::size_t i = 0; i < shown; ++i) ss << "  " << issues[i].describe() << '\n';
        if (issues.size() > shown) {
            ss << "  ... and " << (issues.size() - shown) << " more\n";
        }
    }
    return ss.str();
}

FeatureValidationReport FeatureValidator::validate(const FeatureMatrix& m,
                                                   std::uint64_t required_mask) const {
    FeatureValidationReport report;
    report.rows_checked = m.rows();
    report.ready_rows = m.ready_rows(required_mask).size();

    const auto keys = m.keys();
    for (std::size_t i = 1; i < keys.size(); ++i) {
        if (keys[i].feature_end_time < keys[i - 1].feature_end_time) {
            report.issues.push_back({FeatureIssueCode::NonMonotonicRows,
                                     i,
                                     0,
                                     {},
                                     "row time precedes the previous row"});
        }
    }

    for (std::size_t j = 0; j < m.cols(); ++j) {
        const auto col = m.column(j);
        const std::string name{m.names()[j]};

        std::size_t identical_run = 0;
        double previous = 0.0;
        bool have_previous = false;
        double min_v = 0.0;
        double max_v = 0.0;
        bool first = true;
        bool reported_stale = false;

        for (std::size_t i = 0; i < col.size(); ++i) {
            const double v = col[i];

            if (std::isnan(v)) {
                report.issues.push_back({FeatureIssueCode::NaNValue, i, j, name,
                                         "NaN would propagate into every model fit"});
                continue;
            }
            if (std::isinf(v)) {
                report.issues.push_back({FeatureIssueCode::InfiniteValue, i, j, name,
                                         "infinite value would poison downstream aggregates"});
                continue;
            }

            if (first) {
                min_v = v;
                max_v = v;
                first = false;
            } else {
                min_v = std::min(min_v, v);
                max_v = std::max(max_v, v);
            }

            if (cfg_.detect_stale) {
                if (have_previous && v == previous) {
                    ++identical_run;
                    if (identical_run == cfg_.max_identical_run && !reported_stale) {
                        // Not necessarily wrong -- a feature can plateau -- but a
                        // long identical run is the signature of an estimator
                        // that stopped receiving updates.
                        report.issues.push_back(
                            {FeatureIssueCode::StaleFeature, i, j, name,
                             std::to_string(identical_run) +
                                 " consecutive identical values: the estimator may have "
                                 "stopped updating"});
                        reported_stale = true;
                    }
                } else {
                    identical_run = 0;
                }
            }
            previous = v;
            have_previous = true;
        }

        if (cfg_.detect_constant && !first && col.size() > 1 && min_v == max_v) {
            // A feature that never moves cannot carry information. Almost
            // always a wiring error rather than a property of the market.
            report.issues.push_back(
                {FeatureIssueCode::ConstantFeature, 0, j, name, "zero variance across the sample"});
        }
    }
    return report;
}

FeatureValidationReport FeatureValidator::check_causality(
    const FeatureMatrix& m, std::span<const Timestamp> decision_times) const {
    FeatureValidationReport report;
    report.rows_checked = m.rows();

    if (decision_times.size() != m.rows()) {
        report.issues.push_back({FeatureIssueCode::MissingData,
                                 0,
                                 0,
                                 {},
                                 "decision-time series length does not match the matrix"});
        return report;
    }

    const auto keys = m.keys();
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (!is_set(keys[i].feature_end_time) || !is_set(decision_times[i])) {
            report.issues.push_back(
                {FeatureIssueCode::MissingData, i, 0, {}, "row or decision timestamp is unset"});
            continue;
        }
        if (keys[i].feature_end_time > decision_times[i]) {
            // THE LOOKAHEAD ASSERTION. A feature computed after the decision it
            // informs means the model saw the future, and every result derived
            // from this matrix is fiction.
            report.issues.push_back({FeatureIssueCode::LookaheadDetected,
                                     i,
                                     0,
                                     {},
                                     "feature_end_time " + to_iso8601(keys[i].feature_end_time) +
                                         " is after decision_time " +
                                         to_iso8601(decision_times[i])});
        }
    }
    return report;
}

}  // namespace ptl::features
