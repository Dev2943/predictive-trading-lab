#pragma once

/// \file factor.hpp
/// The factor framework: named computations with declared dependencies.
///
/// A factor is a reusable derived value that may depend on other factors. The
/// graph is resolved ONCE at registration into a topological order, and
/// evaluation then walks that order -- so a factor is never computed before its
/// inputs, and never twice per bar.
///
/// Cycles are rejected at registration, not discovered at evaluation. A cyclic
/// dependency found mid-run would either recurse until the stack died or, worse,
/// silently use a stale value from the previous bar.

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"

namespace ptl::features {

/// Values visible to a factor during evaluation: its declared inputs only.
class FactorInputs {
public:
    FactorInputs(const std::map<std::string, double, std::less<>>& values,
                 std::span<const std::string> allowed) noexcept
        : values_(&values), allowed_(allowed) {}

    /// \returns the input value, or an error when `name` was not DECLARED as a
    ///          dependency. Reading an undeclared input would work by accident
    ///          whenever evaluation order happened to cooperate, and break
    ///          silently when it did not.
    [[nodiscard]] Result<double> get(std::string_view name) const;

    [[nodiscard]] bool declared(std::string_view name) const noexcept;

private:
    const std::map<std::string, double, std::less<>>* values_;
    std::span<const std::string> allowed_;
};

using FactorFn = std::function<double(const FactorInputs&)>;

struct FactorDefinition {
    std::string name;
    std::vector<std::string> dependencies;
    FactorFn compute;
    /// Bars of history the factor needs before its output is meaningful.
    std::size_t warmup = 0;
};

/// Registry plus dependency graph.
class FactorGraph {
public:
    /// Registers a factor. Duplicates are refused: silently replacing would
    /// make the active definition depend on registration order.
    [[nodiscard]] Result<bool> add(FactorDefinition definition);

    /// Resolve the topological order. Must be called before evaluate(); returns
    /// an error naming the cycle or the missing dependency.
    [[nodiscard]] Result<bool> finalize();

    /// Evaluate every factor for one bar, given the base feature values.
    ///
    /// \param base   values not produced by the graph (raw features)
    /// \param dirty  factors to recompute; empty means all. Anything downstream
    ///               of a dirty factor is recomputed too -- that transitive
    ///               closure is the invalidation rule, and computing it from
    ///               the graph is what stops a stale value surviving a partial
    ///               update.
    [[nodiscard]] Result<std::map<std::string, double, std::less<>>> evaluate(
        const std::map<std::string, double, std::less<>>& base,
        std::span<const std::string> dirty = {}) const;

    [[nodiscard]] std::span<const std::string> evaluation_order() const noexcept { return order_; }
    [[nodiscard]] bool finalized() const noexcept { return finalized_; }
    [[nodiscard]] std::size_t size() const noexcept { return definitions_.size(); }
    [[nodiscard]] bool contains(std::string_view name) const noexcept;
    [[nodiscard]] std::size_t warmup_of(std::string_view name) const noexcept;

    /// Every factor that depends on `name`, directly or transitively.
    [[nodiscard]] std::vector<std::string> dependents_of(std::string_view name) const;

    void reset() noexcept;

private:
    std::map<std::string, FactorDefinition, std::less<>> definitions_;
    std::vector<std::string> order_;
    bool finalized_ = false;
};

}  // namespace ptl::features
