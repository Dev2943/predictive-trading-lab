#pragma once

/// \file matrix.hpp
/// Feature storage, serialisation and caching.
///
/// COLUMN-MAJOR. A model standardises one feature at a time across all rows,
/// and a fold selects rows but reads every column; column-major makes the first
/// access contiguous. It is also what a linear-algebra library expects, so the
/// matrix can be handed to a solver in Phase 6 without a transpose.
///
/// The cache key is hash(data_version, feature_set_id). Reusing a cached matrix
/// after a feature definition changed would silently train a model on values
/// that no longer match their names, so the key includes the definition, not
/// just the data.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"
#include "ptl/features/feature.hpp"

namespace ptl::features {

/// Row-identifying metadata, stored separately from the values so a fold can
/// select rows without touching the value columns.
struct RowKey {
    Timestamp feature_end_time{kNoTimestamp};
    InstrumentId instrument{kInvalidInstrument};
    std::uint64_t ready_mask = 0;
};

class FeatureMatrix {
public:
    FeatureMatrix() = default;
    FeatureMatrix(std::vector<std::string> names, std::uint64_t data_version,
                  std::uint64_t feature_set_id);

    /// Append one observation. Rejects a row whose width or lineage disagrees
    /// with the matrix, because a silently truncated row would shift every
    /// later column by one.
    [[nodiscard]] Result<bool> append(const FeatureRow& row);

    [[nodiscard]] std::size_t rows() const noexcept { return keys_.size(); }
    [[nodiscard]] std::size_t cols() const noexcept { return names_.size(); }
    [[nodiscard]] std::span<const std::string> names() const noexcept { return names_; }
    [[nodiscard]] std::span<const RowKey> keys() const noexcept { return keys_; }
    [[nodiscard]] std::uint64_t data_version() const noexcept { return data_version_; }
    [[nodiscard]] std::uint64_t feature_set_id() const noexcept { return feature_set_id_; }

    /// Contiguous view of one feature across all rows.
    [[nodiscard]] std::span<const double> column(std::size_t j) const noexcept;
    [[nodiscard]] double at(std::size_t row, std::size_t col) const noexcept;

    /// Row indices whose ready_mask satisfies `required`. This is the warmup
    /// gate applied at selection time: an unready row is never handed to a
    /// model rather than being handed over and hopefully ignored.
    [[nodiscard]] std::vector<std::size_t> ready_rows(std::uint64_t required) const;

    /// Column index by name, or npos.
    [[nodiscard]] std::size_t index_of_name(std::string_view name) const noexcept;

    void reserve(std::size_t rows);

    /// Binary serialisation. Little-endian, versioned, with a magic number and
    /// a content hash so a truncated or corrupt file is detected on load rather
    /// than producing plausible garbage.
    [[nodiscard]] Result<bool> save(const std::filesystem::path&) const;
    [[nodiscard]] static Result<FeatureMatrix> load(const std::filesystem::path&);

    /// Content hash over names, keys and values. Two matrices with the same
    /// hash are byte-identical, which is what the determinism test compares.
    [[nodiscard]] std::uint64_t content_hash() const;

    /// Cache path for this (data_version, feature_set_id) pair.
    [[nodiscard]] static std::filesystem::path cache_path(const std::filesystem::path& dir,
                                                          std::uint64_t data_version,
                                                          std::uint64_t feature_set_id);

private:
    std::vector<std::string> names_;
    std::vector<RowKey> keys_;
    /// Column-major: columns_[j] holds every row's value for feature j.
    std::vector<std::vector<double>> columns_;
    std::uint64_t data_version_ = 0;
    std::uint64_t feature_set_id_ = 0;
};

}  // namespace ptl::features
