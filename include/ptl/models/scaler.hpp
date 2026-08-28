#pragma once

/// \file scaler.hpp
/// Feature standardisation, fitted on training rows only.
///
/// THE LEAK THIS PREVENTS. Fitting a scaler on the whole dataset lets every
/// training row see the mean and variance of the test period. It is one of the
/// most common leaks in published backtests precisely because it looks
/// harmless: the scaler is "just preprocessing".
///
/// The interface makes it hard to get wrong. fit() takes an explicit row set,
/// and transform() cannot refit -- a scaler that has been fitted is const from
/// then on.

#include <cstddef>
#include <cstdint>
#include <istream>
#include <ostream>
#include <span>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/models/matrix.hpp"

namespace ptl::models {

class StandardScaler {
public:
    /// Fit on the supplied rows and nothing else.
    ///
    /// Clipping quantiles are fitted here too. Winsorising with thresholds
    /// computed on the full sample is the same leak wearing a different hat,
    /// and it is easy to overlook because the scaler gets all the attention.
    [[nodiscard]] Result<bool> fit(const DesignMatrix& training_features, double clip_sigma = 0.0);

    /// Standardise in place. Const: a fitted scaler cannot be refitted by a
    /// later call, so validation and test necessarily receive the training
    /// statistics.
    [[nodiscard]] Result<bool> transform(DesignMatrix& features) const;

    /// Standardise one observation, for single-row inference.
    [[nodiscard]] Result<bool> transform_row(std::span<double> row) const;

    [[nodiscard]] bool fitted() const noexcept { return fitted_; }
    [[nodiscard]] std::size_t cols() const noexcept { return means_.size(); }
    [[nodiscard]] std::span<const double> means() const noexcept { return means_; }
    [[nodiscard]] std::span<const double> stdevs() const noexcept { return stdevs_; }
    [[nodiscard]] double clip_sigma() const noexcept { return clip_sigma_; }

    /// Serialised WITH the model artifact. A model reloaded without its scaler
    /// would receive unstandardised inputs and produce confident nonsense.
    void write(std::ostream&) const;
    [[nodiscard]] static Result<StandardScaler> read(std::istream&);

    [[nodiscard]] std::uint64_t content_hash() const noexcept;

    void reset() noexcept;

private:
    bool fitted_ = false;
    double clip_sigma_ = 0.0;
    std::vector<double> means_;
    std::vector<double> stdevs_;
};

}  // namespace ptl::models
