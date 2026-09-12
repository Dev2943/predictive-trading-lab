"use client";

import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { useState } from "react";
import { ErrorPanel } from "@/components/feedback";
import { api, type OrderRequest } from "@/lib/api";

/**
 * Trading workspace.
 *
 * THE CLIENT SENDS INTENT AND RENDERS STATE. It does not decide whether an
 * order is valid, whether it passed risk, or what it filled at — all of that
 * comes back from the engine.
 *
 * An order submitted here is QUEUED. It reaches the venue at the next market
 * event, and the interface says so rather than showing it as live immediately.
 */

type OrderType = OrderRequest["type"];

const NEEDS_LIMIT: OrderType[] = ["limit", "stop_limit"];
const NEEDS_STOP: OrderType[] = ["stop", "stop_limit"];

export default function TradingPage() {
  const queryClient = useQueryClient();

  const [symbol, setSymbol] = useState("SPY");
  const [side, setSide] = useState<1 | -1>(1);
  const [quantity, setQuantity] = useState("25");
  const [type, setType] = useState<OrderType>("market");
  // Prices are held as STRINGS while editing: coercing with Number() on every
  // keystroke destroys decimal entry ("505.2" passes through "505." -> 505).
  const [limitPrice, setLimitPrice] = useState("");
  const [stopPrice, setStopPrice] = useState("");
  const [tif, setTif] = useState<NonNullable<OrderRequest["time_in_force"]>>("day");

  const mode = useQuery({ queryKey: ["trading-mode"], queryFn: api.tradingMode });
  const session = useQuery({
    queryKey: ["session"],
    queryFn: api.session,
    refetchInterval: 2000,
  });
  const pending = useQuery({
    queryKey: ["pending"],
    queryFn: api.pendingOrders,
    refetchInterval: 2000,
  });
  const history = useQuery({
    queryKey: ["order-history"],
    queryFn: api.orderHistory,
    refetchInterval: 2000,
  });
  const snapshot = useQuery({
    queryKey: ["snapshot"],
    queryFn: api.snapshot,
    refetchInterval: 2000,
  });

  const invalidate = () => {
    void queryClient.invalidateQueries({ queryKey: ["pending"] });
    void queryClient.invalidateQueries({ queryKey: ["order-history"] });
    void queryClient.invalidateQueries({ queryKey: ["snapshot"] });
  };

  const submit = useMutation({
    mutationFn: () =>
      api.submitOrder({
        symbol,
        side,
        quantity: Number(quantity),
        type,
        limit_price: limitPrice ? Number(limitPrice) : 0,
        stop_price: stopPrice ? Number(stopPrice) : 0,
        time_in_force: tif,
      }),
    onSettled: invalidate,
  });
  const cancel = useMutation({
    mutationFn: (id: number) => api.cancelOrder(id),
    onSettled: invalidate,
  });
  const cancelAll = useMutation({ mutationFn: api.cancelAll, onSettled: invalidate });
  const flatten = useMutation({ mutationFn: api.flatten, onSettled: invalidate });
  const halt = useMutation({
    mutationFn: (halted: boolean) => api.halt(halted),
    onSettled: () => {
      invalidate();
      void queryClient.invalidateQueries({ queryKey: ["session"] });
    },
  });

  const running = session.data?.state === "RUNNING";
  const busy = submit.isPending || cancelAll.isPending || flatten.isPending;
  const halted = session.data?.engine?.strategy_halted === true;
  const lastError =
    submit.error ?? cancelAll.error ?? flatten.error ?? cancel.error ?? halt.error;

  const working = (history.data?.orders ?? []).filter((o) =>
    ["working", "partially_filled", "pending_new", "new"].includes(o.state),
  );

  return (
    <div className="space-y-6">
      {/* --- venue label -------------------------------------------------- */}
      <section className="flex flex-wrap items-center gap-4 rounded border border-surface-border bg-surface-raised p-4">
        <div>
          <div className="text-xs text-content-faint">venue</div>
          {/* Unambiguous. No live broker is connected and none is simulated. */}
          <div className={mode.data?.live_available ? "text-gain" : "text-warn"}>
            {mode.data
              ? mode.data.live_available
                ? "LIVE"
                : mode.data.mode === "PAPER"
                  ? "PAPER"
                  : "LIVE (not connected)"
              : "—"}
          </div>
        </div>
        <div>
          <div className="text-xs text-content-faint">session</div>
          <div className={running ? "text-gain" : "text-content-muted"}>
            {session.data?.state ?? "—"}
          </div>
        </div>
        {halted && (
          <div className="text-warn">
            strategy halted — data, marking and manual orders continue
          </div>
        )}
        {(pending.data?.pending ?? 0) > 0 && (
          <div className="text-content-faint">
            {pending.data?.pending} queued — submitted at the next market event
          </div>
        )}
        <div className="ml-auto flex gap-2">
          {/* The control between flatten and stop: suppresses the strategy
              while the session keeps running and the book keeps marking. */}
          <button
            onClick={() => halt.mutate(!halted)}
            disabled={!running || halt.isPending}
            className={`rounded border px-3 py-1 disabled:opacity-30 ${
              halted ? "border-warn/60 text-warn" : "border-surface-border"
            }`}
          >
            {halted ? "Resume strategy" : "Halt strategy"}
          </button>
          <button
            onClick={() => cancelAll.mutate()}
            disabled={!running || busy}
            className="rounded border border-surface-border px-3 py-1 disabled:opacity-30"
          >
            Cancel all
          </button>
          <button
            onClick={() => flatten.mutate()}
            disabled={!running || busy}
            className="rounded border border-loss/60 px-3 py-1 text-loss disabled:opacity-30"
          >
            Flatten
          </button>
        </div>
      </section>

      {mode.data?.detail && (
        <p className="text-xs text-content-faint">{mode.data.detail}</p>
      )}

      <ErrorPanel error={lastError} />

      {/* --- order entry --------------------------------------------------- */}
      <section className="rounded border border-surface-border bg-surface-raised p-4">
        <h2 className="mb-3 text-content-muted">Order entry</h2>
        <div className="flex flex-wrap items-end gap-3">
          <label className="text-xs text-content-faint">
            symbol
            <input
              aria-label="symbol"
              value={symbol}
              onChange={(e) => setSymbol(e.target.value.toUpperCase())}
              className="mt-1 block w-24 rounded border border-surface-border bg-surface p-1.5 text-sm text-content"
            />
          </label>
          <label className="text-xs text-content-faint">
            side
            <select
              aria-label="side"
              value={side}
              onChange={(e) => setSide(Number(e.target.value) as 1 | -1)}
              className="mt-1 block w-24 rounded border border-surface-border bg-surface p-1.5 text-sm text-content"
            >
              <option value={1}>BUY</option>
              <option value={-1}>SELL</option>
            </select>
          </label>
          <label className="text-xs text-content-faint">
            quantity
            <input
              aria-label="quantity"
              value={quantity}
              onChange={(e) => setQuantity(e.target.value)}
              className="mt-1 block w-24 rounded border border-surface-border bg-surface p-1.5 text-sm text-content"
            />
          </label>
          <label className="text-xs text-content-faint">
            type
            <select
              aria-label="order type"
              value={type}
              onChange={(e) => setType(e.target.value as OrderType)}
              className="mt-1 block w-32 rounded border border-surface-border bg-surface p-1.5 text-sm text-content"
            >
              <option value="market">market</option>
              <option value="limit">limit</option>
              <option value="stop">stop</option>
              <option value="stop_limit">stop limit</option>
            </select>
          </label>

          {/* Price fields appear only for the types that use them, so an
              irrelevant field cannot be filled in and silently ignored. */}
          {NEEDS_LIMIT.includes(type) && (
            <label className="text-xs text-content-faint">
              limit price
              <input
                aria-label="limit price"
                value={limitPrice}
                onChange={(e) => setLimitPrice(e.target.value)}
                className="mt-1 block w-28 rounded border border-surface-border bg-surface p-1.5 text-sm text-content"
              />
            </label>
          )}
          {NEEDS_STOP.includes(type) && (
            <label className="text-xs text-content-faint">
              stop price
              <input
                aria-label="stop price"
                value={stopPrice}
                onChange={(e) => setStopPrice(e.target.value)}
                className="mt-1 block w-28 rounded border border-surface-border bg-surface p-1.5 text-sm text-content"
              />
            </label>
          )}

          <label className="text-xs text-content-faint">
            tif
            <select
              aria-label="time in force"
              value={tif}
              onChange={(e) =>
                setTif(e.target.value as NonNullable<OrderRequest["time_in_force"]>)
              }
              className="mt-1 block w-24 rounded border border-surface-border bg-surface p-1.5 text-sm text-content"
            >
              <option value="day">day</option>
              <option value="ioc">ioc</option>
              <option value="fok">fok</option>
              <option value="gtc">gtc</option>
            </select>
          </label>

          <button
            onClick={() => submit.mutate()}
            disabled={!running || busy}
            className={`rounded border px-4 py-1.5 text-sm disabled:opacity-30 ${
              side > 0 ? "border-gain/60 text-gain" : "border-loss/60 text-loss"
            }`}
          >
            {side > 0 ? "Buy" : "Sell"}
          </button>
        </div>

        {!running && (
          <p className="mt-3 text-content-faint">
            start a session on the dashboard before entering orders
          </p>
        )}
        {submit.data && (
          <p className="mt-3 text-content-faint">
            request {submit.data.request_id} queued — {submit.data.detail}
          </p>
        )}
      </section>

      {/* --- working orders ------------------------------------------------ */}
      <section className="rounded border border-surface-border bg-surface-raised">
        <h2 className="border-b border-surface-border px-4 py-2 text-content-muted">
          Working orders
        </h2>
        {working.length === 0 ? (
          <p className="px-4 py-6 text-content-faint">no working orders</p>
        ) : (
          <table className="w-full text-left">
            <caption className="sr-only">Working orders</caption>
            <thead className="text-xs text-content-faint">
              <tr>
                {["id", "symbol", "side", "type", "quantity", "filled", ""].map((c) => (
                  <th key={c} scope="col" className="px-4 py-2 font-normal">
                    {c}
                  </th>
                ))}
              </tr>
            </thead>
            <tbody>
              {working.map((order) => (
                <tr key={order.order_id} className="border-t border-surface-border/50">
                  <td className="px-4 py-1.5">{order.order_id}</td>
                  <td className="px-4 py-1.5">{order.symbol}</td>
                  <td className="px-4 py-1.5">{order.side > 0 ? "BUY" : "SELL"}</td>
                  <td className="px-4 py-1.5">{order.type}</td>
                  <td className="px-4 py-1.5">{order.quantity.toFixed(2)}</td>
                  <td className="px-4 py-1.5">{order.filled.toFixed(2)}</td>
                  <td className="px-4 py-1.5">
                    <button
                      onClick={() => cancel.mutate(order.order_id)}
                      aria-label={`cancel order ${order.order_id}`}
                      disabled={!running}
                      className="text-content-faint hover:text-loss disabled:opacity-30"
                    >
                      cancel
                    </button>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </section>

      {/* --- blotter -------------------------------------------------------- */}
      <section className="rounded border border-surface-border bg-surface-raised">
        <h2 className="border-b border-surface-border px-4 py-2 text-content-muted">
          Trade blotter
        </h2>
        {(snapshot.data?.fills ?? []).length === 0 ? (
          <p className="px-4 py-6 text-content-faint">no executions yet</p>
        ) : (
          <table className="w-full text-left">
            <caption className="sr-only">Trade blotter</caption>
            <thead className="text-xs text-content-faint">
              <tr>
                {["time", "symbol", "side", "quantity", "price", "fees"].map((c) => (
                  <th key={c} scope="col" className="px-4 py-2 font-normal">
                    {c}
                  </th>
                ))}
              </tr>
            </thead>
            <tbody>
              {(snapshot.data?.fills ?? []).slice(0, 20).map((fill, i) => (
                <tr key={i} className="border-t border-surface-border/50">
                  <td className="px-4 py-1.5">{fill.ts.slice(11, 19)}</td>
                  <td className="px-4 py-1.5">{fill.symbol ?? `#${fill.instrument}`}</td>
                  <td
                    className={`px-4 py-1.5 ${fill.side > 0 ? "text-gain" : "text-loss"}`}
                  >
                    {fill.side > 0 ? "BUY" : "SELL"}
                  </td>
                  <td className="px-4 py-1.5">{fill.quantity.toFixed(2)}</td>
                  <td className="px-4 py-1.5">{fill.price.toFixed(2)}</td>
                  <td className="px-4 py-1.5">{fill.commission.toFixed(4)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </section>

      {/* --- rejected requests ----------------------------------------------- */}
      {(pending.data?.outcomes ?? []).some((o) => !o.accepted) && (
        <section className="rounded border border-loss/40 bg-loss/5 p-4">
          <h2 className="mb-2 text-content-muted">Rejected requests</h2>
          <ul className="space-y-1">
            {(pending.data?.outcomes ?? [])
              .filter((o) => !o.accepted)
              .slice(0, 10)
              .map((o) => (
                <li key={o.request_id} className="text-loss">
                  request {o.request_id}: {o.detail}
                </li>
              ))}
          </ul>
        </section>
      )}
    </div>
  );
}
