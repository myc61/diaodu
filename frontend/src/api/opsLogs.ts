export interface OpsLogEntry {
  id: number;
  level: "info" | "warn" | "error" | string;
  source: string;
  message: string;
  detail: Record<string, unknown>;
  at: number;
}

async function getJson<T>(path: string, signal?: AbortSignal): Promise<T> {
  const response = await fetch(path, {
    headers: { Accept: "application/json" },
    signal
  });
  if (!response.ok) {
    throw new Error("request failed: " + response.status);
  }
  return (await response.json()) as T;
}

export function listOpsLogs(
  params?: { limit?: number; after_id?: number },
  signal?: AbortSignal
): Promise<{ items: OpsLogEntry[] }> {
  const query = new URLSearchParams();
  if (params?.limit != null) {
    query.set("limit", String(params.limit));
  }
  if (params?.after_id != null) {
    query.set("after_id", String(params.after_id));
  }
  const suffix = query.toString() ? `?${query.toString()}` : "";
  return getJson(`/api/v1/ops-logs${suffix}`, signal);
}
