"use client";

import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { api, ApiError, type SessionSnapshot, type SessionStatus } from "@/lib/api";

/**
 * The paper session dashboard.
 *
 * EVERY VALUE COMES FROM THE ENGINE. Nothing here computes a P&L, derives an
 * exposure or infers a position. The client renders what the API returns; the
 * moment it starts calculating, there are two answers to "what do we hold?"
 * and no way to tell which is right.
 */

function money(value: number | null | undefined): string {
  // Absent is "—", never 0. A missing figure and a genuine zero are different
  // states, and a dash cannot be mistaken for a measurement.
  if (value === null || value === undefined || !Number.isFinite(value)) return "—";
  return value.toLocaleString("en-US", {
    minimumFractionDigits: 2,
    maximumFractionDigits: 2,
  });
}

function Stat({ label, value, tone }: { label: string; value: string; tone?: string }) {
  return (
    <div className="rounded border border-surface-border bg-surface-raised p-3">
      <div className="text-xs text-content-faint">{label}</div>
      <div className={`mt-1 text-base ${tone ?? ""}`}>{value}</div>
    </div>
  );
}

function pnlTone(value: number | null | undefined): string {
  if (value === null || value === undefined || value === 0) return "";
  return value > 0 ? "text-gain" : "text-loss";
}

const BUSY: SessionStatus["state"][] = ["STARTING", "STOPPING"];

export function Dashboard() {
  const queryClient = useQueryClient();

  const status = useQuery({
    queryKey: ["session"],
    queryFn: api.session,
    // Polled rather than pushed: WebSockets are explicitly out of scope, and a
    // two-second poll is honest about its staleness in a way a stale socket is
    // not.
    refetchInterval: 2000,
  });

  const snapshot = useQuery({
    queryKey: ["snapshot"],
    queryFn: api.snapshot,
    refetchInterval: 2000,
  });

  const invalidate = () => {
    void queryClient.invalidateQueries({ queryKey: ["session"] });
    void queryClient.invalidateQueries({ queryKey: ["snapshot"] });
  };

  const start = useMutation({ mutationFn: () => api.startSession({}), onSettled: invalidate });
  const stop = useMutation({ mutationFn: () => api.stopSession(), onSettled: invalidate });
  const reset = useMutation({ mutationFn: () => api.resetSession({}), onSettled: invalidate });

  const state = status.data?.state ?? "STOPPED";
  const busy =
    BUSY.includes(state) || start.isPending || stop.isPending || reset.isPending;

  // Buttons follow the SERVER's state machine rather than a local guess. The
  // gateway is the authority on what is legal, and a client that guessed would
  // eventually disagree with it.
  const canStart = state === "STOPPED" || state === "ERROR";
  const canStop = state === "RUNNING";
  const canReset = state === "RUNNING";

  const account = snapshot.data?.account;
  const engine = status.data?.engine ?? {};
  const lastError =
    status.data?.error ??
    (start.error ?? stop.error ?? reset.error
      ? ((start.error ?? stop.error ?? reset.error) as Error).message
      : null);

  return (
    <div className="space-y-6">
      {/* --- status bar ------------------------------------------------- */}
      <section className="flex flex-wrap items-center gap-4 rounded border border-surface-border bg-surface-raised p-4">
        <div>
          <div className="text-xs text-content-faint">session</div>
          <div
            className={
              state === "RUNNING"
                ? "text-gain"
                : state === "ERROR"
                  ? "text-loss"
                  : "text-content-muted"
            }
          >
            {state}
          </div>
        </div>
        <div>
          <div className="text-xs text-content-faint">api</div>
          <div className={status.isError ? "text-loss" : "text-gain"}>
            {status.isError ? "unreachable" : "connected"}
          </div>
        </div>
        <div>
          <div className="text-xs text-content-faint">data source</div>
          {/* Labelled, always. A synthetic replay shown as live would be the
              single most misleading thing this interface could do. */}
          <div className="text-warn">{engine.data_source ?? "—"}</div>
        </div>
        {status.data?.replay_exhausted && (
          <div className="text-content-faint">replay exhausted (book still live)</div>
        )}

        <div className="ml-auto flex gap-2">
          <button
            onClick={() => start.mutate()}
            disabled={!canStart || busy}
            className="rounded border border-surface-border px-3 py-1 disabled:opacity-30"
          >
            Start
          </button>
          <button
            onClick={() => stop.mutate()}
            disabled={!canStop || busy}
            className="rounded border border-surface-border px-3 py-1 disabled:opacity-30"
          >
            Stop
          </button>
          <button
            onClick={() => reset.mutate()}
            disabled={!canReset || busy}
            className="rounded border border-surface-border px-3 py-1 disabled:opacity-30"
          >
            Reset
          </button>
        </div>
      </section>

      {lastError && (
        <div className="rounded border border-loss/40 bg-loss/10 p-3 text-loss">
          {lastError}
          {(start.error as ApiError | null)?.status === 409 && (
            <span className="ml-2 text-content-faint">
              (the session state does not permit that)
            </span>
          )}
        </div>
      )}

      {/* --- account ----------------------------------------------------- */}
      <section className="grid grid-cols-2 gap-3 md:grid-cols-4">
        <Stat label="equity" value={money(account?.equity)} />
        <Stat label="cash" value={money(account?.cash)} />
        <Stat label="buying power" value={money(account?.available_buying_power)} />
        <Stat label="position value" value={money(account?.position_value)} />
        <Stat
          label="realized pnl"
          value={money(account?.realized_pnl)}
          tone={pnlTone(account?.realized_pnl)}
        />
        <Stat
          label="unrealized pnl"
          value={money(account?.unrealized_pnl)}
          tone={pnlTone(account?.unrealized_pnl)}
        />
        <Stat label="gross exposure" value={money(account?.gross_exposure)} />
        <Stat label="net exposure" value={money(account?.net_exposure)} />
      </section>

      {/* --- engine counters --------------------------------------------- */}
      <section className="grid grid-cols-2 gap-3 md:grid-cols-4">
        <Stat label="events" value={String(engine.events_processed ?? "—")} />
        <Stat label="orders" value={String(engine.orders_submitted ?? "—")} />
        <Stat label="rejected" value={String(engine.orders_rejected ?? "—")} />
        <Stat label="fills" value={String(engine.fills_received ?? "—")} />
      </section>

      {/* --- tables ------------------------------------------------------- */}
      <div className="grid gap-6 lg:grid-cols-2">
        <Table
          title="Positions"
          empty="no open positions"
          rows={snapshot.data?.positions ?? []}
          columns={["instrument", "quantity", "avg cost"]}
          render={(p) => [
            String(p.instrument),
            p.quantity.toFixed(2),
            money(p.average_cost),
          ]}
        />
        <Table
          title="Open orders"
          empty="no working orders"
          rows={snapshot.data?.orders ?? []}
          columns={["id", "side", "quantity", "filled"]}
          render={(o) => [
            String(o.order_id),
            o.side > 0 ? "BUY" : "SELL",
            o.quantity.toFixed(2),
            o.filled.toFixed(2),
          ]}
        />
      </div>

      <Table
        title="Latest fills"
        empty="no fills yet"
        rows={(snapshot.data?.fills ?? []).slice(0, 15)}
        columns={["time", "id", "side", "quantity", "price"]}
        render={(f) => [
          f.ts.slice(11, 19),
          String(f.order_id),
          f.side > 0 ? "BUY" : "SELL",
          f.quantity.toFixed(2),
          f.price.toFixed(2),
        ]}
      />
    </div>
  );
}

function Table<T>({
  title,
  columns,
  rows,
  render,
  empty,
}: {
  title: string;
  columns: string[];
  rows: T[];
  render: (row: T) => string[];
  empty: string;
}) {
  return (
    <section className="rounded border border-surface-border bg-surface-raised">
      <h2 className="border-b border-surface-border px-4 py-2 text-content-muted">
        {title}
      </h2>
      {rows.length === 0 ? (
        <p className="px-4 py-6 text-content-faint">{empty}</p>
      ) : (
        <table className="w-full text-left">
          <thead className="text-xs text-content-faint">
            <tr>
              {columns.map((c) => (
                <th key={c} className="px-4 py-2 font-normal">
                  {c}
                </th>
              ))}
            </tr>
          </thead>
          <tbody>
            {rows.map((row, i) => (
              <tr key={i} className="border-t border-surface-border/50">
                {render(row).map((cell, j) => (
                  <td key={j} className="px-4 py-1.5">
                    {cell}
                  </td>
                ))}
              </tr>
            ))}
          </tbody>
        </table>
      )}
    </section>
  );
}
