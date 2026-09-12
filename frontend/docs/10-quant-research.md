# P10 — Advanced Analytics & Quant Research Workstation

## Architecture audit

The audit's central finding reframed the whole phase.

**`analytics::MetricsEngine` already computes 25 fields** — cumulative and
annualised return, CAGR, annualised and downside volatility, Sharpe, Sortino,
Calmar, maximum drawdown and its duration, trades, wins, losses, win rate,
average win, average loss, win/loss ratio, profit factor, expectancy, turnover,
cost ratio, skewness, best and worst period, initial and final equity.

**None of it was reachable.** It was computed by the engine from Phase 10 and
had no binding, no endpoint and no consumer. Nearly the entire "Portfolio
Analytics" requirement was already written and simply walled in.

Other findings:

| Capability | State |
|---|---|
| Per-trade execution quality (fill efficiency, participation) | in the engine, unbound |
| Drawdown tracker | bound in F4, used |
| Rolling volatility / Sharpe / VaR / CVaR | bound in F1, exposed F5, consumed F7 |
| Factor contribution | bound F1, exposed F5, consumed F7 |
| **Sector data on instruments** | **does not exist** |
| **Classical TA indicators (RSI, MACD, Bollinger, ATR, ADX, Stochastic)** | **not in the engine** |

## What I implemented, and why it belongs here

**Exposing `MetricsEngine`.** One binding, one endpoint, one page. It belongs in
P10 rather than earlier because earlier phases had no equity series to run it
on — F4 created the history and P10 is the first phase where these numbers have
an input.

The binding takes equity **levels**, not returns: the engine derives returns per
its configured basis, so the basis is decided once rather than twice.

**A research page** with the metrics table, an underwater chart and a return
distribution. All three are *shapes of the same series*, not new statistics: an
underwater curve is equity relative to its running peak, and a histogram is a
count per bucket. Neither invents a number.

## What I rejected, and why

**Sector attribution.** Instruments have no sector attribute anywhere in the
engine. Building it means inventing a mapping and presenting it as analysis.
Rejected outright.

**Technical indicators.** The engine has momentum, intraday and cross-sectional
*features*; it has no RSI, MACD, Bollinger, ATR, ADX or Stochastic. Implementing
them in Python would put eleven new financial calculations in the gateway — the
exact duplication the brief and the architecture forbid. If they are wanted,
they belong in the engine, which is frozen.

**Information Ratio, Beta, Alpha, Tracking Error.** All require a benchmark
instrument. The session trades one synthetic symbol and has no benchmark, which
is the same gap F7 documented. Computing a beta against a flat series is an
alpha reading and is already labelled as such on the analytics page.

**The trade journal with MAE/MFE.** The engine computes per-trade execution
quality, and `accounting::Journal` matches round trips — but neither is reachable
from the host, which tracks fills rather than matched trades. Exposing it is
real work in the session host, not a wrapper, and it would have been the
largest single item in this phase. Deferred honestly rather than approximated by
pairing fills in Python, which would duplicate the engine's matching logic and
could disagree with it.

## Findings

**Trade-derived fields are zero, and the page says so.** `win_rate`,
`profit_factor` and `expectancy` need matched trades; an equity series cannot
distinguish a round trip from a mark. The engine reports zero rather than
inferring, and the interface states that rather than presenting zeros as
results.

**`periods` counts equity observations, not returns.** My first test asserted
`len - 1` and was wrong about the engine's own convention.

**JSON cannot carry an infinity**, so the non-finite guard is unreachable over
HTTP. It still matters for programmatic callers, and is now tested against the
binding directly rather than through a request that can never be made.

**A flat series must not produce an infinite Sharpe.** Verified: zero volatility
yields a finite value.

## Known limitations

- Trade journal, MAE/MFE and sector attribution are absent for the reasons above.
- Technical indicators are absent and should be added to the engine, not here.
- Benchmark-relative statistics need a benchmark instrument the system does not
  have.
- The research page analyses the live session's equity series; comparing saved
  snapshots across sessions is not implemented.
