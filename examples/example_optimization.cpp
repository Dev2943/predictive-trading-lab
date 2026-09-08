/// \file example_optimization.cpp
/// Covariance estimation and the nine portfolio optimizers.
///
/// Shows the full path from a return matrix to a set of weights, including the
/// PSD repair that keeps a near-singular risk model from producing enormous
/// offsetting positions that look like a hedge and are numerical noise.

#include <cmath>
#include <iomanip>
#include <iostream>
#include <vector>

#include "ptl/optimization/optimizer.hpp"

int main() {
    constexpr std::size_t kAssets = 5;
    constexpr std::size_t kRows = 500;

    // Synthetic returns: a common factor plus idiosyncratic noise, so the
    // assets are genuinely correlated and risk parity differs from inverse
    // volatility.
    std::vector<double> observations;
    observations.reserve(kRows * kAssets);
    for (std::size_t r = 0; r < kRows; ++r) {
        const double factor = std::sin(static_cast<double>(r) * 0.11) * 0.01;
        for (std::size_t c = 0; c < kAssets; ++c) {
            const double idiosyncratic =
                std::sin(static_cast<double>(r) * 0.3 + static_cast<double>(c) * 2.0) * 0.004 *
                (1.0 + static_cast<double>(c) * 0.5);
            observations.push_back(factor + idiosyncratic);
        }
    }

    ptl::optimization::CovarianceConfig covariance_config;
    covariance_config.method = ptl::optimization::CovarianceMethod::Shrinkage;
    covariance_config.min_observations_ratio = 0.0;
    ptl::optimization::CovarianceEstimator estimator{covariance_config};

    auto covariance = estimator.estimate(observations, kRows, kAssets);
    if (!covariance) {
        std::cerr << "covariance: " << covariance.error().message << '\n';
        return 1;
    }
    std::cout << estimator.diagnostics().describe() << "\n\n";

    ptl::optimization::OptimizationInput input;
    (void)ptl::parse_timestamp("2024-07-02T20:00:00Z", input.as_of);
    for (std::size_t i = 0; i < kAssets; ++i) {
        input.instruments.push_back(static_cast<ptl::InstrumentId>(i));
        input.expected_returns.push_back(0.02 + 0.01 * static_cast<double>(i % 3));
        input.volatilities.push_back(std::sqrt(covariance->at(i, i)));
        input.signals.push_back(0.5 - 0.2 * static_cast<double>(i));
    }
    input.covariance = *covariance;

    ptl::optimization::OptimizerConfig config;
    config.constraints = ptl::optimization::ConstraintSet::long_only(0.40);
    config.max_iterations = 2000;

    auto registry = ptl::optimization::OptimizerRegistry::with_defaults(config);
    if (!registry) {
        std::cerr << "registry: " << registry.error().message << '\n';
        return 1;
    }

    std::cout << std::fixed << std::setprecision(4);
    for (const auto& name : registry->names()) {
        auto optimizer = registry->create(name);
        if (!optimizer) continue;

        auto result = (*optimizer)->optimize(input);
        if (!result) {
            // Refusing is a legitimate answer: minimum variance without a
            // covariance cannot be computed, and saying so beats inventing one.
            std::cout << std::setw(20) << name << "  refused: " << result.error().message << '\n';
            continue;
        }

        std::cout << std::setw(20) << name << "  weights [";
        for (std::size_t i = 0; i < result->weights.size(); ++i) {
            if (i != 0) std::cout << ' ';
            std::cout << std::setw(7) << result->weights[i];
        }
        std::cout << " ]  vol " << result->expected_volatility << "  sharpe " << result->sharpe
                  << '\n';
    }
    return 0;
}
