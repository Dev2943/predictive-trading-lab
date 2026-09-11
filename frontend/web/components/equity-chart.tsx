"use client";

import {
  Area,
  AreaChart,
  CartesianGrid,
  Line,
  LineChart,
  ReferenceLine,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from "recharts";

import type { EquityHistory } from "@/lib/api";

/**
 * Equity and drawdown charts.
 *
 * THE CLIENT PLOTS; IT DOES NOT CALCULATE. Every value here came from the
 * engine: equity is sampled by the session host, drawdown comes from the
 * engine's own DrawdownTracker. The one thing computed locally is each point's
 * drawdown-from-peak for the shaded series, and that is done from the peak the
 * engine reported rather than from a peak inferred here.
 */

function time(ts: string): string {
  return ts.slice(11, 16);
}

function pct(value: number): string {
  return `${(value * 100).toFixed(2)}%`;
}

export function EquityChart({ history }: { history: EquityHistory | undefined }) {
  // Absent and empty are different states, and neither is a flat line at zero.
  if (!history?.available || history.points.length === 0) {
    return (
      <section className="rounded border border-surface-border bg-surface-raised p-4">
        <h2 className="mb-3 text-content-muted">Equity</h2>
        <p className="py-8 text-center text-content-faint">
          no history yet — start a session
        </p>
      </section>
    );
  }

  const start = history.points[0]!.equity;

  const data = history.points.map((p) => ({
    t: time(p.ts),
    equity: p.equity,
    // From the peak the ENGINE reported. Inferring a peak from the visible
    // window would disagree with the engine whenever the series is
    // downsampled or truncated.
    drawdown:
      history.peak_equity > 0 ? (p.equity - history.peak_equity) / history.peak_equity : 0,
  }));

  const last = data[data.length - 1]!;
  const change = last.equity - start;

  return (
    <div className="space-y-4">
      <section className="rounded border border-surface-border bg-surface-raised p-4">
        <div className="mb-3 flex items-baseline justify-between">
          <h2 className="text-content-muted">Equity</h2>
          <div className="text-xs text-content-faint">
            {history.total_points} samples
            {history.stride > 1 && ` · every ${history.stride}`}
            <span className={`ml-3 ${change >= 0 ? "text-gain" : "text-loss"}`}>
              {change >= 0 ? "+" : ""}
              {change.toFixed(2)}
            </span>
          </div>
        </div>
        <ResponsiveContainer width="100%" height={220}>
          <LineChart data={data} margin={{ top: 4, right: 8, bottom: 4, left: 8 }}>
            <CartesianGrid stroke="#232a33" vertical={false} />
            <XAxis dataKey="t" stroke="#5a6472" fontSize={11} tickLine={false} />
            <YAxis
              stroke="#5a6472"
              fontSize={11}
              tickLine={false}
              width={80}
              // Not anchored at zero: a book worth a million with a hundred
              // dollars of movement would render as a flat line.
              domain={["auto", "auto"]}
              tickFormatter={(v: number) => v.toFixed(0)}
            />
            <Tooltip
              contentStyle={{
                background: "#12161b",
                border: "1px solid #232a33",
                fontSize: 12,
              }}
              formatter={(v: number) => v.toFixed(2)}
            />
            <ReferenceLine y={start} stroke="#5a6472" strokeDasharray="3 3" />
            <Line
              type="monotone"
              dataKey="equity"
              stroke={change >= 0 ? "#26a96c" : "#e5484d"}
              dot={false}
              strokeWidth={1.5}
              isAnimationActive={false}
            />
          </LineChart>
        </ResponsiveContainer>
      </section>

      <section className="rounded border border-surface-border bg-surface-raised p-4">
        <div className="mb-3 flex items-baseline justify-between">
          <h2 className="text-content-muted">Drawdown</h2>
          <div className="text-xs">
            <span className="text-content-faint">max </span>
            <span className="text-loss">{pct(history.max_drawdown)}</span>
            <span className="ml-3 text-content-faint">current </span>
            <span className={history.current_drawdown > 0 ? "text-loss" : "text-content"}>
              {pct(history.current_drawdown)}
            </span>
          </div>
        </div>
        <ResponsiveContainer width="100%" height={140}>
          <AreaChart data={data} margin={{ top: 4, right: 8, bottom: 4, left: 8 }}>
            <CartesianGrid stroke="#232a33" vertical={false} />
            <XAxis dataKey="t" stroke="#5a6472" fontSize={11} tickLine={false} />
            <YAxis
              stroke="#5a6472"
              fontSize={11}
              tickLine={false}
              width={80}
              tickFormatter={(v: number) => `${(v * 100).toFixed(1)}%`}
            />
            <Tooltip
              contentStyle={{
                background: "#12161b",
                border: "1px solid #232a33",
                fontSize: 12,
              }}
              formatter={(v: number) => pct(v)}
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
      </section>
    </div>
  );
}
