"use client";

import { useMutation, useQuery } from "@tanstack/react-query";
import { useState } from "react";
import { api, type OptimizeResponse } from "@/lib/api";

/**
 * Interactive optimizer.
 *
 * THE CLIENT SENDS INPUTS AND RENDERS OUTPUTS. It solves nothing. Weights,
 * expected volatility, Sharpe and the binding-constraint list all come from the
 * engine; duplicating any of that here would create a second answer to a
 * question that already has one.
 */

interface Row {
  symbol: string;
  volatility: string;
  expectedReturn: string;
}

const INITIAL: Row[] = [
  { symbol: "AAA", volatility: "0.12", expectedReturn: "0.04" },
  { symbol: "BBB", volatility: "0.24", expectedReturn: "0.07" },
  { symbol: "CCC", volatility: "0.09", expectedReturn: "0.02" },
];

export default function OptimizationPage() {
  const [rows, setRows] = useState<Row[]>(INITIAL);
  const [optimizer, setOptimizer] = useState("risk_parity");
  const [maxPosition, setMaxPosition] = useState("0.5");

  const capabilities = useQuery({ queryKey: ["optimizers"], queryFn: api.optimizers });

  // Estimated from the same synthetic observations the engine would use, so an
  // optimizer that REQUIRES a covariance can actually be run. Before F7 the
  // page warned the user that minimum_variance would be refused and then
  // offered no way to satisfy it.
  const estimate = useMutation({
    mutationFn: () => {
      const observations = Array.from({ length: 120 }, (_, r) =>
        rows.map((row, c) => {
          const vol = Number(row.volatility) || 0.1;
          const factor = Math.sin(r * 0.11) * 0.01;
          return factor + Math.sin(r * 0.3 + c * 2.0) * vol * 0.05;
        }),
      );
      return api.covariance(observations);
    },
  });

  const run = useMutation<OptimizeResponse, Error>({
    mutationFn: () =>
      api.optimize({
        covariance: estimate.data?.covariance ?? [],
        optimizer,
        symbols: rows.map((r) => r.symbol),
        volatilities: rows.map((r) => Number(r.volatility)),
        expected_returns: rows.map((r) => Number(r.expectedReturn)),
        max_position: Number(maxPosition),
      }),
  });

  const selected = capabilities.data?.optimizers.find((o) => o.name === optimizer);

  const update = (i: number, key: keyof Row, value: string) =>
    setRows((current) => current.map((r, j) => (i === j ? { ...r, [key]: value } : r)));

  return (
    <div className="space-y-6">
      <section className="rounded border border-surface-border bg-surface-raised p-4">
        <h2 className="mb-3 text-content-muted">Inputs</h2>

        <div className="mb-4 flex flex-wrap items-end gap-4">
          <label className="text-xs text-content-faint">
            optimizer
            <select
              aria-label="optimizer"
              value={optimizer}
              onChange={(e) => setOptimizer(e.target.value)}
              className="mt-1 block w-56 rounded border border-surface-border bg-surface p-1.5 text-sm text-content"
            >
              {capabilities.data?.optimizers.map((o) => (
                <option key={o.name} value={o.name}>
                  {o.name}
                </option>
              ))}
            </select>
          </label>

          <label className="text-xs text-content-faint">
            max position
            <input
              aria-label="max position"
              value={maxPosition}
              onChange={(e) => setMaxPosition(e.target.value)}
              className="mt-1 block w-24 rounded border border-surface-border bg-surface p-1.5 text-sm text-content"
            />
          </label>

          <button
            onClick={() => run.mutate()}
            disabled={run.isPending}
            className="rounded border border-surface-border px-4 py-1.5 text-sm disabled:opacity-30"
          >
            {run.isPending ? "solving…" : "Optimize"}
          </button>
        </div>

        {/* The engine's own requirement, surfaced before the request -- and now
            with a way to satisfy it rather than only a warning. */}
        {selected?.requires_covariance && !estimate.data && (
          <div className="mb-3 flex items-center gap-3">
            <p className="text-warn">{optimizer} needs a covariance matrix.</p>
            <button
              onClick={() => estimate.mutate()}
              disabled={estimate.isPending}
              className="rounded border border-surface-border px-3 py-1 text-xs disabled:opacity-30"
            >
              {estimate.isPending ? "estimating…" : "Estimate covariance"}
            </button>
          </div>
        )}
        {estimate.data && (
          <p className="mb-3 text-xs text-content-faint">
            covariance from {estimate.data.observations} observations
            {/* A degraded estimate must never be used unknowingly. */}
            {estimate.data.degraded && (
              <span className="ml-2 text-warn">
                degraded — {estimate.data.degradation_reason}
              </span>
            )}
            {estimate.data.psd_repaired && (
              <span className="ml-2 text-warn">PSD repaired</span>
            )}
          </p>
        )}

        <table className="w-full text-left">
          <thead className="text-xs text-content-faint">
            <tr>
              <th className="py-1 font-normal">symbol</th>
              <th className="py-1 font-normal">volatility</th>
              <th className="py-1 font-normal">expected return</th>
            </tr>
          </thead>
          <tbody>
            {rows.map((row, i) => (
              <tr key={i}>
                {(["symbol", "volatility", "expectedReturn"] as const).map((key) => (
                  <td key={key} className="py-1 pr-3">
                    <input
                      aria-label={`${key} ${i}`}
                      value={row[key]}
                      onChange={(e) => update(i, key, e.target.value)}
                      className="w-full rounded border border-surface-border bg-surface p-1 text-sm"
                    />
                  </td>
                ))}
              </tr>
            ))}
          </tbody>
        </table>
      </section>

      {run.isError && (
        <div className="rounded border border-loss/40 bg-loss/10 p-3 text-loss">
          {/* The engine's reason, which says what to change. */}
          {run.error.message}
        </div>
      )}

      {run.data && (
        <section className="rounded border border-surface-border bg-surface-raised p-4">
          <h2 className="mb-3 text-content-muted">Result</h2>
          <div className="mb-4 grid grid-cols-2 gap-3 md:grid-cols-4">
            {[
              ["status", run.data.status],
              ["expected vol", run.data.expected_volatility.toFixed(4)],
              ["sharpe", run.data.sharpe.toFixed(4)],
              ["gross", run.data.gross_exposure.toFixed(4)],
            ].map(([label, value]) => (
              <div key={label} className="rounded border border-surface-border p-2">
                <div className="text-xs text-content-faint">{label}</div>
                <div>{value}</div>
              </div>
            ))}
          </div>

          <table className="w-full text-left">
            <thead className="text-xs text-content-faint">
              <tr>
                <th className="py-1 font-normal">symbol</th>
                <th className="py-1 font-normal">weight</th>
              </tr>
            </thead>
            <tbody>
              {run.data.weights.map((w, i) => (
                <tr key={i} className="border-t border-surface-border/50">
                  <td className="py-1">{run.data.symbols[i] ?? `#${i}`}</td>
                  <td className="py-1">{(w * 100).toFixed(2)}%</td>
                </tr>
              ))}
            </tbody>
          </table>

          {/* A weight sitting exactly on a limit is a constrained answer, and
              saying so stops it being read as a free optimum. */}
          {run.data.binding_constraints.length > 0 && (
            <p className="mt-3 text-warn">
              binding: {run.data.binding_constraints.join(", ")}
            </p>
          )}
        </section>
      )}
    </div>
  );
}
