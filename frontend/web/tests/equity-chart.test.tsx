import { render, screen } from "@testing-library/react";
import { describe, expect, it, vi } from "vitest";

import { EquityChart } from "@/components/equity-chart";
import type { EquityHistory } from "@/lib/api";

/**
 * Chart tests.
 *
 * recharts measures its container, which jsdom reports as zero, so the SVG
 * never renders in a test environment. ResponsiveContainer is stubbed with a
 * fixed-size div: the charts' data handling is what is under test, not
 * recharts' own layout.
 */
vi.mock("recharts", async () => {
  const actual = await vi.importActual<typeof import("recharts")>("recharts");
  return {
    ...actual,
    ResponsiveContainer: ({ children }: { children: React.ReactNode }) => (
      <div style={{ width: 600, height: 300 }}>{children}</div>
    ),
  };
});

function history(overrides: Partial<EquityHistory> = {}): EquityHistory {
  return {
    available: true,
    total_points: 3,
    stride: 1,
    max_drawdown: 0.0125,
    current_drawdown: 0.004,
    peak_equity: 1020,
    points: [
      { ts: "2024-07-02T14:30:00Z", equity: 1000, cash: 1000, realized_pnl: 0, unrealized_pnl: 0, gross_exposure: 0, net_exposure: 0 },
      { ts: "2024-07-02T14:31:00Z", equity: 1020, cash: 1000, realized_pnl: 20, unrealized_pnl: 0, gross_exposure: 0, net_exposure: 0 },
      { ts: "2024-07-02T14:32:00Z", equity: 1016, cash: 1000, realized_pnl: 16, unrealized_pnl: 0, gross_exposure: 0, net_exposure: 0 },
    ],
    ...overrides,
  };
}

describe("equity chart", () => {
  it("shows a prompt rather than an empty chart when there is no history", () => {
    // Absence is not a flat line at zero.
    render(<EquityChart history={undefined} />);
    expect(screen.getByText(/no history yet/)).toBeInTheDocument();
  });

  it("treats an available-but-empty series as no history", () => {
    render(<EquityChart history={history({ available: true, points: [], total_points: 0 })} />);
    expect(screen.getByText(/no history yet/)).toBeInTheDocument();
  });

  it("reports the drawdown the engine computed", () => {
    // Not recomputed locally: a second definition would eventually disagree
    // with the risk engine that halts on it.
    render(<EquityChart history={history()} />);
    expect(screen.getByText("1.25%")).toBeInTheDocument();
    expect(screen.getByText("0.40%")).toBeInTheDocument();
  });

  it("shows the change from the first sample", () => {
    render(<EquityChart history={history()} />);
    // 1016 - 1000, and positive changes carry a sign.
    expect(screen.getByText("+16.00")).toBeInTheDocument();
  });

  it("reports a loss without a plus sign", () => {
    const losing = history({
      points: [
        { ts: "2024-07-02T14:30:00Z", equity: 1000, cash: 1000, realized_pnl: 0, unrealized_pnl: 0, gross_exposure: 0, net_exposure: 0 },
        { ts: "2024-07-02T14:31:00Z", equity: 980, cash: 1000, realized_pnl: -20, unrealized_pnl: 0, gross_exposure: 0, net_exposure: 0 },
      ],
    });
    render(<EquityChart history={losing} />);
    expect(screen.getByText("-20.00")).toBeInTheDocument();
  });

  it("discloses downsampling rather than hiding it", () => {
    render(<EquityChart history={history({ total_points: 900, stride: 4 })} />);
    expect(screen.getByText(/900 samples/)).toBeInTheDocument();
    expect(screen.getByText(/every 4/)).toBeInTheDocument();
  });
});
