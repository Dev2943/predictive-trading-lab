"use client";

import { useQuery } from "@tanstack/react-query";
import { Dashboard } from "@/components/dashboard";
import { api } from "@/lib/api";

export default function Home() {
  const system = useQuery({ queryKey: ["system"], queryFn: api.system });

  return (
    <main className="mx-auto max-w-6xl p-8 font-mono text-sm">
      <header className="mb-6 flex items-baseline justify-between">
        <div>
          <h1 className="text-lg">Predictive Trading Lab</h1>
          <p className="text-content-faint">Paper trading — F3</p>
        </div>
        {system.data && (
          <div className="text-right text-xs text-content-faint">
            <div>
              engine {system.data.version.engine_version} · {system.data.version.build_type}
            </div>
            <div
              className={
                system.data.fingerprints?.matches_reference ? "text-gain" : "text-warn"
              }
            >
              {system.data.fingerprints?.matches_reference
                ? "reproduces the v1.0 reference build"
                : "does not match the reference build"}
            </div>
          </div>
        )}
      </header>

      <Dashboard />
    </main>
  );
}
