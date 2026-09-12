import { render, screen } from "@testing-library/react";
import { describe, expect, it } from "vitest";

import { Empty, ErrorPanel, Loading } from "@/components/feedback";
import { ApiError } from "@/lib/api";

/**
 * Shared feedback tests.
 *
 * These exist because before F8 the same failure looked like three different
 * problems depending on which page you were standing on.
 */
describe("error panel", () => {
  it("renders nothing when there is no error", () => {
    const { container } = render(<ErrorPanel error={null} />);
    expect(container).toBeEmptyDOMElement();
  });

  it("keeps the engine's own message", () => {
    render(<ErrorPanel error={new ApiError("minimum variance needs a covariance", 422)} />);
    expect(screen.getByText(/needs a covariance/)).toBeInTheDocument();
  });

  it("explains a 409 as a timing problem, not a bad request", () => {
    render(<ErrorPanel error={new ApiError("cannot start while RUNNING", 409)} />);
    expect(screen.getByText(/does not permit that right now/)).toBeInTheDocument();
  });

  it("explains a 503 as the engine being unreachable", () => {
    render(<ErrorPanel error={new ApiError("session host unavailable", 503)} />);
    expect(screen.getByText(/is the gateway running/)).toBeInTheDocument();
  });

  it("turns a bare fetch failure into something readable", () => {
    // "Failed to fetch" on screen tells a user nothing actionable.
    render(<ErrorPanel error={new TypeError("Failed to fetch")} />);
    expect(screen.getByText("could not reach the API")).toBeInTheDocument();
  });

  it("is announced to assistive technology", () => {
    render(<ErrorPanel error={new Error("boom")} />);
    expect(screen.getByRole("alert")).toBeInTheDocument();
  });
});

describe("loading and empty", () => {
  it("announces loading politely", () => {
    render(<Loading label="computing" />);
    const status = screen.getByRole("status");
    expect(status).toHaveTextContent("computing");
    expect(status).toHaveAttribute("aria-live", "polite");
  });

  it("states emptiness distinctly from loading", () => {
    // "nothing here" and "not known yet" are different answers.
    render(<Empty message="no working orders" />);
    expect(screen.getByText("no working orders")).toBeInTheDocument();
    expect(screen.queryByRole("status")).not.toBeInTheDocument();
  });
});
