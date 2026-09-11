#pragma once

/// \file session_host.hpp
/// A paper trading session host, owned entirely in C++.
///
/// WHY THIS IS C++ AND NOT PYTHON.
///
/// A PaperSession needs eleven collaborators -- clock, source, strategy,
/// simulator, broker, portfolio, OMS, risk, journal, artifacts, calendar --
/// none of which is thread-safe and all of which must outlive it. Binding them
/// to Python would mean handing a request handler pointers to live trading
/// objects, which is exactly the failure the F1 boundary was drawn to prevent.
///
/// So the host owns them here. Python sees ONE opaque handle and a set of
/// methods that return JSON strings. `Engine`, `PaperSession`, `PaperBroker`
/// and `PaperAccount` are never bound and never escape this translation unit.
///
/// SINGLE WRITER, NO MUTEX AROUND THE ENGINE.
///
/// This class is NOT thread-safe and does not pretend to be. Exactly one
/// thread -- the Python driver -- may call its mutating methods. Readers do not
/// call it at all: they read snapshots the driver published. That keeps the
/// engine single-threaded and deterministic, and means no lock ever sits on the
/// trading path.
///
/// MARKET DATA IS A DETERMINISTIC REPLAY.
///
/// ADR-0001's entitlement is unverified and no live feed exists, so the session
/// runs over a generated bar series seeded from the session config. It is
/// labelled as such everywhere it surfaces. A "paper" session fed by invented
/// data presented as live would be worse than no session at all.

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ptl/accounting/journal.hpp"
#include "ptl/core/clock.hpp"
#include "ptl/core/result.hpp"
#include "ptl/engine/engine.hpp"
#include "ptl/execution/broker.hpp"
#include "ptl/market/source.hpp"
#include "ptl/paper/session.hpp"
#include "ptl/storage/registry.hpp"

namespace ptl_host {

/// Lifecycle states, mirroring the API contract.
///
/// Deliberately NOT ptl::paper::SessionPhase. That enum describes the engine's
/// internal phases; this one describes what the API promises. Keeping them
/// separate means an engine phase rename is not an API break, and the mapping
/// is written down in one place.
enum class HostState : std::uint8_t {
    Stopped,
    Starting,
    Running,
    Stopping,
    Error,
};

[[nodiscard]] std::string_view to_string(HostState) noexcept;

struct StartOptions {
    std::string   session_id = "paper";
    std::uint64_t seed = 20240101;
    /// Bars of synthetic session data to generate.
    std::size_t bars = 390;
    double      starting_cash = 1'000'000.0;
    std::string artifact_root = "results";
};

/// Owns a paper session and everything it needs.
///
/// Every accessor returns a JSON string. Nothing that could alias engine state
/// crosses the boundary.
class PaperSessionHost {
public:
    // BOTH declared here and defined in the .cpp, not defaulted in-class.
    // `Impl` is incomplete at this point, and a defaulted constructor must be
    // able to destroy members if construction throws -- which needs the
    // complete type. The classic pimpl gotcha, and the error points at the
    // constructor rather than at the member that caused it.
    PaperSessionHost();
    ~PaperSessionHost();

    PaperSessionHost(const PaperSessionHost&) = delete;
    PaperSessionHost& operator=(const PaperSessionHost&) = delete;
    PaperSessionHost(PaperSessionHost&&) = delete;
    PaperSessionHost& operator=(PaperSessionHost&&) = delete;

    /// Construct and start a session. Refuses unless Stopped.
    [[nodiscard]] ptl::Result<bool> start(const StartOptions&);

    /// Shut down and destroy the session. Refuses unless Running.
    [[nodiscard]] ptl::Result<bool> stop();

    /// Advance the session. Returns events processed; zero means the replay is
    /// exhausted, which is not an error.
    [[nodiscard]] ptl::Result<std::size_t> step(std::size_t max_events);

    [[nodiscard]] HostState state() const noexcept { return state_; }
    [[nodiscard]] bool has_session() const noexcept { return impl_ != nullptr; }

    /// Lifecycle and counters.
    [[nodiscard]] std::string state_json() const;
    /// Account, positions, working orders and recent fills, in one document.
    [[nodiscard]] std::string snapshot_json() const;
    [[nodiscard]] std::string orders_json() const;
    [[nodiscard]] std::string fills_json() const;
    [[nodiscard]] std::string positions_json() const;
    [[nodiscard]] std::string portfolio_json() const;

    /// Equity history and drawdown, downsampled to at most `max_points`.
    ///
    /// WHY THE HOST SAMPLES RATHER THAN READING THE ENGINE'S CURVE.
    ///
    /// `Portfolio` keeps an equity curve, but `Engine` only appends to it when
    /// `snapshot_on_bar` is set, and `PaperSession` constructs its Engine
    /// without exposing that flag. Over a single trading day the engine's curve
    /// therefore holds exactly one point -- useless for an intraday chart.
    ///
    /// So the host samples the portfolio itself, on the driver thread, between
    /// steps. It READS equity, cash and exposure; it never calls
    /// `Portfolio::snapshot()`, which would append to the engine's own curve
    /// and corrupt a series the engine believes it owns.
    ///
    /// Drawdown comes from the engine's DrawdownTracker fed from those samples,
    /// not from arithmetic here. A second definition of drawdown is how a chart
    /// comes to disagree with the risk engine that halts on it.
    [[nodiscard]] std::string history_json(std::size_t max_points) const;

    /// Instrument id to symbol. Ids are an internal index; a UI that showed
    /// "instrument 0" would be leaking an implementation detail at the user.
    [[nodiscard]] std::string instruments_json() const;

    /// Sample the portfolio into the history buffer.
    ///
    /// Called by step() on the driver thread. Not public API: a reader calling
    /// it would touch the session from a request thread.
    void record_sample();

private:
    struct Impl;

    HostState                     state_{HostState::Stopped};
    std::string                   last_error_;
    std::unique_ptr<Impl>         impl_;
    StartOptions                  options_;
};

}  // namespace ptl_host
