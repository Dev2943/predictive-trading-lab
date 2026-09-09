"use client";

import { useQuery } from "@tanstack/react-query";
import { api } from "@/lib/api";

/**
 * F2 status page.
 *
 * Deliberately minimal: F2 is the read-only API layer, and this page exists to
 * verify the chain React -> FastAPI -> EngineClient -> pybind11 -> C++ engine
 * is connected. Dashboards and trading screens arrive in later phases.
 *
 * Note what it does NOT do: it computes nothing. Every value shown is one the
 * engine produced, fetched through the gateway.
 */

function Row({ label, value }: { label: string; value: string }) {
  return (
    <>
      <dt className="text-content-faint">{label}</dt>
      <dd className="truncate">{value}</dd>
    </>
  );
}

export default function Home() {
  const system = useQuery({ queryKey: ["system"], queryFn: api.system });

  return (
    <main className="mx-auto max-w-3xl p-10 font-mono text-sm">
      <h1 className="mb-1 text-lg">Predictive Trading Lab</h1>
      <p className="mb-8 text-content-faint">Frontend F2 — read-only API</p>

      {system.isPending && <p className="text-content-faint">connecting…</p>}

      {system.isError && (
        <div className="rounded border border-loss/40 bg-loss/10 p-4">
          {/* The gateway's own message. It distinguishes an unreachable engine
              from a bad request, and a generic string would not. */}
          <p className="text-loss">{(system.error as Error).message}</p>
          <p className="mt-2 text-content-faint">
            Is the gateway running on {process.env.NEXT_PUBLIC_API_BASE}?
          </p>
        </div>
      )}

      {system.data && (
        <div className="space-y-6">
          <section className="rounded border border-surface-border bg-surface-raised p-4">
            <h2 className="mb-3 text-content-muted">Engine</h2>
            <dl className="grid grid-cols-2 gap-y-1">
              <Row label="version" value={system.data.version.engine_version} />
              <Row label="compiler" value={system.data.version.compiler} />
              <Row label="build type" value={system.data.version.build_type} />
              <Row label="transport" value={system.data.version.transport} />
              <Row label="gateway" value={system.data.version.gateway_version} />
            </dl>
          </section>

          <section className="rounded border border-surface-border bg-surface-raised p-4">
            <h2 className="mb-3 text-content-muted">Health</h2>
            <p
              className={
                system.data.health.engine_reachable ? "text-gain" : "text-loss"
              }
            >
              {system.data.health.status}
              {system.data.health.detail && ` — ${system.data.health.detail}`}
            </p>
          </section>

          <section className="rounded border border-surface-border bg-surface-raised p-4">
            <h2 className="mb-3 text-content-muted">Determinism</h2>
            {system.data.fingerprints ? (
              <>
                <dl className="grid grid-cols-2 gap-y-1">
                  <Row
                    label="config hash"
                    value={system.data.fingerprints.config_hash}
                  />
                  <Row
                    label="rng[0..2]"
                    value={system.data.fingerprints.rng.join(" ")}
                  />
                </dl>
                <p
                  className={`mt-3 ${
                    system.data.fingerprints.matches_reference
                      ? "text-gain"
                      : "text-warn"
                  }`}
                >
                  {system.data.fingerprints.matches_reference
                    ? "matches the v1.0 reference build"
                    : "DOES NOT match the v1.0 reference build"}
                </p>
              </>
            ) : (
              /* Omitted rather than faked: a fabricated hash would read as a
                 real mismatch. */
              <p className="text-content-faint">
                unavailable — the configuration could not be read
              </p>
            )}
          </section>
        </div>
      )}
    </main>
  );
}
