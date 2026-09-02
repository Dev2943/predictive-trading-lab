#pragma once

/// \file registry.hpp
/// Dataset and model registries, plus a filesystem artifact store.
///
/// THE INVARIANT THIS EXISTS TO ENFORCE: no model may train on an unknown
/// dataset version. A model whose training data cannot be identified is not
/// reproducible, and a backtest built on it cannot be defended -- the numbers
/// may be correct and there is no way to demonstrate it.
///
/// `ModelRecord::train` therefore takes a `DatasetVersion` that must already be
/// registered, and refuses otherwise. That refusal is the whole point of the
/// module; everything else is bookkeeping around it.
///
/// NO DATABASE. `ptl::experiments::Registry` already owns SQLite for run and
/// trial provenance. This stores ARTIFACTS -- predictions, metrics, manifests --
/// as JSON files, because artifacts are large, write-once and read whole, which
/// is exactly what a filesystem is good at and a relational store is not.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"

namespace ptl::storage {

/// A content hash. Hex, so it survives a config file and a filename.
class Checksum {
public:
    Checksum() = default;
    [[nodiscard]] static Checksum of(std::string_view content) noexcept;
    [[nodiscard]] static Result<Checksum> parse(std::string_view hex);

    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] bool empty() const noexcept { return value_ == 0; }

    friend bool operator==(const Checksum&, const Checksum&) noexcept = default;
    friend auto operator<=>(const Checksum&, const Checksum&) noexcept = default;

private:
    explicit Checksum(std::uint64_t value) noexcept : value_(value) {}
    std::uint64_t value_ = 0;
};

/// One column of a feature or label schema.
struct SchemaField {
    std::string name;
    std::string type;
    /// Lookback the field requires, for causality auditing. A field claiming a
    /// zero lookback that in fact needs history is how a leak enters a schema.
    std::size_t lookback_periods = 0;
};

/// Schema of a dataset's features or labels.
struct Schema {
    std::vector<SchemaField> fields;

    [[nodiscard]] Checksum checksum() const;
    [[nodiscard]] bool compatible_with(const Schema&) const;
    [[nodiscard]] std::string to_json() const;
};

/// A dataset version. Immutable once registered.
struct DatasetVersion {
    std::string dataset_id;
    /// Monotonic within a dataset id.
    std::uint32_t version = 0;

    Timestamp created_at{kNoTimestamp};
    /// Range the data covers. Recorded so a model trained on 2020-2022 can
    /// never be silently evaluated on 2019 without the mismatch being visible.
    Timestamp range_begin{kNoTimestamp};
    Timestamp range_end{kNoTimestamp};

    Checksum content_checksum;
    Schema feature_schema;
    Schema label_schema;

    /// Version of the normalisation applied. A change here invalidates every
    /// model trained against the previous one, even at identical raw data.
    std::string normalization_version;

    /// Walk-forward split boundaries, so a later evaluation can prove it used
    /// the same folds rather than re-deriving them and hoping they match.
    std::vector<Timestamp> split_boundaries;

    std::size_t row_count = 0;
    std::string source;  ///< "databento:cbbo-1m", "alpaca:bars", etc.

    [[nodiscard]] std::string key() const;
    [[nodiscard]] Checksum fingerprint() const;
    [[nodiscard]] Result<bool> validate() const;
    [[nodiscard]] std::string to_json() const;
};

class DatasetRegistry {
public:
    /// Register a version. Refuses a duplicate (id, version) and refuses a
    /// version number that does not advance -- datasets are append-only, and a
    /// rewritten version silently invalidates every model trained on it.
    [[nodiscard]] Result<bool> register_dataset(DatasetVersion);

    [[nodiscard]] const DatasetVersion* find(std::string_view dataset_id,
                                             std::uint32_t version) const noexcept;
    [[nodiscard]] const DatasetVersion* latest(std::string_view dataset_id) const noexcept;
    [[nodiscard]] std::vector<std::string> dataset_ids() const;
    [[nodiscard]] std::size_t size() const noexcept { return datasets_.size(); }

    /// Whether a dataset version exists exactly as described. Used by the model
    /// registry before accepting a training record.
    [[nodiscard]] bool contains(std::string_view dataset_id, std::uint32_t version) const noexcept;

    [[nodiscard]] std::string to_json() const;

private:
    std::map<std::pair<std::string, std::uint32_t>, DatasetVersion> datasets_;
};

// ---------------------------------------------------------------------------
// Models
// ---------------------------------------------------------------------------

/// Where a model sits in the champion/challenger lifecycle.
enum class ModelStatus : std::uint8_t {
    Training,
    Trained,
    Challenger,  ///< being evaluated against the champion
    Champion,    ///< currently selected
    Archived,    ///< superseded, retained for rollback
    Failed,
};

[[nodiscard]] std::string_view to_string(ModelStatus) noexcept;

/// One training run's outcome.
struct TrainingRecord {
    Timestamp started_at{kNoTimestamp};
    Timestamp finished_at{kNoTimestamp};
    std::uint64_t seed = 0;
    std::size_t training_rows = 0;
    std::size_t validation_rows = 0;
    /// Metrics from the training run, ordered by name.
    std::map<std::string, double, std::less<>> metrics;

    [[nodiscard]] Duration duration() const noexcept;
};

/// A versioned model artifact.
struct ModelMetadata {
    std::string model_id;
    std::uint32_t version = 0;
    ModelStatus status{ModelStatus::Training};

    /// THE LINK THAT MAKES REPRODUCIBILITY POSSIBLE. A model without a
    /// registered dataset version cannot be reproduced, and the registry
    /// refuses to accept one.
    std::string dataset_id;
    std::uint32_t dataset_version = 0;

    /// Which strategy and experiment produced it.
    std::string strategy_id;
    std::string experiment_id;

    /// Model family: "ridge", "logistic", "ols".
    std::string kind;
    /// Hyperparameters, ordered by name so the fingerprint is stable.
    std::map<std::string, std::string, std::less<>> hyperparameters;

    TrainingRecord training;
    Checksum artifact_checksum;
    Timestamp created_at{kNoTimestamp};

    [[nodiscard]] std::string key() const;
    [[nodiscard]] Checksum fingerprint() const;
    [[nodiscard]] Result<bool> validate() const;
    [[nodiscard]] std::string to_json() const;
};

/// One promotion or rollback, recorded.
struct ModelTransition {
    std::string model_id;
    std::uint32_t from_version = 0;
    std::uint32_t to_version = 0;
    ModelStatus from_status{ModelStatus::Trained};
    ModelStatus to_status{ModelStatus::Trained};
    Timestamp at{kNoTimestamp};
    std::string reason;
};

class ModelRegistry {
public:
    /// \param datasets borrowed; every registration is checked against it.
    explicit ModelRegistry(const DatasetRegistry& datasets) noexcept : datasets_(&datasets) {}

    /// Register a trained model.
    ///
    /// REFUSES when the dataset version is not registered. This is the
    /// invariant the module exists for: a model whose training data cannot be
    /// identified is not reproducible.
    [[nodiscard]] Result<bool> register_model(ModelMetadata);

    [[nodiscard]] const ModelMetadata* find(std::string_view model_id,
                                            std::uint32_t version) const noexcept;
    [[nodiscard]] const ModelMetadata* champion(std::string_view model_id) const noexcept;
    [[nodiscard]] std::vector<const ModelMetadata*> challengers(std::string_view model_id) const;
    [[nodiscard]] std::vector<std::uint32_t> versions_of(std::string_view model_id) const;

    /// Promote a challenger to champion. The incumbent is ARCHIVED, not
    /// deleted: rollback needs it, and a promotion that destroyed its
    /// predecessor would be irreversible.
    [[nodiscard]] Result<bool> promote(std::string_view model_id, std::uint32_t version,
                                       Timestamp at, std::string reason = {});

    /// Restore a previously archived version to champion.
    [[nodiscard]] Result<bool> rollback(std::string_view model_id, std::uint32_t version,
                                        Timestamp at, std::string reason = {});

    [[nodiscard]] const std::vector<ModelTransition>& history() const noexcept { return history_; }
    [[nodiscard]] std::size_t size() const noexcept { return models_.size(); }
    [[nodiscard]] std::string to_json() const;

private:
    const DatasetRegistry* datasets_;
    std::map<std::pair<std::string, std::uint32_t>, ModelMetadata> models_;
    /// model_id -> champion version. An INDEX, not a second source of truth:
    /// the status on the record stays authoritative and this only accelerates
    /// the lookup. Without it champion() is a linear scan, which a benchmark
    /// over a thousand models showed to be 130x slower than the other
    /// registries -- unacceptable on a path a live system consults per
    /// prediction.
    std::map<std::string, std::uint32_t, std::less<>> champions_;
    std::vector<ModelTransition> history_;
};

// ---------------------------------------------------------------------------
// Artifact store
// ---------------------------------------------------------------------------

/// A write-once JSON artifact store on the filesystem.
///
/// Writes to a temporary path and renames into place, so a crash mid-write
/// leaves no half-written artifact for a later read to trust. The same
/// discipline the Phase 3 journal uses, for the same reason.
class ArtifactStore {
public:
    explicit ArtifactStore(std::string root) : root_(std::move(root)) {}

    /// \param key hierarchical, e.g. "experiments/exp1/predictions".
    [[nodiscard]] Result<std::string> put(std::string_view key, std::string_view json);
    [[nodiscard]] Result<std::string> get(std::string_view key) const;
    [[nodiscard]] bool contains(std::string_view key) const;
    [[nodiscard]] Result<Checksum> checksum_of(std::string_view key) const;

    /// Keys under a prefix, sorted, so a listing is reproducible.
    [[nodiscard]] Result<std::vector<std::string>> list(std::string_view prefix) const;
    [[nodiscard]] Result<bool> remove(std::string_view key);

    [[nodiscard]] const std::string& root() const noexcept { return root_; }

private:
    [[nodiscard]] Result<std::string> path_for(std::string_view key) const;
    std::string root_;
};

}  // namespace ptl::storage
