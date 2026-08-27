#pragma once

/// \file capability.hpp
/// What a data provider is actually entitled to serve.
///
/// ADR-0001 rests on one claim: Alpaca's Basic plan serves historical SIP bars
/// older than fifteen minutes, while real-time SIP requires a subscription. If
/// that claim is wrong, every spread and volume figure in the project comes
/// from IEX -- a small share of consolidated volume -- and the transaction-cost
/// model is built on unrepresentative data.
///
/// The failure mode this file prevents is the quiet one. A provider that
/// silently downgrades from SIP to IEX, or returns an empty result for a range
/// it is not entitled to, produces a backtest that runs to completion and
/// reports a plausible Sharpe. Capabilities are therefore declared up front and
/// checked BEFORE any data is requested.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ptl::market {

/// A capability is a (what, how far back, which feed) triple, not a boolean.
/// "Has quotes" is not a useful thing to know; "has consolidated quotes,
/// historically, at one-minute sampling" is.
enum class Capability : std::uint32_t {
    HistoricalBars = 1U << 0U,
    HistoricalTrades = 1U << 1U,
    HistoricalQuotes = 1U << 2U,
    RealtimeBars = 1U << 3U,
    RealtimeTrades = 1U << 4U,
    RealtimeQuotes = 1U << 5U,
    CorporateActions = 1U << 6U,
    /// Consolidated coverage. Its ABSENCE is the important case: a provider
    /// serving a single venue must not be mistaken for the consolidated tape.
    ConsolidatedFeed = 1U << 7U,
    /// Sampled rather than continuous top-of-book (Databento cbbo-*).
    SampledQuotes = 1U << 8U,
};

class CapabilitySet {
public:
    constexpr CapabilitySet() = default;
    constexpr CapabilitySet(std::initializer_list<Capability> caps) {
        for (const auto c : caps) bits_ |= static_cast<std::uint32_t>(c);
    }

    [[nodiscard]] constexpr bool has(Capability c) const noexcept {
        return (bits_ & static_cast<std::uint32_t>(c)) != 0U;
    }
    [[nodiscard]] constexpr bool contains(const CapabilitySet& other) const noexcept {
        return (bits_ & other.bits_) == other.bits_;
    }
    constexpr CapabilitySet& add(Capability c) noexcept {
        bits_ |= static_cast<std::uint32_t>(c);
        return *this;
    }
    [[nodiscard]] constexpr CapabilitySet missing_from(
        const CapabilitySet& required) const noexcept {
        CapabilitySet m;
        m.bits_ = required.bits_ & ~bits_;
        return m;
    }
    [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0U; }
    [[nodiscard]] constexpr std::uint32_t bits() const noexcept { return bits_; }

    [[nodiscard]] std::vector<std::string_view> names() const;
    [[nodiscard]] std::string describe() const;

private:
    std::uint32_t bits_ = 0;
};

[[nodiscard]] std::string_view to_string(Capability c) noexcept;

}  // namespace ptl::market
