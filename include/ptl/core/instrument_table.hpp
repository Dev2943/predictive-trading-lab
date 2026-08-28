#pragma once

/// \file instrument_table.hpp
/// Interns ticker strings to dense InstrumentIds.
///
/// Built once during ingest, then treated as read-only. Two properties matter
/// downstream: ids are dense from 0, so per-instrument state is a flat vector
/// indexed directly rather than a hash lookup in the simulation loop; and
/// iteration is insertion order, so nothing in the pipeline depends on hash
/// ordering. The second is a determinism requirement, not a style preference --
/// floating-point summation is not associative, so an order that varies by
/// machine produces results that vary by machine.

#include <cstddef>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ptl/core/types.hpp"

namespace ptl {

class InstrumentTable {
public:
    /// Returns the existing id if `symbol` is already known.
    InstrumentId intern(std::string_view symbol);

    [[nodiscard]] std::optional<InstrumentId> find(std::string_view symbol) const noexcept;

    /// Empty string_view if `id` did not come from this table.
    [[nodiscard]] std::string_view symbol(InstrumentId id) const noexcept;

    [[nodiscard]] bool contains(InstrumentId id) const noexcept {
        return index_of(id) < by_id_.size();
    }

    [[nodiscard]] std::size_t size() const noexcept { return by_id_.size(); }
    [[nodiscard]] bool empty() const noexcept { return by_id_.empty(); }

    /// Insertion-ordered view. Iterate this, never the map.
    [[nodiscard]] std::span<const std::string_view> all() const noexcept { return by_id_; }

    void reserve(std::size_t n);
    void clear() noexcept;

private:
    // deque, not vector: the string_views in by_id_ and in the map point into
    // this storage, and a vector<string> would invalidate every one of them on
    // reallocation -- a dangling-reference bug that only appears once the
    // universe grows past the initial capacity.
    std::deque<std::string> storage_;
    std::vector<std::string_view> by_id_;
    std::unordered_map<std::string_view, InstrumentId> index_;
};

}  // namespace ptl
