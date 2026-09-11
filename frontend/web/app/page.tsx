"use client";

import { useQuery } from "@tanstack/react-query";
import { Dashboard } from "@/components/dashboard";
import { api } from "@/lib/api";

export default function Home() {
  const system = useQuery({ queryKey: ["system"], queryFn: api.system });

  return (
    <>
      {system.data && (
        <div className="mb-4 text-right text-xs text-content-faint">
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
      <Dashboard />
    </>
  );
}
