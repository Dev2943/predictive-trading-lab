"use client";

import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { useState, type ReactNode } from "react";

/**
 * React Query holds every piece of engine-derived state.
 *
 * `staleTime` is short and refetch-on-focus is on: a trader returning to the
 * tab must see current numbers, not whatever was cached when they left. Stale
 * P&L presented without qualification is worse than a spinner.
 */
export function Providers({ children }: { children: ReactNode }) {
  const [client] = useState(
    () =>
      new QueryClient({
        defaultOptions: {
          queries: {
            staleTime: 2_000,
            refetchOnWindowFocus: true,
            retry: 1,
          },
        },
      }),
  );

  return <QueryClientProvider client={client}>{children}</QueryClientProvider>;
}
