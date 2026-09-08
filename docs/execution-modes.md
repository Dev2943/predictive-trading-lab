# Replay, paper and live

## The claim

The same strategy, engine, risk gate, OMS, execution algorithms, portfolio and
accounting run in all three modes. They differ in three injected objects:

```cpp
// Replay
engine::Engine engine{sim_clock, replay_source, strategy, simulator, ...};

// Paper
paper::PaperSession session{cfg, clock, source, strategy, simulator,
                            paper_broker, portfolio, oms, risk, journal, ...};

// Live
live::LiveSession session{cfg, clock, connection, live_market_data, live_broker,
                          strategy, simulator, portfolio, oms, risk, journal, ...};
```

Each session **drives** an `Engine`; none dispatches events itself. A session
that dispatched its own events would be a second trading implementation, which
is exactly what the parity guarantee forbids.

## What each mode adds

| | Replay | Paper | Live |
|---|---|---|---|
| Fills from | quote matching | quote matching | the venue |
| Acknowledgements | immediate | modelled latency | real |
| Session lifecycle | run to completion | start/pause/resume/stop | + reconnect |
| Persistence | none | state + recovery | + venue sequence |
| Reconciliation | journal only | journal only | + venue account |
| Failure handling | none needed | none needed | heartbeat, backoff, dedup |

Everything in the "adds" column is operational envelope. None of it touches
trading logic.

## How parity is tested

`tests/leakage/test_paper.cpp` runs a backtest and a paper session over
identical events and asserts:

- identical order sequence, including ids and timestamps
- identical fill sequence
- **exact** final equity (`==`, not approximately)
- identical journal CSV

Persistence and the daily reset are switched **on** for that test. If either
perturbed trading, the comparison would fail — which is the point of enabling
them.

## Engine stepping

`Engine::run()` is the batch path. `begin()` / `step()` / `finish()` is the
stepped path, and `run()` is implemented in terms of them, so there is exactly
one dispatch loop.

The split exists because a live or paper session must do work *between* events —
persist, honour a pause, roll the day, answer a stop. The alternative was a
second dispatch loop inside the session, duplicating strategy dispatch, risk
gating and fill routing. Determinism is unaffected: `step()` is a bounded
`run()`, and neither the events consumed nor their order depends on chunk size.

## Live failure handling

| Failure | Response |
|---|---|
| Disconnect | Supervisor backs off exponentially, capped |
| Heartbeat late | `Degraded` — trading pauses, connection is kept |
| Heartbeat dead | `Reconnecting` |
| Duplicate message | Dropped by sequence or venue fill id |
| Out-of-order | Dropped, counted; never reordered |
| Clock skew | Dropped at the connection, ahead of the data adapter |
| Venue reject | Order cleared locally, reason recorded |
| Reconciliation drift | Session halts by default |

**Trading requires `Ready`**, not merely `Connected`. Connected-but-unsynchronised
is excluded because the local book and the venue's may disagree, and trading on
a stale book is how a reconnect doubles a position.
