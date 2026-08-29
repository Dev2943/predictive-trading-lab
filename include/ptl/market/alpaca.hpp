#pragma once

/// \file alpaca.hpp
/// Alpaca market-data adapter.
///
/// Two things this adapter must get right, both of which are silent when wrong:
///
/// 1. BAR TIMESTAMP CONVENTION. Alpaca stamps minute bars with the LEFT EDGE of
///    the interval. A bar stamped 14:52:00Z covers [14:52:00, 14:53:00) and is
///    not knowable until 14:53:00. The adapter constructs every Bar through
///    Bar::from_left_edge, so the convention is applied once, here, rather than
///    trusted to every consumer.
///
/// 2. FEED PROVENANCE. Basic-plan real-time data is IEX -- a small share of
///    consolidated volume -- while historical SIP is available older than
///    fifteen minutes. The adapter declares which it is entitled to and the
///    EntitlementGate refuses to start on a mismatch, because a spread measured
///    on IEX is not the NBBO spread and no downstream code could tell.

#include <memory>
#include <string>

#include "ptl/auth/credentials.hpp"
#include "ptl/core/instrument_table.hpp"
#include "ptl/market/http.hpp"
#include "ptl/market/provider.hpp"

namespace ptl::market {

enum class AlpacaFeed { Iex, Sip };

[[nodiscard]] std::string_view to_string(AlpacaFeed f) noexcept;

struct AlpacaConfig {
    AlpacaFeed feed = AlpacaFeed::Sip;
    std::string base_url = "https://data.alpaca.markets";

    /// Basic withholds the most recent fifteen minutes. Expressed as a duration
    /// here and resolved against a supplied "now" so it behaves correctly in a
    /// replay, where now is simulated.
    Duration historical_end_delay{std::chrono::minutes{15}};

    /// True only with an Algo Trader Plus subscription. Governs the realtime
    /// capabilities the adapter claims -- and claiming one it does not have is
    /// caught by the entitlement gate at startup, not by a confusing empty
    /// result later.
    bool realtime_entitled = false;

    std::size_t page_limit = 10000;
};

class AlpacaProvider final : public IMarketDataProvider {
public:
    /// \param transport borrowed; must outlive the provider.
    /// \param instruments borrowed; symbols are interned as they are seen.
    AlpacaProvider(AlpacaConfig cfg, auth::ApiCredential credential, IHttpTransport& transport,
                   InstrumentTable& instruments);

    [[nodiscard]] const ProviderIdentity& identity() const noexcept override { return id_; }
    [[nodiscard]] CapabilitySet capabilities() const noexcept override;
    [[nodiscard]] Timestamp available_through(Timestamp now) const noexcept override;

    [[nodiscard]] Result<std::vector<Bar>> fetch_bars(const BarRequest&) override;
    [[nodiscard]] Result<std::vector<Quote>> fetch_quotes(const QuoteRequest&) override;

    /// The exact request ADR-0001 specifies for the Phase 2 entitlement gate.
    /// Exposed so `ptl_gate` issues precisely the documented call rather than
    /// an approximation of it.
    [[nodiscard]] HttpRequest entitlement_probe_request() const;

private:
    [[nodiscard]] HttpRequest make_request(std::string_view path) const;

    AlpacaConfig cfg_;
    auth::ApiCredential credential_;
    IHttpTransport* transport_;
    InstrumentTable* instruments_;
    ProviderIdentity id_;
};

/// Parse Alpaca's bars payload. Separated from the provider so the timestamp
/// convention and the error mapping can be tested directly against a fixture.
[[nodiscard]] Result<std::vector<Bar>> parse_alpaca_bars(std::string_view json, Duration timeframe,
                                                         InstrumentTable& instruments,
                                                         std::string* next_page_token);

}  // namespace ptl::market
