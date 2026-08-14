import type { CapabilityTemplate } from "../types/workspace";

export interface CapabilityProfile {
  id: string;
  name: string;
  description: string;
}

export interface CapabilityUpsertPayload {
  profile_id: string;
  capability_key: string;
  operation_kind: string;
  endpoint_name: string;
  ros_message_type: string;
  motion_ownership: string;
  blocking_type: string;
  timeout_ms: number;
  parameter_schema: Record<string, unknown>;
  request_template: Record<string, unknown>;
  feedback_mapping: Record<string, unknown>;
  result_mapping: Record<string, unknown>;
  success_condition: Record<string, unknown> | null;
  failure_condition: Record<string, unknown> | null;
  retry_policy: Record<string, unknown>;
  cancel_policy: Record<string, unknown>;
  resource_claims: unknown[];
  protocol_config: Record<string, unknown>;
  event_specs: unknown[];
}

export interface CapabilityTestResult {
  capability_id: string;
  capability_key: string;
  robot_id: string;
  operation_kind?: string;
  endpoint_name?: string;
  request_payload: Record<string, unknown>;
  correlation_id?: string;
  success: boolean;
  result?: Record<string, unknown>;
  error?: string;
  elapsed_ms?: number;
  workflow_advanced: boolean;
}

async function requestJson<T>(
  path: string,
  init?: RequestInit,
  signal?: AbortSignal
): Promise<T> {
  const response = await fetch(path, {
    ...init,
    headers: {
      Accept: "application/json",
      "Content-Type": "application/json",
      ...(init?.headers ?? {})
    },
    signal
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
}

export function listCapabilityTemplates(
  signal?: AbortSignal
): Promise<{ items: CapabilityTemplate[] }> {
  return requestJson("/api/v1/capability-templates", undefined, signal);
}

export function listCapabilityProfiles(
  signal?: AbortSignal
): Promise<{ items: CapabilityProfile[] }> {
  return requestJson("/api/v1/capability-profiles", undefined, signal);
}

export function createCapabilityTemplate(
  payload: CapabilityUpsertPayload
): Promise<CapabilityTemplate> {
  return requestJson("/api/v1/capability-templates", {
    method: "POST",
    body: JSON.stringify(payload)
  });
}

export function updateCapabilityTemplate(
  id: string,
  payload: CapabilityUpsertPayload
): Promise<CapabilityTemplate> {
  return requestJson(`/api/v1/capability-templates/${id}`, {
    method: "PUT",
    body: JSON.stringify(payload)
  });
}

export function deleteCapabilityTemplate(
  id: string
): Promise<{ deleted: boolean; id: string }> {
  return requestJson(`/api/v1/capability-templates/${id}`, {
    method: "DELETE"
  });
}

export function testCapabilityTemplate(
  id: string,
  payload: { robot_id: string; parameters: Record<string, unknown> }
): Promise<CapabilityTestResult> {
  return requestJson(`/api/v1/capability-templates/${id}/test`, {
    method: "POST",
    body: JSON.stringify(payload)
  });
}
