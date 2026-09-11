import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { afterEach, describe, expect, it, vi } from "vitest";

import OptimizationPage from "@/app/optimization/page";
import RiskPage from "@/app/risk/page";

/**
 * Optimization and risk page tests.
 *
 * `fetch` is stubbed. What is under test is the client's behaviour: does it
 * send what the user typed, render what the engine returned, and surface the
 * engine's refusal rather than inventing a result.
 */

const CAPABILITIES = {
  count: 2,
  note: "",
  optimizers: [
    { name: "risk_parity", requires_covariance: false, requires_expected_returns: false },
    { name: "minimum_variance", requires_covariance: true, requires_expected_returns: false },
  ],
};

const RESULT = {
  status: "optimal",
  weights: [0.5, 0.3, 0.2],
  symbols: ["AAA", "BBB", "CCC"],
  expected_return: 0.04,
  expected_volatility: 0.11,
  sharpe: 0.36,
  gross_exposure: 1.0,
  net_exposure: 1.0,
  cash_weight: 0.0,
  turnover: 0.0,
  iterations: 12,
  binding_constraints: ["max_position"],
  detail: "",
};

function renderPage(node: React.ReactElement) {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false } } });
  return render(<QueryClientProvider client={client}>{node}</QueryClientProvider>);
}

afterEach(() => vi.restoreAllMocks());

describe("optimization page", () => {
  it("lists the optimizers the engine reports", async () => {
    vi.stubGlobal(
      "fetch",
      vi.fn(async () => new Response(JSON.stringify(CAPABILITIES), { status: 200 })),
    );
    renderPage(<OptimizationPage />);
    await waitFor(() =>
      expect(screen.getByRole("option", { name: "risk_parity" })).toBeInTheDocument(),
    );
  });

  it("warns before sending a request the engine will refuse", async () => {
    // The engine's own requirement, surfaced ahead of the refusal rather than
    // after it.
    vi.stubGlobal(
      "fetch",
      vi.fn(async () => new Response(JSON.stringify(CAPABILITIES), { status: 200 })),
    );
    renderPage(<OptimizationPage />);
    await screen.findByRole("option", { name: "minimum_variance" });

    await userEvent.selectOptions(screen.getByLabelText("optimizer"), "minimum_variance");
    expect(await screen.findByText(/needs a covariance matrix/)).toBeInTheDocument();
  });

  it("renders the weights the engine returned", async () => {
    vi.stubGlobal(
      "fetch",
      vi.fn(async (_url: string, init?: RequestInit) =>
        init?.method === "POST"
          ? new Response(JSON.stringify(RESULT), { status: 200 })
          : new Response(JSON.stringify(CAPABILITIES), { status: 200 }),
      ),
    );
    renderPage(<OptimizationPage />);
    await screen.findByRole("option", { name: "risk_parity" });

    await userEvent.click(screen.getByRole("button", { name: "Optimize" }));
    expect(await screen.findByText("50.00%")).toBeInTheDocument();
    expect(screen.getByText("30.00%")).toBeInTheDocument();
    // A weight on a limit is a constrained answer, and saying so stops it
    // being read as a free optimum.
    expect(screen.getByText(/binding: max_position/)).toBeInTheDocument();
  });

  it("sends the values the user typed", async () => {
    const bodies: string[] = [];
    vi.stubGlobal(
      "fetch",
      vi.fn(async (_url: string, init?: RequestInit) => {
        if (init?.method === "POST") {
          bodies.push(String(init.body));
          return new Response(JSON.stringify(RESULT), { status: 200 });
        }
        return new Response(JSON.stringify(CAPABILITIES), { status: 200 });
      }),
    );
    renderPage(<OptimizationPage />);
    await screen.findByRole("option", { name: "risk_parity" });

    const vol = screen.getByLabelText("volatility 0");
    await userEvent.clear(vol);
    await userEvent.type(vol, "0.33");
    await userEvent.click(screen.getByRole("button", { name: "Optimize" }));

    await waitFor(() => expect(bodies.length).toBe(1));
    expect(JSON.parse(bodies[0]!).volatilities[0]).toBe(0.33);
  });

  it("surfaces the engine's refusal instead of inventing a result", async () => {
    vi.stubGlobal(
      "fetch",
      vi.fn(async (_url: string, init?: RequestInit) =>
        init?.method === "POST"
          ? new Response(
              JSON.stringify({ detail: "minimum variance needs a covariance matrix" }),
              { status: 422 },
            )
          : new Response(JSON.stringify(CAPABILITIES), { status: 200 }),
      ),
    );
    renderPage(<OptimizationPage />);
    await screen.findByRole("option", { name: "risk_parity" });

    await userEvent.click(screen.getByRole("button", { name: "Optimize" }));
    expect(
      await screen.findByText(/minimum variance needs a covariance matrix/),
    ).toBeInTheDocument();
    // No weights table appeared.
    expect(screen.queryByText("50.00%")).not.toBeInTheDocument();
  });
});

describe("risk page", () => {
  it("reports a clean validation", async () => {
    vi.stubGlobal(
      "fetch",
      vi.fn(async () =>
        new Response(JSON.stringify({ ok: true, fatal: 0, warnings: 0, issues: [] }), {
          status: 200,
        }),
      ),
    );
    renderPage(<RiskPage />);
    await userEvent.click(screen.getByRole("button", { name: "Validate" }));
    expect(await screen.findByText("limits are valid")).toBeInTheDocument();
  });

  it("shows every issue with its remedy", async () => {
    // An error saying only "invalid" makes the operator guess.
    vi.stubGlobal(
      "fetch",
      vi.fn(async () =>
        new Response(
          JSON.stringify({
            ok: false,
            fatal: 1,
            warnings: 1,
            issues: [
              {
                severity: "fatal",
                field: "risk.max_order_notional",
                message: "limit is zero; every order would be rejected",
                remedy: "set a positive notional limit",
              },
              {
                severity: "warning",
                field: "risk.max_gross_leverage",
                message: "so large it can never bind",
                remedy: "a limit that cannot trigger provides no protection",
              },
            ],
          }),
          { status: 200 },
        ),
      ),
    );
    renderPage(<RiskPage />);
    await userEvent.click(screen.getByRole("button", { name: "Validate" }));

    expect(await screen.findByText("limits are not usable")).toBeInTheDocument();
    expect(screen.getByText("risk.max_order_notional")).toBeInTheDocument();
    expect(screen.getByText(/set a positive notional limit/)).toBeInTheDocument();
    expect(screen.getByText("[warning]")).toBeInTheDocument();
  });

  it("accepts a decimal limit without mangling it", async () => {
    // REGRESSION. Coercing with Number() on every keystroke turned "0.25" into
    // 25: typing passes through "0.", Number() makes that 0, and the decimal
    // point is discarded. Every limit here except the notionals is a fraction,
    // so the field was unusable for its main purpose.
    const bodies: string[] = [];
    vi.stubGlobal(
      "fetch",
      vi.fn(async (_url: string, init?: RequestInit) => {
        bodies.push(String(init?.body));
        return new Response(JSON.stringify({ ok: true, fatal: 0, warnings: 0, issues: [] }), {
          status: 200,
        });
      }),
    );
    renderPage(<RiskPage />);
    const field = screen.getByLabelText("max drawdown");
    await userEvent.clear(field);
    await userEvent.type(field, "0.35");
    expect(field).toHaveValue("0.35");

    await userEvent.click(screen.getByRole("button", { name: "Validate" }));
    await waitFor(() => expect(bodies.length).toBe(1));
    expect(JSON.parse(bodies[0]!).max_drawdown_pct).toBe(0.35);
  });

  it("sends the edited limits", async () => {
    const bodies: string[] = [];
    vi.stubGlobal(
      "fetch",
      vi.fn(async (_url: string, init?: RequestInit) => {
        bodies.push(String(init?.body));
        return new Response(JSON.stringify({ ok: true, fatal: 0, warnings: 0, issues: [] }), {
          status: 200,
        });
      }),
    );
    renderPage(<RiskPage />);
    const field = screen.getByLabelText("max concentration");
    await userEvent.clear(field);
    await userEvent.type(field, "0.25");
    await userEvent.click(screen.getByRole("button", { name: "Validate" }));

    await waitFor(() => expect(bodies.length).toBe(1));
    expect(JSON.parse(bodies[0]!).max_concentration).toBe(0.25);
  });
});
