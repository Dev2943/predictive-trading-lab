#include "ptl/signal/generator.hpp"

#include <algorithm>
#include <cmath>

namespace ptl::signal {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

/// Shared causality check. Every generator runs this before anything else.
[[nodiscard]] Result<bool> check_causality(const GeneratorInput& in) {
    if (!is_set(in.as_of)) return fail(bad("generator input has no decision time"));
    if (in.instrument == kInvalidInstrument) {
        return fail(bad("generator input has no instrument"));
    }
    if (!is_finite(in.prediction)) return fail(bad("prediction is not finite"));
    if (is_set(in.prediction_time) && in.prediction_time > in.as_of) {
        // THE CENTRAL LEAK CHECK OF THE SIGNAL LAYER. A prediction stamped
        // after the decision it informs means the model saw the future, and
        // every downstream number is fiction.
        return fail(bad("prediction is stamped after the decision it informs",
                        to_iso8601(in.prediction_time) + " > " + to_iso8601(in.as_of)));
    }
    return true;
}

/// Direction and confidence from a raw prediction.
struct Interpretation {
    Direction direction{Direction::Flat};
    double expected_return = 0.0;
    double confidence = 0.0;
};

[[nodiscard]] Interpretation interpret(const GeneratorInput& in, const GeneratorConfig& cfg) {
    Interpretation out;
    const double raw = in.prediction * cfg.direction_multiplier;

    if (cfg.kind == PredictionKind::Probability) {
        // A classifier gives direction, not magnitude. Converting a probability
        // to an expected return needs an explicit scale -- stating that
        // conversion is more honest than pretending a probability is a return.
        const double centred = raw - 0.5;
        // An exact coin flip is FLAT, not short. Without this the comparison
        // `centred > 0.0` sends 0.5 down the bearish branch, so a model with no
        // view at all would open a short -- and with a zero entry threshold the
        // magnitude check does not catch it.
        if (centred == 0.0) return out;
        if (std::abs(centred) < cfg.entry_threshold) return out;
        out.direction = centred > 0.0 ? Direction::Long : Direction::Short;
        out.expected_return = centred * 2.0 * cfg.probability_return_scale;
        // Confidence is distance from the coin flip, rescaled to [0, 1].
        out.confidence = std::clamp(std::abs(centred) * 2.0, 0.0, 1.0);
    } else {
        // Same reasoning as the probability branch: an exactly zero predicted
        // return is no view, not a bearish one.
        if (raw == 0.0) return out;
        if (std::abs(raw) < cfg.entry_threshold) return out;
        out.direction = raw > 0.0 ? Direction::Long : Direction::Short;
        out.expected_return = raw;
        // A regressor has no probability. Confidence is the predicted move
        // scaled by trailing volatility -- a move of one sigma is treated as
        // meaningful, and larger moves saturate rather than exceeding 1.
        const double sigma = in.volatility > 0.0 ? in.volatility : 1.0;
        out.confidence = std::clamp(std::abs(raw) / sigma, 0.0, 1.0);
    }

    if (out.direction == Direction::Short && !cfg.allow_short) {
        // Shorting disabled means flat, not long. Flipping the direction would
        // convert a bearish view into a bullish trade.
        return Interpretation{};
    }
    return out;
}

}  // namespace

std::string_view to_string(EnsembleMethod m) noexcept {
    switch (m) {
        case EnsembleMethod::WeightedAverage:
            return "weighted_average";
        case EnsembleMethod::Voting:
            return "voting";
        case EnsembleMethod::ConfidenceWeighted:
            return "confidence_weighted";
    }
    return "unknown";
}

Result<Signal> ModelSignalGenerator::generate(const GeneratorInput& in) const {
    if (auto ok = check_causality(in); !ok) return fail(ok.error());

    const Interpretation view = interpret(in, cfg_);
    if (view.direction == Direction::Flat) {
        return Signal::flat(in.as_of, in.instrument, model_id_);
    }
    return Signal::create(in.as_of, in.instrument, view.direction, view.expected_return,
                          view.confidence, cfg_.horizon, model_id_, in.costs);
}

Result<Signal> RuleSignalGenerator::generate(const GeneratorInput& in) const {
    if (auto ok = check_causality(in); !ok) return fail(ok.error());

    // A rule reads the feature value directly and applies its sign convention.
    // Same interface, same causality checks, same cost accounting as a model --
    // which is what makes the comparison against a model meaningful.
    const Interpretation view = interpret(in, cfg_);
    if (view.direction == Direction::Flat) {
        return Signal::flat(in.as_of, in.instrument, model_id_);
    }
    return Signal::create(in.as_of, in.instrument, view.direction, view.expected_return,
                          view.confidence, cfg_.horizon, model_id_, in.costs);
}

Result<bool> EnsembleSignalGenerator::add(std::shared_ptr<ISignalGenerator> member, double weight) {
    if (member == nullptr) return fail(bad("ensemble member is null"));
    if (!is_finite(weight) || weight <= 0.0) {
        // A zero-weight member contributes nothing while appearing to be part
        // of the ensemble, which makes the reported composition a lie.
        return fail(bad("ensemble member weight must be positive"));
    }
    members_.push_back(Member{std::move(member), weight});
    return true;
}

Result<Signal> EnsembleSignalGenerator::generate(const GeneratorInput& in) const {
    if (auto ok = check_causality(in); !ok) return fail(ok.error());
    if (members_.empty()) return fail(bad("ensemble has no members"));

    // EVERY MEMBER RECEIVES THE SAME INPUT. No member can see another's output
    // or any instant other than as_of, so an ensemble cannot leak where its
    // members do not. Members are visited in registration order, so the
    // summation is reproducible.
    std::vector<Signal> votes;
    std::vector<double> weights;
    votes.reserve(members_.size());
    weights.reserve(members_.size());

    for (const auto& m : members_) {
        auto s = m.generator->generate(in);
        if (!s) return fail(s.error());
        votes.push_back(*s);
        weights.push_back(m.weight);
    }

    double total_weight = 0.0;
    double weighted_return = 0.0;
    double weighted_confidence = 0.0;
    double long_weight = 0.0;
    double short_weight = 0.0;

    for (std::size_t i = 0; i < votes.size(); ++i) {
        const Signal& v = votes[i];
        double w = weights[i];
        if (method_ == EnsembleMethod::ConfidenceWeighted) {
            // An uncertain member contributes little without needing a
            // hand-tuned weight.
            w *= v.confidence();
        }
        total_weight += w;
        weighted_return += v.expected_return() * w;
        weighted_confidence += v.confidence() * w;
        if (v.direction() == Direction::Long) long_weight += w;
        if (v.direction() == Direction::Short) short_weight += w;
    }

    if (total_weight <= 0.0) {
        // Every member abstained or had zero confidence. Flat is the honest
        // answer; dividing would produce a NaN signal.
        return Signal::flat(in.as_of, in.instrument, model_id_);
    }

    Direction direction = Direction::Flat;
    double expected_return = 0.0;
    double confidence = 0.0;

    if (method_ == EnsembleMethod::Voting) {
        if (long_weight > short_weight)
            direction = Direction::Long;
        else if (short_weight > long_weight)
            direction = Direction::Short;
        // An exact tie is genuinely undecided, and flat is the correct answer.
        const double winning = std::max(long_weight, short_weight);
        confidence = std::clamp(winning / total_weight, 0.0, 1.0);
        expected_return = weighted_return / total_weight;
    } else {
        expected_return = weighted_return / total_weight;
        confidence = std::clamp(weighted_confidence / total_weight, 0.0, 1.0);
        if (expected_return > 0.0)
            direction = Direction::Long;
        else if (expected_return < 0.0)
            direction = Direction::Short;
    }

    if (direction == Direction::Flat) {
        return Signal::flat(in.as_of, in.instrument, model_id_);
    }
    return Signal::create(in.as_of, in.instrument, direction, expected_return, confidence, horizon_,
                          model_id_, in.costs);
}

}  // namespace ptl::signal
