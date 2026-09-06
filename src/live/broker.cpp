#include "ptl/live/broker.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace ptl::live {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

[[nodiscard]] std::string num(double v) {
    if (!is_finite(v)) return "null";
    std::ostringstream ss;
    ss << std::setprecision(std::numeric_limits<double>::max_digits10) << v;
    return ss.str();
}

}  // namespace

std::string BrokerAccountSnapshot::to_json() const {
    std::ostringstream ss;
    ss << "{\"ts\": \"" << (is_set(ts) ? to_iso8601(ts) : std::string{})
       << "\", \"cash\": " << num(cash.get()) << ", \"buying_power\": " << num(buying_power.get())
       << ", \"equity\": " << num(equity.get())
       << ", \"position_value\": " << num(position_value.get())
       << ", \"realized_pnl\": " << num(realized_pnl.get())
       << ", \"unrealized_pnl\": " << num(unrealized_pnl.get())
       << ", \"open_orders\": " << open_order_count << ", \"positions\": {";
    bool first = true;
    for (const auto& [instrument, quantity] : positions) {
        if (!first) ss << ", ";
        first = false;
        ss << '"' << instrument << "\": " << num(quantity);
    }
    ss << "}}";
    return ss.str();
}

std::string ReconciliationReport::describe() const {
    std::ostringstream ss;
    ss.precision(6);
    ss << std::fixed << "reconciliation at " << (is_set(ts) ? to_iso8601(ts) : std::string{"unset"})
       << ": " << (clean() ? "clean" : "DRIFT") << '\n';
    if (!cash_matches) ss << "  cash drift " << cash_drift.get() << '\n';
    for (const auto& [instrument, drift] : position_drift) {
        ss << "  instrument#" << instrument << " drift " << drift << '\n';
    }
    for (const auto& note : notes) ss << "  " << note << '\n';
    return ss.str();
}

std::string LiveBrokerStats::describe() const {
    std::ostringstream ss;
    ss << "live broker: " << submitted << " submitted, " << accepted << " accepted, " << rejected
       << " rejected, " << cancelled << " cancelled, " << replaced << " replaced\n";
    ss << "  fills ingested " << fills_ingested << ", refused " << fills_refused << '\n';
    ss << "  reconciliations " << reconciliations << ", failures " << reconciliation_failures
       << '\n';
    return ss.str();
}

LiveBroker::LiveBroker(const IClock& clock, LiveConnection& connection,
                       const IOrderTranslator& translator, execution::BrokerSimulator& simulator,
                       const portfolio::Portfolio& portfolio, LiveBrokerConfig config)
    : clock_(&clock),
      connection_(&connection),
      translator_(&translator),
      simulator_(&simulator),
      portfolio_(&portfolio),
      config_(config) {}

Result<bool> LiveBroker::submit(const oms::Order& order) {
    ++stats_.submitted;

    if (!connection_->trading_permitted()) {
        // Refused, not queued. A queued order sent on reconnect would execute
        // against a market that has moved, on a decision made minutes ago.
        ++stats_.rejected;
        return fail(bad("connection is " + std::string{to_string(connection_->state())} +
                        "; orders are not permitted"));
    }

    const Price reference =
        order.limit_price().has_value() ? *order.limit_price() : order.arrival_price();
    const double notional = order.quantity().get() * reference.get();
    if (config_.max_order_notional.get() > 0.0 && is_finite(notional) &&
        notional > config_.max_order_notional.get()) {
        ++stats_.rejected;
        return fail(bad("order notional exceeds the venue limit", num(notional)));
    }

    auto encoded = translator_->encode_new(order);
    if (!encoded) {
        // Translation failed: the venue cannot express this order. Refused
        // rather than downgraded, so the strategy never gets an order it did
        // not ask for.
        ++stats_.rejected;
        return fail(encoded.error());
    }

    // The order is registered with the SIMULATOR too. That is what lets
    // ingest_external_fill validate a venue report against a known working
    // order, and it keeps one authority over what is outstanding.
    if (auto tracked = simulator_->submit(order); !tracked) {
        ++stats_.rejected;
        return fail(tracked.error());
    }
    if (auto listening = listener_.track(encoded->client_order_id, order.id()); !listening) {
        return fail(listening.error());
    }

    if (auto sent = connection_->send(encoded->payload); !sent) {
        // The venue never saw it: stop tracking so a later reply cannot be
        // matched to an order we did not place.
        listener_.forget(encoded->client_order_id);
        (void)simulator_->cancel(order.id());
        ++stats_.rejected;
        return fail(sent.error());
    }

    working_[oms::value_of(order.id())] = true;
    ++stats_.accepted;
    return true;
}

Result<bool> LiveBroker::cancel(oms::OrderId id) {
    const auto venue_id = venue_id_of(id);
    auto encoded = translator_->encode_cancel(id, venue_id);
    if (!encoded) return fail(encoded.error());

    if (auto sent = connection_->send(encoded->payload); !sent) return fail(sent.error());
    // NOT removed from working_ here. The order is cancelled when the VENUE
    // says so; assuming success would leave us flat locally while a live order
    // rests at the exchange.
    ++stats_.cancelled;
    return true;
}

Result<bool> LiveBroker::replace(const oms::Order& amended) {
    const auto venue_id = venue_id_of(amended.id());
    auto encoded = translator_->encode_replace(amended, venue_id);
    if (!encoded) return fail(encoded.error());
    if (auto sent = connection_->send(encoded->payload); !sent) return fail(sent.error());
    ++stats_.replaced;
    return true;
}

Result<std::vector<oms::Fill>> LiveBroker::poll() {
    std::vector<oms::Fill> fills;

    for (const auto& message : connection_->poll()) {
        if (message.kind == MessageKind::AccountUpdate) {
            BrokerAccountSnapshot snapshot;
            snapshot.ts = message.venue_time;
            snapshot.cash = Notional{message.price.get()};
            observe_account(std::move(snapshot));
            continue;
        }

        auto update = listener_.interpret(message);
        if (!update) {
            // An update we cannot attribute is reported, never guessed at.
            // Applying a fill to an unidentified order would corrupt the
            // portfolio silently.
            ++stats_.fills_refused;
            return fail(update.error());
        }
        if (update->kind == OrderUpdate::Kind::Ignored) continue;

        switch (update->kind) {
            case OrderUpdate::Kind::Accepted:
                if (!update->venue_order_id.empty()) {
                    venue_ids_[oms::value_of(update->order_id)] = update->venue_order_id;
                }
                break;

            case OrderUpdate::Kind::Rejected:
            case OrderUpdate::Kind::Cancelled:
                working_.erase(oms::value_of(update->order_id));
                listener_.forget(translator_->client_order_id(update->order_id));
                (void)simulator_->cancel(update->order_id);
                if (update->kind == OrderUpdate::Kind::Rejected) ++stats_.rejected;
                break;

            case OrderUpdate::Kind::Replaced:
                if (!update->venue_order_id.empty()) {
                    venue_ids_[oms::value_of(update->order_id)] = update->venue_order_id;
                }
                break;

            case OrderUpdate::Kind::PartiallyFilled:
            case OrderUpdate::Kind::Filled: {
                execution::ExternalFillReport report;
                report.order_id = update->order_id;
                report.quantity = update->filled_quantity;
                report.price = update->fill_price;
                // The VENUE's time, not ours. Using receipt time would make
                // every latency measurement read as zero.
                report.fill_time = is_set(update->venue_time) ? update->venue_time : clock_->now();
                report.commission = update->commission;
                report.exchange_fee = update->exchange_fee;
                report.venue_fill_id = update->venue_fill_id;

                // The simulator constructs it, after validating against the
                // working order. This adapter never constructs a Fill.
                auto fill = simulator_->ingest_external_fill(report);
                if (!fill) {
                    ++stats_.fills_refused;
                    return fail(fill.error());
                }
                ++stats_.fills_ingested;
                fills.push_back(std::move(*fill));

                if (update->kind == OrderUpdate::Kind::Filled) {
                    working_.erase(oms::value_of(update->order_id));
                    listener_.forget(translator_->client_order_id(update->order_id));
                }
                break;
            }
            case OrderUpdate::Kind::Ignored:
                break;
        }
    }
    return fills;
}

void LiveBroker::observe_account(BrokerAccountSnapshot snapshot) {
    account_ = std::move(snapshot);
}

ReconciliationReport LiveBroker::reconcile(const BrokerAccountSnapshot& venue) const {
    ReconciliationReport report;
    report.ts = venue.ts;

    // OUR cash minus THEIRS. Tolerated within a threshold because fees post
    // asynchronously at most venues, and demanding exactness would fire on
    // every ordinary session.
    const double drift = portfolio_->cash().get() - venue.cash.get();
    report.cash_drift = Notional{drift};
    report.cash_matches = std::abs(drift) <= config_.cash_tolerance;
    if (!report.cash_matches) {
        report.notes.emplace_back(
            "cash differs by more than the tolerance; check for "
            "fees or dividends the portfolio has not booked");
    }

    // Positions must match EXACTLY. A share count either agrees or something is
    // wrong, and there is no benign reason for a fractional discrepancy.
    report.positions_match = true;
    for (const auto& [key, position] : portfolio_->positions()) {
        const double ours = position.quantity().get();
        const auto it = venue.positions.find(key);
        const double theirs = it == venue.positions.end() ? 0.0 : it->second;
        if (std::abs(ours - theirs) > config_.position_tolerance) {
            report.position_drift[key] = ours - theirs;
            report.positions_match = false;
        }
    }
    // A position the venue holds and we do not is the more dangerous direction:
    // it is risk nobody local is watching.
    for (const auto& [key, theirs] : venue.positions) {
        if (report.position_drift.contains(key)) continue;
        const auto& positions = portfolio_->positions();
        if (positions.find(key) == positions.end() &&
            std::abs(theirs) > config_.position_tolerance) {
            report.position_drift[key] = -theirs;
            report.positions_match = false;
            report.notes.emplace_back("venue reports a position in instrument#" +
                                      std::to_string(key) + " that the portfolio does not hold");
        }
    }
    return report;
}

std::vector<oms::OrderId> LiveBroker::working_orders() const {
    std::vector<oms::OrderId> out;
    out.reserve(working_.size());
    for (const auto& [id, live] : working_) {
        if (live) out.push_back(static_cast<oms::OrderId>(id));
    }
    return out;
}

std::string LiveBroker::venue_id_of(oms::OrderId id) const {
    const auto it = venue_ids_.find(oms::value_of(id));
    return it == venue_ids_.end() ? std::string{} : it->second;
}

}  // namespace ptl::live
