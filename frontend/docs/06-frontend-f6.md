# Frontend F6 — Trading

## The decision the phase turns on

Manual order entry needs a route to the venue. The engine has exactly one:
`OrderSink`, which is handed to the strategy inside `on_bar` and nowhere else.

Two options:

1. **Call risk, OMS and the broker directly from the host.** This would
   duplicate the engine's submission sequence — and a second copy of that
   sequence is one that can disagree about whether an order passed the risk
   gate.
2. **Queue the order; let the strategy drain it through the real sink.**
   Chosen.

So a manual order waits in an inbox and the host's strategy submits it at the
next event, through the same sink it uses for its own orders. The risk gate and
OMS cannot tell the two apart, which is the point: **there is one submission
path, not two.**

It also models reality. An order entered between events reaches the venue at the
next one, never instantaneously, and the API says `queued` rather than implying
a fill.

## Live trading

Interfaces only: `BrokerAdapter`, `ExecutionProvider`, `AccountProvider`,
`OrderRouter`. `LiveBrokerAdapter` **raises on every method**. There is no
fallback to paper — an interface that quietly executed against paper while
believing it was live would be the most dangerous bug this system could have.

The UI labels the venue `PAPER`, and `live_available` stays false until a real
adapter exists.

## Findings

**A failed trading command was halting the whole session.** The driver treated
any command exception as a session failure, so one mistyped symbol drove the
session to ERROR and left a live book unattended for a reason unrelated to the
book. Lifecycle failures still ERROR — if start or stop failed, the session
really is in an unknown state — but a rejected order no longer does.

**`shutdown()` could not recover from ERROR.** The C++ host is one instance per
process, so a driver that gave up left a session behind and the next start was
refused — a failure in one place surfacing as a confusing error somewhere
unrelated.

**The risk gate rejected my first manual order.** A limit 20% from the market
tripped the engine's price collar. That was the proof the design works: manual
orders are not privileged, and the rejection reaches the user with the engine's
own reason.

**Orders queued after the replay ends stay pending, indefinitely.** The inbox is
drained inside `on_bar`; no bars, no drain. This is correct — an order rests
until the market ticks — and the pending count is what tells a user it is
waiting rather than lost. Documented and tested rather than papered over.

## Determinism

Manual entry is external input, so a human-driven session is not reproducible
the way an unattended replay is. What holds, and is tested: **given the same
command sequence, two runs agree exactly.** The automated path is untouched.

## New bindings

Four, each justified by the same constraint — the inbox lives in C++ because the
sink does: `session_submit_order`, `session_cancel_order`, `session_cancel_all`,
`session_flatten`. All take plain data and return plain data. No engine type is
bound.
