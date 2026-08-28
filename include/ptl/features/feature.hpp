#pragma once

/// \file feature.hpp
/// The streaming feature interface.
///
/// LOOK AT WHAT IS ABSENT. There is no history(), no operator[], no series
/// access, no way to ask for the value at time t-k. An estimator receives one
/// observation at a time and can report only its current value.
///
/// That is the project's primary structural defence against lookahead bias. A
/// vectorised rolling implementation makes peeking a one-character mistake --
/// a missing .shift(1) -- that no reviewer reliably catches. An interface with
/// no random access makes it impossible to express. The same property makes
/// replay and live identical for free: an estimator that has only ever seen a
/// prefix cannot behave differently depending on how it was fed.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/time.hpp"
#include "ptl/core/types.hpp"
#include "ptl/market/bar.hpp"
#include "ptl/market/quote.hpp"

namespace ptl::features {

/// Dense index into a FeatureSet's value array.
enum class FeatureId : std::uint16_t {};
inline constexpr FeatureId kInvalidFeature{0xFFFFu};

[[nodiscard]] constexpr std::uint16_t index_of(FeatureId f) noexcept {
    return static_cast<std::uint16_t>(f);
}

/// A single computed observation.
///
/// `feature_end_time` is the maximum information timestamp used to produce the
/// values -- the lineage field the reconciliation requires (row D1). A consumer
/// asserts feature_end_time <= decision_time; without it, "when did this row
/// become knowable?" has no answer and leakage is undetectable after the fact.
struct FeatureRow {
    Timestamp feature_end_time{kNoTimestamp};
    InstrumentId instrument{kInvalidInstrument};
    /// Hash of the dataset manifest the values derive from.
    std::uint64_t data_version = 0;
    /// Hash of the feature-set definition: names, order, parameters. Two rows
    /// with different feature_set_ids are not comparable, and a cached matrix
    /// keyed by it cannot be silently reused after a definition changes.
    std::uint64_t feature_set_id = 0;
    /// Bit per feature: set when that feature is past its warmup.
    std::uint64_t ready_mask = 0;
    std::span<const double> values;

    [[nodiscard]] bool ready(FeatureId f) const noexcept {
        return (ready_mask & (1ULL << index_of(f))) != 0;
    }
    [[nodiscard]] bool all_ready(std::uint64_t required) const noexcept {
        return (ready_mask & required) == required;
    }
};

/// One streaming estimator.
///
/// Concrete estimators are also usable directly as value types -- the virtual
/// interface exists for heterogeneous composition in a FeatureSet, not because
/// every call must be a virtual one. Hot loops instantiate the concrete type.
class IFeature {
public:
    IFeature() = default;
    virtual ~IFeature() = default;
    IFeature(const IFeature&) = delete;
    IFeature& operator=(const IFeature&) = delete;

protected:
    IFeature(IFeature&&) = default;
    IFeature& operator=(IFeature&&) = default;

public:
    virtual void on_bar(const market::Bar&) noexcept = 0;
    virtual void on_quote(const market::Quote&) noexcept {}

    /// Current value. Meaningful only when ready().
    [[nodiscard]] virtual double value() const noexcept = 0;

    /// False until enough observations have been seen. A consumer must not use
    /// value() before this is true: the first N rows of an unguarded rolling
    /// window are garbage that would otherwise be traded on.
    [[nodiscard]] virtual bool ready() const noexcept = 0;

    /// Observations required before ready() turns true.
    [[nodiscard]] virtual std::size_t warmup() const noexcept = 0;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// Parameters, for the feature_set_id hash. Two estimators with the same
    /// name but different windows must not collide.
    [[nodiscard]] virtual std::string signature() const = 0;

    virtual void reset() noexcept = 0;
};

/// FNV-1a over the ordered feature signatures. Order matters: the values array
/// is positional, so a reordering produces a different matrix.
[[nodiscard]] std::uint64_t compute_feature_set_id(std::span<const std::string> signatures);

}  // namespace ptl::features
