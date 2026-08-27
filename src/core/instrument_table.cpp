#include "ptl/core/instrument_table.hpp"

namespace ptl {

InstrumentId InstrumentTable::intern(std::string_view symbol) {
    if (const auto it = index_.find(symbol); it != index_.end()) return it->second;

    const auto id = static_cast<InstrumentId>(static_cast<std::uint32_t>(by_id_.size()));
    // Own the characters first: the views below must point at stable storage.
    const std::string& owned = storage_.emplace_back(symbol);
    const std::string_view view{owned};
    by_id_.push_back(view);
    index_.emplace(view, id);
    return id;
}

std::optional<InstrumentId> InstrumentTable::find(std::string_view symbol) const noexcept {
    const auto it = index_.find(symbol);
    if (it == index_.end()) return std::nullopt;
    return it->second;
}

std::string_view InstrumentTable::symbol(InstrumentId id) const noexcept {
    const auto i = index_of(id);
    if (i >= by_id_.size()) return {};
    return by_id_[i];
}

void InstrumentTable::reserve(std::size_t n) {
    by_id_.reserve(n);
    index_.reserve(n);
}

void InstrumentTable::clear() noexcept {
    index_.clear();
    by_id_.clear();
    storage_.clear();
}

}  // namespace ptl
