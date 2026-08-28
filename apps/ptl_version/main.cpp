/// ptl_version -- build provenance and configuration probe.
///
/// Small, but not a toy. It prints exactly the facts that make a result
/// reproducible: git commit, compiler, build type, resolved config hash and
/// RunId, and which optional C++23 library features this toolchain actually
/// has. When someone asks "which build produced results/a3f9.../?", this is
/// how they find out, and it exercises every Phase 1 module end to end.

#include <CLI/CLI.hpp>
#include <cstdio>
#include <string>
#include <vector>

#include "ptl/config/config.hpp"
#include "ptl/core/clock.hpp"
#include "ptl/core/compiler.hpp"
#include "ptl/core/rng.hpp"
#include "ptl/core/time.hpp"
#include "ptl/core/types.hpp"
#include "ptl/core/version.hpp"
#include "ptl/experiments/registry.hpp"
#include "ptl/log/logger.hpp"

namespace {

void print_kv(const char* k, const std::string& v) {
    std::printf("  %-22s %s\n", k, v.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"predictive-trading-lab build and configuration probe", "ptl_version"};
    argv = app.ensure_utf8(argv);

    std::string config_path;
    std::vector<std::string> overrides;
    std::string registry_path;
    bool json_log = false;

    app.add_option("-c,--config", config_path, "TOML configuration file")->check(CLI::ExistingFile);
    app.add_option("--set", overrides, "Override a config value: --set run.seed=42 (repeatable)");
    app.add_option("--registry", registry_path,
                   "Register this invocation in the experiment registry at PATH");
    app.add_flag("--json-log", json_log, "Emit structured JSONL logs to stdout");

    CLI11_PARSE(app, argc, argv);

    std::printf("predictive-trading-lab %s\n\n", std::string{ptl::kVersion}.c_str());

    std::puts("build");
    print_kv("git", std::string{ptl::kGitDescribe} + " (" + std::string{ptl::kGitDirty} + ")");
    print_kv("commit", std::string{ptl::kGitSha});
    print_kv("compiler", std::string{ptl::kCompilerId} + " " + std::string{ptl::kCompilerVer});
    print_kv("build type", std::string{ptl::kBuildType});

    std::puts("\ntoolchain capability probes");
    print_kv("std::expected", PTL_HAS_STD_EXPECTED ? "yes" : "no (using ptl::Result fallback)");
    print_kv("std::format", PTL_HAS_STD_FORMAT ? "yes" : "no");
    print_kv("chrono tzdb", PTL_HAS_CHRONO_TZDB ? "present (still unused by design -- ADR-0001 A1)"
                                                : "absent (as expected; engine is UTC-only)");

    // The chain is the project's central invariant, so the probe demonstrates
    // it rather than merely describing it: a decision that fills at its own
    // timestamp is rejected here exactly as it would be in the engine.
    std::puts("\npoint-in-time chain self-check");
    {
        ptl::Timestamp t0{};
        (void)ptl::parse_timestamp("2024-01-02T14:52:00Z", t0);

        ptl::LifecycleTimes good;
        good.exchange_time = t0;
        good.receive_time = t0 + std::chrono::milliseconds{2};
        good.feature_end_time = t0 + std::chrono::seconds{60};
        good.decision_time = t0 + std::chrono::seconds{60};
        good.arrival_time = t0 + std::chrono::seconds{61};
        good.fill_time = t0 + std::chrono::seconds{61};
        const auto v1 = ptl::validate_chain(good);
        print_kv("well-ordered chain", v1 ? ("REJECTED: " + v1->describe()) : "accepted");

        ptl::LifecycleTimes same_bar = good;
        same_bar.arrival_time = same_bar.decision_time;  // decide and fill at one instant
        const auto v2 = ptl::validate_chain(same_bar);
        print_kv("same-bar execution",
                 v2 ? ("rejected: " + v2->describe()) : "ACCEPTED -- INVARIANT BROKEN");
    }

    if (!config_path.empty()) {
        auto cfg = ptl::config::load(config_path, overrides);
        if (!cfg) {
            std::fprintf(stderr, "\nconfig error: %s\n", cfg.error().describe().c_str());
            return 2;
        }
        ptl::log::Config lc;
        lc.level = cfg->log.level;
        lc.console = cfg->log.console;
        lc.json = json_log || cfg->log.json;
        lc.file = cfg->log.file;
        ptl::log::init(lc);

        const auto run_id = ptl::config::make_run_id(cfg->canonical, "no-data-manifest",
                                                     ptl::kGitSha, cfg->run.seed);
        char hashbuf[32];
        std::snprintf(hashbuf, sizeof(hashbuf), "%016llx",
                      static_cast<unsigned long long>(cfg->hash()));

        std::puts("\nconfiguration");
        print_kv("source", cfg->source_path);
        print_kv("config hash", hashbuf);
        print_kv("run id", run_id.hex());
        print_kv("seed", std::to_string(cfg->run.seed));
        print_kv("holdout boundary",
                 cfg->holdout.boundary_date.empty() ? "(not yet set)" : cfg->holdout.boundary_date);
        print_kv("holdout", cfg->holdout.unlocked ? "UNLOCKED" : "locked");
        print_kv("session", cfg->session.regular_open + "-" + cfg->session.regular_close +
                                " (length is calendar-driven; Phase 2)");
        print_kv("t1", cfg->t1.provider + " " + cfg->t1.feed + " " + cfg->t1.schema);
        print_kv("t2", cfg->t2.provider + " " + cfg->t2.schema);

        // Determinism demonstration: the RNG sequence is a pure function of the
        // seed and is identical on every platform, because we never touch a
        // <random> distribution.
        ptl::DeterministicRng rng{cfg->run.seed};
        // Draw into locals FIRST. The order in which function arguments are
        // evaluated is unspecified in C++, so calling next_u64() three times
        // inside one snprintf() would print the sequence in whatever order the
        // compiler chose -- and a different compiler would choose differently.
        // A stateful generator must never be advanced from an argument list.
        const auto r0 = static_cast<unsigned long long>(rng.next_u64());
        const auto r1 = static_cast<unsigned long long>(rng.next_u64());
        const auto r2 = static_cast<unsigned long long>(rng.next_u64());
        char rngbuf[64];
        std::snprintf(rngbuf, sizeof(rngbuf), "%016llx %016llx %016llx", r0, r1, r2);
        print_kv("rng[0..2] from seed", rngbuf);

        auto& lg = ptl::log::get("ptl_version");
        PTL_INFO(lg, "probe complete", ptl::log::kv("run_id", run_id.hex()),
                 ptl::log::kv("config_hash", std::string_view{hashbuf}),
                 ptl::log::kv("seed", cfg->run.seed));

        if (!registry_path.empty()) {
            auto reg = ptl::experiments::Registry::open(registry_path);
            if (!reg) {
                std::fprintf(stderr, "registry error: %s\n", reg.error().describe().c_str());
                return 3;
            }
            ptl::experiments::RunRecord rec;
            rec.run_id = run_id.hex();
            rec.git_sha = ptl::kGitSha;
            rec.git_dirty = ptl::kGitDirty;
            rec.config_hash = hashbuf;
            rec.config_canonical = cfg->canonical;
            rec.data_manifest_sha = "no-data-manifest";
            rec.seed = cfg->run.seed;
            rec.compiler = ptl::kCompilerId;
            rec.compiler_version = ptl::kCompilerVer;
            rec.build_type = ptl::kBuildType;
            rec.tag = cfg->run.tag;
            rec.status = "completed";
            rec.chain_violations = ptl::chain_violation_count();
            if (auto ok = reg->insert_run(rec); !ok) {
                std::fprintf(stderr, "registry insert failed: %s\n", ok.error().describe().c_str());
                return 3;
            }
            print_kv("registered in", registry_path);
        }
        ptl::log::shutdown();
    } else {
        std::puts("\n(no --config given; pass -c config/base.toml for the full probe)");
    }
    return 0;
}
