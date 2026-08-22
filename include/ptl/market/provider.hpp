#pragma once

/// \file provider.hpp
/// The data-provider seam, and the entitlement gate in front of it.

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"
#include "ptl/market/capability.hpp"
#include "ptl/market/event.hpp"

namespace ptl::market {

/// Half-open [begin, end). Half-open everywhere, so concatenating adjacent
/// ranges cannot duplicate a boundary observation.
struct TimeRange {
    Timestamp begin{kNoTimestamp};
    Timestamp end{kNoTimestamp};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return is_set(begin) && is_set(end) && begin < end;
    }
    [[nodiscard]] constexpr bool contains(Timestamp ts) const noexcept {
        return ts >= begin && ts < end;
    }
};

struct BarRequest {
    std::vector<InstrumentId> instruments;
    TimeRange range;
    Duration timeframe{std::chrono::minutes{1}};
};

struct QuoteRequest {
    std::vector<InstrumentId> instruments;
    TimeRange range;
    /// Sampling interval for cbbo-style schemas; zero means every update.
    Duration sampling{std::chrono::minutes{1}};
};

/// Provenance, persisted verbatim into the run manifest so a result set records
/// exactly which feed and schema produced it.
struct ProviderIdentity {
    std::string name;      // "alpaca", "databento", "replay"
    std::string feed;      // "sip", "iex", ""
    std::string schema;    // "ohlcv-1m", "cbbo-1m"
    std::string coverage;  // "consolidated_sip_historical"
    std::string dataset;   // vendor dataset id, when applicable
};

/// A source of market data. Deliberately narrow: providers fetch and normalise,
/// nothing else. They do not validate business rules, generate features, or
/// know what a strategy is.
class IMarketDataProvider {
public:
    IMarketDataProvider() = default;
    virtual ~IMarketDataProvider() = default;
    IMarketDataProvider(const IMarketDataProvider&) = delete;
    IMarketDataProvider& operator=(const IMarketDataProvider&) = delete;
    IMarketDataProvider(IMarketDataProvider&&) = delete;
    IMarketDataProvider& operator=(IMarketDataProvider&&) = delete;

    [[nodiscard]] virtual const ProviderIdentity& identity() const noexcept = 0;

    /// What this provider is entitled to, as configured. Declared, then
    /// checked -- see EntitlementGate.
    [[nodiscard]] virtual CapabilitySet capabilities() const noexcept = 0;

    /// Data at or after this instant is withheld. Alpaca Basic withholds the
    /// most recent fifteen minutes; a provider with no such limit returns
    /// kMaxTimestamp. Expressed as an instant rather than a duration so it is
    /// meaningful in a replay, where "now" is simulated.
    [[nodiscard]] virtual Timestamp available_through(Timestamp now) const noexcept = 0;

    /// Chronologically ordered bars. The provider is responsible for the
    /// left-edge/right-edge convention of its own vendor; callers receive Bars
    /// whose timestamps already mean what they say.
    [[nodiscard]] virtual Result<std::vector<Bar>> fetch_bars(const BarRequest&) = 0;

    [[nodiscard]] virtual Result<std::vector<Quote>> fetch_quotes(const QuoteRequest&) = 0;
};

// ---------------------------------------------------------------------------
// Entitlement gate
// ---------------------------------------------------------------------------

/// What a phase of work needs from its provider.
struct EntitlementRequest {
    CapabilitySet required;
    TimeRange range;
    /// Simulated or wall-clock "now", used to evaluate available_through.
    Timestamp now{kNoTimestamp};
    std::string purpose;  // appears in the failure message and the manifest
};

struct EntitlementReport {
    bool granted = false;
    ProviderIdentity provider;
    CapabilitySet required;
    CapabilitySet available;
    CapabilitySet missing;
    Timestamp available_through{kNoTimestamp};
    std::string detail;

    [[nodiscard]] std::string describe() const;
};

/// Checks a provider against a requirement BEFORE any data is requested.
///
/// Fail-fast is the entire point. A provider that quietly serves a narrower
/// feed, or returns nothing for a range it is not entitled to, yields a
/// backtest that completes and reports a plausible number. Refusing to start is
/// the only safe behaviour, and the report explains precisely what was missing
/// rather than saying "request failed".
class EntitlementGate {
public:
    [[nodiscard]] static EntitlementReport evaluate(const IMarketDataProvider& provider,
                                                    const EntitlementRequest& request);

    /// Same check, as a hard failure. Call this at startup.
    [[nodiscard]] static Result<EntitlementReport> enforce(const IMarketDataProvider& provider,
                                                           const EntitlementRequest& request);
};

}  // namespace ptl::market
