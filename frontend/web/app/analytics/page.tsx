"use client";

import { useMutation, useQuery } from "@tanstack/react-query";
import { useMemo } from "react";
import {
  CartesianGrid,
  Line,
  LineChart,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from "recharts";
import { ErrorPanel } from "@/components/feedback";
import { api, type EquityPoint } from "@/lib/api";

/**
 * Analytics, computed from the LIVE SESSION's own equity history.
 *
 * This is the cross-page integration F5 could not make: the computation
 * endpoints existed but had no data to run on, so they were unreachable from
 * the browser. The session's equity series supplies it.
 *
 * THE CLIENT DERIVES ONE THING: period returns from the equity series, because
 * the analytics endpoints take returns and the session publishes levels. That
 * is a change of representation, not a metric — every statistic below
 * (volatility, Sharpe, VaR, CVaR, Omega, beta, alpha) comes from the engine.
 */

function returnsFrom(points: EquityPoint[]): number[] {
  const out: number[] = [];
  for (let i = 1; i < points.length; i += 1) {
    const previous = points[i - 1]!.equity;
    // A zero or non-finite previous level would produce an infinite return and
    // poison every downstream statistic, so the period is skipped instead.
    if (!Number.isFinite(previous) || previous === 0) continue;
    out.push(points[i]!.equity / previous - 1);
  }
  return out;
}

function fmt(value: number | null | undefined, digits = 4): string {
  if (value === null || value === undefined || !Number.isFinite(value)) return "—";
  return value.toFixed(digits);
}

export default function AnalyticsPage() {
  const snapshot = useQuery({
    queryKey: ["snapshot"],
    queryFn: api.snapshot,
    refetchInterval: 4000,
  });

  const points = snapshot.data?.history?.points ?? [];
  const returns = useMemo(() => returnsFrom(points), [points]);

  // The window must fit the sample: asking for 60 observations from 12 would
  // return an all-null series, which reads as broken rather than as "not
  // enough data yet".
  const window = Math.max(2, Math.min(20, Math.floor(returns.length / 2)));
  const enoughData = returns.length >= 4;

  const rolling = useMutation({
    mutationFn: () => api.rolling(returns, window),
  });
  const factors = useMutation({
    mutationFn: () =>
      // Benchmark: a flat series of the same length. Beta against a constant is
      // undefined, so this is deliberately labelled as an alpha-only reading
      // rather than presented as a market beta the session cannot know.
      api.factors(returns, returns.map(() => 0)),
  });

  const series = useMemo(() => {
    if (!rolling.data) return [];
    return rolling.data.volatility.map((v, i) => ({
      i,
      volatility: v,
      sharpe: rolling.data!.sharpe[i] ?? null,
      var: rolling.data!.var[i] ?? null,
    }));
  }, [rolling.data]);

  return (
    <div className="space-y-6">
      <section className="rounded border border-surface-border bg-surface-raised p-4">
        <h2 className="mb-1 text-content-muted">Session analytics</h2>
        <p className="mb-4 text-xs text-content-faint">
          Computed by the engine from this session&apos;s equity history —{" "}
          {points.length} samples, {returns.length} periods.
        </p>

        {!enoughData ? (
          /* Absence stated plainly. Running the computation on three points
             would return a series of nulls and look like a failure. */
          <p className="py-4 text-content-faint">
            not enough history yet — start a session and let it run
          </p>
        ) : (
          <div className="flex flex-wrap gap-3">
            <button
              onClick={() => rolling.mutate()}
              disabled={rolling.isPending}
              className="rounded border border-surface-border px-4 py-1.5 text-sm disabled:opacity-30"
            >
              {rolling.isPending ? "computing…" : `Rolling metrics (window ${window})`}
            </button>
            <button
              onClick={() => factors.mutate()}
              disabled={factors.isPending}
              className="rounded border border-surface-border px-4 py-1.5 text-sm disabled:opacity-30"
            >
              {factors.isPending ? "computing…" : "Attribution"}
            </button>
          </div>
        )}
      </section>

      <ErrorPanel error={rolling.error ?? factors.error} />

      {rolling.data && (
        <section className="rounded border border-surface-border bg-surface-raised p-4">
          <div className="mb-3 flex items-baseline justify-between">
            <h2 className="text-content-muted">Rolling metrics</h2>
            <span className="text-xs text-content-faint">
              omega {fmt(rolling.data.omega)}
            </span>
          </div>
          <ResponsiveContainer width="100%" height={200}>
            <LineChart data={series} margin={{ top: 4, right: 8, bottom: 4, left: 8 }}>
              <CartesianGrid stroke="#232a33" vertical={false} />
              <XAxis dataKey="i" stroke="#5a6472" fontSize={11} tickLine={false} />
              <YAxis stroke="#5a6472" fontSize={11} tickLine={false} width={80} />
              <Tooltip
                contentStyle={{
                  background: "#12161b",
                  border: "1px solid #232a33",
                  fontSize: 12,
                }}
              />
              {/* connectNulls is OFF: a gap before the window fills is real, and
                  bridging it would draw a line through data that does not exist. */}
              <Line
                type="monotone"
                dataKey="volatility"
                stroke="#3b82f6"
                dot={false}
                connectNulls={false}
                isAnimationActive={false}
              />
              <Line
                type="monotone"
                dataKey="sharpe"
                stroke="#26a96c"
                dot={false}
                connectNulls={false}
                isAnimationActive={false}
              />
            </LineChart>
          </ResponsiveContainer>
          <p className="mt-2 text-xs text-content-faint">
            blue volatility · green sharpe · values before the window fills are
            absent, not zero
          </p>
        </section>
      )}

      {factors.data && (
        <section className="rounded border border-surface-border bg-surface-raised p-4">
          <h2 className="mb-3 text-content-muted">Attribution</h2>
          <dl className="grid grid-cols-2 gap-y-1 md:grid-cols-4">
            {[
              ["periods", String(factors.data.periods)],
              ["portfolio return", fmt(factors.data.portfolio_return, 6)],
              ["alpha contribution", fmt(factors.data.alpha_contribution, 6)],
              ["residual", fmt(factors.data.residual_contribution, 6)],
            ].map(([label, value]) => (
              <div key={label}>
                <dt className="text-xs text-content-faint">{label}</dt>
                <dd>{value}</dd>
              </div>
            ))}
          </dl>
          <p className="mt-3 text-xs text-warn">
            Benchmarked against a flat series: this is an alpha reading, not a
            market beta — the session has no benchmark instrument.
          </p>
        </section>
      )}
    </div>
  );
}
