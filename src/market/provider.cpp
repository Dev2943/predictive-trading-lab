#include "ptl/market/provider.hpp"

#include <array>

namespace ptl::market {
namespace {

constexpr std::array<Capability, 9> kAllCapabilities{
    Capability::HistoricalBars,   Capability::HistoricalTrades, Capability::HistoricalQuotes,
    Capability::RealtimeBars,     Capability::RealtimeTrades,   Capability::RealtimeQuotes,
    Capability::CorporateActions, Capability::ConsolidatedFeed, Capability::SampledQuotes};

}  // namespace

std::string_view to_string(Capability c) noexcept {
    switch (c) {
        case Capability::HistoricalBars:
            return "historical_bars";
        case Capability::HistoricalTrades:
            return "historical_trades";
        case Capability::HistoricalQuotes:
            return "historical_quotes";
        case Capability::RealtimeBars:
            return "realtime_bars";
        case Capability::RealtimeTrades:
            return "realtime_trades";
        case Capability::RealtimeQuotes:
            return "realtime_quotes";
        case Capability::CorporateActions:
            return "corporate_actions";
        case Capability::ConsolidatedFeed:
            return "consolidated_feed";
        case Capability::SampledQuotes:
            return "sampled_quotes";
    }
    return "unknown";
}

std::vector<std::string_view> CapabilitySet::names() const {
    std::vector<std::string_view> out;
    for (const auto c : kAllCapabilities) {
        if (has(c)) out.push_back(to_string(c));
    }
    return out;
}

std::string CapabilitySet::describe() const {
    const auto n = names();
    if (n.empty()) return "(none)";
    std::string out;
    for (std::size_t i = 0; i < n.size(); ++i) {
        if (i != 0) out += ", ";
        out += n[i];
    }
    return out;
}

std::string EntitlementReport::describe() const {
    std::string out;
    out += granted ? "entitlement granted" : "ENTITLEMENT DENIED";
    out += " for provider '";
    out += provider.name;
    if (!provider.feed.empty()) {
        out += "/";
        out += provider.feed;
    }
    out += "' (coverage: ";
    out += provider.coverage.empty() ? "unspecified" : provider.coverage;
    out += ")";
    if (!granted) {
        out += "\n  required : " + required.describe();
        out += "\n  available: " + available.describe();
        if (!missing.empty()) out += "\n  MISSING  : " + missing.describe();
    }
    if (!detail.empty()) out += "\n  " + detail;
    return out;
}

EntitlementReport EntitlementGate::evaluate(const IMarketDataProvider& provider,
                                            const EntitlementRequest& request) {
    EntitlementReport r;
    r.provider = provider.identity();
    r.required = request.required;
    r.available = provider.capabilities();
    r.missing = r.available.missing_from(r.required);
    r.available_through = provider.available_through(request.now);

    if (!r.missing.empty()) {
        r.granted = false;
        r.detail = "provider cannot serve: " + r.missing.describe();
        if (!r.available.has(Capability::ConsolidatedFeed) &&
            r.required.has(Capability::ConsolidatedFeed)) {
            // Named explicitly because this is the failure that would otherwise
            // be invisible: single-venue data looks exactly like consolidated
            // data until you compare a spread against the NBBO.
            r.detail +=
                " -- this provider serves a single venue, not the consolidated tape. "
                "Spreads and volumes from it are not representative and must not be "
                "used for transaction-cost modelling.";
        }
        return r;
    }

    // Range availability. A provider entitled to the right CAPABILITIES may
    // still be barred from the requested WINDOW: Alpaca Basic withholds the
    // most recent fifteen minutes.
    if (request.range.valid() && is_set(r.available_through) &&
        request.range.end > r.available_through) {
        r.granted = false;
        r.detail = "requested range ends at " + to_iso8601(request.range.end) +
                   " but this provider is only entitled through " + to_iso8601(r.available_through);
        return r;
    }

    r.granted = true;
    return r;
}

Result<EntitlementReport> EntitlementGate::enforce(const IMarketDataProvider& provider,
                                                   const EntitlementRequest& request) {
    const EntitlementReport r = evaluate(provider, request);
    if (!r.granted) {
        // Refusing to start is the only safe behaviour. A provider that quietly
        // serves a narrower feed produces a backtest that completes and reports
        // a plausible number, which is far worse than a startup failure.
        return fail(make_error(ErrorCode::ValidationFailed, r.describe(), request.purpose));
    }
    return r;
}

}  // namespace ptl::market
