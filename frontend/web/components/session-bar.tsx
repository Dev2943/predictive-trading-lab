"use client";

import { useQuery } from "@tanstack/react-query";
import { api } from "@/lib/api";

/**
 * Session status, visible on every page.
 *
 * Before F7 the session state was only on the dashboard, so an operator on the
 * Trading or Analytics page could not tell whether the session was running,
 * halted, or gone. A trading interface that hides whether it is connected is
 * the wrong kind of quiet.
 *
 * Lives in the layout so there is ONE source of this on screen, rather than
 * each page rendering its own and eventually disagreeing.
 */
export function SessionBar() {
  const session = useQuery({
    queryKey: ["session"],
    queryFn: api.session,
    refetchInterval: 3000,
  });
  const mode = useQuery({ queryKey: ["trading-mode"], queryFn: api.tradingMode });

  const state = session.data?.state;
  const halted = session.data?.engine?.strategy_halted === true;

  const tone =
    session.isError || state === "ERROR"
      ? "text-loss"
      : state === "RUNNING"
        ? halted
          ? "text-warn"
          : "text-gain"
        : "text-content-faint";

  return (
    <div className="flex flex-wrap items-center gap-4 border-b border-surface-border px-1 pb-2 text-xs">
      <span className="text-content-faint">session</span>
      <span className={tone}>
        {session.isError ? "api unreachable" : (state ?? "…")}
        {/* Halted is surfaced everywhere, because a suppressed strategy that
            looks like a running one is how an operator concludes the model is
            broken. */}
        {halted && " · strategy halted"}
      </span>
      {mode.data && (
        <span className={mode.data.live_available ? "text-gain" : "text-warn"}>
          {mode.data.live_available ? "LIVE" : "PAPER"}
        </span>
      )}
      {session.data?.replay_exhausted && (
        <span className="text-content-faint">replay exhausted</span>
      )}
    </div>
  );
}
