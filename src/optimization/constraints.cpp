#include "ptl/optimization/constraints.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>

namespace ptl::optimization {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::InvalidArgument, std::move(message), std::move(context));
}

void note(std::vector<std::string>* binding, std::string name) {
    if (binding == nullptr) return;
    if (std::find(binding->begin(), binding->end(), name) != binding->end()) return;
    binding->push_back(std::move(name));
}

[[nodiscard]] double gross_of(std::span<const double> w) {
    double total = 0.0;
    for (const double x : w) total += std::abs(x);
    return total;
}

[[nodiscard]] double net_of(std::span<const double> w) {
    return std::accumulate(w.begin(), w.end(), 0.0);
}

}  // namespace

std::string_view to_string(DirectionMode m) noexcept {
    switch (m) {
        case DirectionMode::LongOnly:
            return "long_only";
        case DirectionMode::LongShort:
            return "long_short";
        case DirectionMode::DollarNeutral:
            return "dollar_neutral";
    }
    return "unknown";
}

Result<bool> ConstraintSet::validate() const {
    if (!is_finite(max_position) || max_position <= 0.0) {
        return fail(bad("max_position must be positive"));
    }
    if (!is_finite(min_position) || min_position > max_position) {
        return fail(bad("min_position must not exceed max_position"));
    }
    if (direction == DirectionMode::LongOnly && min_position < 0.0) {
        // A long-only book with a negative floor is a contradiction, and
        // resolving it silently would let shorts through under a name that
        // promises none.
        return fail(bad("long-only requires a non-negative min_position"));
    }
    if (max_gross_leverage <= 0.0 || !is_finite(max_gross_leverage)) {
        return fail(bad("max_gross_leverage must be positive"));
    }
    if (max_net_leverage < 0.0 || !is_finite(max_net_leverage)) {
        return fail(bad("max_net_leverage cannot be negative"));
    }
    if (cash_reserve < 0.0 || cash_reserve >= 1.0) {
        return fail(bad("cash_reserve must lie in [0, 1)"));
    }
    if (max_concentration <= 0.0 || max_concentration > 1.0) {
        return fail(bad("max_concentration must lie in (0, 1]"));
    }
    if (max_sector_exposure <= 0.0) {
        return fail(bad("max_sector_exposure must be positive"));
    }
    return true;
}

std::string ConstraintSet::describe() const {
    std::ostringstream ss;
    ss.precision(4);
    ss << std::fixed << to_string(direction) << ": position [" << min_position << ", "
       << max_position << "], gross <= " << max_gross_leverage << ", net <= " << max_net_leverage
       << ", sector <= " << max_sector_exposure;
    if (max_turnover > 0.0) ss << ", turnover <= " << max_turnover;
    if (cash_reserve > 0.0) ss << ", cash reserve " << cash_reserve;
    if (beta_neutral) ss << ", beta neutral";
    return ss.str();
}

ConstraintSet ConstraintSet::long_only(double max_position) {
    ConstraintSet c;
    c.direction = DirectionMode::LongOnly;
    c.max_position = max_position;
    c.min_position = 0.0;
    c.max_gross_leverage = 1.0;
    c.max_net_leverage = 1.0;
    return c;
}

ConstraintSet ConstraintSet::market_neutral(double max_position) {
    ConstraintSet c;
    c.direction = DirectionMode::DollarNeutral;
    c.max_position = max_position;
    c.min_position = -max_position;
    c.max_gross_leverage = 2.0;
    // Dollar neutrality is enforced by the projection; the net cap is a
    // belt-and-braces bound in case betas or rounding leave a residue.
    c.max_net_leverage = 0.01;
    c.beta_neutral = true;
    return c;
}

Result<bool> ConstraintProjector::project(std::vector<double>& weights,
                                          const OptimizationInput& input,
                                          std::vector<std::string>* binding) const {
    if (auto ok = constraints_.validate(); !ok) return fail(ok.error());
    const std::size_t n = weights.size();
    if (n == 0) return true;
    if (n != input.size()) {
        return fail(bad("weight vector length does not match the universe"));
    }

    for (double& w : weights) {
        if (!is_finite(w)) w = 0.0;
    }

    // The steps interact: clipping a position changes the gross, rescaling the
    // gross can push a position back over its cap. Iterating to a fixed point
    // is what makes the result actually feasible rather than approximately so.
    constexpr std::size_t kMaxPasses = 12;
    for (std::size_t pass = 0; pass < kMaxPasses; ++pass) {
        const std::vector<double> before = weights;

        // --- direction -------------------------------------------------------
        if (constraints_.direction == DirectionMode::LongOnly) {
            for (double& w : weights) {
                if (w < 0.0) {
                    w = 0.0;
                    note(binding, "long_only");
                }
            }
        }

        // --- per-position bounds --------------------------------------------
        for (double& w : weights) {
            if (w > constraints_.max_position) {
                w = constraints_.max_position;
                note(binding, "max_position");
            }
            if (w < constraints_.min_position) {
                w = constraints_.min_position;
                note(binding, "min_position");
            }
        }

        // --- minimum trade size ---------------------------------------------
        // Applied BEFORE the leverage steps, so the gross budget freed by
        // dropping dust is redistributed rather than lost.
        if (constraints_.min_trade_size > 0.0) {
            for (double& w : weights) {
                if (w != 0.0 && std::abs(w) < constraints_.min_trade_size) {
                    w = 0.0;
                    note(binding, "min_trade_size");
                }
            }
        }

        // --- sector exposure -------------------------------------------------
        if (!input.sectors.empty() && input.sectors.size() == n) {
            std::map<std::int32_t, double> sector_gross;
            for (std::size_t i = 0; i < n; ++i) {
                if (input.sectors[i] < 0) continue;
                sector_gross[input.sectors[i]] += std::abs(weights[i]);
            }
            for (const auto& [sector, gross] : sector_gross) {
                if (gross <= constraints_.max_sector_exposure || gross <= 0.0) continue;
                // Scale the sector down proportionally: every name in it keeps
                // its relative size, which preserves the optimizer's view of
                // which names within the sector are preferable.
                const double scale = constraints_.max_sector_exposure / gross;
                for (std::size_t i = 0; i < n; ++i) {
                    if (input.sectors[i] == sector) weights[i] *= scale;
                }
                note(binding, "max_sector_exposure");
            }
        }

        // --- dollar neutrality ------------------------------------------------
        if (constraints_.direction == DirectionMode::DollarNeutral) {
            const double net = net_of(weights);
            if (std::abs(net) > 1e-12) {
                // Subtract the mean: the cheapest projection onto the
                // zero-sum hyperplane, and it leaves relative preferences
                // intact.
                const double adjustment = net / static_cast<double>(n);
                for (double& w : weights) w -= adjustment;
                note(binding, "dollar_neutral");
            }
        }

        // --- beta neutrality --------------------------------------------------
        if (constraints_.beta_neutral && input.betas.size() == n) {
            double portfolio_beta = 0.0;
            double beta_sq = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                portfolio_beta += weights[i] * input.betas[i];
                beta_sq += input.betas[i] * input.betas[i];
            }
            if (std::abs(portfolio_beta) > 1e-9 && beta_sq > 0.0) {
                // Project out the beta component: w -= (w'β / β'β) β.
                const double scale = portfolio_beta / beta_sq;
                for (std::size_t i = 0; i < n; ++i) weights[i] -= scale * input.betas[i];
                note(binding, "beta_neutral");
            }
        }

        // --- gross leverage and cash reserve ----------------------------------
        const double gross_budget =
            constraints_.max_gross_leverage * (1.0 - constraints_.cash_reserve);
        const double gross = gross_of(weights);
        if (gross > gross_budget && gross > 0.0) {
            const double scale = gross_budget / gross;
            for (double& w : weights) w *= scale;
            note(binding, constraints_.cash_reserve > 0.0 ? "cash_reserve" : "max_gross_leverage");
        }

        // --- net leverage -----------------------------------------------------
        const double net = net_of(weights);
        if (std::abs(net) > constraints_.max_net_leverage) {
            const double excess = std::abs(net) - constraints_.max_net_leverage;
            const double adjustment = std::copysign(excess / static_cast<double>(n), net);
            for (double& w : weights) w -= adjustment;
            note(binding, "max_net_leverage");
        }

        // --- concentration ----------------------------------------------------
        const double gross_now = gross_of(weights);
        if (gross_now > 0.0 && constraints_.max_concentration < 1.0) {
            const double cap = constraints_.max_concentration * gross_now;
            bool clipped = false;
            for (double& w : weights) {
                if (std::abs(w) > cap) {
                    w = std::copysign(cap, w);
                    clipped = true;
                }
            }
            if (clipped) note(binding, "max_concentration");
        }

        // --- turnover ---------------------------------------------------------
        if (constraints_.max_turnover > 0.0 && input.current_weights.size() == n) {
            double turnover = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                turnover += std::abs(weights[i] - input.current_weights[i]);
            }
            if (turnover > constraints_.max_turnover && turnover > 0.0) {
                // Move only part of the way toward the target. Trading to a
                // partial target is what a turnover budget actually means; the
                // remainder is picked up on the next rebalance.
                const double alpha = constraints_.max_turnover / turnover;
                for (std::size_t i = 0; i < n; ++i) {
                    weights[i] =
                        input.current_weights[i] + alpha * (weights[i] - input.current_weights[i]);
                }
                note(binding, "max_turnover");
            }
        }

        // Converged?
        double delta = 0.0;
        for (std::size_t i = 0; i < n; ++i) delta += std::abs(weights[i] - before[i]);
        if (delta < 1e-12) break;
    }

    for (double& w : weights) {
        if (!is_finite(w)) w = 0.0;
    }
    return true;
}

bool ConstraintProjector::feasible(std::span<const double> weights, const OptimizationInput& input,
                                   double tolerance) const {
    const std::size_t n = weights.size();
    if (n != input.size()) return false;

    for (const double w : weights) {
        if (!is_finite(w)) return false;
        if (w > constraints_.max_position + tolerance) return false;
        if (w < constraints_.min_position - tolerance) return false;
        if (constraints_.direction == DirectionMode::LongOnly && w < -tolerance) {
            return false;
        }
    }

    const double gross_budget = constraints_.max_gross_leverage * (1.0 - constraints_.cash_reserve);
    if (gross_of(weights) > gross_budget + tolerance) return false;
    if (std::abs(net_of(weights)) > constraints_.max_net_leverage + tolerance) {
        return false;
    }

    if (!input.sectors.empty() && input.sectors.size() == n) {
        std::map<std::int32_t, double> sector_gross;
        for (std::size_t i = 0; i < n; ++i) {
            if (input.sectors[i] < 0) continue;
            sector_gross[input.sectors[i]] += std::abs(weights[i]);
        }
        for (const auto& [sector, gross] : sector_gross) {
            if (gross > constraints_.max_sector_exposure + tolerance) return false;
        }
    }
    return true;
}

}  // namespace ptl::optimization
