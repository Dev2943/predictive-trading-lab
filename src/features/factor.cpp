#include "ptl/features/factor.hpp"

#include <algorithm>
#include <set>

namespace ptl::features {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

}  // namespace

Result<double> FactorInputs::get(std::string_view name) const {
    if (!declared(name)) {
        // Reading an undeclared input would work by accident whenever
        // evaluation order happened to cooperate and break silently when it did
        // not. Refusing keeps the graph honest about what it actually needs.
        return fail(bad("factor read an undeclared dependency: " + std::string{name}));
    }
    const auto it = values_->find(name);
    if (it == values_->end()) {
        return fail(bad("declared dependency has no value: " + std::string{name}));
    }
    return it->second;
}

bool FactorInputs::declared(std::string_view name) const noexcept {
    return std::find(allowed_.begin(), allowed_.end(), name) != allowed_.end();
}

Result<bool> FactorGraph::add(FactorDefinition definition) {
    if (definition.name.empty()) return fail(bad("factor name cannot be empty"));
    if (definition.compute == nullptr) {
        return fail(bad("factor has no compute function: " + definition.name));
    }
    if (definitions_.contains(definition.name)) {
        return fail(bad("duplicate factor registration: " + definition.name));
    }
    // Any addition invalidates a previously resolved order.
    finalized_ = false;
    order_.clear();
    definitions_.emplace(definition.name, std::move(definition));
    return true;
}

Result<bool> FactorGraph::finalize() {
    order_.clear();

    // Kahn's algorithm over a std::map, so the order is deterministic: with
    // several factors simultaneously ready, the lexicographically smallest is
    // emitted first. An unordered container would give a different valid order
    // per run, and floating-point summation is not associative.
    std::map<std::string, std::size_t, std::less<>> indegree;
    std::map<std::string, std::vector<std::string>, std::less<>> dependents;

    for (const auto& [name, def] : definitions_) {
        indegree.try_emplace(name, 0);
        for (const auto& dep : def.dependencies) {
            // A dependency outside the graph is a BASE feature, supplied at
            // evaluation time. Only intra-graph edges constrain ordering.
            if (!definitions_.contains(dep)) continue;
            ++indegree[name];
            dependents[dep].push_back(name);
        }
    }

    std::vector<std::string> ready;
    for (const auto& [name, deg] : indegree) {
        if (deg == 0) ready.push_back(name);
    }
    std::sort(ready.begin(), ready.end());

    while (!ready.empty()) {
        const std::string current = ready.front();
        ready.erase(ready.begin());
        order_.push_back(current);

        const auto it = dependents.find(current);
        if (it == dependents.end()) continue;
        auto next_batch = it->second;
        std::sort(next_batch.begin(), next_batch.end());
        for (const auto& d : next_batch) {
            if (--indegree[d] == 0) {
                ready.push_back(d);
                std::sort(ready.begin(), ready.end());
            }
        }
    }

    if (order_.size() != definitions_.size()) {
        // Whatever is left has an unresolved in-edge, which means a cycle.
        // Naming the members turns an abstract failure into a fixable one.
        std::string stuck;
        for (const auto& [name, deg] : indegree) {
            if (deg > 0) {
                if (!stuck.empty()) stuck += ", ";
                stuck += name;
            }
        }
        return fail(bad("factor dependency cycle detected among: " + stuck));
    }

    finalized_ = true;
    return true;
}

std::vector<std::string> FactorGraph::dependents_of(std::string_view name) const {
    // Transitive closure. A partial update that refreshed only DIRECT
    // dependents would leave anything two hops away holding a stale value --
    // the classic invalidation bug, and invisible because the stale value is
    // still a plausible number.
    std::set<std::string> found;
    std::vector<std::string> frontier{std::string{name}};

    while (!frontier.empty()) {
        const std::string current = frontier.back();
        frontier.pop_back();
        for (const auto& [other, def] : definitions_) {
            if (found.contains(other)) continue;
            if (std::find(def.dependencies.begin(), def.dependencies.end(), current) !=
                def.dependencies.end()) {
                found.insert(other);
                frontier.push_back(other);
            }
        }
    }
    return {found.begin(), found.end()};
}

Result<std::map<std::string, double, std::less<>>> FactorGraph::evaluate(
    const std::map<std::string, double, std::less<>>& base,
    std::span<const std::string> dirty) const {
    if (!finalized_) {
        return fail(bad("factor graph must be finalized before evaluation"));
    }

    std::map<std::string, double, std::less<>> values = base;

    std::set<std::string> to_compute;
    if (dirty.empty()) {
        for (const auto& n : order_) to_compute.insert(n);
    } else {
        for (const auto& d : dirty) {
            to_compute.insert(d);
            // Everything downstream must be recomputed too, or a stale value
            // survives the update.
            for (const auto& dep : dependents_of(d)) to_compute.insert(dep);
        }
    }

    // Walk the resolved order, so a factor is never computed before its inputs.
    for (const auto& name : order_) {
        if (!to_compute.contains(name)) continue;
        const auto it = definitions_.find(name);
        if (it == definitions_.end()) continue;

        const FactorInputs inputs{values, it->second.dependencies};
        const double v = it->second.compute(inputs);
        // A factor returning a non-finite value would poison every dependent.
        // Refusing here names the culprit; letting it through would surface as
        // a NaN Sharpe with no attribution.
        if (!is_finite(v)) {
            return fail(bad("factor produced a non-finite value: " + name));
        }
        values[name] = v;
    }
    return values;
}

bool FactorGraph::contains(std::string_view name) const noexcept {
    return definitions_.find(name) != definitions_.end();
}

std::size_t FactorGraph::warmup_of(std::string_view name) const noexcept {
    const auto it = definitions_.find(name);
    return it == definitions_.end() ? 0 : it->second.warmup;
}

void FactorGraph::reset() noexcept {
    definitions_.clear();
    order_.clear();
    finalized_ = false;
}

}  // namespace ptl::features
