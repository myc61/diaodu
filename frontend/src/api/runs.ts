export interface WorkflowRunSummary {
  id: string;
  workflow_version_id: string;
  workflow_definition_id: string;
  workflow_name: string;
  workflow_version: number;
  trigger_type: string;
  trigger_metadata: Record<string, unknown>;
  state: string;
  input_data: Record<string, unknown>;
  context_data: Record<string, unknown>;
  parent_run_id: string;
  root_run_id: string;
  source_run_id: string;
  parent_node_run_id: string;
  causation_event_id: string;
  business_key: string;
  started_at: string;
  finished_at: string;
  archived_at: string;
  created_at: string;
}

export interface NodeRun {
  id: string;
  workflow_run_id: string;
  node_key: string;
  attempt: number;
  state: string;
  assigned_robot_id: string;
  input_data: Record<string, unknown>;
  output_data: Record<string, unknown>;
  error_data: Record<string, unknown> | null;
  started_at: string;
  finished_at: string;
}

export interface CommandRun {
  id: string;
  command_id: string;
  workflow_run_id: string;
  node_run_id: string;
  robot_id: string;
  capability_definition_id: string;
  operation_kind: string;
  endpoint_name: string;
  correlation_id: string;
  state: string;
  request_payload: Record<string, unknown>;
  last_feedback: Record<string, unknown> | null;
  result_payload: Record<string, unknown> | null;
  error_data: Record<string, unknown> | null;
  dispatched_at: string;
  completed_at: string;
  created_at: string;
}

export interface WorkflowEvent {
  id: string;
  workflow_run_id: string;
  node_run_id: string;
  event_type: string;
  payload: Record<string, unknown>;
  occurred_at: string;
}

export interface WorkflowRunDetail extends WorkflowRunSummary {
  graph: {
    nodes?: Array<Record<string, unknown>>;
    edges?: Array<Record<string, unknown>>;
  };
  nodes: NodeRun[];
  commands: CommandRun[];
  events: WorkflowEvent[];
}

async function parseError(response: Response): Promise<string> {
  try {
    const body = (await response.json()) as { error?: string };
    return body.error || `HTTP ${response.status}`;
  } catch {
    return `HTTP ${response.status}`;
  }
}

function engineerSessionId(): string {
  const storageKey = "dispatcher.engineer_session_id";
  let value = window.sessionStorage.getItem(storageKey);
  if (!value) {
    value = window.crypto?.randomUUID?.() ?? `session-${Date.now()}`;
    window.sessionStorage.setItem(storageKey, value);
  }
  return value;
}

export async function listWorkflowRuns(
  archived: "exclude" | "only" | "include" = "exclude"
): Promise<{ items: WorkflowRunSummary[] }> {
  const response = await fetch(
    `/api/v1/workflow-runs?archived=${encodeURIComponent(archived)}`
  );
  if (!response.ok) {
    throw new Error(await parseError(response));
  }
  return (await response.json()) as { items: WorkflowRunSummary[] };
}

export async function archiveWorkflowRun(id: string): Promise<void> {
  const response = await fetch(`/api/v1/workflow-runs/${id}/archive`, {
    method: "POST"
  });
  if (!response.ok) {
    throw new Error(await parseError(response));
  }
}

export async function cleanupWorkflowRunHistory(
  olderThanDays: number
): Promise<{ deleted: number; older_than_days: number }> {
  const response = await fetch("/api/v1/workflow-runs/history/cleanup", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      older_than_days: olderThanDays,
      confirm: true
    })
  });
  if (!response.ok) {
    throw new Error(await parseError(response));
  }
  return (await response.json()) as {
    deleted: number;
    older_than_days: number;
  };
}

export async function getWorkflowRun(id: string): Promise<WorkflowRunDetail> {
  const response = await fetch(`/api/v1/workflow-runs/${id}`);
  if (!response.ok) {
    throw new Error(await parseError(response));
  }
  return (await response.json()) as WorkflowRunDetail;
}

export async function startWorkflowRun(
  workflowId: string,
  inputData: Record<string, unknown> = {}
): Promise<WorkflowRunDetail> {
  const response = await fetch("/api/v1/workflow-runs", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ workflow_id: workflowId, input_data: inputData })
  });
  if (!response.ok) {
    throw new Error(await parseError(response));
  }
  return (await response.json()) as WorkflowRunDetail;
}

export async function cancelWorkflowRun(id: string): Promise<WorkflowRunDetail> {
  const response = await fetch(`/api/v1/workflow-runs/${id}/cancel`, {
    method: "POST"
  });
  if (!response.ok) {
    throw new Error(await parseError(response));
  }
  return (await response.json()) as WorkflowRunDetail;
}

export async function decideManualConfirmation(
  runId: string,
  nodeRunId: string,
  decision: "APPROVE" | "REJECT",
  note = ""
): Promise<WorkflowRunDetail> {
  const response = await fetch(
    `/api/v1/workflow-runs/${runId}/nodes/${nodeRunId}/manual-confirm`,
    {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "X-Engineer-Session": engineerSessionId()
      },
      body: JSON.stringify({ decision, note })
    }
  );
  if (!response.ok) {
    throw new Error(await parseError(response));
  }
  return (await response.json()) as WorkflowRunDetail;
}

export async function injectWorkflowEvent(
  eventName: string,
  payload: Record<string, unknown> = {},
  routing: {
    target_run_id?: string;
    target_workflow_definition_id?: string;
    business_key?: string;
    deduplication_key?: string;
  } = {}
): Promise<{
  signal_id: string;
  event_name: string;
  woken_runs: string[];
  started_runs: string[];
}> {
  const response = await fetch("/api/v1/workflow-events", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ event_name: eventName, payload, ...routing })
  });
  if (!response.ok) {
    throw new Error(await parseError(response));
  }
  return (await response.json()) as {
    signal_id: string;
    event_name: string;
    woken_runs: string[];
    started_runs: string[];
  };
}
