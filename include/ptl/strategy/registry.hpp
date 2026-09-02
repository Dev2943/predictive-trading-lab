#pragma once

/// \file registry.hpp
/// The strategy catalogue: what strategies exist, at what version, needing what.
///
/// WHAT THIS IS NOT. It is not `engine::IStrategy`, which is the RUNTIME
/// interface a strategy implements to receive bars. This is the CATALOGUE
/// entry: identity, version, authorship, parameter schema, declared
/// dependencies. One describes behaviour, the other describes provenance, and
/// a platform running hundreds of strategies needs both.
///
/// THE REGISTRY MUST NEVER KNOW ABOUT EXECUTION (Phase 13 design constraint).
/// Nothing in this header includes the engine, the OMS or the broker, and the
/// library links only ptl::core. A catalogue that could reach the venue would
/// be a catalogue that could trade, and the whole point of separating them is
/// that listing a strategy is not running one.
///
/// NO SINGLETON. A registry is an object the caller owns. A global one would
/// make the set of available strategies depend on static initialisation order
/// and share mutable state between concurrent experiments -- both forbidden.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"

namespace ptl::strategy {

/// A strategy's stable identity, independent of version.
///
/// A string rather than an integer: strategy names outlive any numbering
/// scheme, appear in configuration files written by humans, and must survive
/// being copied between repositories. An integer id would have to be minted
/// somewhere, and that somewhere becomes a global.
class StrategyId {
public:
    StrategyId() = default;

    /// Rejects anything that would be ambiguous in a filename, a config key or
    /// a JSON field. Permissive validation here means every downstream layer
    /// can assume the id is safe to embed.
    [[nodiscard]] static Result<StrategyId> create(std::string value);

    [[nodiscard]] const std::string& value() const noexcept { return value_; }
    [[nodiscard]] bool empty() const noexcept { return value_.empty(); }
    [[nodiscard]] std::uint64_t hash() const noexcept;

    friend bool operator==(const StrategyId&, const StrategyId&) noexcept = default;
    friend auto operator<=>(const StrategyId&, const StrategyId&) noexcept = default;

private:
    explicit StrategyId(std::string value) : value_(std::move(value)) {}
    std::string value_;
};

/// Semantic version.
///
/// Ordered, so "is this newer?" is a comparison rather than a string sort --
/// under which 1.10.0 precedes 1.9.0 and a rollback silently picks the wrong
/// artifact.
struct StrategyVersion {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;

    [[nodiscard]] static Result<StrategyVersion> parse(std::string_view);
    [[nodiscard]] std::string to_string() const;

    /// A major bump means the strategy's behaviour changed incompatibly, so
    /// results either side of one must not be pooled.
    [[nodiscard]] bool compatible_with(const StrategyVersion& other) const noexcept {
        return major == other.major;
    }

    friend auto operator<=>(const StrategyVersion&, const StrategyVersion&) noexcept = default;
    friend bool operator==(const StrategyVersion&, const StrategyVersion&) noexcept = default;
};

/// Where a strategy is in its lifecycle.
///
/// Explicit rather than a boolean pair, because "can this run in production?"
/// and "should new experiments use it?" are different questions and a
/// deprecated strategy answers them differently.
enum class StrategyState : std::uint8_t {
    Draft,       ///< under development; not runnable in production
    Research,    ///< runnable in experiments only
    Candidate,   ///< passed research gates, awaiting promotion
    Production,  ///< live-eligible
    Deprecated,  ///< runnable for reproduction only; no new experiments
    Retired,     ///< not runnable at all
};

[[nodiscard]] std::string_view to_string(StrategyState) noexcept;
/// Whether a new experiment may be started against this state.
[[nodiscard]] bool accepts_new_experiments(StrategyState) noexcept;

/// One declared parameter.
struct ParameterSpec {
    std::string name;
    std::string type;  ///< "double", "int", "bool", "string"
    std::string description;
    bool required = false;
    std::string default_value;

    [[nodiscard]] Result<bool> validate_value(std::string_view) const;
};

/// Human and machine metadata about a strategy.
struct StrategyMetadata {
    std::string author;
    std::string description;
    /// Free-form tags for search: "mean-reversion", "intraday", "equities".
    std::vector<std::string> tags;
    Timestamp created_at{kNoTimestamp};
    /// Asset classes and data the strategy needs. Declared rather than
    /// discovered, so a missing feed is a startup error and not a silent
    /// stream of empty features.
    std::vector<std::string> required_data;
    std::vector<std::string> required_features;

    [[nodiscard]] std::string describe() const;
};

/// A complete catalogue entry.
struct StrategyDescriptor {
    StrategyId id;
    StrategyVersion version;
    StrategyState state{StrategyState::Draft};
    StrategyMetadata metadata;
    std::vector<ParameterSpec> parameters;

    /// Other strategies this one composes. Validated for cycles on
    /// registration: a dependency loop would make instantiation infinite.
    std::vector<StrategyId> depends_on;

    /// Fingerprint over identity, version and parameter schema. Two
    /// descriptors with the same fingerprint are interchangeable; a change to
    /// any parameter's name or type changes it, which is what stops an
    /// experiment being compared against a differently-shaped predecessor.
    [[nodiscard]] std::uint64_t fingerprint() const;
    [[nodiscard]] Result<bool> validate() const;
    [[nodiscard]] std::string describe() const;

    /// Deterministic JSON. Ordered fields, fixed precision -- the Phase 10
    /// property, preserved so a manifest can be diffed between runs.
    [[nodiscard]] std::string to_json() const;
};

/// A registered strategy, ready to be listed or instantiated.
///
/// The factory is deliberately type-erased to `void*`-free `std::any`-free
/// form: it returns nothing here. Instantiation belongs to whoever owns the
/// runtime types, and a catalogue that constructed engine strategies would
/// have to include the engine -- exactly the dependency this module forbids.
using ParameterMap = std::map<std::string, std::string, std::less<>>;

/// Validates a parameter set against a descriptor's schema.
[[nodiscard]] Result<bool> validate_parameters(const StrategyDescriptor&, const ParameterMap&);

/// The catalogue.
class StrategyRegistry {
public:
    /// Register a descriptor. Refuses a duplicate (id, version) pair: two
    /// definitions of one version means a lookup would have to choose, and
    /// choosing silently is how an experiment runs code nobody intended.
    [[nodiscard]] Result<bool> register_strategy(StrategyDescriptor);

    /// Exact lookup.
    [[nodiscard]] const StrategyDescriptor* find(const StrategyId&,
                                                 const StrategyVersion&) const noexcept;
    /// Highest registered version of a strategy.
    [[nodiscard]] const StrategyDescriptor* latest(const StrategyId&) const noexcept;
    /// Highest version within the same major, for a compatible upgrade.
    [[nodiscard]] const StrategyDescriptor* latest_compatible(
        const StrategyId&, const StrategyVersion&) const noexcept;

    [[nodiscard]] std::vector<StrategyId> ids() const;
    [[nodiscard]] std::vector<StrategyVersion> versions_of(const StrategyId&) const;
    [[nodiscard]] std::vector<const StrategyDescriptor*> with_state(StrategyState) const;
    [[nodiscard]] std::vector<const StrategyDescriptor*> with_tag(std::string_view) const;

    /// Move a strategy through its lifecycle. Illegal transitions are refused:
    /// a retired strategy cannot return to production without an explicit
    /// re-registration, which leaves a trace.
    [[nodiscard]] Result<bool> transition(const StrategyId&, const StrategyVersion&, StrategyState);

    /// Resolve a strategy's full dependency closure, in topological order.
    /// Refuses on a cycle or a missing dependency.
    [[nodiscard]] Result<std::vector<StrategyId>> resolve_dependencies(
        const StrategyId&, const StrategyVersion&) const;

    [[nodiscard]] std::size_t size() const noexcept { return descriptors_.size(); }
    [[nodiscard]] bool empty() const noexcept { return descriptors_.empty(); }

    /// Whole-catalogue manifest, deterministic.
    [[nodiscard]] std::string to_json() const;
    void clear() noexcept;

private:
    /// Keyed by (id, version) so ordering is by name then version, and any
    /// listing is reproducible.
    using Key = std::pair<std::string, StrategyVersion>;
    std::map<Key, StrategyDescriptor> descriptors_;
};

}  // namespace ptl::strategy
