import type {
  OrchestrationAssets,
  WorkflowDetail,
  WorkflowGraph,
  WorkflowSummary,
  WorkflowTriggerType
} from "../types/workflow";

async function parseError(response: Response): Promise<string> {
  try {
    const body = (await response.json()) as { error?: string };
    return body.error || `HTTP ${response.status}`;
  } catch {
    return `HTTP ${response.status}`;
  }
}

export async function listWorkflows(
  sceneId?: string
): Promise<{ items: WorkflowSummary[] }> {
  const query = sceneId ? `?scene_id=${encodeURIComponent(sceneId)}` : "";
  const response = await fetch(`/api/v1/workflows${query}`);
  if (!response.ok) {
    throw new Error(await parseError(response));
  }
  return (await response.json()) as { items: WorkflowSummary[] };
}

export async function getWorkflow(id: string): Promise<WorkflowDetail> {
  const response = await fetch(`/api/v1/workflows/${id}`);
  if (!response.ok) {
    throw new Error(await parseError(response));
  }
  return (await response.json()) as WorkflowDetail;
}

export async function createWorkflow(payload: {
  name: string;
  description?: string;
  scene_id?: string | null;
  trigger_type?: WorkflowTriggerType;
  trigger_config?: Record<string, unknown>;
  graph?: WorkflowGraph;
}): Promise<WorkflowDetail> {
  const response = await fetch("/api/v1/workflows", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload)
  });
  if (!response.ok) {
    throw new Error(await parseError(response));
  }
  return (await response.json()) as WorkflowDetail;
}

export async function updateWorkflow(
  id: string,
  payload: {
    name: string;
    description?: string;
    scene_id?: string | null;
    trigger_type?: WorkflowTriggerType;
    trigger_config?: Record<string, unknown>;
    graph: WorkflowGraph;
    validate?: boolean;
  }
): Promise<WorkflowDetail> {
  const response = await fetch(`/api/v1/workflows/${id}`, {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload)
  });
  if (!response.ok) {
    throw new Error(await parseError(response));
  }
  return (await response.json()) as WorkflowDetail;
}

export async function publishWorkflow(id: string): Promise<WorkflowDetail> {
  const response = await fetch(`/api/v1/workflows/${id}/publish`, {
    method: "POST"
  });
  if (!response.ok) {
    throw new Error(await parseError(response));
  }
  return (await response.json()) as WorkflowDetail;
}

export async function deleteWorkflow(id: string): Promise<void> {
  const response = await fetch(`/api/v1/workflows/${id}`, { method: "DELETE" });
  if (!response.ok) {
    throw new Error(await parseError(response));
  }
}

export async function getOrchestrationAssets(
  sceneId: string
): Promise<OrchestrationAssets> {
  const response = await fetch(
    `/api/v1/scenes/${sceneId}/orchestration-assets`
  );
  if (!response.ok) {
    throw new Error(await parseError(response));
  }
  return (await response.json()) as OrchestrationAssets;
}
