import type { RobotConfig, RobotUpsertPayload } from "../types/robot";
import type {
  RobotStartupProfile,
  RobotStartupProfilePayload
} from "../types/workflow";

const REQUEST_TIMEOUT_MS = 15_000;

async function requestJson<T>(
  path: string,
  init?: RequestInit,
  signal?: AbortSignal
): Promise<T> {
  const controller = new AbortController();
  let timedOut = false;
  const timeoutId = globalThis.setTimeout(() => {
    timedOut = true;
    controller.abort();
  }, REQUEST_TIMEOUT_MS);
  const abortRequest = () => controller.abort();
  if (signal) {
    if (signal.aborted) {
      controller.abort();
    } else {
      signal.addEventListener("abort", abortRequest, { once: true });
    }
  }

  try {
    const response = await fetch(path, {
      ...init,
      headers: {
        Accept: "application/json",
        ...(init?.headers ?? {})
      },
      signal: controller.signal
    });
    if (!response.ok) {
      let detail = response.statusText;
      try {
        const body = (await response.json()) as { error?: string };
        if (body.error) {
          detail = body.error;
        }
      } catch {
        // ignore
      }
      throw new Error(detail || `request failed: ${response.status}`);
    }
    return (await response.json()) as T;
  } catch (error) {
    if (timedOut) {
      throw new Error("请求超时，请检查 Dispatcher 服务状态");
    }
    throw error;
  } finally {
    globalThis.clearTimeout(timeoutId);
    signal?.removeEventListener("abort", abortRequest);
  }
}

export function listRobotConfigs(
  signal?: AbortSignal
): Promise<{ items: RobotConfig[] }> {
  return requestJson("/api/v1/robots", undefined, signal);
}

export function getRobotConfig(
  id: string,
  signal?: AbortSignal
): Promise<RobotConfig> {
  return requestJson(`/api/v1/robots/${id}`, undefined, signal);
}

export function createRobotConfig(
  payload: RobotUpsertPayload,
  signal?: AbortSignal
): Promise<RobotConfig> {
  return requestJson(
    "/api/v1/robots",
    {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload)
    },
    signal
  );
}

export function updateRobotConfig(
  id: string,
  payload: RobotUpsertPayload,
  signal?: AbortSignal
): Promise<RobotConfig> {
  return requestJson(
    `/api/v1/robots/${id}`,
    {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload)
    },
    signal
  );
}

export function deleteRobotConfig(
  id: string,
  signal?: AbortSignal
): Promise<{ deleted: boolean; id: string }> {
  return requestJson(
    `/api/v1/robots/${id}`,
    {
      method: "DELETE"
    },
    signal
  );
}

export function listRobotStartupProfiles(
  id: string,
  signal?: AbortSignal
): Promise<{ items: RobotStartupProfile[] }> {
  return requestJson(
    `/api/v1/robots/${id}/startup-profiles`,
    undefined,
    signal
  );
}

export function createRobotStartupProfile(
  id: string,
  payload: RobotStartupProfilePayload,
  signal?: AbortSignal
): Promise<RobotStartupProfile> {
  return requestJson(
    `/api/v1/robots/${id}/startup-profiles`,
    {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload)
    },
    signal
  );
}

export function updateRobotStartupProfile(
  robotId: string,
  profileId: string,
  payload: RobotStartupProfilePayload,
  signal?: AbortSignal
): Promise<RobotStartupProfile> {
  return requestJson(
    `/api/v1/robots/${robotId}/startup-profiles/${profileId}`,
    {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload)
    },
    signal
  );
}

export function deleteRobotStartupProfile(
  robotId: string,
  profileId: string,
  signal?: AbortSignal
): Promise<{ deleted: boolean }> {
  return requestJson(
    `/api/v1/robots/${robotId}/startup-profiles/${profileId}`,
    { method: "DELETE" },
    signal
  );
}

export interface DiscoveredEndpoint {
  name: string;
  message_type: string;
  operation_kind: string;
  transport: string;
}

export interface RobotInterfaceCatalog {
  entity_kind: string;
  entity_id: string;
  transport: string;
  scanned_at: string;
  live: boolean;
  topics: DiscoveredEndpoint[];
  services: DiscoveredEndpoint[];
  actions: DiscoveredEndpoint[];
  topic_error?: string;
  service_error?: string;
  action_error?: string;
  error?: string;
  counts?: {
    topics: number;
    services: number;
    actions: number;
  };
}

/** Cached catalog (survives offline). Pass refresh=true for live rosapi scan. */
export function getRobotInterfaces(
  id: string,
  options?: { refresh?: boolean; signal?: AbortSignal }
): Promise<RobotInterfaceCatalog> {
  const query = options?.refresh ? "?refresh=1" : "";
  return requestJson(
    `/api/v1/robots/${id}/interfaces${query}`,
    undefined,
    options?.signal
  );
}

export function scanRobotInterfaces(
  id: string,
  signal?: AbortSignal
): Promise<RobotInterfaceCatalog> {
  return requestJson(
    `/api/v1/robots/${id}/interfaces/scan`,
    { method: "POST", headers: { "Content-Type": "application/json" } },
    signal
  );
}

export function resolveRobotInterfaceType(
  id: string,
  kind: string,
  name: string,
  signal?: AbortSignal
): Promise<{ message_type: string }> {
  const params = new URLSearchParams({ kind, name });
  return requestJson(
    `/api/v1/robots/${id}/interfaces/type?${params.toString()}`,
    undefined,
    signal
  );
}

export interface RobotInterfaceSchema {
  entity_id: string;
  operation_kind: string;
  name: string;
  endpoint_name?: string;
  message_type: string;
  type_source?: string;
  root_type?: string;
  typedef_count?: number;
  empty_request?: boolean;
  parameter_schema: Record<string, unknown>;
  request_defaults: Record<string, unknown>;
  transport: string;
  error?: string;
}

/** Resolve ROS type + request fields via rosapi typedefs → JSON Schema. */
export function resolveRobotInterfaceSchema(
  id: string,
  kind: string,
  name: string,
  options?: { type?: string; signal?: AbortSignal }
): Promise<RobotInterfaceSchema> {
  const params = new URLSearchParams({ kind, name });
  if (options?.type) {
    params.set("type", options.type);
  }
  return requestJson(
    `/api/v1/robots/${id}/interfaces/schema?${params.toString()}`,
    undefined,
    options?.signal
  );
}

/** Reserved stub for device MQTT/HTTP discovery (currently 501). */
export function scanDeviceInterfaces(
  id: string,
  signal?: AbortSignal
): Promise<RobotInterfaceCatalog> {
  return requestJson(
    `/api/v1/devices/${id}/interfaces/scan`,
    { method: "POST", headers: { "Content-Type": "application/json" } },
    signal
  );
}
