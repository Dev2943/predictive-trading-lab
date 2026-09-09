/// \file ptl_bindings.cpp
/// Python bindings for the Predictive Trading Lab engine.
///
/// THIS IS A CONSUMER OF THE ENGINE, NEVER A PART OF IT. It includes public
/// headers and links the public targets exactly as any downstream project
/// would. Nothing under src/, include/, tests/, apps/ or benchmarks/ changes to
/// accommodate it, and deleting this directory would leave the engine
/// untouched.
///
/// THE BOUNDARY IS JSON, NOT OBJECTS.
///
/// Every non-trivial result crosses as a JSON string produced by the engine's
/// own `to_json`. That looks like a wasted serialization and is the most
/// important decision here:
///
///   - The gateway never holds a pointer into engine memory, so it cannot
///     mutate trading state or outlive an object it borrowed.
///   - The wire format is identical whether the engine is in-process or on
///     another machine. Moving it remote later is a transport swap, not an API
///     rewrite.
///   - The engine already emits ordered, deterministic JSON. Re-deriving that
///     shape here would be a second serializer that could disagree with the
///     first.
///
/// NOT BOUND, DELIBERATELY: Engine, PaperSession, LiveSession. Handing a
/// request handler a pointer to a live trading object is the failure this
/// design exists to prevent. Those remain internal, reachable only through the
/// single-writer command queue introduced in a later phase.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "ptl/analytics/rolling.hpp"
#include "ptl/attribution/pnl.hpp"
#include "ptl/config/config.hpp"
#include "ptl/core/clock.hpp"
#include "ptl/core/rng.hpp"
#include "ptl/core/types.hpp"
#include "ptl/core/version.hpp"
#include "ptl/optimization/optimizer.hpp"
#include "ptl/ops/diagnostics.hpp"

namespace py = pybind11;

namespace {

/// Translates a failed `Result<T>` into a Python exception.
///
/// The engine returns errors as values; Python expects them raised. Converting
/// once here means no caller has to remember, and an ignored error becomes
/// impossible rather than merely discouraged.
template <typename T>
[[nodiscard]] T unwrap(ptl::Result<T> result, const char* what) {
    if (!result) {
        std::string message = std::string{what} + ": " + result.error().message;
        if (!result.error().context.empty()) {
            message += " (" + result.error().context + ")";
        }
        throw std::runtime_error(message);
    }
    return std::move(*result);
}

// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------

/// The RNG fingerprint, computed through the bindings.
///
/// Exists so a test can prove the binding layer did not perturb determinism. If
/// this disagrees with `ptl_version`, the boundary is not transparent and
/// nothing above it can be trusted.
[[nodiscard]] std::vector<std::string> rng_fingerprint(std::uint64_t seed,
                                                       std::size_t count) {
    ptl::DeterministicRng    rng{seed};
    std::vector<std::string> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        std::ostringstream ss;
        ss << std::hex << std::setw(16) << std::setfill('0') << rng.next_u64();
        out.push_back(ss.str());
    }
    return out;
}

[[nodiscard]] std::string config_hash(const std::string& path) {
    auto               config = unwrap(ptl::config::load(path), "config load");
    std::ostringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << config.hash();
    return ss.str();
}

// ---------------------------------------------------------------------------
// Optimization
// ---------------------------------------------------------------------------

[[nodiscard]] ptl::optimization::OptimizationInput make_input(
    const std::vector<double>& expected_returns,
    const std::vector<double>& volatilities,
    const std::vector<std::vector<double>>& covariance,
    const std::vector<double>& signals) {
    ptl::optimization::OptimizationInput input;

    const std::size_t n =
        expected_returns.empty() ? volatilities.size() : expected_returns.size();
    for (std::size_t i = 0; i < n; ++i) {
        input.instruments.push_back(static_cast<ptl::InstrumentId>(i));
    }
    input.expected_returns = expected_returns;
    input.volatilities = volatilities;
    input.signals = signals;

    if (!covariance.empty()) {
        ptl::optimization::SymmetricMatrix matrix{covariance.size()};
        for (std::size_t i = 0; i < covariance.size(); ++i) {
            if (covariance[i].size() != covariance.size()) {
                throw std::runtime_error("covariance must be square");
            }
            for (std::size_t j = 0; j < covariance.size(); ++j) {
                matrix.at(i, j) = covariance[i][j];
            }
        }
        input.covariance = matrix;
    }
    return input;
}

[[nodiscard]] ptl::optimization::OptimizerConfig make_config(double max_position,
                                                             bool   long_only,
                                                             double max_gross_leverage,
                                                             double risk_aversion,
                                                             double target_volatility) {
    ptl::optimization::OptimizerConfig config;
    config.constraints = long_only
                             ? ptl::optimization::ConstraintSet::long_only(max_position)
                             : ptl::optimization::ConstraintSet{};
    if (!long_only) {
        config.constraints.max_position = max_position;
        config.constraints.min_position = -max_position;
    }
    config.constraints.max_gross_leverage = max_gross_leverage;
    config.objective.risk_aversion = risk_aversion;
    config.target_volatility = target_volatility;
    return config;
}

/// Serialize an optimization result.
///
/// Hand-written because `OptimizationResult` predates the engine's JSON
/// convention and has no `to_json`. The shape deliberately mirrors the engine's
/// other serializers so the gateway sees one style. Standardising this belongs
/// in a future engine minor release, not here -- v1.0 is frozen.
[[nodiscard]] std::string result_to_json(
    const ptl::optimization::OptimizationResult& result,
    const std::vector<std::string>&              symbols) {
    std::ostringstream ss;
    ss.precision(12);
    ss << std::fixed;
    ss << "{\"status\": \"" << ptl::optimization::to_string(result.status)
       << "\", \"weights\": [";
    for (std::size_t i = 0; i < result.weights.size(); ++i) {
        if (i != 0) ss << ", ";
        ss << result.weights[i];
    }
    ss << "], \"symbols\": [";
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        if (i != 0) ss << ", ";
        ss << '"' << symbols[i] << '"';
    }
    ss << "], \"expected_return\": " << result.expected_return
       << ", \"expected_volatility\": " << result.expected_volatility
       << ", \"sharpe\": " << result.sharpe
       << ", \"gross_exposure\": " << result.gross_exposure
       << ", \"net_exposure\": " << result.net_exposure
       << ", \"cash_weight\": " << result.cash_weight
       << ", \"turnover\": " << result.turnover
       << ", \"iterations\": " << result.iterations << ", \"binding_constraints\": [";
    for (std::size_t i = 0; i < result.binding_constraints.size(); ++i) {
        if (i != 0) ss << ", ";
        ss << '"' << result.binding_constraints[i] << '"';
    }
    ss << "], \"detail\": \"" << result.detail << "\"}";
    return ss.str();
}

[[nodiscard]] std::string optimize(const std::string&         optimizer_name,
                                   const std::vector<double>& expected_returns,
                                   const std::vector<double>& volatilities,
                                   const std::vector<std::vector<double>>& covariance,
                                   const std::vector<double>&             signals,
                                   const std::vector<std::string>&        symbols,
                                   double max_position, bool long_only,
                                   double max_gross_leverage, double risk_aversion,
                                   double target_volatility) {
    const auto input  = make_input(expected_returns, volatilities, covariance, signals);
    const auto config = make_config(max_position, long_only, max_gross_leverage,
                                    risk_aversion, target_volatility);

    auto registry = unwrap(ptl::optimization::OptimizerRegistry::with_defaults(config),
                           "optimizer registry");
    auto optimizer = unwrap(registry.create(optimizer_name), "unknown optimizer");
    auto result    = unwrap(optimizer->optimize(input), "optimization");
    return result_to_json(result, symbols);
}

[[nodiscard]] std::vector<std::string> optimizer_names() {
    auto registry = unwrap(ptl::optimization::OptimizerRegistry::with_defaults(),
                           "optimizer registry");
    std::vector<std::string> out;
    for (const auto& name : registry.names()) out.emplace_back(name);
    return out;
}

// ---------------------------------------------------------------------------
// Covariance
// ---------------------------------------------------------------------------

[[nodiscard]] py::dict estimate_covariance(
    const std::vector<std::vector<double>>& observations, const std::string& method,
    std::size_t window, double shrinkage, double min_observations_ratio) {
    if (observations.empty()) throw std::runtime_error("no observations supplied");
    const std::size_t assets = observations.front().size();

    std::vector<double> flat;
    flat.reserve(observations.size() * assets);
    for (const auto& row : observations) {
        if (row.size() != assets) {
            throw std::runtime_error("observation rows must be equal length");
        }
        flat.insert(flat.end(), row.begin(), row.end());
    }

    ptl::optimization::CovarianceConfig config;
    if (method == "sample") {
        config.method = ptl::optimization::CovarianceMethod::Sample;
    } else if (method == "rolling") {
        config.method = ptl::optimization::CovarianceMethod::Rolling;
    } else if (method == "ewma") {
        config.method = ptl::optimization::CovarianceMethod::Ewma;
    } else if (method == "shrinkage") {
        config.method = ptl::optimization::CovarianceMethod::Shrinkage;
    } else if (method == "identity") {
        config.method = ptl::optimization::CovarianceMethod::Identity;
    } else {
        throw std::runtime_error("unknown covariance method: " + method);
    }

    config.window              = window;
    config.shrinkage_intensity = shrinkage;
    // EXPOSED, not hardcoded. An earlier draft pinned this to 0.0 to make the
    // estimator permissive, which silently disabled the engine's guard against
    // estimating a 50x50 covariance from 20 rows. A binding that turns that off
    // has weakened a guarantee its caller believes it still has.
    config.min_observations_ratio = min_observations_ratio;

    ptl::optimization::CovarianceEstimator estimator{config};
    auto matrix = unwrap(estimator.estimate(flat, observations.size(), assets),
                         "covariance estimation");

    std::vector<std::vector<double>> rows(assets, std::vector<double>(assets, 0.0));
    for (std::size_t i = 0; i < assets; ++i) {
        for (std::size_t j = 0; j < assets; ++j) rows[i][j] = matrix.at(i, j);
    }

    auto correlation = unwrap(
        ptl::optimization::CovarianceEstimator::to_correlation(matrix), "correlation");
    std::vector<std::vector<double>> corr(assets, std::vector<double>(assets, 0.0));
    for (std::size_t i = 0; i < assets; ++i) {
        for (std::size_t j = 0; j < assets; ++j) corr[i][j] = correlation.at(i, j);
    }

    py::dict out;
    out["covariance"]  = rows;
    out["correlation"] = corr;
    // The DIAGNOSTICS travel with the matrix, so a caller cannot use a degraded
    // estimate without being told it was degraded.
    out["degraded"]           = estimator.diagnostics().degraded;
    out["degradation_reason"] = estimator.diagnostics().degradation_reason;
    out["applied_shrinkage"]  = estimator.diagnostics().applied_shrinkage;
    out["psd_repaired"]       = estimator.diagnostics().psd_repaired;
    out["observations"]       = estimator.diagnostics().observations;
    return out;
}

// ---------------------------------------------------------------------------
// Rolling analytics
// ---------------------------------------------------------------------------

[[nodiscard]] py::dict rolling_metrics(const std::vector<double>& returns,
                                       std::size_t window, double periods_per_year,
                                       double var_confidence) {
    if (returns.empty()) throw std::runtime_error("no returns supplied");

    // Synthetic daily timestamps. The rolling analyzer requires a monotone
    // series; the gateway supplies returns without dates, so they are generated
    // here rather than demanding the caller invent them.
    ptl::Timestamp t{};
    (void)ptl::parse_timestamp("2020-01-01T00:00:00Z", t);
    std::vector<ptl::Timestamp> timestamps;
    timestamps.reserve(returns.size());
    for (std::size_t i = 0; i < returns.size(); ++i) {
        timestamps.push_back(t);
        t += std::chrono::hours{24};
    }

    ptl::analytics::RollingConfig config;
    config.window                          = window;
    config.periods_per_year                = periods_per_year;
    config.var_confidence                  = var_confidence;
    const ptl::analytics::RollingAnalyzer analyzer{config};

    const auto to_list = [](const ptl::analytics::RollingSeries& series) {
        std::vector<py::object> out;
        out.reserve(series.values.size());
        for (const auto& value : series.values) {
            // ABSENT, not zero. A rolling window that is not yet full has no
            // value, and emitting zero would draw a line through the origin
            // that no data supports.
            out.push_back(value.has_value() ? py::cast(*value) : py::none());
        }
        return out;
    };

    py::dict out;
    out["volatility"] =
        to_list(unwrap(analyzer.volatility(timestamps, returns), "rolling volatility"));
    out["sharpe"] =
        to_list(unwrap(analyzer.sharpe(timestamps, returns), "rolling sharpe"));
    out["var"] =
        to_list(unwrap(analyzer.value_at_risk(timestamps, returns), "rolling var"));
    out["cvar"] =
        to_list(unwrap(analyzer.conditional_var(timestamps, returns), "rolling cvar"));
    out["omega"]  = ptl::analytics::RollingAnalyzer::omega_ratio(returns, 0.0);
    out["window"] = window;
    return out;
}

// ---------------------------------------------------------------------------
// Attribution
// ---------------------------------------------------------------------------

[[nodiscard]] py::dict factor_contribution(const std::vector<double>& portfolio,
                                           const std::vector<double>& benchmark) {
    auto contribution = unwrap(
        ptl::attribution::PnlAttributor::factor_contribution(portfolio, benchmark),
        "factor contribution");

    py::dict out;
    out["beta"]                  = contribution.beta;
    out["beta_contribution"]     = contribution.beta_contribution;
    out["alpha_contribution"]    = contribution.alpha_contribution;
    out["residual_contribution"] = contribution.residual_contribution;
    out["portfolio_return"]      = contribution.portfolio_return;
    out["benchmark_return"]      = contribution.benchmark_return;
    out["periods"]               = contribution.periods;
    return out;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

[[nodiscard]] std::string validate_risk_limits(double max_order_notional,
                                               double max_position_notional,
                                               double max_gross_leverage,
                                               double max_concentration,
                                               double max_drawdown_pct,
                                               double max_daily_turnover,
                                               bool   require_live) {
    ptl::risk::RiskLimits limits;
    limits.max_order_notional    = ptl::Notional{max_order_notional};
    limits.max_position_notional = ptl::Notional{max_position_notional};
    limits.max_gross_leverage    = max_gross_leverage;
    limits.max_concentration     = max_concentration;
    limits.max_drawdown_pct      = max_drawdown_pct;
    limits.max_daily_turnover    = max_daily_turnover;

    ptl::ops::ConfigValidator::Options options;
    options.require_risk_limits = require_live;
    const ptl::ops::ConfigValidator validator{options};
    return validator.validate_risk(limits).to_json();
}

}  // namespace

PYBIND11_MODULE(ptl, m) {
    m.doc() =
        "Predictive Trading Lab engine bindings.\n\n"
        "Results cross this boundary as JSON or plain Python data, never as\n"
        "handles into engine memory. That keeps the wire format identical\n"
        "whether the engine is in-process or remote, and prevents a caller\n"
        "holding a pointer to live trading state.";

    m.attr("__version__")    = ptl::kVersion;
    m.attr("engine_version") = ptl::kVersion;
    m.attr("compiler")       = ptl::kCompilerId;
    m.attr("build_type")     = ptl::kBuildType;

    m.def("rng_fingerprint", &rng_fingerprint, py::arg("seed"), py::arg("count") = 3,
          "Deterministic RNG fingerprint. Must match ptl_version exactly; a\n"
          "difference means the binding layer perturbed determinism.");
    m.def("config_hash", &config_hash, py::arg("path"),
          "Hash of a loaded configuration, as hex.");

    m.def("optimizer_names", &optimizer_names, "Registered optimizer names.");
    m.def("optimize", &optimize, py::arg("optimizer"),
          py::arg("expected_returns") = std::vector<double>{},
          py::arg("volatilities") = std::vector<double>{},
          py::arg("covariance") = std::vector<std::vector<double>>{},
          py::arg("signals") = std::vector<double>{},
          py::arg("symbols") = std::vector<std::string>{}, py::arg("max_position") = 0.20,
          py::arg("long_only") = true, py::arg("max_gross_leverage") = 1.0,
          py::arg("risk_aversion") = 1.0, py::arg("target_volatility") = 0.10,
          "Run one optimizer. Returns JSON. Raises on refusal, so an optimizer\n"
          "that cannot answer never returns a plausible-looking empty result.");

    m.def("estimate_covariance", &estimate_covariance, py::arg("observations"),
          py::arg("method") = "shrinkage", py::arg("window") = 0,
          py::arg("shrinkage") = -1.0, py::arg("min_observations_ratio") = 1.5,
          "Estimate covariance and correlation, with diagnostics attached.");

    m.def("rolling_metrics", &rolling_metrics, py::arg("returns"),
          py::arg("window") = 60, py::arg("periods_per_year") = 252.0,
          py::arg("var_confidence") = 0.05,
          "Rolling volatility, Sharpe, VaR and CVaR. Values before the window\n"
          "is full are None, never zero.");

    m.def("factor_contribution", &factor_contribution, py::arg("portfolio"),
          py::arg("benchmark"), "Split returns into beta and alpha contributions.");

    m.def("validate_risk_limits", &validate_risk_limits,
          py::arg("max_order_notional") = 1e6, py::arg("max_position_notional") = 1e6,
          py::arg("max_gross_leverage") = 1.0, py::arg("max_concentration") = 0.10,
          py::arg("max_drawdown_pct") = 0.20, py::arg("max_daily_turnover") = 10.0,
          py::arg("require_live") = false,
          "Semantic validation of risk limits. Returns JSON listing every\n"
          "issue, not just the first.");
}
