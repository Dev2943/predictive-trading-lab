#pragma once

/// \file historical.hpp
/// Historical quote replay through the standard event source.
///
/// The output is a plain market::IMarketDataSource. Nothing downstream knows
/// the events came from Databento rather than a file or a socket -- which is
/// what keeps replay and live on one code path.

#include <memory>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/market/event.hpp"
#include "ptl/market/source.hpp"

namespace ptl::databento {

/// Interleave bars and quotes into one chronological event stream.
///
/// ORDERING RULE ON TIES: when a quote and a bar carry the same timestamp, the
/// QUOTE IS EMITTED FIRST. A bar close and a quote sampled at the same instant
/// describe the same moment, and the simulator must have the tradeable prices
/// in hand before it is asked to act on the bar. Emitting the bar first would
/// price that bar's fills from the PREVIOUS quote -- a one-interval staleness
/// introduced by ordering alone.
[[nodiscard]] Result<std::vector<market::MarketEvent>> merge_quotes_and_bars(
    std::vector<market::Quote> quotes, std::vector<market::Bar> bars);

/// Build a replayable source from a merged stream, with session events.
[[nodiscard]] Result<std::vector<market::MarketEvent>> build_replay_events(
    std::vector<market::Quote> quotes, std::vector<market::Bar> bars,
    const market::Calendar& calendar);

}  // namespace ptl::databento
