"use client";

import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { useEffect, useRef, useState } from "react";
import { ErrorPanel } from "@/components/feedback";
import { api, marketStreamUrl, type MarketFrame, type MarketQuote } from "@/lib/api";

/**
 * Live watchlist.
 *
 * STREAMED, not polled. A twenty-symbol watchlist polled every second is twenty
 * requests a second that mostly return unchanged rows; polled every two, it
 * misses the moves it exists to show. Session state elsewhere is still polled,
 * because a two-second-old session phase is fine and a two-second-old bid is
 * not.
 *
 * REST renders the first frame so the table is populated before the socket
 * opens, and the rows are the same shape either way.
 */

function num(value: number | null, digits = 2): string {
  // Absent is a dash, never 0. A missing bid and a bid of zero are different
  // observations and only one of them is a market.
  return value === null || !Number.isFinite(value) ? "—" : value.toFixed(digits);
}

export default function MarketPage() {
  const queryClient = useQueryClient();
  const [frame, setFrame] = useState<MarketFrame | null>(null);
  const [streaming, setStreaming] = useState(false);
  const [draft, setDraft] = useState("");
  const socketRef = useRef<WebSocket | null>(null);

  const initial = useQuery({ queryKey: ["market-status"], queryFn: api.marketStatus });
  const initialQuotes = useQuery({ queryKey: ["market-quotes"], queryFn: api.marketQuotes });

  const setMode = useMutation({
    mutationFn: (mode: "replay" | "live") => api.setMarketMode(mode),
    onSettled: () => queryClient.invalidateQueries({ queryKey: ["market-status"] }),
  });
  const setWatchlist = useMutation({
    mutationFn: (symbols: string[]) => api.setWatchlist(symbols),
    onSuccess: () => setDraft(""),
    onSettled: () => queryClient.invalidateQueries({ queryKey: ["market-status"] }),
  });

  useEffect(() => {
    const socket = new WebSocket(marketStreamUrl());
    socketRef.current = socket;
    socket.onopen = () => setStreaming(true);
    socket.onmessage = (event) => setFrame(JSON.parse(event.data) as MarketFrame);
    // A closed socket is reported rather than silently leaving the last frame
    // on screen, which would show stale prices as current ones.
    socket.onclose = () => setStreaming(false);
    socket.onerror = () => setStreaming(false);
    return () => socket.close();
  }, []);

  const status = frame?.status ?? initial.data;
  const quotes: MarketQuote[] = frame?.quotes ?? initialQuotes.data ?? [];
  const live = status?.mode === "live";

  return (
    <div className="space-y-6">
      <section className="flex flex-wrap items-center gap-4 rounded border border-surface-border bg-surface-raised p-4">
        <div>
          <div className="text-xs text-content-faint">source</div>
          {/* Replay is labelled as synthetic wherever it appears. */}
          <div className={live ? "text-gain" : "text-warn"}>
            {status ? (live ? "LIVE" : "REPLAY (synthetic)") : "—"}
          </div>
        </div>
        <div>
          <div className="text-xs text-content-faint">provider</div>
          <div>{status?.provider ?? "—"}</div>
        </div>
        <div>
          <div className="text-xs text-content-faint">stream</div>
          <div className={streaming ? "text-gain" : "text-loss"}>
            {streaming ? "connected" : "disconnected"}
          </div>
        </div>

        <div className="ml-auto flex gap-2">
          {(["replay", "live"] as const).map((mode) => (
            <button
              key={mode}
              onClick={() => setMode.mutate(mode)}
              disabled={setMode.isPending || status?.mode === mode}
              aria-pressed={status?.mode === mode}
              className={`rounded border px-3 py-1 disabled:opacity-40 ${
                status?.mode === mode ? "border-content text-content" : "border-surface-border"
              }`}
            >
              {mode === "replay" ? "Replay" : "Live"}
            </button>
          ))}
        </div>
      </section>

      {status?.detail && <p className="text-xs text-content-faint">{status.detail}</p>}
      <ErrorPanel error={setMode.error ?? setWatchlist.error ?? initial.error} />

      <section className="rounded border border-surface-border bg-surface-raised p-4">
        <h2 className="mb-3 text-content-muted">Watchlist</h2>
        <div className="flex flex-wrap gap-2">
          <input
            aria-label="symbols"
            value={draft}
            placeholder={status?.symbols.join(", ") ?? "SPY, QQQ"}
            onChange={(e) => setDraft(e.target.value)}
            className="w-72 rounded border border-surface-border bg-surface p-1.5 text-sm text-content"
          />
          <button
            onClick={() => setWatchlist.mutate(draft.split(",").map((s) => s.trim()))}
            disabled={!draft.trim() || setWatchlist.isPending}
            className="rounded border border-surface-border px-3 py-1.5 text-sm disabled:opacity-30"
          >
            Replace
          </button>
        </div>
      </section>

      <section className="rounded border border-surface-border bg-surface-raised">
        <h2 className="border-b border-surface-border px-4 py-2 text-content-muted">
          Top of book
        </h2>
        <table className="w-full text-left">
          {/* Top of book only: the system models no depth (ADR-0003), so a
              Level II panel would be inventing data. */}
          <caption className="sr-only">Watchlist top of book</caption>
          <thead className="text-xs text-content-faint">
            <tr>
              {["symbol", "bid", "ask", "spread", "bps", "last", "volume"].map((c) => (
                <th key={c} scope="col" className="px-4 py-2 font-normal">
                  {c}
                </th>
              ))}
            </tr>
          </thead>
          <tbody>
            {quotes.map((q) => (
              <tr key={q.symbol} className="border-t border-surface-border/50">
                <td className="px-4 py-1.5">{q.symbol}</td>
                <td className="px-4 py-1.5">{num(q.bid)}</td>
                <td className="px-4 py-1.5">{num(q.ask)}</td>
                <td className="px-4 py-1.5">{num(q.spread_value, 4)}</td>
                <td className="px-4 py-1.5">{num(q.spread_bps, 1)}</td>
                <td className="px-4 py-1.5">{num(q.last)}</td>
                <td className="px-4 py-1.5">{num(q.volume, 0)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </section>
    </div>
  );
}
