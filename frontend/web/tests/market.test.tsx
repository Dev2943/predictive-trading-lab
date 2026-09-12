import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

import MarketPage from "@/app/market/page";

/**
 * Market page tests.
 *
 * The WebSocket is stubbed with a controllable fake, so streaming behaviour is
 * tested without a server. The point is the client's behaviour: does it label
 * the source honestly, show absence as absence, and report a dropped stream.
 */

const STATUS = {
  mode: "replay",
  connected: true,
  provider: "synthetic-replay",
  detail: "deterministic synthetic series; not market data",
  symbols: ["SPY"],
  last_update: null,
};

const QUOTES = [
  {
    symbol: "SPY",
    bid: 503.11,
    ask: 503.13,
    last: 503.12,
    volume: 1000,
    ts: null,
    source: "replay",
    spread_value: 0.02,
    spread_bps: 0.4,
  },
];

class FakeSocket {
  static last: FakeSocket | null = null;
  onopen: (() => void) | null = null;
  onmessage: ((e: { data: string }) => void) | null = null;
  onclose: (() => void) | null = null;
  onerror: (() => void) | null = null;
  closed = false;
  constructor(public url: string) {
    FakeSocket.last = this;
    queueMicrotask(() => this.onopen?.());
  }
  send() {}
  close() {
    this.closed = true;
    this.onclose?.();
  }
  push(frame: unknown) {
    this.onmessage?.({ data: JSON.stringify(frame) });
  }
}

function stubFetch(status = STATUS, quotes = QUOTES) {
  return vi.fn(async (url: string, init?: RequestInit) => {
    if (init?.method === "POST") {
      return new Response(JSON.stringify(status), { status: 200 });
    }
    const body = url.includes("/market/quotes") ? quotes : status;
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
      <MarketPage />
    </QueryClientProvider>,
  );
}

beforeEach(() => {
  vi.stubGlobal("WebSocket", FakeSocket as unknown as typeof WebSocket);
});
afterEach(() => vi.restoreAllMocks());

describe("market page", () => {
  it("renders from REST before the socket delivers anything", async () => {
    // The table must be populated on first paint, not blank until a frame
    // arrives.
    vi.stubGlobal("fetch", stubFetch());
    renderPage();
    expect(await screen.findByText("503.11")).toBeInTheDocument();
  });

  it("labels replay as synthetic, never as a market", async () => {
    vi.stubGlobal("fetch", stubFetch());
    renderPage();
    expect(await screen.findByText(/REPLAY \(synthetic\)/)).toBeInTheDocument();
    expect(await screen.findByText(/not market data/)).toBeInTheDocument();
  });

  it("updates from a streamed frame", async () => {
    vi.stubGlobal("fetch", stubFetch());
    renderPage();
    await screen.findByText("503.11");

    await waitFor(() => expect(FakeSocket.last).not.toBeNull());
    FakeSocket.last!.push({
      status: STATUS,
      quotes: [{ ...QUOTES[0], bid: 504.01, ask: 504.03 }],
    });
    expect(await screen.findByText("504.01")).toBeInTheDocument();
  });

  it("reports a dropped stream rather than leaving stale prices unlabelled", async () => {
    vi.stubGlobal("fetch", stubFetch());
    renderPage();
    await waitFor(() => expect(screen.getByText("connected")).toBeInTheDocument());

    FakeSocket.last!.close();
    expect(await screen.findByText("disconnected")).toBeInTheDocument();
  });

  it("shows an absent bid as a dash, not zero", async () => {
    // A missing bid and a bid of zero are different observations.
    const silent = [{ ...QUOTES[0], bid: null, ask: null, spread_value: null, spread_bps: null }];
    vi.stubGlobal("fetch", stubFetch(STATUS, silent));
    renderPage();
    await screen.findByText("SPY");
    expect((await screen.findAllByText("—")).length).toBeGreaterThanOrEqual(3);
    expect(screen.queryByText("0.00")).not.toBeInTheDocument();
  });

  it("surfaces the refusal when live is unavailable", async () => {
    // Never a silent downgrade to replay.
    vi.stubGlobal(
      "fetch",
      vi.fn(async (url: string, init?: RequestInit) => {
        if (init?.method === "POST") {
          return new Response(
            JSON.stringify({ detail: "no live provider is configured; set PTL_ALPACA_KEY" }),
            { status: 422 },
          );
        }
        const body = url.includes("/market/quotes") ? QUOTES : STATUS;
        return new Response(JSON.stringify(body), { status: 200 });
      }),
    );
    renderPage();
    await screen.findByText("SPY");

    await userEvent.click(screen.getByRole("button", { name: "Live" }));
    expect(await screen.findByText(/no live provider is configured/)).toBeInTheDocument();
  });

  it("sends a replacement watchlist", async () => {
    const posts: string[] = [];
    vi.stubGlobal(
      "fetch",
      vi.fn(async (url: string, init?: RequestInit) => {
        if (init?.method === "POST") {
          posts.push(String(init.body));
          return new Response(JSON.stringify(STATUS), { status: 200 });
        }
        const body = url.includes("/market/quotes") ? QUOTES : STATUS;
        return new Response(JSON.stringify(body), { status: 200 });
      }),
    );
    renderPage();
    await screen.findByText("SPY");

    await userEvent.type(screen.getByLabelText("symbols"), "tsla, nvda");
    await userEvent.click(screen.getByRole("button", { name: "Replace" }));
    await waitFor(() => expect(posts.length).toBe(1));
    expect(JSON.parse(posts[0]!).symbols).toEqual(["tsla", "nvda"]);
  });
});
