#pragma once

/// \file translator.hpp
/// Order translation and inbound update handling.
///
/// THE BROKER BOUNDARY. Every venue-specific string in the system lives behind
/// `IOrderTranslator`. Above it there are only `ptl::oms::Order` values; below
/// it there is whatever wire format the venue speaks. Adding Interactive
/// Brokers means writing one more translator, and nothing in Engine, OMS, Risk,
/// Execution or any strategy changes -- which is the whole point of the
/// interface existing rather than the adapter calling the API directly.
///
/// TRANSLATION IS TOTAL OR IT FAILS. A translator that silently downgrades an
/// unsupported order -- FOK becoming Day, a stop becoming a market -- would send
/// the venue something the strategy did not ask for. Every such case returns an
/// error instead, and the caller decides.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/instrument_table.hpp"
#include "ptl/core/result.hpp"
#include "ptl/live/connection.hpp"
#include "ptl/oms/order.hpp"

namespace ptl::live {

/// A request encoded for the venue, plus the identity needed to match its
/// reply back to our order.
struct EncodedRequest {
    std::string payload;
    /// OUR identifier, echoed by the venue. Client-assigned rather than
    /// venue-assigned, because a request that fails before the venue replies
    /// still needs an id to reconcile against.
    std::string client_order_id;
    oms::OrderId order_id{oms::kNoOrder};
};

/// Translates between internal orders and one venue's protocol.
class IOrderTranslator {
public:
    IOrderTranslator() = default;
    virtual ~IOrderTranslator() = default;
    IOrderTranslator(const IOrderTranslator&) = delete;
    IOrderTranslator& operator=(const IOrderTranslator&) = delete;

protected:
    IOrderTranslator(IOrderTranslator&&) = default;
    IOrderTranslator& operator=(IOrderTranslator&&) = default;

public:
    [[nodiscard]] virtual std::string_view venue() const noexcept = 0;

    [[nodiscard]] virtual Result<EncodedRequest> encode_new(const oms::Order&) const = 0;
    [[nodiscard]] virtual Result<EncodedRequest> encode_cancel(
        oms::OrderId, std::string_view venue_order_id) const = 0;
    /// Replace (amend) an existing order. Distinct from cancel-then-new: a
    /// venue that supports replace keeps queue priority, and emulating it with
    /// two messages loses that silently.
    [[nodiscard]] virtual Result<EncodedRequest> encode_replace(
        const oms::Order& amended, std::string_view venue_order_id) const = 0;

    /// Whether this venue can express the order at all. Checked BEFORE
    /// encoding, so an unsupported combination is refused with a reason rather
    /// than quietly downgraded.
    [[nodiscard]] virtual Result<bool> supports(const oms::Order&) const = 0;

    /// The client order id this translator would assign. Deterministic: the
    /// same order must always produce the same id, or a reconnect cannot match
    /// its own outstanding requests.
    [[nodiscard]] virtual std::string client_order_id(oms::OrderId) const = 0;
};

/// Alpaca. The one concrete venue in Phase 15.
///
/// Encodes to the JSON body Alpaca's orders endpoint expects. It does NOT make
/// network calls: encoding and transport are separate so the translation can be
/// tested exhaustively without credentials or a socket.
class AlpacaOrderTranslator final : public IOrderTranslator {
public:
    struct Config {
        /// Prefix on every client order id, so orders from this system are
        /// identifiable in the broker's own UI during an incident.
        std::string client_id_prefix = "ptl-";
        /// Alpaca rejects fractional quantities on some order types.
        bool allow_fractional = false;
        /// Extended-hours flag on the request.
        bool extended_hours = false;
    };

    AlpacaOrderTranslator() = default;
    explicit AlpacaOrderTranslator(Config config, const InstrumentTable* instruments = nullptr)
        : config_(std::move(config)), instruments_(instruments) {}

    [[nodiscard]] std::string_view venue() const noexcept override { return "alpaca"; }

    [[nodiscard]] Result<EncodedRequest> encode_new(const oms::Order&) const override;
    [[nodiscard]] Result<EncodedRequest> encode_cancel(
        oms::OrderId, std::string_view venue_order_id) const override;
    [[nodiscard]] Result<EncodedRequest> encode_replace(
        const oms::Order&, std::string_view venue_order_id) const override;
    [[nodiscard]] Result<bool> supports(const oms::Order&) const override;
    [[nodiscard]] std::string client_order_id(oms::OrderId) const override;

    /// Alpaca's spelling of an order type and time in force. Exposed for
    /// testing, because a wrong string here is a wrong order at the venue.
    [[nodiscard]] static Result<std::string_view> type_string(oms::OrderType) noexcept;
    [[nodiscard]] static Result<std::string_view> tif_string(oms::TimeInForce) noexcept;

private:
    Config config_;
    /// DEFAULT MEMBER INITIALIZER, not left to the constructor.
    ///
    /// `AlpacaOrderTranslator() = default` leaves a raw pointer INDETERMINATE.
    /// GCC happened to zero it and Clang did not, so the null check passed on
    /// garbage and the symbol lookup segfaulted -- a crash that appeared on one
    /// compiler only. Every raw pointer member here carries its own initializer
    /// for exactly this reason.
    const InstrumentTable* instruments_ = nullptr;
};

/// What an inbound venue message means for our order book.
struct OrderUpdate {
    enum class Kind : std::uint8_t {
        Accepted,
        Rejected,
        Cancelled,
        Replaced,
        PartiallyFilled,
        Filled,
        Ignored,
    };

    Kind kind{Kind::Ignored};
    oms::OrderId order_id{oms::kNoOrder};
    std::string venue_order_id;
    Qty filled_quantity{};
    Price fill_price{};
    Notional commission{};
    Notional exchange_fee{};
    Timestamp venue_time{kNoTimestamp};
    std::string venue_fill_id;
    std::string reason;

    [[nodiscard]] bool is_fill() const noexcept {
        return kind == Kind::PartiallyFilled || kind == Kind::Filled;
    }
    [[nodiscard]] std::string describe() const;
};

/// Turns venue messages into order updates.
///
/// Owns the client-id-to-order mapping, which is the only state needed to
/// interpret a reply. Kept here rather than in the broker so that a reconnect
/// can rebuild it from persisted state without touching the broker.
class LiveOrderListener {
public:
    /// Register an order awaiting a venue reply.
    [[nodiscard]] Result<bool> track(std::string client_order_id, oms::OrderId);
    /// Forget an order once it is terminal.
    void forget(std::string_view client_order_id) noexcept;

    /// Interpret a message. Returns an Ignored update for anything not
    /// order-related rather than failing: a quote arriving here is normal.
    [[nodiscard]] Result<OrderUpdate> interpret(const BrokerMessage&) const;

    [[nodiscard]] std::optional<oms::OrderId> resolve(std::string_view client_order_id) const;
    [[nodiscard]] std::size_t tracked() const noexcept { return by_client_id_.size(); }
    void clear() noexcept;

    /// Snapshot for persistence, ordered so a restore is reproducible.
    [[nodiscard]] std::vector<std::pair<std::string, std::uint64_t>> snapshot() const;
    [[nodiscard]] Result<bool> restore(const std::vector<std::pair<std::string, std::uint64_t>>&);

private:
    std::map<std::string, oms::OrderId, std::less<>> by_client_id_;
};

}  // namespace ptl::live
