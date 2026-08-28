#pragma once

/// \file label.hpp
/// Forward-looking target construction.
///
/// ⚠ THIS IS THE ONLY MODULE IN THE PROJECT PERMITTED TO INDEX FORWARD IN TIME.
///
/// It is physically isolated: `ptl_labels` is a separate CMake target, and the
/// live binaries -- ptl_gate, ptl_version, and every future paper-trading
/// runner -- do NOT link it. A leak through labels is therefore a LINKER ERROR,
/// not a subtle bug found by inspection. That guarantee is worth more than any
/// amount of review discipline, and it is checked by CI.
///
/// Two rules the research is emphatic about:
///
///  1. LABEL ON MIDPRICE, never on the close used for fills. Using the same
///     price series as both the prediction target and the execution price
///     couples the model to the simulator: a change to the fill model would
///     silently change what the model was trained to predict.
///
///  2. CARRY FOUR TIMESTAMPS. sample_start, feature_end, label_start and
///     label_end. Purging tests interval OVERLAP, and with a horizon longer
///     than the decision step the labels overlap -- an endpoint comparison
///     leaves contaminated rows in training.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/core/time.hpp"
#include "ptl/core/types.hpp"

namespace ptl::labels {

enum class LabelKind : std::uint8_t {
    /// log(mid_{t+h} / mid_t). The research's primary formulation.
    ForwardLogReturn,
    /// The same, divided by trailing realised volatility known at t. Makes the
    /// target roughly stationary and comparable across instruments whose
    /// volatilities differ by 3x.
    VolNormalisedReturn,
    /// Forward return net of an estimated round-trip cost. A target that is
    /// only positive when the move actually pays for the trade.
    CostAwareReturn,
    /// 1 when the forward return exceeds a threshold, 0 otherwise. Secondary
    /// diagnostic: direction accuracy is not P&L.
    Direction,
    /// -1 / 0 / +1 from the triple-barrier method: which barrier was touched
    /// first, or neither before the horizon expired.
    TripleBarrier,
};

[[nodiscard]] std::string_view to_string(LabelKind) noexcept;

struct LabelConfig {
    LabelKind kind = LabelKind::ForwardLogReturn;

    /// Horizon in BARS. Declared up front and hashed into the config, so it
    /// cannot be tuned after seeing results.
    std::size_t horizon = 15;

    /// Trailing window for the volatility normaliser. Uses only data known at
    /// the decision instant -- that is what keeps normalisation causal.
    std::size_t vol_window = 60;

    /// Clip at +/- this many standard deviations. Heavy tails otherwise let a
    /// handful of observations dominate a least-squares fit.
    double winsor_sigma = 4.0;
    bool winsorize = true;

    /// Threshold for Direction labels, in the label's own units.
    double direction_threshold = 0.0;

    /// Round-trip cost for CostAwareReturn, in basis points.
    double round_trip_cost_bps = 2.0;

    /// Triple-barrier: profit and loss barriers as multiples of trailing
    /// volatility. Volatility-scaled rather than fixed, so one parameter works
    /// across instruments and regimes.
    double barrier_upper_sigma = 2.0;
    double barrier_lower_sigma = 2.0;

    [[nodiscard]] std::string signature() const;
    [[nodiscard]] std::uint64_t hash() const;
};

/// One training observation's target and its full lineage.
struct Label {
    InstrumentId instrument{kInvalidInstrument};
    ObservationInterval interval{};
    double value = 0.0;
    /// Sample weight. Uniform by default; overlapping labels can be
    /// down-weighted so a horizon longer than the step does not silently
    /// multiply the effective sample size.
    double weight = 1.0;
    bool valid = false;
    /// Barriers only: which one was touched. 0 when the horizon expired first.
    int barrier_touched = 0;
};

/// Column-oriented label set, parallel to a FeatureMatrix.
struct LabelSet {
    std::vector<Label> labels;
    LabelConfig config;
    std::uint64_t config_hash = 0;

    [[nodiscard]] std::size_t size() const noexcept { return labels.size(); }
    [[nodiscard]] std::size_t valid_count() const noexcept;
    /// Row indices whose label is usable. Rows near the end of the sample have
    /// no future to look at and are invalid by construction.
    [[nodiscard]] std::vector<std::size_t> valid_rows() const;
    [[nodiscard]] std::vector<double> values() const;
    [[nodiscard]] std::vector<ObservationInterval> intervals() const;
};

/// One observation of the price series labels are built from.
///
/// MIDPRICE, deliberately. Requiring the caller to supply a mid rather than a
/// bar makes it impossible to accidentally label on the close the simulator
/// fills at.
struct PricePoint {
    Timestamp ts{kNoTimestamp};
    InstrumentId instrument{kInvalidInstrument};
    Price mid{};
};

class LabelBuilder {
public:
    explicit LabelBuilder(LabelConfig cfg) : cfg_(cfg) {}

    /// Build labels for one instrument's chronological midprice series.
    ///
    /// \param prices strictly increasing in time; anything else is an error,
    ///        because an unsorted series would look forward by accident.
    [[nodiscard]] Result<LabelSet> build(std::span<const PricePoint> prices) const;

    /// Build across several instruments, concatenated and re-sorted by
    /// decision time so the result aligns with a pooled feature matrix.
    [[nodiscard]] Result<LabelSet> build_panel(
        std::span<const std::vector<PricePoint>> per_instrument) const;

    [[nodiscard]] const LabelConfig& config() const noexcept { return cfg_; }

private:
    LabelConfig cfg_;
};

}  // namespace ptl::labels
