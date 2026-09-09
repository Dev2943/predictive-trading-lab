/**
 * The single point of contact with the gateway.
 *
 * EVERY network call in the application goes through here. No component calls
 * fetch directly, so when the engine moves behind a remote gateway the change
 * is one base URL in one file.
 *
 * The web layer never computes portfolio state, never derives a metric the
 * engine already produces, and never holds trading state of its own. It
 * visualises what the API returns.
 */

const BASE = process.env.NEXT_PUBLIC_API_BASE ?? "http://localhost:8000";

export class ApiError extends Error {
  constructor(
    message: string,
    readonly status: number,
  ) {
    super(message);
    this.name = "ApiError";
  }
}

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const response = await fetch(`${BASE}${path}`, {
    ...init,
    headers: { "Content-Type": "application/json", ...init?.headers },
  });

  if (!response.ok) {
    // The gateway puts the engine's own reason in `detail`. Surfacing it beats
    // a generic message: "minimum variance needs a covariance matrix" tells the
    // user what to do, and "Request failed" does not.
    let detail = response.statusText;
    try {
      detail = ((await response.json()) as { detail?: string }).detail ?? detail;
    } catch {
      /* response had no JSON body */
    }
    throw new ApiError(detail, response.status);
  }
  return (await response.json()) as T;
}

export interface VersionInfo {
  engine_version: string;
  compiler: string;
  build_type: string;
  transport: "in-process" | "remote";
  gateway_version: string;
}

export interface Fingerprints {
  config_hash: string;
  rng: string[];
  matches_reference: boolean;
}

export interface HealthResponse {
  status: "ok" | "degraded" | "unavailable";
  engine_reachable: boolean;
  detail: string;
}

export interface SystemInfo {
  health: HealthResponse;
  version: VersionInfo;
  fingerprints: Fingerprints | null;
}

export const api = {
  health: () => request<HealthResponse>("/health"),
  version: () => request<VersionInfo>("/version"),
  fingerprints: () => request<Fingerprints>("/fingerprints"),
  /** One request for a status header, rather than three. */
  system: () => request<SystemInfo>("/system"),
};
