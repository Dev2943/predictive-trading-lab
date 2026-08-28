#pragma once

/// \file live.hpp
/// Live streaming source.
///
/// THE PARITY POINT. LiveQuoteSource implements the SAME
/// market::IMarketDataSource as ReplaySource. A live session differs from a
/// backtest in exactly two objects -- the clock and the source -- and nothing
/// downstream can tell which it is holding.
///
/// The transport is an interface, so the live path is fully testable without a
/// socket: a test feeds records through the same decode-and-normalise pipeline
/// a real session would use.

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ptl/core/clock.hpp"
#include "ptl/core/result.hpp"
#include "ptl/databento/decoder.hpp"
#include "ptl/market/source.hpp"

namespace ptl::databento {

/// Supplies raw payloads as they arrive. An interface so a test can drive the
/// live path deterministically.
class ILiveTransport {
public:
    ILiveTransport() = default;
    virtual ~ILiveTransport() = default;
    ILiveTransport(const ILiveTransport&) = delete;
    ILiveTransport& operator=(const ILiveTransport&) = delete;

protected:
    ILiveTransport(ILiveTransport&&) = default;
    ILiveTransport& operator=(ILiveTransport&&) = default;

public:
    /// \returns the next payload, or nullopt when the stream has ended.
    [[nodiscard]] virtual std::optional<std::string> next_payload() = 0;
    [[nodiscard]] virtual std::string_view description() const noexcept = 0;
};

/// Replays queued payloads. The test double for the live path.
class QueuedLiveTransport final : public ILiveTransport {
public:
    void push(std::string payload) { queue_.push_back(std::move(payload)); }

    [[nodiscard]] std::optional<std::string> next_payload() override;
    [[nodiscard]] std::string_view description() const noexcept override { return "queued"; }
    [[nodiscard]] std::size_t pending() const noexcept { return queue_.size(); }

private:
    std::deque<std::string> queue_;
};

/// A live quote source, satisfying the same interface as ReplaySource.
class LiveQuoteSource final : public market::IMarketDataSource {
public:
    /// \param replay_clock OPTIONAL. Pass a SimulatedClock to replay recorded
    ///        payloads deterministically; pass nullptr for a genuinely live
    ///        session, where the wall clock advances itself.
    ///
    /// The parameter is a SimulatedClock rather than an IClock deliberately.
    /// IClock has no advance_to, and correctly so: a wall clock cannot be told
    /// what time it is. Widening the interface to make this uniform would put a
    /// method on WallClock that must either lie or throw. Naming the asymmetry
    /// is more honest, and the source's OUTPUT is identical either way -- which
    /// is the parity property that actually matters.
    LiveQuoteSource(ILiveTransport& transport, Decoder& decoder,
                    SimulatedClock* replay_clock = nullptr);

    LiveQuoteSource(LiveQuoteSource&&) = default;
    LiveQuoteSource& operator=(LiveQuoteSource&&) = default;
    ~LiveQuoteSource() override = default;

    [[nodiscard]] std::optional<market::MarketEvent> next() override;
    [[nodiscard]] Timestamp peek_time() const noexcept override;
    [[nodiscard]] std::string_view description() const noexcept override { return "live"; }

    [[nodiscard]] std::size_t decoded() const noexcept { return decoded_; }
    [[nodiscard]] std::size_t payloads_read() const noexcept { return payloads_; }

private:
    /// Pull and decode the next payload into the pending buffer.
    [[nodiscard]] bool refill();

    ILiveTransport* transport_;
    Decoder* decoder_;
    SimulatedClock* replay_clock_;
    std::deque<market::MarketEvent> pending_;
    Timestamp last_emitted_{kNoTimestamp};
    std::size_t decoded_ = 0;
    std::size_t payloads_ = 0;
};

}  // namespace ptl::databento
