# Known limitations

Stated plainly. A platform that hides these is harder to trust than one that
does not.

## Unverified assumptions

**The market data entitlement gate (ADR-0001) has never been verified.**
Phases 8 and 9 built quote-aware execution on the assumption that a Databento
`cbbo-1m` subscription is available. `schema_probe_request()` constructs exactly
the documented call, and no authenticated request has ever been made — the gate
has been open for sixteen phases. If the entitlement is absent, the quote-driven
fill model rests on data that cannot be obtained.

**No live broker connection has ever been made.** `AlpacaOrderTranslator`
encodes request bodies and is tested exhaustively against a scripted transport.
It has never spoken to a venue. The first real connection is still ahead.

**`nlohmann/json` has not been through dependency review.** It has been
load-bearing for the Databento decoder since Phase 8.

## Deliberate design limits

**Single-threaded.** Throughput is bounded by one core. This buys determinism
and replay/live parity, and those were judged worth more than parallelism.

**Conservative fill model, not queue position** (ADR-0003). Fill rates for
passive strategies are understated. Modelling queue position honestly requires
full order-by-order data and a venue matching model; modelling it dishonestly is
worse than not modelling it.

**No intraday borrow or locate modelling.** Short availability is assumed.

**Bucketed latency histograms.** Quantiles are interpolated and approximate by
construction. Do not build an SLA on the third digit.

## Verification gaps

**AppleClang is built in CI but has never been verified locally.** All local
numbers come from GCC 13 and Clang 18 on Linux.

**Benchmarks come from a shared container**, with 20–25% run-to-run spread. They
establish scaling, not production latency.

**`to_iso8601` on an unset timestamp overflows.** Guarded at every call site in
Phases 13–16, but the underlying function in `core` still has the sharp edge,
and `describe()` methods in earlier phases may pass a partially-populated struct.

## Not implemented

No GUI, dashboard, container image, deployment tooling, or multi-account
support. Each was explicitly out of scope, not overlooked.
