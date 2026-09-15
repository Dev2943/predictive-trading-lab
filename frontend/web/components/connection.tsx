"use client";

import { useQuery } from "@tanstack/react-query";
import { useEffect, useState } from "react";
import { API_BASE, api } from "@/lib/api";

/**
 * Connection state for the whole application.
 *
 * TWO FAILURES THAT LOOK THE SAME AND ARE NOT: the browser being offline, and
 * the backend being unreachable while the network is fine. A user can fix the
 * first themselves; the second means the service is down. Telling them apart is
 * the entire point of this component.
 *
 * On a public deployment the backend may also be asleep — Render's free tier
 * spins down after inactivity and takes some seconds to wake. That reads as
 * "unreachable" without explanation, so it is named.
 */
export function ConnectionGate({ children }: { children: React.ReactNode }) {
  const [online, setOnline] = useState(true);

  useEffect(() => {
    // navigator.onLine is only meaningful in the browser.
    setOnline(navigator.onLine);
    const up = () => setOnline(true);
    const down = () => setOnline(false);
    window.addEventListener("online", up);
    window.addEventListener("offline", down);
    return () => {
      window.removeEventListener("online", up);
      window.removeEventListener("offline", down);
    };
  }, []);

  const health = useQuery({
    queryKey: ["healthz"],
    queryFn: api.healthz,
    // Retried on a cadence rather than once: a sleeping backend becomes
    // reachable on its own, and a page that gave up would need a manual reload.
    refetchInterval: (query) => (query.state.error ? 3000 : 20000),
    retry: false,
  });

  if (!online) {
    return (
      <Notice
        title="You are offline"
        detail="The browser reports no network connection. The interface will recover on its own when the connection returns."
      />
    );
  }

  if (health.isError) {
    return (
      <Notice
        title="Backend unavailable"
        detail={`No response from ${API_BASE}. If this is a free-tier deployment the service may be waking from idle, which takes a few seconds. Retrying automatically.`}
      />
    );
  }

  return <>{children}</>;
}

function Notice({ title, detail }: { title: string; detail: string }) {
  return (
    <div
      role="alert"
      className="rounded border border-warn/40 bg-warn/10 p-6 text-center"
    >
      <p className="text-warn">{title}</p>
      <p className="mt-2 text-xs text-content-faint">{detail}</p>
      <p role="status" aria-live="polite" className="mt-3 text-xs text-content-faint">
        retrying…
      </p>
    </div>
  );
}
