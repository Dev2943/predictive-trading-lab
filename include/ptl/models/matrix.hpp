#pragma once

/// \file matrix.hpp
/// A thin design-matrix abstraction over Eigen.
///
/// Eigen appears in exactly one place in the project: the private
/// implementation of this module. Nothing in ptl::features, ptl::validation or
/// ptl::research includes an Eigen header, so the linear-algebra backend stays
/// replaceable and compile times elsewhere are unaffected.
///
/// The abstraction is deliberately small -- it exists to move data across the
/// boundary, not to reimplement a matrix library.

#include <cstddef>
#include <span>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"
#include "ptl/features/matrix.hpp"

namespace ptl::models {

/// A dense, row-major design matrix.
///
/// Row-major here even though FeatureMatrix is column-major: a solver consumes
/// observations, and the conversion happens once at the boundary rather than
/// being paid on every access.
class DesignMatrix {
public:
    DesignMatrix() = default;
    DesignMatrix(std::size_t rows, std::size_t cols)
        : rows_(rows), cols_(cols), data_(rows * cols, 0.0) {}

    /// Extract the given rows of a FeatureMatrix.
    ///
    /// The row selection IS the fold. A model never sees the matrix, only the
    /// indices a fold handed it, so a training call cannot reach a test row
    /// even by mistake.
    [[nodiscard]] static Result<DesignMatrix> from_features(const features::FeatureMatrix& source,
                                                            std::span<const std::size_t> rows);

    [[nodiscard]] std::size_t rows() const noexcept { return rows_; }
    [[nodiscard]] std::size_t cols() const noexcept { return cols_; }
    [[nodiscard]] bool empty() const noexcept { return rows_ == 0 || cols_ == 0; }

    [[nodiscard]] double at(std::size_t r, std::size_t c) const noexcept {
        return data_[r * cols_ + c];
    }
    void set(std::size_t r, std::size_t c, double v) noexcept { data_[r * cols_ + c] = v; }

    [[nodiscard]] std::span<const double> row(std::size_t r) const noexcept {
        return {data_.data() + r * cols_, cols_};
    }
    [[nodiscard]] std::span<const double> raw() const noexcept { return data_; }
    [[nodiscard]] std::span<double> raw_mutable() noexcept { return data_; }

    /// Every value finite. Checked before any solve: a single NaN turns an
    /// entire coefficient vector into NaNs, and the failure would surface as an
    /// unexplained model rather than a bad row.
    [[nodiscard]] bool all_finite() const noexcept;

private:
    std::size_t rows_ = 0;
    std::size_t cols_ = 0;
    std::vector<double> data_;
};

/// Observations, targets and weights for one fit.
struct TrainingData {
    DesignMatrix features;
    std::vector<double> targets;
    /// Uniform when empty. Overlapping labels arrive already down-weighted
    /// from the label builder.
    std::vector<double> weights;

    [[nodiscard]] Result<bool> validate() const;
    [[nodiscard]] std::size_t size() const noexcept { return targets.size(); }
};

/// Assemble a TrainingData from a feature matrix, a label series and a row set.
///
/// \param rows indices into BOTH the feature matrix and the label series. They
///        must already be aligned -- ResearchValidator::check_alignment is what
///        proves that, and a one-row offset is pure lookahead.
[[nodiscard]] Result<TrainingData> make_training_data(const features::FeatureMatrix& features,
                                                      std::span<const double> targets,
                                                      std::span<const std::size_t> rows,
                                                      std::span<const double> weights = {});

}  // namespace ptl::models
