/// \file example_execution.cpp
/// Execution algorithm schedules.
///
/// Shows how a parent order becomes a schedule of child slices under each
/// algorithm. Schedules are pure functions of simulated time (ADR-0004) -- no
/// algorithm holds a clock -- which is what lets an execution be replayed
/// exactly.

#include <cmath>
#include <iomanip>
#include <iostream>
#include <vector>

#include "ptl/algo/algorithms.hpp"

int main() {
    ptl::Timestamp decision{};
    ptl::Timestamp window_begin{};
    ptl::Timestamp window_end{};
    (void)ptl::parse_timestamp("2024-07-02T14:29:00Z", decision);
    (void)ptl::parse_timestamp("2024-07-02T14:30:00Z", window_begin);
    (void)ptl::parse_timestamp("2024-07-02T15:30:00Z", window_end);

    ptl::LifecycleTimes times;
    times.decision_time = decision;

    // The parent order is a real oms::Order: execution algorithms slice what
    // the strategy actually asked for, not a separate description of it.
    auto parent = ptl::oms::Order::market(ptl::oms::OrderId{1}, ptl::InstrumentId{0},
                                          ptl::Side::Buy, ptl::Qty{10'000}, times);
    if (!parent) {
        std::cerr << "order: " << parent.error().message << '\n';
        return 1;
    }

    // VWAP REFUSES without a profile rather than silently degenerating to
    // TWAP, so a realistic U-shaped intraday profile is supplied.
    std::vector<double> volume_profile;
    volume_profile.reserve(12);
    for (std::size_t i = 0; i < 12; ++i) {
        const double x = static_cast<double>(i) / 11.0;
        volume_profile.push_back(0.5 + 2.0 * (x - 0.5) * (x - 0.5));
    }

    // EVERY field is initialised explicitly. ExecutionRequest holds an
    // oms::Order, whose default constructor is private, so the struct is not
    // default-constructible -- an order with no identity should not be
    // reachable by accident.
    const ptl::algo::ExecutionRequest request{parent->with_arrival_price(ptl::Price{500.0}),
                                              window_begin,
                                              window_end,
                                              ptl::algo::ExecutionPolicy{},
                                              12,
                                              std::move(volume_profile)};

    auto registry = ptl::algo::AlgorithmRegistry::with_defaults();
    if (!registry) {
        std::cerr << "registry: " << registry.error().message << '\n';
        return 1;
    }

    std::cout << std::fixed << std::setprecision(1);
    for (const auto& name : registry->names()) {
        auto algorithm = registry->create(name);
        if (!algorithm) continue;

        auto schedule = (*algorithm)->plan(request);
        if (!schedule) {
            // Refusing is a legitimate answer. An algorithm that cannot express
            // this request says so rather than producing something adjacent.
            std::cout << std::setw(16) << name << "  refused: " << schedule.error().message << '\n';
            continue;
        }

        const double total = schedule->total_quantity().get();
        std::cout << std::setw(16) << name << "  slices " << std::setw(3) << schedule->size()
                  << "  total " << std::setw(9) << total;

        // A schedule must account for the WHOLE parent order. One that silently
        // drops quantity leaves the strategy short of the position it asked for.
        const bool complete = std::abs(total - request.parent.quantity().get()) < 1e-6;
        std::cout << (complete ? "  (complete)" : "  (INCOMPLETE)") << '\n';
    }
    return 0;
}
