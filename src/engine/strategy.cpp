#include "ptl/engine/strategy.hpp"

namespace ptl::engine {

Result<bool> StrategyRegistry::register_strategy(std::string name, StrategyFactory factory) {
    if (name.empty()) {
        return fail(make_error(ErrorCode::InvalidArgument, "strategy name cannot be empty"));
    }
    if (factory == nullptr) {
        return fail(make_error(ErrorCode::InvalidArgument, "strategy factory is null"));
    }
    if (factories_.contains(name)) {
        // Silently replacing would make the active strategy depend on
        // registration order, which is exactly the hidden non-determinism a
        // registry is supposed to remove.
        return fail(make_error(ErrorCode::InvalidArgument, "strategy already registered: " + name));
    }
    factories_.emplace(std::move(name), std::move(factory));
    return true;
}

Result<std::unique_ptr<IStrategy>> StrategyRegistry::create(std::string_view name,
                                                            const std::string& config_toml) const {
    const auto it = factories_.find(name);
    if (it == factories_.end()) {
        std::string msg = "unknown strategy '" + std::string{name} + "'. Registered: ";
        bool first = true;
        for (const auto& [k, v] : factories_) {
            if (!first) msg += ", ";
            first = false;
            msg += k;
        }
        if (factories_.empty()) msg += "(none)";
        return fail(make_error(ErrorCode::NotFound, std::move(msg)));
    }
    return it->second(config_toml);
}

bool StrategyRegistry::contains(std::string_view name) const noexcept {
    return factories_.find(name) != factories_.end();
}

std::vector<std::string_view> StrategyRegistry::names() const {
    std::vector<std::string_view> out;
    out.reserve(factories_.size());
    for (const auto& [k, v] : factories_) out.emplace_back(k);
    return out;
}

}  // namespace ptl::engine
