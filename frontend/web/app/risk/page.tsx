"use client";

import { useMutation } from "@tanstack/react-query";
import { useState } from "react";
import { ErrorPanel } from "@/components/feedback";
import { api, type RiskLimits, type ValidationResponse } from "@/lib/api";

/**
 * Risk limit validation.
 *
 * VALIDATION ONLY. Live risk state — current exposure, utilisation against
 * limits — requires a running session and belongs with the session surface.
 * Showing a zeroed risk panel here would display an account with no exposure,
 * which is indistinguishable from a flat book and would read as real.
 *
 * Every verdict comes from the engine's own validator. The client does not
 * decide what a valid limit is.
 */

const FIELDS: { key: keyof RiskLimits; label: string }[] = [
  { key: "max_order_notional", label: "max order notional" },
  { key: "max_position_notional", label: "max position notional" },
  { key: "max_gross_leverage", label: "max gross leverage" },
  { key: "max_concentration", label: "max concentration" },
  { key: "max_drawdown_pct", label: "max drawdown" },
  { key: "max_daily_turnover", label: "max daily turnover" },
];

/**
 * Held as STRINGS while editing, converted only on submit.
 *
 * Coercing with Number() on every keystroke destroys decimal entry: typing
 * "0.25" passes through "0." , which Number() turns into 0, so the decimal
 * point is discarded and the field jumps to 25. A user cannot enter a
 * fractional limit at all — and every limit here except the notionals is a
 * fraction.
 */
const DEFAULTS: Record<string, string> = {
  max_order_notional: "1000000",
  max_position_notional: "1000000",
  max_gross_leverage: "1.0",
  max_concentration: "0.1",
  max_drawdown_pct: "0.2",
  max_daily_turnover: "10.0",
};

export default function RiskPage() {
  const [draft, setDraft] = useState<Record<string, string>>(DEFAULTS);
  const [requireLive, setRequireLive] = useState(false);

  const toLimits = (): RiskLimits => ({
    max_order_notional: Number(draft.max_order_notional),
    max_position_notional: Number(draft.max_position_notional),
    max_gross_leverage: Number(draft.max_gross_leverage),
    max_concentration: Number(draft.max_concentration),
    max_drawdown_pct: Number(draft.max_drawdown_pct),
    max_daily_turnover: Number(draft.max_daily_turnover),
    require_live: requireLive,
  });

  const validate = useMutation<ValidationResponse, Error>({
    mutationFn: () => api.validateRisk(toLimits()),
  });

  return (
    <div className="space-y-6">
      <section className="rounded border border-surface-border bg-surface-raised p-4">
        <h2 className="mb-1 text-content-muted">Risk limits</h2>
        <p className="mb-4 text-xs text-content-faint">
          Semantic validation: catches values that parse perfectly and are still
          wrong.
        </p>

        <div className="grid gap-3 md:grid-cols-3">
          {FIELDS.map((field) => (
            <label key={field.key} className="text-xs text-content-faint">
              {field.label}
              <input
                aria-label={field.label}
                value={draft[field.key] ?? ""}
                onChange={(e) =>
                  setDraft((d) => ({ ...d, [field.key]: e.target.value }))
                }
                className="mt-1 block w-full rounded border border-surface-border bg-surface p-1.5 text-sm text-content"
              />
            </label>
          ))}
        </div>

        <label className="mt-4 flex items-center gap-2 text-xs text-content-faint">
          <input
            type="checkbox"
            aria-label="require live"
            checked={requireLive}
            onChange={(e) => setRequireLive(e.target.checked)}
          />
          live session (requires limits a backtest may omit)
        </label>

        <button
          onClick={() => validate.mutate()}
          disabled={validate.isPending}
          className="mt-4 rounded border border-surface-border px-4 py-1.5 text-sm disabled:opacity-30"
        >
          {validate.isPending ? "validating…" : "Validate"}
        </button>
      </section>

      <ErrorPanel error={validate.error} />

      {validate.data && (
        <section className="rounded border border-surface-border bg-surface-raised p-4">
          <h2 className="mb-3 text-content-muted">Result</h2>
          <p className={validate.data.ok ? "text-gain" : "text-loss"}>
            {validate.data.ok ? "limits are valid" : "limits are not usable"}
            <span className="ml-3 text-content-faint">
              {validate.data.fatal} fatal · {validate.data.warnings} warning
            </span>
          </p>

          {validate.data.issues.length > 0 && (
            <ul className="mt-4 space-y-2">
              {validate.data.issues.map((issue, i) => (
                <li
                  key={i}
                  className={`rounded border p-2 ${
                    issue.severity === "fatal"
                      ? "border-loss/40 bg-loss/10"
                      : "border-warn/40 bg-warn/10"
                  }`}
                >
                  <div className="flex gap-2">
                    <span
                      className={issue.severity === "fatal" ? "text-loss" : "text-warn"}
                    >
                      [{issue.severity}]
                    </span>
                    <span>{issue.field}</span>
                  </div>
                  <div className="mt-1 text-content-muted">{issue.message}</div>
                  {/* The remedy, not just the complaint: an error saying only
                      "invalid" makes the operator guess. */}
                  {issue.remedy && (
                    <div className="mt-1 text-content-faint">→ {issue.remedy}</div>
                  )}
                </li>
              ))}
            </ul>
          )}
        </section>
      )}
    </div>
  );
}
