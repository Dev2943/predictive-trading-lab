"use client";

import { ApiError } from "@/lib/api";

/**
 * Shared error, loading and empty states.
 *
 * Before F8 each page reported failures its own way: some rendered the raw
 * message, some rendered nothing, and the dashboard had a third shape. The same
 * 409 therefore looked like three different problems depending on where you
 * stood.
 *
 * One component, so a failure reads the same everywhere and the engine's own
 * reason always survives to the screen.
 */

function explain(error: unknown): { message: string; hint?: string } {
  if (error instanceof ApiError) {
    // The status carries meaning the message alone does not: a 409 is a timing
    // problem the user can retry, a 422 is something they must change.
    if (error.status === 409) {
      return {
        message: error.message,
        hint: "the session state does not permit that right now",
      };
    }
    if (error.status === 422) {
      return { message: error.message, hint: "adjust the request and try again" };
    }
    if (error.status === 503) {
      return {
        message: error.message,
        hint: "the engine is not reachable — is the gateway running?",
      };
    }
    return { message: error.message };
  }
  if (error instanceof Error) {
    // A fetch that never reached the gateway produces a bare TypeError, which
    // is useless on screen.
    if (error.message === "Failed to fetch") {
      return {
        message: "could not reach the API",
        hint: "check that the gateway is running",
      };
    }
    return { message: error.message };
  }
  return { message: "an unexpected error occurred" };
}

export function ErrorPanel({ error }: { error: unknown }) {
  if (!error) return null;
  const { message, hint } = explain(error);
  return (
    <div
      role="alert"
      className="rounded border border-loss/40 bg-loss/10 p-3 text-loss"
    >
      <p>{message}</p>
      {hint && <p className="mt-1 text-xs text-content-faint">{hint}</p>}
    </div>
  );
}

export function Loading({ label = "loading" }: { label?: string }) {
  return (
    <p role="status" aria-live="polite" className="py-4 text-content-faint">
      {label}…
    </p>
  );
}

export function Empty({ message }: { message: string }) {
  // Distinct from Loading on purpose: "nothing here" and "not known yet" are
  // different answers, and a reader must be able to tell which they got.
  return <p className="py-6 text-center text-content-faint">{message}</p>;
}
