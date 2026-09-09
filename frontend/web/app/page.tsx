"use client";

import { useQuery } from "@tanstack/react-query";
import { api } from "@/lib/api";

/**
 * F1 status page.
 *
 * Deliberately minimal: F1 is infrastructure, and this exists only to prove the
 * chain React -> FastAPI -> pybind11 -> C++ engine is connected end to end. The
 * dashboard and trading screens arrive in later phases.
 */
export default function Home() {
  const version = useQuery({ queryKey: ["version"], queryFn: api.version });
  const fingerprints = useQuery({
    queryKey: ["fingerprints"],
    queryFn: api.fingerprints,
  });

  return (
    <main className="mx-auto max-w-3xl p-10 font-mono text-sm">
      <h1 className="mb-1 text-lg text-content">Predictive Trading Lab</h1>
      <p className="mb-8 text-content-faint">Frontend F1 — infrastructure only</p>

      <section className="mb-6 rounded border border-surface-border bg-surface-raised p-4">
        <h2 className="mb-3 text-content-muted">Engine</h2>
        {version.isPending && <p className="text-content-faint">connecting…</p>}
        {version.isError && (
          /* The gateway's message, not a generic one: it says whether the
             engine is unreachable or the request was wrong. */
          <p className="text-loss">{(version.error as Error).message}</p>
        )}
        {version.data && (
          <dl className="grid grid-cols-2 gap-y-1">
            <dt className="text-content-faint">version</dt>
            <dd>{version.data.engine_version}</dd>
            <dt className="text-content-faint">compiler</dt>
            <dd>{version.data.compiler}</dd>
            <dt className="text-content-faint">transport</dt>
            <dd>{version.data.transport}</dd>
          </dl>
        )}
      </section>

      <section className="rounded border border-surface-border bg-surface-raised p-4">
        <h2 className="mb-3 text-content-muted">Determinism</h2>
        {fingerprints.data && (
          <>
            <dl className="grid grid-cols-2 gap-y-1">
              <dt className="text-content-faint">config hash</dt>
              <dd>{fingerprints.data.config_hash}</dd>
              <dt className="text-content-faint">rng[0..2]</dt>
              <dd className="truncate">{fingerprints.data.rng.join(" ")}</dd>
            </dl>
            {/* Whether this engine reproduces the reference build is worth
                knowing before anyone trusts a backtest from it. */}
            <p
              className={`mt-3 ${
                fingerprints.data.matches_reference ? "text-gain" : "text-warn"
              }`}
            >
              {fingerprints.data.matches_reference
                ? "matches the v1.0 reference build"
                : "DOES NOT match the v1.0 reference build"}
            </p>
          </>
        )}
      </section>
    </main>
  );
}
