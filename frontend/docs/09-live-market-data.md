# P9 — Live Market Data

## The decision this phase turns on

Live data is a **display feed**. It does not reach the engine, and the paper
session continues on its own deterministic replay regardless of what the
watchlist shows.

Two designs were possible.

**Feed live bars into the session.** Paper execution against live prices is a
legitimate product, and the engine would not need changing — `IMarketDataSource`
is the injection point. It was rejected for two reasons. First, `PaperSessionHost`
generates its bar series *inside* `start()`; making it injectable is a
restructuring of the one component every trading guarantee rests on. Second, and
decisively: fills would then depend on wall-clock arrival, so a paper session
would no longer be reproducible. The brief asks to preserve deterministic
replay, and the surest way to preserve it is not to put a network stream on the
path that produces fills.

**Live data as an observation surface.** Chosen. The engine never learns that
live data exists. A test asserts the service holds no session, driver or binding
handle, so there is no path from a price to a fill.

If paper-on-live is wanted later, it is a real phase with its own determinism
story — not something to slip in behind a watchlist.

## Provider: Alpaca

Compared against the alternatives rather than chosen by default:

| Provider | Verdict |
|---|---|
| **Alpaca** | **Chosen.** Already this project's venue — ADR-0001 selects it for data and F6's translator targets it. A live feed adds no new vendor, no second credential story, no second symbology. Free tier carries real-time IEX trades and quotes over one WebSocket. |
| Finnhub | Simplest WebSocket of the five and a genuine free real-time tier. Rejected only because it would mean two vendors and two symbologies to reconcile. |
| Polygon | Best documentation and infrastructure, but real-time needs a paid tier; the free tier is delayed. Presenting delayed data as live is the failure this project refuses everywhere else. |
| Twelve Data | Broad non-US coverage this system does not need; WebSocket behind paid credits. |
| Interactive Brokers | Requires a running TWS/Gateway desktop process. Unreasonable for a web gateway, and would not survive a container restart. |

## What is verified, and what is not

**Every market data host is unreachable from this environment.** `data.alpaca.markets`,
`finnhub.io` and `api.polygon.io` are all blocked by the network allowlist.

So the Alpaca client's **message handling is verified** — authentication,
errors, quote frames, trade frames, volume accumulation, unparseable frames,
subscription protocol — against a fake that speaks Alpaca's documented shapes.
Its **connection handling is not verified**: no socket has ever been opened to
Alpaca. That distinction is stated here rather than buried, because it is what
someone about to rely on this needs to know.

## WebSocket, and why here specifically

Earlier phases avoided WebSockets deliberately. Session state is slow-moving and
a visible two-second staleness is a feature. A bid is not: a twenty-symbol
watchlist polled every second is twenty requests a second mostly returning
unchanged rows, and polled every two it misses the moves it exists to show.

REST remains for what a stream serves badly — the first render, a client that
cannot hold a socket, and every mutation. `GET /market/quotes` returns the same
rows the stream sends, so a client starts on REST and upgrades without a second
code path.

## Why the service is not the SessionDriver

The driver exists to serialise writes to mutable engine state. Market data has
none. Routing a network stream through the driver would put it on the thread
that steps the engine, so a slow feed would stall trading — the precise failure
the single-writer design was built to prevent.

## Findings

**Refusing `live` beats falling back.** Switching to live without credentials
returns 422 with the reason. A silent downgrade would leave a panel labelled
LIVE showing synthetic prices, which is the most misleading thing this interface
could do.

**A quote frame carries no trade price.** Overwriting `last` with `None` on
every quote update would blank the column between trades; the previous trade is
retained instead.

**Spread is `None`, not `0.0`, when a side is missing.** A zero spread is a real
and very different observation from an unknown one.

**The replay provider is not the engine's replay.** They are separate series on
purpose: sharing one would mean the display advancing the engine's clock.
