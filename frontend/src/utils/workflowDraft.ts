const STORAGE_KEY = "diaodu.workflow.editor.draft";

export type WorkflowDraftNode = {
  id: string;
  type?: string;
  position: { x: number; y: number };
  label?: string;
  data?: Record<string, unknown>;
};

export type WorkflowDraftEdge = {
  id: string;
  source: string;
  target: string;
  sourceHandle?: string | null;
  targetHandle?: string | null;
  label?: string;
  animated?: boolean;
  data?: Record<string, unknown>;
  markerEnd?: { type: string; color?: string };
  style?: Record<string, string>;
};

export type WorkflowEditorDraft = {
  v: 1;
  selectedSceneId: string;
  selectedWorkflowId: string;
  workflowName: string;
  workflowDescription: string;
  triggerType: string;
  triggerEventName: string;
  loopCount: number;
  loopDelayMs: number;
  navRobotId: string;
  capabilityRobotId: string;
  connectEdgeKind: "success" | "event" | "failure";
  connectEventName: string;
  lastSavedFingerprint: string;
  nodes: WorkflowDraftNode[];
  edges: WorkflowDraftEdge[];
};

export function loadWorkflowDraft(): WorkflowEditorDraft | null {
  try {
    const raw = window.sessionStorage.getItem(STORAGE_KEY);
    if (!raw) {
      return null;
    }
    const parsed = JSON.parse(raw) as WorkflowEditorDraft;
    if (parsed?.v !== 1 || !Array.isArray(parsed.nodes) || !Array.isArray(parsed.edges)) {
      return null;
    }
    return parsed;
  } catch {
    return null;
  }
}

export function saveWorkflowDraft(draft: WorkflowEditorDraft): void {
  try {
    window.sessionStorage.setItem(
      STORAGE_KEY,
      JSON.stringify({ ...draft, v: 1 })
    );
  } catch {
    // Ignore quota / private-mode failures; in-memory keep-alive still works.
  }
}

export function clearWorkflowDraft(): void {
  try {
    window.sessionStorage.removeItem(STORAGE_KEY);
  } catch {
    // ignore
  }
}
