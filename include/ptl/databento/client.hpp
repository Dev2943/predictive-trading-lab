#pragma once

/// \file client.hpp
/// Databento provider: capabilities, cost guard, and the entitlement gate.
///
/// ADR-0001 imposes a HARD SPEND GUARD: no historical download may proceed
/// without first calling the vendor's cost estimator and comparing it against a
/// configured maximum. That guard is implemented here as a refusal, not a
/// warning, because a warning in a build log is a warning nobody reads and the
/// consequence is a real invoice.

#include <cstdint>
#include <string>
#include <vector>

#include "ptl/auth/credentials.hpp"
#include "ptl/core/instrument_table.hpp"
#include "ptl/core/result.hpp"
#include "ptl/databento/decoder.hpp"
#include "ptl/market/http.hpp"
#include "ptl/market/provider.hpp"

namespace ptl::databento {

struct DatabentoConfig {
    std::string base_url = "https://hist.databento.com";
    /// Dataset code, e.g. "XNAS.ITCH" or "EQUS.SUMMARY".
    std::string dataset = "EQUS.SUMMARY";
    Schema schema{Schema::Cbbo1m};

    /// ADR-0001 spend guard. A cost estimate above this refuses the download.
    double max_spend_usd = 25.00;
    /// Explicit override, which must be set deliberately and is recorded in the
    /// run manifest.
    bool allow_spend_override = false;

    /// Databento historical data has no real-time component on this plan.
    bool realtime_entitled = false;
};

/// A cost estimate from the vendor's estimator.
struct CostEstimate {
    double usd = 0.0;
    std::uint64_t bytes = 0;
    std::uint64_t record_count = 0;
    bool estimated = false;
};

class DatabentoProvider final : public market::IMarketDataProvider {
public:
    DatabentoProvider(DatabentoConfig cfg, auth::ApiCredential credential,
                      market::IHttpTransport& transport, InstrumentTable& instruments,
                      const market::SymbolMapper& mapper);

    [[nodiscard]] const market::ProviderIdentity& identity() const noexcept override { return id_; }
    [[nodiscard]] market::CapabilitySet capabilities() const noexcept override;
    [[nodiscard]] Timestamp available_through(Timestamp now) const noexcept override;

    [[nodiscard]] Result<std::vector<market::Bar>> fetch_bars(const market::BarRequest&) override;
    [[nodiscard]] Result<std::vector<market::Quote>> fetch_quotes(
        const market::QuoteRequest&) override;

    /// Ask the vendor what a range would cost.
    ///
    /// The returned estimate is AUTHORITATIVE per ADR-0001: no per-GB
    /// assumption is hardcoded anywhere, because a stale assumption would make
    /// the guard meaningless precisely when prices changed.
    [[nodiscard]] Result<CostEstimate> estimate_cost(const market::QuoteRequest&) const;

    /// Refuse the download when the estimate exceeds the configured maximum.
    [[nodiscard]] Result<CostEstimate> enforce_spend_guard(const CostEstimate&) const;

    /// The schema-availability probe ADR-0001 Addendum A5 requires before
    /// Phase 8 ingest. Exposed so `ptl_gate` issues exactly the documented call.
    [[nodiscard]] market::HttpRequest schema_probe_request() const;

    [[nodiscard]] const DatabentoConfig& config() const noexcept { return cfg_; }

private:
    [[nodiscard]] market::HttpRequest make_request(std::string_view path) const;

    DatabentoConfig cfg_;
    auth::ApiCredential credential_;
    market::IHttpTransport* transport_;
    InstrumentTable* instruments_;
    const market::SymbolMapper* mapper_;
    market::ProviderIdentity id_;
};

}  // namespace ptl::databento
