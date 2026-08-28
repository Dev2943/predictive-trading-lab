#include "ptl/portfolio/position.hpp"

#include <algorithm>
#include <cmath>

namespace ptl::portfolio {

Notional Position::apply(const oms::Fill& fill) {
    const double delta = fill.signed_quantity().get();
    const double px = fill.price().get();
    const double before = quantity_.get();
    const double after = before + delta;

    commission_ = commission_ + fill.total_cost();
    if (delta > 0.0) {
        bought_ = bought_ + Qty{delta};
    } else {
        sold_ = sold_ + Qty{-delta};
    }

    double realized_now = 0.0;

    if (before == 0.0 || (before > 0.0) == (delta > 0.0)) {
        // Opening or adding in the same direction: no realisation, and the
        // average cost moves toward the new fill.
        const double denom = before + delta;
        if (denom != 0.0) {
            average_cost_ = Price{(average_cost_.get() * before + px * delta) / denom};
        }
    } else {
        // Reducing, closing, or reversing.
        const double closing = std::min(std::abs(delta), std::abs(before));
        // Realisation is computed on the CLOSING portion only, and against the
        // matched cost basis -- never against the current mark, which would
        // book unrealised gains as realised.
        const double direction = before > 0.0 ? 1.0 : -1.0;
        realized_now = (px - average_cost_.get()) * closing * direction;
        realized_ = realized_ + Notional{realized_now};

        if (std::abs(delta) > std::abs(before)) {
            // REVERSAL through zero. The cost basis must RESET to the price of
            // the opening portion. Carrying the old average across would leave
            // a short marked against a long's entry, and every unrealised
            // number afterwards would be wrong.
            average_cost_ = Price{px};
        } else if (after == 0.0) {
            average_cost_ = Price{0.0};
        }
        // Partial reduction leaves the average cost unchanged, by definition of
        // weighted-average accounting.
    }

    quantity_ = Qty{after};
    if (quantity_.get() == 0.0) average_cost_ = Price{0.0};
    return Notional{realized_now};
}

void Position::apply_split(double ratio) noexcept {
    if (!is_finite(ratio) || ratio <= 0.0) return;
    // Shares scale up, cost basis scales down. The product -- the economic
    // value of the position -- is invariant, which is exactly what the
    // synthetic-split test asserts.
    quantity_ = Qty{quantity_.get() * ratio};
    average_cost_ = Price{average_cost_.get() / ratio};
    bought_ = Qty{bought_.get() * ratio};
    sold_ = Qty{sold_.get() * ratio};
}

Notional Position::dividend_cash(Notional per_share) const noexcept {
    // Signed by holding: a short position PAYS the dividend. Treating a
    // dividend as a price return without the cash leg is one of the classic
    // accounting errors.
    return Notional{per_share.get() * quantity_.get()};
}

}  // namespace ptl::portfolio
