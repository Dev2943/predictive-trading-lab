import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { afterEach, describe, expect, it, vi } from "vitest";

import TradingPage from "@/app/trading/page";

/**
 * Trading workspace tests.
 *
 * `fetch` is stubbed. What matters is the client's behaviour: does it send what
 * the user entered, refuse to imply a fill, label the venue honestly, and
 * surface the engine's rejections.
 */

const MODE = {
  mode: "PAPER",
  live_available: false,
  label: "PAPER",
  detail: "Live trading requires a broker adapter.",
};

const RUNNING = { state: "RUNNING", error: null, replay_exhausted: false, engine: {} };
const STOPPED = { state: "STOPPED", error: null, replay_exhausted: false, engine: {} };

const HISTORY = {
  available: true,
  orders: [
    {
      order_id: 7,
      symbol: "SPY",
      state: "working",
      side: 1,
      type: "limit",
      quantity: 50,
      filled: 0,
      reject_reason: "",
    },
    {
      order_id: 6,
      symbol: "SPY",
      state: "filled",
      side: 1,
      type: "market",
      quantity: 25,
      filled: 25,
      reject_reason: "",
    },
  ],
};

const SNAPSHOT = {
  state: "RUNNING",
  available: true,
  account: null,
  positions: [],
  orders: [],
  fills: [
    {
      ts: "2024-07-02T14:31:05Z",
      order_id: 6,
      instrument: 0,
      symbol: "SPY",
      side: 1,
      quantity: 25,
      price: 503.17,
      commission: 0.0125,
    },
  ],
  history: { available: false, total_points: 0, stride: 1, max_drawdown: 0, current_drawdown: 0, peak_equity: 0, points: [] },
  instruments: [],
  engine: {},
};

function stub(session: unknown, pending: unknown, onCall?: (u: string, i?: RequestInit) => void) {
  return vi.fn(async (url: string, init?: RequestInit) => {
    onCall?.(url, init);
    if (init && init.method && init.method !== "GET") {
      return new Response(
        JSON.stringify({ request_id: 1, queued: true, detail: "queued; submitted at the next market event", action: "flatten", queued_count: 2 }),
        { status: 200, headers: { "Content-Type": "application/json" } },
      );
    }
    const body = url.includes("/trading/mode")
      ? MODE
      : url.includes("/trading/pending")
        ? pending
        : url.includes("/trading/orders")
          ? HISTORY
          : url.includes("/session/snapshot")
            ? SNAPSHOT
            : session;
    return new Response(JSON.stringify(body), {
      status: 200,
      headers: { "Content-Type": "application/json" },
    });
  });
}

function renderPage() {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false } } });
  return render(
    <QueryClientProvider client={client}>
      <TradingPage />
    </QueryClientProvider>,
  );
}

const NO_PENDING = { pending: 0, outcomes: [] };

afterEach(() => vi.restoreAllMocks());

describe("trading workspace", () => {
  it("labels the venue as PAPER and never implies a live connection", async () => {
    vi.stubGlobal("fetch", stub(RUNNING, NO_PENDING));
    renderPage();
    expect(await screen.findByText("PAPER")).toBeInTheDocument();
    expect(screen.queryByText("LIVE")).not.toBeInTheDocument();
  });

  it("disables entry until a session is running", async () => {
    vi.stubGlobal("fetch", stub(STOPPED, NO_PENDING));
    renderPage();
    await screen.findByText("STOPPED");
    expect(screen.getByRole("button", { name: "Buy" })).toBeDisabled();
    expect(screen.getByRole("button", { name: "Flatten" })).toBeDisabled();
    expect(await screen.findByText(/start a session/)).toBeInTheDocument();
  });

  it("enables entry once running", async () => {
    vi.stubGlobal("fetch", stub(RUNNING, NO_PENDING));
    renderPage();
    await screen.findByText("RUNNING");
    expect(screen.getByRole("button", { name: "Buy" })).toBeEnabled();
  });

  it("shows price fields only for the types that use them", async () => {
    // An irrelevant field that can be filled in and silently ignored is worse
    // than no field.
    vi.stubGlobal("fetch", stub(RUNNING, NO_PENDING));
    renderPage();
    await screen.findByText("RUNNING");

    expect(screen.queryByLabelText("limit price")).not.toBeInTheDocument();
    await userEvent.selectOptions(screen.getByLabelText("order type"), "limit");
    expect(screen.getByLabelText("limit price")).toBeInTheDocument();
    expect(screen.queryByLabelText("stop price")).not.toBeInTheDocument();

    await userEvent.selectOptions(screen.getByLabelText("order type"), "stop_limit");
    expect(screen.getByLabelText("limit price")).toBeInTheDocument();
    expect(screen.getByLabelText("stop price")).toBeInTheDocument();
  });

  it("sends the order the user entered, with a decimal price intact", async () => {
    // Regression guard: coercing per keystroke would turn "505.25" into 505.
    const bodies: string[] = [];
    vi.stubGlobal(
      "fetch",
      stub(RUNNING, NO_PENDING, (_u, init) => {
        if (init?.method === "POST") bodies.push(String(init.body));
      }),
    );
    renderPage();
    await screen.findByText("RUNNING");

    await userEvent.selectOptions(screen.getByLabelText("order type"), "limit");
    await userEvent.type(screen.getByLabelText("limit price"), "505.25");
    const qty = screen.getByLabelText("quantity");
    await userEvent.clear(qty);
    await userEvent.type(qty, "40");
    await userEvent.click(screen.getByRole("button", { name: "Buy" }));

    await waitFor(() => expect(bodies.length).toBe(1));
    const sent = JSON.parse(bodies[0]!);
    expect(sent.limit_price).toBe(505.25);
    expect(sent.quantity).toBe(40);
    expect(sent.type).toBe("limit");
    expect(sent.side).toBe(1);
  });

  it("reports an order as queued, never as filled", async () => {
    vi.stubGlobal("fetch", stub(RUNNING, NO_PENDING));
    renderPage();
    await screen.findByText("RUNNING");
    await userEvent.click(screen.getByRole("button", { name: "Buy" }));
    expect(await screen.findByText(/queued/)).toBeInTheDocument();
  });

  it("lists working orders and offers a cancel", async () => {
    const calls: string[] = [];
    vi.stubGlobal(
      "fetch",
      stub(RUNNING, NO_PENDING, (u, init) => {
        if (init?.method === "DELETE") calls.push(u);
      }),
    );
    renderPage();
    await screen.findByText("RUNNING");

    // Only the working order appears here; the filled one belongs in the blotter.
    expect(await screen.findByText("7")).toBeInTheDocument();
    await userEvent.click(screen.getByRole("button", { name: "cancel order 7" }));
    await waitFor(() =>
      expect(calls.some((c) => c.endsWith("/trading/orders/7"))).toBe(true),
    );
  });

  it("shows executions in the blotter with fees", async () => {
    vi.stubGlobal("fetch", stub(RUNNING, NO_PENDING));
    renderPage();
    expect(await screen.findByText("503.17")).toBeInTheDocument();
    expect(screen.getByText("0.0125")).toBeInTheDocument();
  });

  it("surfaces a rejected request with the engine's reason", async () => {
    // An order that vanished would leave the user believing it was live.
    const rejected = {
      pending: 0,
      outcomes: [
        { request_id: 3, order_id: 0, accepted: false, detail: "price_collar: limit price is far from the market" },
      ],
    };
    vi.stubGlobal("fetch", stub(RUNNING, rejected));
    renderPage();
    expect(await screen.findByText(/price_collar/)).toBeInTheDocument();
  });

  it("shows how many requests are still queued", async () => {
    vi.stubGlobal("fetch", stub(RUNNING, { pending: 2, outcomes: [] }));
    renderPage();
    // Queued rather than lost: the count is what tells the user that.
    expect(await screen.findByText(/2 queued/)).toBeInTheDocument();
  });

  it("sends flatten and cancel-all", async () => {
    const posts: string[] = [];
    vi.stubGlobal(
      "fetch",
      stub(RUNNING, NO_PENDING, (u, init) => {
        if (init?.method === "POST") posts.push(u);
      }),
    );
    renderPage();
    await screen.findByText("RUNNING");

    await userEvent.click(screen.getByRole("button", { name: "Cancel all" }));
    await userEvent.click(screen.getByRole("button", { name: "Flatten" }));
    await waitFor(() => expect(posts.length).toBeGreaterThanOrEqual(2));
    expect(posts.some((p) => p.endsWith("/trading/cancel-all"))).toBe(true);
    expect(posts.some((p) => p.endsWith("/trading/flatten"))).toBe(true);
  });
});
