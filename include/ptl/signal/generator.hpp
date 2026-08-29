#pragma once

/// \file generator.hpp
/// Signal generators: rule, model and ensemble.
///
/// A generator turns a PREDICTION into an INTENTION. That is a real conversion,
/// not a relabelling: it applies a direction convention, a confidence mapping
/// and a cost estimate, and any of the three can turn a positive prediction
/// into a flat signal.
///
/// Generators are strictly causal by construction. Every one takes a
/// GeneratorInput carrying a single instant's state and returns a Signal
/// stamped at that instant. There is no series argument and no history access,
/// so a generator cannot consult a future prediction even by mistake.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/signal/signal.hpp"

namespace ptl::signal {

/// One instant's worth of everything a generator may see.
struct GeneratorInput {
    Timestamp as_of{kNoTimestamp};
    InstrumentId instrument{kInvalidInstrument};

    /// Model output. For a regressor this is an expected return over the
    /// horizon; for a classifier, a probability of the positive class. Which
    /// one it is comes from the generator's configuration, never guessed.
    double prediction = 0.0;

    /// Instant the prediction was produced. Must not be after `as_of`: a
    /// prediction from the future is the leak this whole project exists to
    /// prevent, and the generator refuses it.
    Timestamp prediction_time{kNoTimestamp};

    /// Trailing volatility, for scaling an expected return into a magnitude.
    double volatility = 0.0;
    /// Reference price, for converting basis points into return units.
    Price reference_price{};
    CostEstimate costs{};
};

class ISignalGenerator {
public:
    ISignalGenerator() = default;
    virtual ~ISignalGenerator() = default;
    ISignalGenerator(const ISignalGenerator&) = delete;
    ISignalGenerator& operator=(const ISignalGenerator&) = delete;

protected:
    ISignalGenerator(ISignalGenerator&&) = default;
    ISignalGenerator& operator=(ISignalGenerator&&) = default;

public:
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// Stable identity, recorded on every signal it emits.
    [[nodiscard]] virtual std::uint64_t model_id() const noexcept = 0;

    [[nodiscard]] virtual Result<Signal> generate(const GeneratorInput&) const = 0;

    [[nodiscard]] virtual Duration horizon() const noexcept = 0;
};

/// How a raw prediction becomes a direction and a confidence.
enum class PredictionKind : std::uint8_t {
    /// Expected return over the horizon, in return units.
    Regression,
    /// Probability of the positive class, in [0, 1].
    Probability,
};

struct GeneratorConfig {
    PredictionKind kind{PredictionKind::Regression};
    Duration horizon{std::chrono::minutes{15}};

    /// Below this magnitude the generator emits Flat. For a probability this is
    /// distance from 0.5; for a regression it is the absolute expected return.
    double entry_threshold = 0.0;

    /// Scales a probability into an expected return. A classifier tells you
    /// direction, not magnitude, so a magnitude must be supplied from
    /// somewhere -- and stating that conversion explicitly is better than
    /// pretending a probability is a return.
    double probability_return_scale = 0.001;

    /// -1 inverts the signal, for a reversal rule.
    double direction_multiplier = 1.0;

    bool allow_short = true;
};

/// Turns a model prediction into a signal.
class ModelSignalGenerator final : public ISignalGenerator {
public:
    ModelSignalGenerator(std::string name, std::uint64_t model_id, GeneratorConfig cfg = {})
        : name_(std::move(name)), model_id_(model_id), cfg_(cfg) {}

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }
    [[nodiscard]] std::uint64_t model_id() const noexcept override { return model_id_; }
    [[nodiscard]] Duration horizon() const noexcept override { return cfg_.horizon; }
    [[nodiscard]] Result<Signal> generate(const GeneratorInput&) const override;

    [[nodiscard]] const GeneratorConfig& config() const noexcept { return cfg_; }

private:
    std::string name_;
    std::uint64_t model_id_;
    GeneratorConfig cfg_;
};

/// A signal from a feature value alone, with no fitted model.
///
/// The permanent benchmark. If a model's signals do not beat this after costs,
/// the model is adding nothing, and that is a finding rather than a failure
/// (ADR: cost-aware rule baseline, Phase 6).
class RuleSignalGenerator final : public ISignalGenerator {
public:
    RuleSignalGenerator(std::string name, std::uint64_t model_id, GeneratorConfig cfg = {})
        : name_(std::move(name)), model_id_(model_id), cfg_(cfg) {}

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }
    [[nodiscard]] std::uint64_t model_id() const noexcept override { return model_id_; }
    [[nodiscard]] Duration horizon() const noexcept override { return cfg_.horizon; }
    [[nodiscard]] Result<Signal> generate(const GeneratorInput&) const override;

private:
    std::string name_;
    std::uint64_t model_id_;
    GeneratorConfig cfg_;
};

enum class EnsembleMethod : std::uint8_t {
    /// Weighted mean of expected returns.
    WeightedAverage,
    /// Majority direction; confidence is the share agreeing.
    Voting,
    /// Weighted by each member's own confidence, so an uncertain member
    /// contributes little without needing a hand-tuned weight.
    ConfidenceWeighted,
};

[[nodiscard]] std::string_view to_string(EnsembleMethod) noexcept;

/// Combines several generators.
///
/// NO LEAKAGE BY CONSTRUCTION: every member receives the SAME GeneratorInput,
/// so no member can see another's output or any instant other than `as_of`.
/// Combination happens after all members have spoken, and members are visited
/// in registration order so the arithmetic is reproducible.
class EnsembleSignalGenerator final : public ISignalGenerator {
public:
    EnsembleSignalGenerator(std::string name, std::uint64_t model_id,
                            EnsembleMethod method = EnsembleMethod::WeightedAverage,
                            Duration horizon = std::chrono::minutes{15})
        : name_(std::move(name)), model_id_(model_id), method_(method), horizon_(horizon) {}

    /// \param weight must be positive; a zero-weight member would silently
    ///        contribute nothing while appearing to be part of the ensemble.
    [[nodiscard]] Result<bool> add(std::shared_ptr<ISignalGenerator> member, double weight = 1.0);

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }
    [[nodiscard]] std::uint64_t model_id() const noexcept override { return model_id_; }
    [[nodiscard]] Duration horizon() const noexcept override { return horizon_; }
    [[nodiscard]] Result<Signal> generate(const GeneratorInput&) const override;

    [[nodiscard]] std::size_t size() const noexcept { return members_.size(); }
    [[nodiscard]] EnsembleMethod method() const noexcept { return method_; }

private:
    struct Member {
        std::shared_ptr<ISignalGenerator> generator;
        double weight = 1.0;
    };

    std::string name_;
    std::uint64_t model_id_;
    EnsembleMethod method_;
    Duration horizon_;
    std::vector<Member> members_;
};

}  // namespace ptl::signal
