import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

import { Dashboard } from "@/components/dashboard";

/**
 * Dashboard tests.
 *
 * `fetch` is stubbed, so these run without a gateway or an engine. The point is
 * the CLIENT's behaviour: does it poll, does it disable the right buttons, and
 * does it show absence as absence rather than as zero.
 */

const STOPPED = {
  state: "STOPPED",
  error: null,
  replay_exhausted: false,
  engine: { data_source: "synthetic-replay" },
};

const RUNNING = {
  state: "RUNNING",
  error: null,
  replay_exhausted: false,
  engine: {
    data_source: "synthetic-replay",
    events_processed: 120,
    orders_submitted: 8,
    orders_rejected: 0,
    fills_received: 8,
  },
};

const EMPTY_SNAPSHOT = {
  state: "STOPPED",
  available: false,
  account: null,
  positions: [],
  orders: [],
  fills: [],
  engine: {},
};

const LIVE_SNAPSHOT = {
  state: "RUNNING",
  available: true,
  account: {
    cash: 950000,
    equity: 1000052.29,
    position_value: 50052.29,
    realized_pnl: 120.5,
    unrealized_pnl: -68.21,
    gross_exposure: 50052.29,
    net_exposure: 50052.29,
    available_buying_power: 1949947.71,
    status: "active",
  },
  positions: [{ instrument: 0, quantity: 100, average_cost: 500.5, realized_pnl: 0 }],
  orders: [{ order_id: 7, instrument: 0, side: 1, quantity: 25, filled: 0 }],
  fills: [
    {
      ts: "2024-07-02T14:31:00Z",
      order_id: 6,
      instrument: 0,
      side: 1,
      quantity: 25,
      price: 500.25,
      commission: 0.25,
    },
  ],
  engine: {},
};

function stubFetch(session: unknown, snapshot: unknown, onPost?: (url: string) => void) {
  return vi.fn(async (url: string, init?: RequestInit) => {
    if (init?.method === "POST") {
      onPost?.(url);
      return new Response(JSON.stringify({ action: "start", state: "RUNNING" }), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      });
    }
    const body = url.includes("snapshot") ? snapshot : session;
    return new Response(JSON.stringify(body), {
      status: 200,
      headers: { "Content-Type": "application/json" },
    });
  });
}

function renderDashboard() {
  const client = new QueryClient({
    defaultOptions: { queries: { retry: false } },
  });
  return render(
    <QueryClientProvider client={client}>
      <Dashboard />
    </QueryClientProvider>,
  );
}

beforeEach(() => {
  vi.useRealTimers();
});

afterEach(() => {
  vi.restoreAllMocks();
});

describe("dashboard", () => {
  it("loads and shows the session state", async () => {
    vi.stubGlobal("fetch", stubFetch(STOPPED, EMPTY_SNAPSHOT));
    renderDashboard();
    expect(await screen.findByText("STOPPED")).toBeInTheDocument();
    expect(await screen.findByText("connected")).toBeInTheDocument();
  });

  it("labels the data source rather than implying it is live", async () => {
    // A synthetic replay shown as live would be the single most misleading
    // thing this interface could do.
    vi.stubGlobal("fetch", stubFetch(STOPPED, EMPTY_SNAPSHOT));
    renderDashboard();
    expect(await screen.findByText("synthetic-replay")).toBeInTheDocument();
  });

  it("shows absent figures as a dash, never as zero", async () => {
    vi.stubGlobal("fetch", stubFetch(STOPPED, EMPTY_SNAPSHOT));
    renderDashboard();
    await screen.findByText("STOPPED");
    // A missing equity and an equity of zero are different states.
    const dashes = await screen.findAllByText("—");
    expect(dashes.length).toBeGreaterThan(4);
    expect(screen.queryByText("0.00")).not.toBeInTheDocument();
  });

  it("enables Start and disables Stop when stopped", async () => {
    vi.stubGlobal("fetch", stubFetch(STOPPED, EMPTY_SNAPSHOT));
    renderDashboard();
    await screen.findByText("STOPPED");
    expect(screen.getByRole("button", { name: "Start" })).toBeEnabled();
    expect(screen.getByRole("button", { name: "Stop" })).toBeDisabled();
    expect(screen.getByRole("button", { name: "Reset" })).toBeDisabled();
  });

  it("enables Stop and Reset when running", async () => {
    vi.stubGlobal("fetch", stubFetch(RUNNING, LIVE_SNAPSHOT));
    renderDashboard();
    await screen.findByText("RUNNING");
    expect(screen.getByRole("button", { name: "Start" })).toBeDisabled();
    expect(screen.getByRole("button", { name: "Stop" })).toBeEnabled();
    expect(screen.getByRole("button", { name: "Reset" })).toBeEnabled();
  });

  it("renders account, positions, orders and fills from the API", async () => {
    vi.stubGlobal("fetch", stubFetch(RUNNING, LIVE_SNAPSHOT));
    renderDashboard();
    await screen.findByText("RUNNING");

    // Every value is one the engine produced; the client computes none of them.
    expect(await screen.findByText("1,000,052.29")).toBeInTheDocument();
    expect(screen.getByText("Positions")).toBeInTheDocument();
    expect(screen.getByText("500.25")).toBeInTheDocument();
    expect(screen.getAllByText("BUY").length).toBeGreaterThan(0);
  });

  it("sends a start command when Start is pressed", async () => {
    const posted: string[] = [];
    vi.stubGlobal(
      "fetch",
      stubFetch(STOPPED, EMPTY_SNAPSHOT, (url) => posted.push(url)),
    );
    renderDashboard();
    await screen.findByText("STOPPED");

    await userEvent.click(screen.getByRole("button", { name: "Start" }));
    await waitFor(() => expect(posted.some((u) => u.endsWith("/session/start"))).toBe(true));
  });

  it("polls for updates", async () => {
    const fetchMock = stubFetch(RUNNING, LIVE_SNAPSHOT);
    vi.stubGlobal("fetch", fetchMock);
    renderDashboard();
    await screen.findByText("RUNNING");

    const initial = fetchMock.mock.calls.length;
    // A two-second poll is honest about its staleness in a way a stale socket
    // is not.
    await waitFor(() => expect(fetchMock.mock.calls.length).toBeGreaterThan(initial), {
      timeout: 4000,
    });
  });

  it("surfaces the gateway's own error message", async () => {
    vi.stubGlobal(
      "fetch",
      vi.fn(async () =>
        new Response(JSON.stringify({ detail: "cannot start while the session is RUNNING" }), {
          status: 409,
          headers: { "Content-Type": "application/json" },
        }),
      ),
    );
    renderDashboard();
    // The engine's reason, not "Request failed".
    expect(await screen.findByText("unreachable")).toBeInTheDocument();
  });
});
