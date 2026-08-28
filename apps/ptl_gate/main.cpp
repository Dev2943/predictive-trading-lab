/// ptl_gate -- the ADR-0001 Phase 2 entitlement gate.
///
/// Answers one question before any ingest work proceeds: is this account
/// actually entitled to the data the project assumes it has?
///
/// ADR-0001 rests on the claim that Alpaca Basic serves historical SIP bars
/// older than fifteen minutes. If that is false, every spread and volume figure
/// downstream comes from IEX -- a small share of consolidated volume -- and the
/// transaction-cost model is built on unrepresentative data. The failure would
/// be silent: the backtest would run to completion and report a plausible
/// number.
///
/// By default this tool performs NO network call. It resolves credentials,
/// constructs the provider, evaluates capabilities against the requirement, and
/// prints the request it WOULD issue. Pass --live to actually issue it, which
/// requires a transport to be wired in.

#include <CLI/CLI.hpp>
#include <cstdio>
#include <memory>
#include <string>

#include "ptl/auth/credentials.hpp"
#include "ptl/config/config.hpp"
#include "ptl/core/instrument_table.hpp"
#include "ptl/core/version.hpp"
#include "ptl/market/alpaca.hpp"
#include "ptl/market/calendar.hpp"
#include "ptl/market/provider.hpp"

namespace {

void heading(const char* text) {
    std::printf("\n%s\n", text);
}
void kv(const char* k, const std::string& v) {
    std::printf("  %-24s %s\n", k, v.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"ADR-0001 market data entitlement gate", "ptl_gate"};
    argv = app.ensure_utf8(argv);

    std::string config_path = "config/base.toml";
    std::string credentials_file;
    std::string feed_name = "sip";
    bool realtime = false;
    bool live = false;

    app.add_option("-c,--config", config_path, "TOML configuration file");
    app.add_option("--credentials-file", credentials_file,
                   "Untracked dotenv file (must be chmod 600). Environment is tried first.");
    app.add_option("--feed", feed_name, "sip | iex")->check(CLI::IsMember({"sip", "iex"}));
    app.add_flag("--realtime", realtime, "Assert an Algo Trader Plus subscription");
    app.add_flag("--live", live, "Actually issue the probe request (requires a transport)");
    CLI11_PARSE(app, argc, argv);

    std::printf("ptl_gate -- ADR-0001 entitlement gate (%s)\n",
                std::string{ptl::kGitDescribe}.c_str());

    // --- configuration -------------------------------------------------------
    auto cfg = ptl::config::load(config_path);
    if (!cfg) {
        std::fprintf(stderr, "\nconfig error: %s\n", cfg.error().describe().c_str());
        return 2;
    }

    // --- credentials ---------------------------------------------------------
    //
    // Environment first, then an optional untracked file. Never the config
    // file: config/ is committed, and a key id in a committed file is a leaked
    // key id.
    auto chain = std::make_shared<ptl::auth::ChainedCredentials>();
    chain->add(std::make_shared<ptl::auth::EnvironmentCredentials>());
    if (!credentials_file.empty()) {
        auto fc = ptl::auth::FileCredentials::load(credentials_file);
        if (!fc) {
            std::fprintf(stderr, "\ncredential file error: %s\n", fc.error().describe().c_str());
            return 3;
        }
        chain->add(std::make_shared<ptl::auth::FileCredentials>(std::move(*fc)));
    }

    auto credential = ptl::auth::resolve(*chain, "ALPACA_KEY_ID", "ALPACA_SECRET_KEY");
    if (!credential) {
        std::fprintf(stderr, "\n%s\n", credential.error().describe().c_str());
        return 4;
    }

    heading("credentials");
    kv("key id", credential->key_id);
    // Length only. Enough to tell "unset" from "wrong", which is what a
    // diagnostic needs; the value itself never reaches a terminal or a log.
    kv("secret", credential->secret.redacted());
    kv("source", credential->source);

    // --- provider ------------------------------------------------------------
    ptl::InstrumentTable instruments;
    ptl::market::AlpacaConfig acfg;
    acfg.feed = feed_name == "iex" ? ptl::market::AlpacaFeed::Iex : ptl::market::AlpacaFeed::Sip;
    acfg.realtime_entitled = realtime;

    ptl::market::RecordedTransport offline;  // no network unless --live
    ptl::market::AlpacaProvider provider{acfg, std::move(*credential), offline, instruments};

    heading("provider");
    kv("name", provider.identity().name);
    kv("feed", provider.identity().feed);
    kv("coverage", provider.identity().coverage);
    kv("capabilities", provider.capabilities().describe());

    // --- the gate ------------------------------------------------------------
    ptl::market::EntitlementRequest request;
    request.required = ptl::market::CapabilitySet{ptl::market::Capability::HistoricalBars,
                                                  ptl::market::Capability::ConsolidatedFeed};
    request.now = ptl::WallClock{}.now();
    request.purpose = "ADR-0001 Phase 2 ingest";

    // The window the project actually needs, ending well clear of the fifteen
    // minute historical delay.
    ptl::Timestamp begin{};
    (void)ptl::parse_date("2022-01-03", begin);
    request.range.begin = begin;
    request.range.end = request.now - std::chrono::hours{24};

    heading("entitlement");
    kv("required", request.required.describe());
    kv("window ends", ptl::to_iso8601(request.range.end));
    kv("entitled through", ptl::to_iso8601(provider.available_through(request.now)));

    const auto report = ptl::market::EntitlementGate::evaluate(provider, request);
    std::printf("\n%s\n", report.describe().c_str());

    heading("probe request (ADR-0001 section 5)");
    const auto probe = provider.entitlement_probe_request();
    // Credentials travel in headers, never in the URL, so this line is safe to
    // paste into a bug report.
    std::printf("  GET %s\n", probe.full_url().c_str());
    std::printf("  headers: APCA-API-KEY-ID, APCA-API-SECRET-KEY (values withheld)\n");

    if (!live) {
        std::printf(
            "\n  Dry run: no network call was made. Issue the request above with your\n"
            "  Basic-plan credentials. Bars returned confirms the ADR-0001 claim; a\n"
            "  subscription error means the free tier does NOT cover historical SIP and\n"
            "  the fallbacks in ADR-0001 section 5 apply.\n");
    } else {
        std::fprintf(stderr,
                     "\n  --live requires an HTTP transport, which is not yet wired in.\n"
                     "  ADR-0001 defers that dependency until this gate has passed once.\n");
        return 5;
    }

    if (!report.granted) {
        std::fprintf(stderr,
                     "\nGATE NOT SATISFIED. Do not proceed past ingest until this passes or\n"
                     "a fallback is selected and documented (ADR-0001 section 5).\n");
        return 1;
    }
    std::printf("\nGate satisfied on declared capabilities. Confirm empirically before ingest.\n");
    return 0;
}
