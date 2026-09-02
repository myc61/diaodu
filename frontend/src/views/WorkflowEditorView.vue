<script setup lang="ts">
import { Background } from "@vue-flow/background";
import { Controls } from "@vue-flow/controls";
import {
  MarkerType,
  VueFlow,
  addEdge,
  type Connection,
  type NodeTypesObject
} from "@vue-flow/core";
import {
  Play,
  Plus,
  Redo2,
  Save,
  Trash2,
  Undo2,
  Upload
} from "lucide-vue-next";
import { computed, onBeforeUnmount, onMounted, ref, watch } from "vue";
import { useRouter } from "vue-router";

import { startWorkflowRun } from "../api/runs";
import { listScenes } from "../api/workspace";
import SchemaForm from "../components/SchemaForm.vue";
import WorkflowGraphNode from "../components/WorkflowGraphNode.vue";
import {
  createWorkflow,
  deleteWorkflow,
  getOrchestrationAssets,
  getWorkflow,
  listWorkflows,
  publishWorkflow,
  updateWorkflow
} from "../api/workflows";
import type { EventSpec, Scene } from "../types/workspace";
import type {
  OrchestrationAssets,
  WorkflowSummary,
  WorkflowTriggerType
} from "../types/workflow";

type FlowNode = {
  id: string;
  type?: string;
  position: { x: number; y: number };
  label?: string;
  data?: Record<string, unknown>;
  selected?: boolean;
};

type FlowEdge = {
  id: string;
  source: string;
  target: string;
  sourceHandle?: string | null;
  targetHandle?: string | null;
  label?: string;
  animated?: boolean;
  data?: Record<string, unknown>;
  selected?: boolean;
  markerEnd?: { type: MarkerType; color?: string };
  style?: Record<string, string>;
};

const nodeTypes: NodeTypesObject = {
  dispatch: WorkflowGraphNode
};

type GraphSnapshot = {
  nodes: FlowNode[];
  edges: FlowEdge[];
};

import "@vue-flow/core/dist/style.css";
import "@vue-flow/core/dist/theme-default.css";
import "@vue-flow/controls/dist/style.css";

const scenes = ref<Scene[]>([]);
const workflows = ref<WorkflowSummary[]>([]);
const selectedSceneId = ref("");
const selectedWorkflowId = ref("");
const workflowName = ref("");
const workflowDescription = ref("");
const triggerType = ref<WorkflowTriggerType>("MANUAL");
const triggerEventName = ref("");
const loopCount = ref(1);
const loopDelayMs = ref(0);
const assets = ref<OrchestrationAssets | null>(null);
const nodes = ref<FlowNode[]>([]);
const edges = ref<FlowEdge[]>([]);
const selectedNodeId = ref("");
const selectedEdgeId = ref("");
const undoStack = ref<GraphSnapshot[]>([]);
const redoStack = ref<GraphSnapshot[]>([]);
let dragStartSnapshot: GraphSnapshot | null = null;
const statusMessage = ref("");
const errorMessage = ref("");
const loading = ref(false);
const connectEdgeKind = ref<"success" | "event" | "failure">("success");
const connectEventName = ref("");
/** Default robot used when adding NAVIGATION nodes from the palette. */
const navRobotId = ref("");
/** Robot assigned when adding a point-independent capability node. */
const capabilityRobotId = ref("");
const router = useRouter();

const selectedNode = computed(
  () => nodes.value.find((node) => node.id === selectedNodeId.value) ?? null
);
const selectedEdge = computed(
  () => edges.value.find((edge) => edge.id === selectedEdgeId.value) ?? null
);
const canUndo = computed(() => undoStack.value.length > 0);
const canRedo = computed(() => redoStack.value.length > 0);
const directCapabilities = computed(() =>
  (assets.value?.capabilities ?? []).filter(
    (item) =>
      item.capability_key !== "navigation" &&
      ["TOPIC", "SERVICE", "ACTION", "SSH"].includes(item.operation_kind)
  )
);
const startupProfiles = computed(() => assets.value?.startup_profiles ?? []);
const selectedStartupProfiles = computed(() => {
  const robotId = String(selectedNode.value?.data?.robot_id ?? "");
  return startupProfiles.value.filter((item) => item.robot_id === robotId);
});
const publishedSubflows = computed(() =>
  workflows.value
    .filter(
      (item) =>
        item.published_version && item.id !== selectedWorkflowId.value
    )
    .map((item) => ({
      id: item.id,
      name: item.name,
      version: item.published_version ?? 0
    }))
);
const selectedDirectCapability = computed(() => {
  if (selectedNode.value?.data?.nodeType !== "ROBOT_CAPABILITY") {
    return null;
  }
  const capabilityId = String(
    selectedNode.value.data?.capability_definition_id ?? ""
  );
  return (
    assets.value?.capabilities.find((item) => item.id === capabilityId) ?? null
  );
});
const selectedDirectParameters = computed(() => {
  const value = selectedNode.value?.data?.parameters;
  return value && typeof value === "object" && !Array.isArray(value)
    ? (value as Record<string, unknown>)
    : {};
});
const selectedNodeUsesCapabilityEvents = computed(() =>
  ["ROBOT_CAPABILITY", "STATION_ACTION"].includes(
    String(selectedNode.value?.data?.nodeType ?? "")
  )
);

function stationActionForNode(node: FlowNode) {
  const actionId = String(node.data?.station_action_id ?? "");
  return assets.value?.points
    .flatMap((point) => point.actions)
    .find((action) => action.id === actionId);
}

function resolvedEventSpecsForNode(node: FlowNode): EventSpec[] {
  const nodeType = String(node.data?.nodeType ?? "");
  if (nodeType === "ROBOT_CAPABILITY") {
    const capabilityId = String(node.data?.capability_definition_id ?? "");
    const capability = assets.value?.capabilities.find(
      (item) => item.id === capabilityId
    );
    return capability?.event_specs ?? [];
  }
  if (nodeType !== "STATION_ACTION") {
    return [];
  }
  const action = stationActionForNode(node);
  if (!action) {
    return [];
  }
  const capability = assets.value?.capabilities.find(
    (item) => item.id === action.capability_definition_id
  );
  const specs = cloneGraph(
    action.event_specs?.length
      ? action.event_specs
      : capability?.event_specs ?? []
  );
  if (
    action.success_event_name &&
    !specs.some(
      (spec) =>
        spec.event_name === action.success_event_name &&
        spec.source === "RESULT"
    )
  ) {
    specs.push({
      event_name: action.success_event_name,
      source: "RESULT",
      enabled: true,
      when: { op: "ros_success" },
      max_firings: 1,
      emit_on_node: true,
      start_workflows: true
    });
  }
  return specs;
}

const selectedNodeEventSpecs = computed(() =>
  selectedNode.value ? resolvedEventSpecsForNode(selectedNode.value) : []
);
const selectedNodeConnectableEvents = computed(() =>
  selectedNodeEventSpecs.value.filter(
    (spec) =>
      spec.enabled !== false &&
      spec.emit_on_node !== false &&
      Boolean(spec.event_name?.trim())
  )
);

const publishedLabel = computed(() => {
  const item = workflows.value.find((w) => w.id === selectedWorkflowId.value);
  if (!item?.published_version) {
    return "未发布";
  }
  return `已发布 v${item.published_version}`;
});

function defaultStartEnd(): FlowNode[] {
  return [
    {
      id: "start",
      type: "dispatch",
      position: { x: 80, y: 160 },
      data: { label: "START", nodeType: "START" },
      label: "START"
    },
    {
      id: "end",
      type: "dispatch",
      position: { x: 640, y: 160 },
      data: { label: "END", nodeType: "END" },
      label: "END"
    }
  ];
}

function cloneGraph<T>(value: T): T {
  return JSON.parse(JSON.stringify(value)) as T;
}

function captureGraph(): GraphSnapshot {
  return cloneGraph({ nodes: nodes.value, edges: edges.value });
}

function graphEquals(left: GraphSnapshot, right: GraphSnapshot): boolean {
  return JSON.stringify(left) === JSON.stringify(right);
}

function pushUndoSnapshot(snapshot: GraphSnapshot): void {
  if (graphEquals(snapshot, captureGraph())) {
    return;
  }
  undoStack.value = [...undoStack.value.slice(-49), snapshot];
  redoStack.value = [];
}

function recordGraphMutation(): void {
  undoStack.value = [...undoStack.value.slice(-49), captureGraph()];
  redoStack.value = [];
}

function clearGraphSelection(): void {
  selectedNodeId.value = "";
  selectedEdgeId.value = "";
  nodes.value = nodes.value.map((node) => ({ ...node, selected: false }));
  edges.value = edges.value.map((edge) => ({ ...edge, selected: false }));
}

function restoreGraph(snapshot: GraphSnapshot): void {
  nodes.value = cloneGraph(snapshot.nodes);
  edges.value = cloneGraph(snapshot.edges);
  clearGraphSelection();
}

function resetGraphHistory(): void {
  undoStack.value = [];
  redoStack.value = [];
  dragStartSnapshot = null;
  clearGraphSelection();
}

function undoGraph(): void {
  const previous = undoStack.value.at(-1);
  if (!previous) {
    return;
  }
  redoStack.value = [...redoStack.value.slice(-49), captureGraph()];
  undoStack.value = undoStack.value.slice(0, -1);
  restoreGraph(previous);
}

function redoGraph(): void {
  const next = redoStack.value.at(-1);
  if (!next) {
    return;
  }
  undoStack.value = [...undoStack.value.slice(-49), captureGraph()];
  redoStack.value = redoStack.value.slice(0, -1);
  restoreGraph(next);
}

function toApiGraph() {
  return {
    nodes: nodes.value.map((node) => ({
      id: node.id,
      type: String(node.data?.nodeType || node.type || "DEFAULT"),
      position: node.position,
      data: node.data ?? {}
    })),
    edges: edges.value.map((edge) => ({
      id: edge.id,
      source: edge.source,
      target: edge.target,
      edge_kind: edge.data?.edge_kind || "success",
      event_name: edge.data?.event_name || "",
      label: edge.label
    })),
    run_policy: {
      loop_count: Math.max(1, Math.trunc(loopCount.value || 1)),
      loop_delay_ms: Math.max(0, Math.trunc(loopDelayMs.value || 0))
    }
  };
}

function edgePresentation(kind: string, eventName = ""): {
  label: string;
  animated: boolean;
  style: { stroke: string };
  sourceHandle: string;
  targetHandle: string;
  markerEnd: { type: MarkerType; color: string };
} {
  if (kind === "event") {
    return {
      label: `event:${eventName}`,
      animated: true,
      style: { stroke: "#7e57c2" },
      sourceHandle: "success",
      targetHandle: "in",
      markerEnd: { type: MarkerType.ArrowClosed, color: "#7e57c2" }
    };
  }
  if (kind === "failure") {
    return {
      label: "失败",
      animated: false,
      style: { stroke: "#b03a2e" },
      sourceHandle: "failure",
      targetHandle: "in",
      markerEnd: { type: MarkerType.ArrowClosed, color: "#b03a2e" }
    };
  }
  return {
    label: "成功",
    animated: false,
    style: { stroke: "#238636" },
    sourceHandle: "success",
    targetHandle: "in",
    markerEnd: { type: MarkerType.ArrowClosed, color: "#238636" }
  };
}

function fromApiGraph(graph: {
  nodes?: Array<Record<string, unknown>>;
  edges?: Array<Record<string, unknown>>;
  run_policy?: { loop_count?: number; loop_delay_ms?: number };
}): void {
  const rawNodes = graph.nodes ?? [];
  const rawEdges = graph.edges ?? [];
  loopCount.value = Math.max(1, Number(graph.run_policy?.loop_count ?? 1));
  loopDelayMs.value = Math.max(
    0,
    Number(graph.run_policy?.loop_delay_ms ?? 0)
  );
  if (rawNodes.length === 0) {
    nodes.value = defaultStartEnd();
    edges.value = [];
    return;
  }
  nodes.value = rawNodes.map((node) => {
    const type = String(node.type ?? "DEFAULT");
    const data = (node.data as Record<string, unknown>) ?? {};
    const label = String(data.label ?? type);
    return {
      id: String(node.id),
      type: "dispatch",
      position: (node.position as { x: number; y: number }) ?? { x: 0, y: 0 },
      data: { ...data, nodeType: type, label },
      label
    };
  });
  edges.value = rawEdges.map((edge) => {
    const kind = String(edge.edge_kind ?? "success");
    const eventName = String(edge.event_name ?? "");
    const visual = edgePresentation(kind, eventName);
    return {
      id: String(edge.id),
      source: String(edge.source),
      target: String(edge.target),
      sourceHandle: visual.sourceHandle,
      targetHandle: visual.targetHandle,
      label: visual.label,
      animated: visual.animated,
      style: visual.style,
      markerEnd: visual.markerEnd,
      data: { edge_kind: kind, event_name: eventName }
    };
  });
}

async function refreshScenes(): Promise<void> {
  const result = await listScenes();
  scenes.value = result.items;
  if (!selectedSceneId.value && scenes.value.length > 0) {
    selectedSceneId.value = scenes.value[0].id;
  }
}

async function refreshWorkflows(): Promise<void> {
  const result = await listWorkflows(
    selectedSceneId.value || undefined
  );
  workflows.value = result.items;
}

async function refreshAssets(): Promise<void> {
  if (!selectedSceneId.value) {
    assets.value = null;
    navRobotId.value = "";
    capabilityRobotId.value = "";
    return;
  }
  assets.value = await getOrchestrationAssets(selectedSceneId.value);
  refreshStationActionLabels();
  if (
    !navRobotId.value ||
    !assets.value.robots.some((item) => item.id === navRobotId.value)
  ) {
    navRobotId.value = assets.value.robots[0]?.id ?? "";
  }
  if (
    !capabilityRobotId.value ||
    !assets.value.robots.some((item) => item.id === capabilityRobotId.value)
  ) {
    capabilityRobotId.value = assets.value.robots[0]?.id ?? "";
  }
}

function refreshStationActionLabels(): void {
  const currentAssets = assets.value;
  if (!currentAssets) {
    return;
  }
  const actions = new Map(
    currentAssets.points.flatMap((point) =>
      point.actions.map((action) => [action.id, { point, action }] as const)
    )
  );
  nodes.value = nodes.value.map((node) => {
    if (node.data?.nodeType !== "STATION_ACTION") {
      return node;
    }
    const match = actions.get(String(node.data.station_action_id ?? ""));
    if (!match) {
      return node;
    }
    const actionName = match.action.action_name || match.action.capability_key;
    const robotName =
      match.action.robot_name || robotNameById(match.action.robot_id ?? "");
    const label = `动作·${actionName} · ${robotName}`;
    return {
      ...node,
      label,
      data: {
        ...node.data,
        action_name: actionName,
        capability_key: match.action.capability_key,
        robot_id: match.action.robot_id,
        robot_name: robotName,
        label
      }
    };
  });
}

async function bootstrap(): Promise<void> {
  loading.value = true;
  errorMessage.value = "";
  try {
    await refreshScenes();
    await Promise.all([refreshWorkflows(), refreshAssets()]);
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "加载流程编排失败";
  } finally {
    loading.value = false;
  }
}

async function handleCreate(): Promise<void> {
  if (!workflowName.value.trim()) {
    errorMessage.value = "请输入流程名称";
    return;
  }
  try {
    loading.value = true;
    const created = await createWorkflow({
      name: workflowName.value.trim(),
      description: workflowDescription.value,
      scene_id: selectedSceneId.value || null,
      trigger_type: triggerType.value,
      trigger_config: {
        event_name: triggerEventName.value
      },
      graph: {
        nodes: defaultStartEnd().map((node) => ({
          id: node.id,
          type: node.data?.nodeType,
          position: node.position,
          data: node.data
        })),
        edges: [],
        run_policy: {
          loop_count: loopCount.value,
          loop_delay_ms: loopDelayMs.value
        }
      }
    });
    selectedWorkflowId.value = created.id;
    fromApiGraph(created.graph);
    resetGraphHistory();
    await refreshWorkflows();
    statusMessage.value = `已创建流程 ${created.name}`;
    errorMessage.value = "";
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "创建流程失败";
  } finally {
    loading.value = false;
  }
}

async function loadSelectedWorkflow(): Promise<void> {
  if (!selectedWorkflowId.value) {
    return;
  }
  try {
    loading.value = true;
    const detail = await getWorkflow(selectedWorkflowId.value);
    workflowName.value = detail.name;
    workflowDescription.value = detail.description;
    selectedSceneId.value = detail.scene_id ?? selectedSceneId.value;
    triggerType.value = detail.trigger_type;
    triggerEventName.value = String(
      detail.trigger_config?.event_name ?? ""
    );
    fromApiGraph(detail.graph);
    await refreshAssets();
    resetGraphHistory();
    statusMessage.value = `已加载 ${detail.name}`;
    errorMessage.value = "";
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "加载流程失败";
  } finally {
    loading.value = false;
  }
}

async function handleSave(): Promise<void> {
  if (!selectedWorkflowId.value) {
    errorMessage.value = "请先创建或选择流程";
    return;
  }
  try {
    loading.value = true;
    await updateWorkflow(selectedWorkflowId.value, {
      name: workflowName.value.trim() || "untitled",
      description: workflowDescription.value,
      scene_id: selectedSceneId.value || null,
      trigger_type: triggerType.value,
      trigger_config: { event_name: triggerEventName.value },
      graph: toApiGraph()
    });
    await refreshWorkflows();
    statusMessage.value = "草稿已保存";
    errorMessage.value = "";
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "保存失败";
  } finally {
    loading.value = false;
  }
}

async function handlePublish(): Promise<void> {
  if (!selectedWorkflowId.value) {
    return;
  }
  try {
    loading.value = true;
    await updateWorkflow(selectedWorkflowId.value, {
      name: workflowName.value.trim() || "untitled",
      description: workflowDescription.value,
      scene_id: selectedSceneId.value || null,
      trigger_type: triggerType.value,
      trigger_config: { event_name: triggerEventName.value },
      graph: toApiGraph(),
      validate: true
    });
    await publishWorkflow(selectedWorkflowId.value);
    await refreshWorkflows();
    statusMessage.value = "流程已发布（版本不可变）；已生成新草稿供继续编辑";
    errorMessage.value = "";
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "发布失败";
  } finally {
    loading.value = false;
  }
}

async function handleStartRun(): Promise<void> {
  if (!selectedWorkflowId.value) {
    errorMessage.value = "请先选择流程";
    return;
  }
  try {
    loading.value = true;
    await handlePublish();
    const run = await startWorkflowRun(selectedWorkflowId.value);
    statusMessage.value = `已启动运行 ${run.id.slice(0, 8)}（${run.state}）`;
    errorMessage.value = "";
    await router.push("/runs");
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "启动运行失败";
  } finally {
    loading.value = false;
  }
}

async function handleDelete(): Promise<void> {
  if (!selectedWorkflowId.value) {
    return;
  }
  if (!window.confirm("确认删除该流程？")) {
    return;
  }
  try {
    await deleteWorkflow(selectedWorkflowId.value);
    selectedWorkflowId.value = "";
    nodes.value = defaultStartEnd();
    edges.value = [];
    resetGraphHistory();
    await refreshWorkflows();
    statusMessage.value = "流程已删除";
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "删除失败";
  }
}

function uid(prefix: string): string {
  return `${prefix}_${Math.random().toString(36).slice(2, 9)}`;
}

function addNode(
  nodeType: string,
  label: string,
  data: Record<string, unknown> = {}
): void {
  recordGraphMutation();
  const id = uid(nodeType.toLowerCase());
  const count = nodes.value.length;
  nodes.value = [
    ...nodes.value,
    {
      id,
      type: "dispatch",
      position: {
        x: 220 + (count % 4) * 48,
        y: 80 + count * 40
      },
      data: { ...data, nodeType, label },
      label
    }
  ];
  selectNode(id);
}

function addRobotNode(robotId: string, name: string): void {
  addNode("ROBOT", `机器人·${name}`, { robot_id: robotId, label: name });
}

function addStationNode(pointId: string, name: string): void {
  addNode("STATION", `点位·${name}`, { station_id: pointId, label: name });
}

function addStationActionNode(
  pointName: string,
  actionId: string,
  stationId: string,
  actionName: string,
  capabilityKey: string,
  robotId: string | null,
  associatedRobotName: string,
  eventName: string
): void {
  const displayName = actionName || capabilityKey;
  const robotName = associatedRobotName || robotNameById(robotId ?? "");
  addNode("STATION_ACTION", `动作·${displayName} · ${robotName}`, {
    station_action_id: actionId,
    station_id: stationId,
    station_name: pointName,
    action_name: displayName,
    capability_key: capabilityKey,
    robot_id: robotId,
    robot_name: robotName,
    success_event_name: eventName,
    label: `${displayName} · ${robotName}`
  });
}

function defaultsFromSchema(schema: Record<string, unknown>): Record<string, unknown> {
  const properties =
    schema.properties &&
    typeof schema.properties === "object" &&
    !Array.isArray(schema.properties)
      ? (schema.properties as Record<string, Record<string, unknown>>)
      : {};
  const defaults: Record<string, unknown> = {};
  for (const [key, field] of Object.entries(properties)) {
    if (Object.prototype.hasOwnProperty.call(field, "default")) {
      defaults[key] = cloneGraph(field.default);
      continue;
    }
    if (field.type === "object") {
      defaults[key] = defaultsFromSchema(field);
    } else if (field.type === "array") {
      defaults[key] = [];
    } else if (field.type === "boolean") {
      defaults[key] = false;
    } else if (field.type === "number" || field.type === "integer") {
      defaults[key] = 0;
    } else {
      defaults[key] = "";
    }
  }
  return defaults;
}

function addRobotCapabilityNode(
  capability: OrchestrationAssets["capabilities"][number]
): void {
  if (!capabilityRobotId.value) {
    errorMessage.value = "请先选择直接能力的执行机器人";
    return;
  }
  const robotName = robotNameById(capabilityRobotId.value);
  const label = `能力·${capability.capability_key} · ${robotName}`;
  addNode("ROBOT_CAPABILITY", label, {
    robot_id: capabilityRobotId.value,
    robot_name: robotName,
    capability_definition_id: capability.id,
    capability_key: capability.capability_key,
    action_name: capability.capability_key,
    operation_kind: capability.operation_kind,
    endpoint_name: capability.endpoint_name,
    parameters: defaultsFromSchema(capability.parameter_schema ?? {}),
    timeout_ms: capability.timeout_ms,
    retry_count: 0,
    retry_delay_ms: 1000,
    label
  });
  errorMessage.value = "";
}

function robotNameById(robotId: string): string {
  if (!robotId) {
    return "未指定机器人";
  }
  const robot = assets.value?.robots.find((item) => item.id === robotId);
  return robot?.name ?? robotId.slice(0, 8);
}

function addNavigationNode(toStationId: string, toName: string): void {
  if (!navRobotId.value) {
    errorMessage.value = "请先在左侧选择「导航机器人」";
    return;
  }
  const robotName = robotNameById(navRobotId.value);
  addNode("NAVIGATION", `${robotName}→${toName}`, {
    robot_id: navRobotId.value,
    to_station_id: toStationId,
    distance_tolerance: 0.04,
    heading_tolerance: 0.04,
    label: `${robotName}→${toName}`
  });
  errorMessage.value = "";
}

function updateSelectedNavigationNumber(
  key: "distance_tolerance" | "heading_tolerance",
  value: number
): void {
  const node = selectedNode.value;
  if (!node || node.data?.nodeType !== "NAVIGATION") {
    return;
  }
  if (Number(node.data?.[key]) === value) {
    return;
  }
  recordGraphMutation();
  node.data = { ...(node.data ?? {}), [key]: value };
  nodes.value = [...nodes.value];
}

function updateSelectedNavigationRobot(robotId: string): void {
  const node = selectedNode.value;
  if (!node || node.data?.nodeType !== "NAVIGATION") {
    return;
  }
  if (String(node.data?.robot_id ?? "") === robotId) {
    return;
  }
  recordGraphMutation();
  const toId = String(node.data?.to_station_id ?? "");
  const point = assets.value?.points.find((item) => item.id === toId);
  const toName = point?.name ?? String(node.data?.label ?? "点");
  const robotName = robotNameById(robotId);
  const label = `${robotName}→${toName}`;
  node.data = {
    ...(node.data ?? {}),
    robot_id: robotId,
    label
  };
  node.label = label;
  nodes.value = [...nodes.value];
}

function updateSelectedCapabilityRobot(robotId: string): void {
  const node = selectedNode.value;
  if (!node || node.data?.nodeType !== "ROBOT_CAPABILITY") {
    return;
  }
  recordGraphMutation();
  const capability = selectedDirectCapability.value;
  const robotName = robotNameById(robotId);
  const capabilityName = capability?.capability_key ?? "未选能力";
  const label = `能力·${capabilityName} · ${robotName}`;
  node.data = { ...(node.data ?? {}), robot_id: robotId, robot_name: robotName, label };
  node.label = label;
  nodes.value = [...nodes.value];
}

function updateSelectedCapabilityDefinition(capabilityId: string): void {
  const node = selectedNode.value;
  if (!node || node.data?.nodeType !== "ROBOT_CAPABILITY") {
    return;
  }
  const capability = directCapabilities.value.find(
    (item) => item.id === capabilityId
  );
  if (!capability) {
    return;
  }
  recordGraphMutation();
  const robotId = String(node.data?.robot_id ?? "");
  const robotName = robotNameById(robotId);
  const label = `能力·${capability.capability_key} · ${robotName}`;
  node.data = {
    ...(node.data ?? {}),
    capability_definition_id: capability.id,
    capability_key: capability.capability_key,
    action_name: capability.capability_key,
    operation_kind: capability.operation_kind,
    endpoint_name: capability.endpoint_name,
    parameters: defaultsFromSchema(capability.parameter_schema ?? {}),
    timeout_ms: capability.timeout_ms,
    retry_count: 0,
    retry_delay_ms: 1000,
    label
  };
  node.label = label;
  nodes.value = [...nodes.value];
}

function updateSelectedCapabilityParameters(value: Record<string, unknown>): void {
  const node = selectedNode.value;
  if (!node || node.data?.nodeType !== "ROBOT_CAPABILITY") {
    return;
  }
  recordGraphMutation();
  node.data = { ...(node.data ?? {}), parameters: value };
  nodes.value = [...nodes.value];
}

function updateSelectedCapabilityTimeout(value: number): void {
  const node = selectedNode.value;
  if (
    !node ||
    node.data?.nodeType !== "ROBOT_CAPABILITY" ||
    !Number.isFinite(value) ||
    value <= 0
  ) {
    return;
  }
  recordGraphMutation();
  node.data = { ...(node.data ?? {}), timeout_ms: value };
  nodes.value = [...nodes.value];
}

function updateSelectedCapabilityRetry(
  key: "retry_count" | "retry_delay_ms",
  value: number
): void {
  const node = selectedNode.value;
  if (
    !node ||
    node.data?.nodeType !== "ROBOT_CAPABILITY" ||
    !Number.isFinite(value) ||
    value < 0
  ) {
    return;
  }
  recordGraphMutation();
  node.data = { ...(node.data ?? {}), [key]: Math.trunc(value) };
  nodes.value = [...nodes.value];
}

function eventConditionSummary(spec: EventSpec): string {
  const op = spec.when?.op || "ros_success";
  const field = spec.when?.field?.trim();
  if (op === "ros_success") {
    return "ROS 调用成功";
  }
  const opLabel =
    {
      eq: "等于",
      neq: "不等于",
      gt: "大于",
      gte: "大于等于",
      lt: "小于",
      lte: "小于等于",
      contains: "包含",
      exists: "字段存在",
      truthy: "值为真"
    }[op] ?? op;
  const value = spec.when?.value;
  const valueText =
    value === undefined
      ? ""
      : typeof value === "string"
        ? value
        : JSON.stringify(value);
  return [field, opLabel, valueText].filter(Boolean).join(" ");
}

function useEventForConnection(eventName: string): void {
  connectEdgeKind.value = "event";
  connectEventName.value = eventName;
  statusMessage.value = `已选择事件边 ${eventName}，请从当前动作节点拖线到目标节点`;
  errorMessage.value = "";
}

function addEventWait(eventName: string): void {
  if (!eventName.trim()) {
    errorMessage.value = "请填写事件名";
    return;
  }
  addNode("EVENT_WAIT", `等待·${eventName}`, {
    event_name: eventName.trim(),
    timeout_ms: 60000,
    label: eventName.trim()
  });
}

function addDelayNode(): void {
  addNode("DELAY", "延时·1000 ms", {
    delay_ms: 1000,
    label: "延时·1000 ms"
  });
}

function addManualConfirmNode(): void {
  addNode("MANUAL_CONFIRM", "人工确认·请确认是否继续执行", {
    prompt: "请确认现场条件满足，是否继续执行？",
    label: "人工确认·请确认是否继续执行"
  });
}

function addRobotStartupNode(): void {
  const profile = startupProfiles.value[0];
  const robot = assets.value?.robots.find(
    (item) => item.id === profile?.robot_id
  );
  const label = "启动·" + (robot?.name ?? "机器人");
  addNode("ROBOT_SSH", label, {
    robot_id: profile?.robot_id ?? "",
    startup_profile_id: profile?.id ?? "",
    startup_profile_name: profile?.name ?? "",
    timeout_ms: profile?.timeout_ms ?? 120000,
    label
  });
}

function addSubflowNode(): void {
  const workflow = publishedSubflows.value[0];
  const label = "子流程·" + (workflow?.name ?? "未选择");
  addNode("SUBFLOW", label, {
    workflow_definition_id: workflow?.id ?? "",
    workflow_name: workflow?.name ?? "",
    label
  });
}

function updateSelectedStartup(
  key: "robot_id" | "startup_profile_id",
  value: string
): void {
  const node = selectedNode.value;
  if (node?.data?.nodeType !== "ROBOT_STARTUP" &&
      node?.data?.nodeType !== "ROBOT_SSH") {
    return;
  }
  recordGraphMutation();
  const next = { ...(node.data ?? {}), [key]: value };
  if (key === "robot_id") {
    next.startup_profile_id = "";
    next.startup_profile_name = "";
  } else {
    const profile = startupProfiles.value.find((item) => item.id === value);
    next.startup_profile_name = profile?.name ?? "";
    next.timeout_ms = profile?.timeout_ms ?? 120000;
  }
  const robot = assets.value?.robots.find(
    (item) => item.id === String(next.robot_id ?? "")
  );
  next.label =
    "启动·" +
    (robot?.name ?? "机器人") +
    (next.startup_profile_name ? " · " + next.startup_profile_name : "");
  node.data = next;
  node.label = String(next.label);
  nodes.value = [...nodes.value];
}

function updateSelectedSubflow(value: string): void {
  const node = selectedNode.value;
  if (node?.data?.nodeType !== "SUBFLOW") {
    return;
  }
  recordGraphMutation();
  const workflow = publishedSubflows.value.find((item) => item.id === value);
  const label = "子流程·" + (workflow?.name ?? "未选择");
  node.data = {
    ...(node.data ?? {}),
    workflow_definition_id: value,
    workflow_name: workflow?.name ?? "",
    label
  };
  node.label = label;
  nodes.value = [...nodes.value];
}

function updateSelectedManualConfirmPrompt(value: string): void {
  const node = selectedNode.value;
  if (node?.data?.nodeType !== "MANUAL_CONFIRM") {
    return;
  }
  const prompt = value.trim();
  if (!prompt || prompt.length > 500) {
    errorMessage.value = prompt ? "确认提示不能超过 500 个字符" : "确认提示不能为空";
    return;
  }
  recordGraphMutation();
  const label = `人工确认·${prompt}`;
  node.data = { ...(node.data ?? {}), prompt, label };
  node.label = label;
  nodes.value = [...nodes.value];
  errorMessage.value = "";
}

function updateSelectedDelay(value: number): void {
  const node = selectedNode.value;
  if (
    !node ||
    node.data?.nodeType !== "DELAY" ||
    !Number.isFinite(value) ||
    value < 0
  ) {
    return;
  }
  recordGraphMutation();
  const delayMs = Math.trunc(value);
  const label = `延时·${delayMs} ms`;
  node.data = { ...(node.data ?? {}), delay_ms: delayMs, label };
  node.label = label;
  nodes.value = [...nodes.value];
}

function resolveConnectKind(connection: Connection): "success" | "event" | "failure" {
  if (connection.sourceHandle === "failure") {
    return "failure";
  }
  if (connectEdgeKind.value === "event") {
    return "event";
  }
  return "success";
}

function handleConnect(connection: Connection): void {
  const kind = resolveConnectKind(connection);
  const eventName =
    kind === "event"
      ? connectEventName.value || triggerEventName.value
      : "";
  if (kind === "event" && !eventName) {
    errorMessage.value = "事件边需要事件名";
    return;
  }
  const sourceNode = nodes.value.find((node) => node.id === connection.source);
  if (
    sourceNode?.data?.nodeType === "START" &&
    (kind === "event" || kind === "failure")
  ) {
    errorMessage.value = "START 只能使用成功边启动首个节点";
    return;
  }
  if (kind === "event") {
    if (sourceNode && ["ROBOT_CAPABILITY", "STATION_ACTION"].includes(
      String(sourceNode.data?.nodeType ?? "")
    )) {
      const allowed = resolvedEventSpecsForNode(sourceNode).some(
        (spec) =>
          spec.event_name === eventName &&
          spec.enabled !== false &&
          spec.emit_on_node !== false
      );
      if (!allowed) {
        errorMessage.value = `事件 ${eventName} 不是该动作节点可用于连线的事件`;
        return;
      }
    }
  }
  const visual = edgePresentation(kind, eventName);
  recordGraphMutation();
  edges.value = addEdge(
    {
      ...connection,
      id: uid("e"),
      sourceHandle: visual.sourceHandle,
      targetHandle: visual.targetHandle,
      label: visual.label,
      animated: visual.animated,
      style: visual.style,
      markerEnd: visual.markerEnd,
      data: { edge_kind: kind, event_name: eventName }
    },
    edges.value
  ) as FlowEdge[];
}

function onNodeClick(payload: { node: { id: string } }): void {
  selectNode(payload.node.id);
}

function onEdgeClick(payload: { edge: { id: string } }): void {
  selectEdge(payload.edge.id);
}

function selectNode(id: string): void {
  selectedNodeId.value = id;
  selectedEdgeId.value = "";
  nodes.value = nodes.value.map((node) => ({
    ...node,
    selected: node.id === id
  }));
  edges.value = edges.value.map((edge) => ({ ...edge, selected: false }));
}

function selectEdge(id: string): void {
  selectedNodeId.value = "";
  selectedEdgeId.value = id;
  nodes.value = nodes.value.map((node) => ({ ...node, selected: false }));
  edges.value = edges.value.map((edge) => ({
    ...edge,
    selected: edge.id === id
  }));
}

function removeSelection(): boolean {
  if (selectedEdge.value) {
    recordGraphMutation();
    edges.value = edges.value.filter((edge) => edge.id !== selectedEdge.value?.id);
    clearGraphSelection();
    return true;
  }
  const current = selectedNode.value;
  if (!current) {
    return false;
  }
  if (current.data?.nodeType === "START" || current.data?.nodeType === "END") {
    statusMessage.value = "START 和 END 节点不能删除";
    return false;
  }
  recordGraphMutation();
  nodes.value = nodes.value.filter((node) => node.id !== current.id);
  edges.value = edges.value.filter(
    (edge) => edge.source !== current.id && edge.target !== current.id
  );
  clearGraphSelection();
  return true;
}

function onNodeDragStart(): void {
  dragStartSnapshot = captureGraph();
}

function onNodeDragStop(): void {
  if (!dragStartSnapshot) {
    return;
  }
  pushUndoSnapshot(dragStartSnapshot);
  dragStartSnapshot = null;
}

function isEditingTarget(target: EventTarget | null): boolean {
  return (
    target instanceof HTMLElement &&
    Boolean(target.closest("input, textarea, select, [contenteditable='true']"))
  );
}

function handleWindowKeydown(event: KeyboardEvent): void {
  if (event.defaultPrevented || isEditingTarget(event.target)) {
    return;
  }
  const key = event.key.toLowerCase();
  const primaryModifier = event.ctrlKey || event.metaKey;
  if (primaryModifier && key === "z") {
    event.preventDefault();
    if (event.shiftKey) {
      redoGraph();
    } else {
      undoGraph();
    }
    return;
  }
  if (primaryModifier && key === "y") {
    event.preventDefault();
    redoGraph();
    return;
  }
  if (!primaryModifier && (event.key === "Delete" || event.key === "Backspace")) {
    if (removeSelection()) {
      event.preventDefault();
    }
  }
}

const selectedNodeEntries = computed(() => {
  const data = selectedNode.value?.data;
  if (!data || typeof data !== "object") {
    return [] as Array<[string, unknown]>;
  }
  return Object.entries(data as Record<string, unknown>).filter(
    ([key]) =>
      key !== "nodeType" &&
      key !== "label" &&
      !(selectedNode.value?.data?.nodeType === "ROBOT_CAPABILITY" &&
        key === "parameters")
  );
});

const selectedEdgeEntries = computed(() => {
  const data = selectedEdge.value?.data;
  if (!data || typeof data !== "object") {
    return [] as Array<[string, unknown]>;
  }
  return Object.entries(data as Record<string, unknown>);
});

watch(selectedSceneId, () => {
  void refreshWorkflows();
  void refreshAssets();
});

onMounted(() => {
  nodes.value = defaultStartEnd();
  window.addEventListener("keydown", handleWindowKeydown);
  void bootstrap();
});

onBeforeUnmount(() => {
  window.removeEventListener("keydown", handleWindowKeydown);
});
</script>

<template>
  <section class="workflow-page">
    <header class="workflow-toolbar">
      <label>
        场景
        <select v-model="selectedSceneId">
          <option disabled value="">选择场景</option>
          <option v-for="scene in scenes" :key="scene.id" :value="scene.id">
            {{ scene.name }}
          </option>
        </select>
      </label>
      <label>
        流程
        <select v-model="selectedWorkflowId" @change="loadSelectedWorkflow">
          <option value="">未选择</option>
          <option
            v-for="item in workflows"
            :key="item.id"
            :value="item.id"
          >
            {{ item.name }}
            <template v-if="item.published_version">
              · pub v{{ item.published_version }}
            </template>
          </option>
        </select>
      </label>
      <label>
        名称
        <input v-model="workflowName" placeholder="map_job_1" />
      </label>
      <label>
        触发
        <select v-model="triggerType">
          <option value="MANUAL">人工</option>
          <option value="EVENT">事件</option>
          <option value="MANUAL_OR_EVENT">人工或事件</option>
          <option value="CRON">Cron</option>
        </select>
      </label>
      <label>
        触发事件名
        <input
          v-model="triggerEventName"
          placeholder="point_b_completed"
          :disabled="triggerType === 'MANUAL' || triggerType === 'CRON'"
        />
      </label>
      <label>
        流程循环次数
        <input v-model.number="loopCount" type="number" min="1" max="1000" />
      </label>
      <label>
        循环间隔 (ms)
        <input v-model.number="loopDelayMs" type="number" min="0" max="86400000" />
      </label>
      <button class="primary-button" type="button" @click="handleCreate">
        <Plus :size="15" />
        新建
      </button>
      <button
        class="primary-button"
        type="button"
        :disabled="loading || !selectedWorkflowId"
        @click="handleSave"
      >
        <Save :size="15" />
        保存草稿
      </button>
      <button
        class="ok-button"
        type="button"
        :disabled="loading || !selectedWorkflowId"
        @click="handlePublish"
      >
        <Upload :size="15" />
        发布
      </button>
      <button
        class="primary-button"
        type="button"
        :disabled="loading || !selectedWorkflowId"
        @click="handleStartRun"
      >
        <Play :size="15" />
        发布并运行
      </button>
      <button
        class="danger-button"
        type="button"
        :disabled="!selectedWorkflowId"
        @click="handleDelete"
      >
        <Trash2 :size="15" />
        删除
      </button>
      <button
        class="icon-button"
        type="button"
        title="撤销"
        :disabled="!canUndo"
        @click="undoGraph"
      >
        <Undo2 :size="16" />
      </button>
      <button
        class="icon-button"
        type="button"
        title="重做"
        :disabled="!canRedo"
        @click="redoGraph"
      >
        <Redo2 :size="16" />
      </button>
      <span class="workflow-meta">{{ publishedLabel }}</span>
    </header>

    <p v-if="statusMessage" class="status-ok">{{ statusMessage }}</p>
    <p v-if="errorMessage" class="status-error">{{ errorMessage }}</p>

    <div class="workflow-body">
      <aside class="workflow-palette">
        <h3>场景资产</h3>
        <section class="connect-mode">
          <h4>连线</h4>
          <p class="hint-text">
            服务/动作节点底部<strong>绿点</strong>拉成功边，右侧<strong>红点</strong>拉失败边。
            START 只有成功边。
          </p>
          <label>
            <input v-model="connectEdgeKind" type="radio" value="success" />
            成功边（绿点）
          </label>
          <label>
            <input v-model="connectEdgeKind" type="radio" value="event" />
            事件边（绿点）
          </label>
          <input
            v-if="connectEdgeKind === 'event'"
            v-model="connectEventName"
            placeholder="边事件名"
          />
        </section>
        <p class="hint-text">
          从地图场景加载机器人、点位与点位动作；拖入画布后连线。动作底层为
          ROS service/action 能力模板。
        </p>

        <section>
          <h4>通用控制</h4>
          <button type="button" class="palette-item" @click="addDelayNode">
            延时
            <small>等待指定时间后沿成功边继续</small>
          </button>
          <button type="button" class="palette-item" @click="addManualConfirmNode">
            人工确认
            <small>暂停当前分支，由工程师允许继续或拒绝终止</small>
          </button>
          <button
            type="button"
            class="palette-item"
            :disabled="startupProfiles.length === 0"
            @click="addRobotStartupNode"
          >
            机器人 SSH / 启动
            <small>选择机器人和 SSH 执行方案，运行脚本或结构化命令</small>
          </button>
          <button
            type="button"
            class="palette-item"
            :disabled="publishedSubflows.length === 0"
            @click="addSubflowNode"
          >
            子流程
            <small>启动已发布流程并等待其成功或失败</small>
          </button>
        </section>

        <section>
          <h4>机器人</h4>
          <button
            v-for="robot in assets?.robots ?? []"
            :key="robot.id"
            type="button"
            class="palette-item"
            @click="addRobotNode(robot.id, robot.name)"
          >
            {{ robot.name }}
            <small>{{ robot.connection_state }}</small>
          </button>
          <p v-if="!(assets?.robots.length)" class="hint-text">
            场景未绑定机器人（请先在地图场景页勾选保存）
          </p>
        </section>

        <section>
          <h4>直接能力（无需点位）</h4>
          <label class="inline-add">
            执行机器人
            <select v-model="capabilityRobotId">
              <option value="" disabled>请选择机器人</option>
              <option
                v-for="robot in assets?.robots ?? []"
                :key="robot.id"
                :value="robot.id"
              >
                {{ robot.name }}
              </option>
            </select>
          </label>
          <button
            v-for="capability in directCapabilities"
            :key="capability.id"
            type="button"
            class="palette-item capability"
            :disabled="!capabilityRobotId"
            @click="addRobotCapabilityNode(capability)"
          >
            {{ capability.capability_key }}
            <small>
              {{ capability.operation_kind }} · {{ capability.endpoint_name }}
            </small>
          </button>
          <p v-if="directCapabilities.length === 0" class="hint-text">
            暂无可直接执行的 Service / Action / Topic / SSH 能力
          </p>
        </section>

        <section>
          <h4>点位 / 导航</h4>
          <label class="inline-add">
            导航机器人（选谁去点）
            <select v-model="navRobotId">
              <option value="" disabled>请选择机器人</option>
              <option
                v-for="robot in assets?.robots ?? []"
                :key="robot.id"
                :value="robot.id"
              >
                {{ robot.name }}
              </option>
            </select>
          </label>
          <div
            v-for="point in assets?.points ?? []"
            :key="point.id"
            class="palette-block"
          >
            <button
              type="button"
              class="palette-item"
              @click="addStationNode(point.id, point.name)"
            >
              {{ point.name }}
              <small>({{ point.x.toFixed(2) }}, {{ point.y.toFixed(2) }})</small>
            </button>
            <button
              type="button"
              class="ghost-button"
              :disabled="!navRobotId"
              @click="addNavigationNode(point.id, point.name)"
            >
              + 选定机器人导航至此
            </button>
            <button
              v-for="action in point.actions"
              :key="action.id"
              type="button"
              class="palette-item action"
              @click="
                addStationActionNode(
                  point.name,
                  action.id,
                  point.id,
                  action.action_name,
                  action.capability_key,
                  action.robot_id,
                  action.robot_name,
                  action.success_event_name
                )
              "
            >
              #{{ action.sequence_no }}
              {{ action.action_name || action.capability_key }}
              <small>
                {{ action.robot_name || robotNameById(action.robot_id ?? "") }} ·
                {{ action.capability_key }} · {{ action.motion_ownership }}
                <template v-if="action.success_event_name">
                  · evt {{ action.success_event_name }}
                </template>
              </small>
            </button>
          </div>
          <p v-if="!(assets?.points.length)" class="hint-text">暂无点位</p>
        </section>

        <section>
          <h4>外部事件（实验）</h4>
          <p class="hint-text">
            当前只支持按事件名等待或人工注入；ROS Topic/MQTT/设备订阅来源、参数条件和超时出口尚未实现。
          </p>
          <button
            v-for="event in assets?.suggested_events ?? []"
            :key="event.station_action_id + event.event_name"
            type="button"
            class="palette-item"
            @click="addEventWait(event.event_name)"
          >
            {{ event.event_name }}
            <small>
              {{ event.source || "RESULT" }} · {{ event.station_name }} /
              {{ event.action_name || event.capability_key }}
            </small>
          </button>
          <label class="inline-add">
            按名称等待外部事件（实验）
            <input v-model="connectEventName" placeholder="event_name" />
            <button
              type="button"
              class="ghost-button"
              @click="addEventWait(connectEventName)"
            >
              添加 EVENT_WAIT
            </button>
          </label>
        </section>
      </aside>

      <div class="workflow-canvas">
        <VueFlow
          v-model:nodes="nodes"
          v-model:edges="edges"
          :node-types="nodeTypes"
          fit-view-on-init
          @connect="handleConnect"
          @node-click="onNodeClick"
          @edge-click="onEdgeClick"
          @pane-click="clearGraphSelection"
          @node-drag-start="onNodeDragStart"
          @node-drag-stop="onNodeDragStop"
        >
          <Background pattern-color="#b7c4bc" :gap="18" />
          <Controls />
        </VueFlow>
      </div>

      <aside class="workflow-inspector">
        <h3>{{ selectedEdge ? "连线属性" : "节点属性" }}</h3>
        <template v-if="selectedNode">
          <dl class="inspector-kv">
            <div>
              <dt>ID</dt>
              <dd>{{ selectedNode.id }}</dd>
            </div>
            <div>
              <dt>类型</dt>
              <dd>{{ selectedNode.data?.nodeType }}</dd>
            </div>
            <div
              v-for="[key, value] in selectedNodeEntries"
              :key="key"
            >
              <dt>{{ key }}</dt>
              <dd>{{ value }}</dd>
            </div>
          </dl>
          <div
            v-if="selectedNode.data?.nodeType === 'NAVIGATION'"
            class="node-parameter-form"
          >
            <label>
              执行机器人
              <select
                :value="String(selectedNode.data?.robot_id ?? '')"
                @change="
                  updateSelectedNavigationRobot(
                    ($event.target as HTMLSelectElement).value
                  )
                "
              >
                <option value="" disabled>请选择</option>
                <option
                  v-for="robot in assets?.robots ?? []"
                  :key="robot.id"
                  :value="robot.id"
                >
                  {{ robot.name }}
                </option>
              </select>
            </label>
            <label>
              距离容差 (m)
              <input
                :value="Number(selectedNode.data?.distance_tolerance ?? 0.04)"
                type="number"
                min="0.001"
                step="0.01"
                @input="updateSelectedNavigationNumber('distance_tolerance', Number(($event.target as HTMLInputElement).value))"
              />
            </label>
            <label>
              朝向容差 (rad)
              <input
                :value="Number(selectedNode.data?.heading_tolerance ?? 0.04)"
                type="number"
                min="0.001"
                step="0.01"
                @input="updateSelectedNavigationNumber('heading_tolerance', Number(($event.target as HTMLInputElement).value))"
              />
            </label>
          </div>
          <div
            v-if="selectedNode.data?.nodeType === 'ROBOT_CAPABILITY'"
            class="node-parameter-form"
          >
            <label>
              执行机器人
              <select
                :value="String(selectedNode.data?.robot_id ?? '')"
                @change="
                  updateSelectedCapabilityRobot(
                    ($event.target as HTMLSelectElement).value
                  )
                "
              >
                <option value="" disabled>请选择</option>
                <option
                  v-for="robot in assets?.robots ?? []"
                  :key="robot.id"
                  :value="robot.id"
                >
                  {{ robot.name }}
                </option>
              </select>
            </label>
            <label>
              能力模板
              <select
                :value="String(selectedNode.data?.capability_definition_id ?? '')"
                @change="
                  updateSelectedCapabilityDefinition(
                    ($event.target as HTMLSelectElement).value
                  )
                "
              >
                <option value="" disabled>请选择</option>
                <option
                  v-for="capability in directCapabilities"
                  :key="capability.id"
                  :value="capability.id"
                >
                  {{ capability.capability_key }} · {{ capability.operation_kind }}
                </option>
              </select>
            </label>
            <label>
              超时 (ms)
              <input
                :value="Number(selectedNode.data?.timeout_ms ?? selectedDirectCapability?.timeout_ms ?? 30000)"
                type="number"
                min="100"
                step="100"
                @change="updateSelectedCapabilityTimeout(Number(($event.target as HTMLInputElement).value))"
              />
            </label>
            <label>
              失败后重试次数（0=不重试）
              <input
                :value="Number(selectedNode.data?.retry_count ?? 0)"
                type="number"
                min="0"
                max="100"
                @change="updateSelectedCapabilityRetry('retry_count', Number(($event.target as HTMLInputElement).value))"
              />
            </label>
            <label>
              重试间隔 (ms)
              <input
                :value="Number(selectedNode.data?.retry_delay_ms ?? 1000)"
                type="number"
                min="0"
                max="86400000"
                @change="updateSelectedCapabilityRetry('retry_delay_ms', Number(($event.target as HTMLInputElement).value))"
              />
            </label>
            <div>
              <strong>执行参数</strong>
              <SchemaForm
                :model-value="selectedDirectParameters"
                :schema="selectedDirectCapability?.parameter_schema"
                @update:model-value="updateSelectedCapabilityParameters"
              />
            </div>
            <p v-if="selectedDirectCapability" class="hint-text">
              {{ selectedDirectCapability.endpoint_name }} ·
              {{ selectedDirectCapability.motion_ownership }} ·
              {{ selectedDirectCapability.blocking_type }}
            </p>
          </div>
          <div
            v-if="selectedNode.data?.nodeType === 'DELAY'"
            class="node-parameter-form"
          >
            <label>
              延时时间 (ms)
              <input
                :value="Number(selectedNode.data?.delay_ms ?? 1000)"
                type="number"
                min="0"
                max="86400000"
                @change="updateSelectedDelay(Number(($event.target as HTMLInputElement).value))"
              />
            </label>
          </div>
          <div
            v-if="selectedNode.data?.nodeType === 'MANUAL_CONFIRM'"
            class="node-parameter-form"
          >
            <label>
              工程师确认提示
              <textarea
                :value="String(selectedNode.data?.prompt ?? '')"
                maxlength="500"
                rows="3"
                @change="updateSelectedManualConfirmPrompt(($event.target as HTMLTextAreaElement).value)"
              ></textarea>
            </label>
            <p class="hint-text">
              运行到此节点后，只暂停当前分支。请在「运行监控」中允许继续或拒绝终止。
            </p>
          </div>
          <div
            v-if="selectedNode.data?.nodeType === 'ROBOT_STARTUP' || selectedNode.data?.nodeType === 'ROBOT_SSH'"
            class="node-parameter-form"
          >
            <label>
              启动机器人
              <select
                :value="String(selectedNode.data?.robot_id ?? '')"
                @change="updateSelectedStartup('robot_id', ($event.target as HTMLSelectElement).value)"
              >
                <option value="" disabled>请选择</option>
                <option
                  v-for="robot in assets?.robots ?? []"
                  :key="robot.id"
                  :value="robot.id"
                >
                  {{ robot.name }}
                </option>
              </select>
            </label>
            <label>
              启动方案
              <select
                :value="String(selectedNode.data?.startup_profile_id ?? '')"
                @change="updateSelectedStartup('startup_profile_id', ($event.target as HTMLSelectElement).value)"
              >
                <option value="" disabled>请选择</option>
                <option
                  v-for="profile in selectedStartupProfiles"
                  :key="profile.id"
                  :value="profile.id"
                >
                  {{ profile.name }} · v{{ profile.version }}
                </option>
              </select>
            </label>
            <p class="hint-text">
              主机取机器人 IP，默认用户为 naviai。方案可执行 ./start.sh、绝对路径脚本或 command 数组；不接受 sh -c、管道和重定向。
            </p>
          </div>
          <div
            v-if="selectedNode.data?.nodeType === 'SUBFLOW'"
            class="node-parameter-form"
          >
            <label>
              已发布子流程
              <select
                :value="String(selectedNode.data?.workflow_definition_id ?? '')"
                @change="updateSelectedSubflow(($event.target as HTMLSelectElement).value)"
              >
                <option value="" disabled>请选择</option>
                <option
                  v-for="workflow in publishedSubflows"
                  :key="workflow.id"
                  :value="workflow.id"
                >
                  {{ workflow.name }} · pub v{{ workflow.version }}
                </option>
              </select>
            </label>
            <p class="hint-text">
              父流程等待子流程终态；子流程失败时父流程同步失败。
            </p>
          </div>
          <section
            v-if="selectedNodeUsesCapabilityEvents"
            class="node-event-panel"
          >
            <div class="node-event-heading">
              <strong>模板事件（{{ selectedNodeEventSpecs.length }}）</strong>
              <small>选择后，从此节点拖出事件边</small>
            </div>
            <article
              v-for="(spec, index) in selectedNodeEventSpecs"
              :key="`${spec.event_name}-${spec.source}-${index}`"
              class="node-event-item"
              :class="{
                disabled: spec.enabled === false || spec.emit_on_node === false
              }"
            >
              <div>
                <strong>{{ spec.event_name || "未命名事件" }}</strong>
                <span class="event-source">{{ spec.source || "RESULT" }}</span>
              </div>
              <small>{{ eventConditionSummary(spec) }}</small>
              <small v-if="spec.enabled === false">事件已禁用</small>
              <small v-else-if="spec.emit_on_node === false">
                仅启动/唤醒流程，不允许作为当前节点事件边
              </small>
              <button
                type="button"
                class="ghost-button"
                :disabled="
                  spec.enabled === false ||
                  spec.emit_on_node === false ||
                  !spec.event_name
                "
                @click="useEventForConnection(spec.event_name)"
              >
                用此事件连线
              </button>
            </article>
            <p v-if="selectedNodeEventSpecs.length === 0" class="hint-text">
              当前能力模板尚未定义事件，请先到「能力模板」添加 RESULT / FEEDBACK
              事件。
            </p>
            <p
              v-else-if="selectedNodeConnectableEvents.length === 0"
              class="hint-text"
            >
              当前事件均已禁用或未开启「作为节点事件边」。
            </p>
          </section>
          <button
            type="button"
            class="danger-button"
            @click="removeSelection"
          >
            删除节点
          </button>
        </template>
        <template v-else-if="selectedEdge">
          <dl class="inspector-kv">
            <div>
              <dt>ID</dt>
              <dd>{{ selectedEdge.id }}</dd>
            </div>
            <div v-for="[key, value] in selectedEdgeEntries" :key="key">
              <dt>{{ key }}</dt>
              <dd>{{ value }}</dd>
            </div>
          </dl>
          <button
            type="button"
            class="danger-button"
            @click="removeSelection"
          >
            删除连线
          </button>
        </template>
        <p v-else class="hint-text">点击画布节点查看属性</p>

        <h3>说明</h3>
        <p class="hint-text">
          导航须指定机器人与目标点。服务/动作节点右侧红点拉出失败边；
          失败且重试耗尽后走该边，没有失败边则整次运行 FAILED。事件来自能力「事件定义」或点位完成事件名：
          RESULT/FEEDBACK 条件满足后可走事件边、唤醒 EVENT_WAIT，或启动
          trigger_type=EVENT 的已发布流程（流程级填写同名事件）。
        </p>
      </aside>
    </div>
  </section>
</template>

<style scoped>
.workflow-page {
  display: grid;
  gap: 10px;
  height: calc(100dvh - 110px);
  min-height: 560px;
}

.workflow-toolbar {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  align-items: end;
  padding: 10px 12px;
  border: 1px solid var(--line);
  border-radius: 8px;
  background: #fff;
}

.workflow-toolbar label {
  display: grid;
  gap: 3px;
  color: var(--muted);
  font-size: 11px;
}

.workflow-toolbar input,
.workflow-toolbar select {
  min-width: 120px;
  height: 32px;
  padding: 0 8px;
  border: 1px solid var(--line);
  border-radius: 6px;
}

.workflow-meta {
  margin-left: auto;
  color: var(--muted);
  font-size: 12px;
  font-family: var(--mono);
}

.workflow-body {
  display: grid;
  grid-template-columns: 280px minmax(0, 1fr) 260px;
  gap: 10px;
  min-height: 0;
  flex: 1;
  height: 100%;
}

.workflow-palette,
.workflow-inspector {
  min-height: 0;
  overflow: auto;
  padding: 12px;
  border: 1px solid var(--line);
  border-radius: 8px;
  background: #fff;
}

.node-parameter-form {
  display: grid;
  gap: 8px;
  margin: 12px 0;
}

.node-parameter-form label {
  display: grid;
  gap: 4px;
  color: var(--muted);
  font-size: 12px;
}

.node-parameter-form input,
.node-parameter-form select {
  width: 100%;
  min-height: 32px;
  padding: 5px 8px;
  border: 1px solid var(--line);
  border-radius: 6px;
  background: #fff;
}

.node-event-panel {
  display: grid;
  gap: 8px;
  margin: 12px 0;
  padding-top: 12px;
  border-top: 1px solid var(--line);
}

.node-event-heading,
.node-event-item > div {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
}

.node-event-heading small,
.node-event-item small {
  color: var(--muted);
  font-size: 11px;
}

.node-event-item {
  display: grid;
  gap: 5px;
  padding: 8px;
  border: 1px solid var(--line);
  border-radius: 7px;
  background: #f7faf8;
}

.node-event-item.disabled {
  opacity: 0.62;
}

.event-source {
  padding: 2px 5px;
  border-radius: 4px;
  background: #e4edf8;
  color: #31577f;
  font-size: 10px;
  font-family: var(--mono);
}

.workflow-palette h3,
.workflow-inspector h3,
.workflow-palette h4 {
  margin: 0 0 8px;
}

.workflow-palette h4 {
  margin-top: 14px;
  font-size: 12px;
  letter-spacing: 0.06em;
  text-transform: uppercase;
  color: var(--muted);
}

.connect-mode {
  position: sticky;
  top: 0;
  z-index: 1;
  margin: 0 0 10px;
  padding: 0 0 10px;
  border-bottom: 1px solid var(--line);
  background: #fff;
}

.connect-mode h4 {
  margin-top: 0;
}

.connect-mode label {
  display: flex;
  align-items: center;
  gap: 6px;
  margin-top: 6px;
  font-size: 12px;
}

.palette-item {
  display: grid;
  width: 100%;
  margin-bottom: 6px;
  padding: 8px 10px;
  border: 1px solid var(--line);
  border-radius: 7px;
  background: #f7faf8;
  text-align: left;
}

.palette-item.action {
  background: #eef6f2;
}

.palette-item.capability {
  background: #eef3fb;
}

.palette-item small {
  color: var(--muted);
  font-size: 11px;
}

.palette-block {
  margin-bottom: 10px;
  padding-bottom: 8px;
  border-bottom: 1px dashed var(--line);
}

.workflow-canvas {
  min-height: 0;
  border: 1px solid var(--line);
  border-radius: 8px;
  background: #e7eee9;
  overflow: hidden;
}

.workflow-canvas :deep(.vue-flow__node-dispatch) {
  padding: 0;
  border: none;
  background: transparent;
  width: auto;
  font-size: inherit;
  text-align: left;
  box-shadow: none;
}

.inline-add {
  display: grid;
  gap: 6px;
  margin-top: 8px;
  font-size: 12px;
  color: var(--muted);
}

@media (max-width: 1100px) {
  .workflow-body {
    grid-template-columns: 1fr;
  }

  .workflow-canvas {
    min-height: 420px;
  }
}
</style>
