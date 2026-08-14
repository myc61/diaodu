import type { SystemHealth, SystemSummary } from "../types/system";

async function getJson<T>(path: string, signal?: AbortSignal): Promise<T> {
  const response = await fetch(path, {
    headers: {
      Accept: "application/json"
    },
    signal
  });

  if (!response.ok) {
    throw new Error("request failed: " + response.status);
  }

  return (await response.json()) as T;
}

export function getSystemHealth(signal?: AbortSignal): Promise<SystemHealth> {
  return getJson<SystemHealth>("/api/v1/health", signal);
}

export function getSystemSummary(signal?: AbortSignal): Promise<SystemSummary> {
  return getJson<SystemSummary>("/api/v1/system/summary", signal);
}

