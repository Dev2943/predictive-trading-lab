import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { render, screen } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";

import { ConnectionGate } from "@/components/connection";

/**
 * Connection gate tests.
 *
 * The distinction under test: being offline and the backend being unreachable
 * look identical to a naive implementation, and a user can only fix the first.
 */
function renderGate() {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false } } });
  return render(
    <QueryClientProvider client={client}>
      <ConnectionGate>
        <p>application</p>
      </ConnectionGate>
    </QueryClientProvider>,
  );
}

afterEach(() => vi.restoreAllMocks());

describe("connection gate", () => {
  it("renders the application when healthy", async () => {
    vi.stubGlobal(
      "fetch",
      vi.fn(async () => new Response(JSON.stringify({ status: "ok", uptime_seconds: 1 }), { status: 200 })),
    );
    renderGate();
    expect(await screen.findByText("application")).toBeInTheDocument();
  });

  it("reports being offline distinctly from a dead backend", async () => {
    vi.stubGlobal("navigator", { onLine: false });
    vi.stubGlobal("fetch", vi.fn(async () => new Response("{}", { status: 200 })));
    renderGate();
    expect(await screen.findByText("You are offline")).toBeInTheDocument();
    expect(screen.queryByText("application")).not.toBeInTheDocument();
  });

  it("names a sleeping free-tier backend rather than showing a bare failure", async () => {
    vi.stubGlobal("navigator", { onLine: true });
    vi.stubGlobal("fetch", vi.fn(async () => { throw new TypeError("Failed to fetch"); }));
    renderGate();
    expect(await screen.findByText("Backend unavailable")).toBeInTheDocument();
    expect(screen.getByText(/waking from idle/)).toBeInTheDocument();
  });

  it("announces the retry to assistive technology", async () => {
    vi.stubGlobal("navigator", { onLine: false });
    vi.stubGlobal("fetch", vi.fn(async () => new Response("{}", { status: 200 })));
    renderGate();
    const status = await screen.findByRole("status");
    expect(status).toHaveAttribute("aria-live", "polite");
    expect(screen.getByRole("alert")).toBeInTheDocument();
  });
});
