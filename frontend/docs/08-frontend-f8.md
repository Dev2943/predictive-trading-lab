# Frontend F8 — Final Polish, Completion and Verification

The last phase. No new capability; everything here closes, cleans or verifies
what F1–F7 built.

## What the audit found

**Dead code I had introduced.** Eight API client methods with no consumer —
`health`, `version`, `fingerprints`, `history`, `instruments`, `artifacts`,
`diagnostics`, `reports`. The first five were superseded when pages moved to
`/system` and `/session/snapshot`; the last three I added in F7 and never wired.
Carrying them suggests a UI path that does not exist.

**A Zustand store with zero consumers**, present since F1. It was written to
hold client-side UI state and never needed one: engine-derived state lives in
React Query, where it is a cache with a visible origin and staleness. Removed
rather than left as a place for trading state to drift into.

**Six declared dependencies that were never imported** — `zustand`,
`framer-motion`, `lucide-react`, `clsx`, `tailwind-merge`,
`class-variance-authority`.

**Three different shapes for the same failure.** Each page rendered errors its
own way and the dashboard had a third. A 409 therefore looked like three
different problems depending on where you stood.

**The test suite polluted the repository.** Sessions persist state under their
artifact root, which defaults to `results/`. Running the suite wrote real
session directories into the checkout, so the tree was no longer clean
afterwards.

## What was rejected as out of scope

**A persisted-artifact browser.** `/artifacts`, `/diagnostics`, `/metrics` and
`/reports` still have no UI. Building one is a new page, new navigation, new
state — a feature, not completion. The endpoints remain available to
programmatic clients and are documented in the OpenAPI schema and the README's
limitations section, which is the honest treatment.

**Journalling manual commands against event indices.** This would make a
human-driven session fully replayable and is the right long-term answer to the
timing gap. It is also new machinery in the host and the driver, which is
outside a polish phase. Documented as a known limitation instead.

**Animation.** `framer-motion` was in the original stack and never adopted. The
interface is dense and numeric, and motion on a blotter can mask an update
rather than draw attention to it. Removing the unused dependency and saying why
is more honest than adding decoration to justify a line in `package.json`.

## Changes

**Dead code removed** — eight client methods, the Zustand store, six
dependencies, and the two types the removed methods used.

**One feedback component.** `ErrorPanel`, `Loading` and `Empty`, used by every
page. The panel maps status codes to what the reader can do: a 409 is a timing
problem to retry, a 422 is something to change, a 503 means the engine is
unreachable, and a bare `Failed to fetch` becomes "could not reach the API"
rather than a `TypeError` on screen. The engine's own message always survives.

**Accessibility.** Tables carry `sr-only` captions and `scope="col"` headers;
the nav is labelled; per-row cancel buttons say which order they cancel — with
several rows on screen, "cancel" alone is ambiguous to a screen reader. Error
panels are `role="alert"`; loading states are `role="status"` with
`aria-live="polite"`.

**Test isolation.** `SessionDriver.start` now defaults its artifact root from
`PTL_RESULTS`, matching the `EngineClient`, and an autouse fixture points it at
a temporary directory. A full suite run leaves the repository byte-identical.

## Verification performed

Determinism was checked at all three layers rather than assumed at one:

```
binary:  config 30b44e5972450aad  rng d05ef55272cdfb14 2e2f422341add64e 1c120f3d1ce63170
binding: config 30b44e5972450aad  rng d05ef55272cdfb14 2e2f422341add64e 1c120f3d1ce63170
gateway: config 30b44e5972450aad  matches_reference: true
```

Every v1.0 file — every directory and every root file — is byte-identical.

Performance sanity: a full snapshot read, including equity history and blotter,
costs 0.27 ms (~3,700/sec), comfortably inside the interface's 2-second poll.

## Project status

Complete. Eight frontend phases over a frozen v1.0 engine, which was never
modified.
