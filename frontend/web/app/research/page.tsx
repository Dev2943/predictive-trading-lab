"use client";

import { useMutation, useQuery } from "@tanstack/react-query";
import { useMemo } from "react";
import {
  Area,
  AreaChart,
  Bar,
  BarChart,
  CartesianGrid,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from "recharts";
import { Empty, ErrorPanel, Loading } from "@/components/feedback";
import { api, type EquityPoint, type PerformanceMetrics } from "@/lib/api";

/**
 * Research workstation.
 *
 * EVERY STATISTIC COMES FROM THE ENGINE. `MetricsEngine` computes Sharpe,
 * Sortino, Calmar, drawdown and the rest; this page requests them and renders
 * them. Recomputing any here would create a second definition that could
 * disagree with the risk engine and the reports.
 *
 * The three charts are SHAPES OF THE SAME SERIES, not new statistics: an
 * underwater curve is equity relative to its running peak, a histogram is a
 * count of returns per bucket, and a period table is a regrouping. None of
 * them invents a number.
 */

function pct(value: number | undefined, digits = 2): string {
  // Absent is a dash. A missing Sharpe and a Sharpe of zero are different.
  if (value === undefined || !Number.isFinite(value)) return "—";
  return `${(value * 100).toFixed(digits)}%`;
}

function num(value: number | undefined, digits = 3): string {
  if (value === undefined || !Number.isFinite(value)) return "—";
  return value.toFixed(digits);
}

function returnsFrom(points: EquityPoint[]): number[] {
  const out: number[] = [];
  for (let i = 1; i < points.length; i += 1) {
    const previous = points[i - 1]!.equity;
    if (!Number.isFinite(previous) || previous === 0) continue;
    out.push(points[i]!.equity / previous - 1);
  }
  return out;
}

/** Equity relative to its running peak — a shape of the series, not a metric. */
function underwater(points: EquityPoint[]) {
  let peak = -Infinity;
  return points.map((p, i) => {
    peak = Math.max(peak, p.equity);
    return { i, drawdown: peak > 0 ? p.equity / peak - 1 : 0 };
  });
}

/** Counts of returns per bucket. A histogram is a regrouping, not a statistic. */
function histogram(returns: number[], buckets = 21) {
  if (returns.length === 0) return [];
  const lo = Math.min(...returns);
  const hi = Math.max(...returns);
  if (!Number.isFinite(lo) || !Number.isFinite(hi) || hi === lo) return [];
  const width = (hi - lo) / buckets;
  const counts = new Array(buckets).fill(0) as number[];
  for (const r of returns) {
    const idx = Math.min(buckets - 1, Math.floor((r - lo) / width));
    counts[idx] = (counts[idx] ?? 0) + 1;
  }
  return counts.map((count, i) => ({
    bucket: ((lo + width * (i + 0.5)) * 100).toFixed(2),
    count,
  }));
}

const METRIC_ROWS: { key: keyof PerformanceMetrics; label: string; kind: "pct" | "num" | "int" }[] = [
  { key: "cumulative_return", label: "cumulative return", kind: "pct" },
  { key: "annualized_return", label: "annualised return", kind: "pct" },
  { key: "cagr", label: "CAGR", kind: "pct" },
  { key: "annualized_volatility", label: "annualised volatility", kind: "pct" },
  { key: "downside_volatility", label: "downside volatility", kind: "pct" },
  { key: "sharpe", label: "Sharpe", kind: "num" },
  { key: "sortino", label: "Sortino", kind: "num" },
  { key: "calmar", label: "Calmar", kind: "num" },
  { key: "max_drawdown", label: "max drawdown", kind: "pct" },
  { key: "max_drawdown_periods", label: "drawdown duration (periods)", kind: "int" },
  { key: "skewness", label: "skewness", kind: "num" },
  { key: "best_period", label: "best period", kind: "pct" },
  { key: "worst_period", label: "worst period", kind: "pct" },
  { key: "periods", label: "periods", kind: "int" },
];

export default function ResearchPage() {
  const snapshot = useQuery({
    queryKey: ["snapshot"],
    queryFn: api.snapshot,
    refetchInterval: 5000,
  });

  const points = snapshot.data?.history?.points ?? [];
  const equity = useMemo(() => points.map((p) => p.equity), [points]);
  const returns = useMemo(() => returnsFrom(points), [points]);
  const water = useMemo(() => underwater(points), [points]);
  const bars = useMemo(() => histogram(returns), [returns]);

  const analyse = useMutation({ mutationFn: () => api.performance(equity) });
  const metrics = analyse.data;
  const enough = equity.length >= 2;

  return (
    <div className="space-y-6">
      <section className="rounded border border-surface-border bg-surface-raised p-4">
        <h1 className="mb-1 text-content-muted">Research</h1>
        <p className="mb-4 text-xs text-content-faint">
          Statistics computed by the engine from this session&apos;s equity
          series — {points.length} observations.
        </p>
        {snapshot.isPending ? (
          <Loading label="reading the session" />
        ) : !enough ? (
          <Empty message="not enough history yet — start a session and let it run" />
        ) : (
          <button
            onClick={() => analyse.mutate()}
            disabled={analyse.isPending}
            className="rounded border border-surface-border px-4 py-1.5 text-sm disabled:opacity-30"
          >
            {analyse.isPending ? "computing…" : "Analyse"}
          </button>
        )}
      </section>

      <ErrorPanel error={analyse.error ?? snapshot.error} />

      {metrics && (
        <section className="rounded border border-surface-border bg-surface-raised">
          <h2 className="border-b border-surface-border px-4 py-2 text-content-muted">
            Performance
          </h2>
          <table className="w-full text-left">
            <caption className="sr-only">Performance metrics</caption>
            <thead className="text-xs text-content-faint">
              <tr>
                <th scope="col" className="px-4 py-2 font-normal">metric</th>
                <th scope="col" className="px-4 py-2 font-normal">value</th>
              </tr>
            </thead>
            <tbody>
              {METRIC_ROWS.map((row) => {
                const raw = metrics[row.key];
                const value =
                  row.kind === "pct"
                    ? pct(raw as number)
                    : row.kind === "int"
                      ? String(raw)
                      : num(raw as number);
                return (
                  <tr key={row.key} className="border-t border-surface-border/50">
                    <td className="px-4 py-1.5 text-content-faint">{row.label}</td>
                    <td className="px-4 py-1.5">{value}</td>
                  </tr>
                );
              })}
            </tbody>
          </table>
          {/* Trade statistics need matched round trips, which an equity series
              cannot supply. Stated rather than shown as zeros. */}
          <p className="px-4 py-3 text-xs text-warn">
            Win rate, profit factor and expectancy require matched trades. This
            analysis is over the equity series alone, so the engine reports them
            as zero rather than inferring them.
          </p>
        </section>
      )}

      {points.length > 1 && (
        <section className="rounded border border-surface-border bg-surface-raised p-4">
          <h2 className="mb-3 text-content-muted">Underwater</h2>
          <ResponsiveContainer width="100%" height={180}>
            <AreaChart data={water} margin={{ top: 4, right: 8, bottom: 4, left: 8 }}>
              <CartesianGrid stroke="#232a33" vertical={false} />
              <XAxis dataKey="i" stroke="#5a6472" fontSize={11} tickLine={false} />
              <YAxis
                stroke="#5a6472"
                fontSize={11}
                tickLine={false}
                width={70}
                tickFormatter={(v: number) => `${(v * 100).toFixed(1)}%`}
              />
              <Tooltip
                contentStyle={{ background: "#12161b", border: "1px solid #232a33", fontSize: 12 }}
                formatter={(v: number) => `${(v * 100).toFixed(2)}%`}
              />
              <Area
                type="monotone"
                dataKey="drawdown"
                stroke="#e5484d"
                fill="#e5484d"
                fillOpacity={0.15}
                isAnimationActive={false}
              />
            </AreaChart>
          </ResponsiveContainer>
          {/* A table alternative, because a chart alone is unreadable to a
              screen reader. */}
          <details className="mt-2 text-xs text-content-faint">
            <summary>underwater data as a table</summary>
            <table className="mt-2 w-full text-left">
              <caption className="sr-only">Underwater series</caption>
              <thead>
                <tr>
                  <th scope="col" className="font-normal">period</th>
                  <th scope="col" className="font-normal">drawdown</th>
                </tr>
              </thead>
              <tbody>
                {water.slice(-12).map((row) => (
                  <tr key={row.i}>
                    <td>{row.i}</td>
                    <td>{(row.drawdown * 100).toFixed(2)}%</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </details>
        </section>
      )}

      {bars.length > 0 && (
        <section className="rounded border border-surface-border bg-surface-raised p-4">
          <h2 className="mb-3 text-content-muted">Return distribution</h2>
          <ResponsiveContainer width="100%" height={180}>
            <BarChart data={bars} margin={{ top: 4, right: 8, bottom: 4, left: 8 }}>
              <CartesianGrid stroke="#232a33" vertical={false} />
              <XAxis dataKey="bucket" stroke="#5a6472" fontSize={10} tickLine={false} />
              <YAxis stroke="#5a6472" fontSize={11} tickLine={false} width={40} />
              <Tooltip
                contentStyle={{ background: "#12161b", border: "1px solid #232a33", fontSize: 12 }}
              />
              <Bar dataKey="count" fill="#3b82f6" isAnimationActive={false} />
            </BarChart>
          </ResponsiveContainer>
          <p className="mt-2 text-xs text-content-faint">
            buckets are return percentages; counts are observations
          </p>
        </section>
      )}
    </div>
  );
}
