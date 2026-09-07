<script setup lang="ts">
import {
  Link2,
  MapPinPlus,
  Maximize2,
  Navigation,
  ScrollText,
  Upload,
  X
} from "lucide-vue-next";
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from "vue";

import {
  createMapPoint,
  createScene,
  deleteMapPoint,
  deleteMapVersion,
  deleteScene,
  getWorkspace,
  importMap,
  listCapabilityTemplates,
  listRobots,
  listSceneMaps,
  listScenes,
  replaceMapPointActions,
  sendNavigationGoal,
  setActiveMap,
  updateMapPoint,
  updateSceneRobots,
  worldFromPixel
} from "../api/workspace";
import MapCanvas from "../components/MapCanvas.vue";
import OpsLogPanel from "../components/OpsLogPanel.vue";
import SchemaForm from "../components/SchemaForm.vue";
import type {
  CapabilityTemplate,
  MapPoint,
  MapPointAction,
  MapVersion,
  RobotSummary,
  Scene,
  SceneWorkspace,
  WorldPose
} from "../types/workspace";
import { worldToPixel } from "../utils/mapTransform";

const scenes = ref<Scene[]>([]);
const robots = ref<RobotSummary[]>([]);
const mapVersions = ref<MapVersion[]>([]);
const capabilityTemplates = ref<CapabilityTemplate[]>([]);
const selectedSceneId = ref("");
const selectedMapVersionId = ref("");
const workspace = ref<SceneWorkspace | null>(null);
const selectedRobotIds = ref<string[]>([]);
const errorMessage = ref("");
const statusMessage = ref("");
const loading = ref(false);
const savingPoint = ref(false);
const goalPixel = ref<{ x: number; y: number } | null>(null);
const goalWorld = ref<WorldPose | null>(null);
const selectedNavRobotId = ref("");
const showConfirm = ref(false);
const showPointPanel = ref(false);
const distanceTolerance = ref(0.04);
const headingTolerance = ref(0.04);
const pointEditMode = ref<"position" | "actions" | "all">("all");
const pointContextMenu = ref<{
  pointId: string;
  clientX: number;
  clientY: number;
} | null>(null);
const yawDegrees = ref(0);
const pendingPointName = ref("P1");
const creatingPoint = ref(false);
const newSceneName = ref("");
const yamlFile = ref<File | null>(null);
const pgmFile = ref<File | null>(null);
const mapCanvasRef = ref<InstanceType<typeof MapCanvas> | null>(null);
const toolMode = ref<"browse" | "place" | "navigate">("browse");
const selectedPointId = ref("");
const editingPoint = ref<MapPoint | null>(null);
const draftName = ref("");
const draftX = ref(0);
const draftY = ref(0);
const draftYaw = ref(0);
const draftNotes = ref("");
const draftActions = ref<MapPointAction[]>([]);
const panelError = ref("");
const layerPoints = ref(true);
const layerRobots = ref(true);
const layerLabels = ref(true);
const layerGrid = ref(true);
const selectedInspectRobotId = ref("");
const showBindPanel = ref(false);
const showLogs = ref(false);
const showMore = ref(false);
/** Hover-reveal for the right drawer (guide / lists). */
const inspectorHovered = ref(false);

const businessCapabilities = computed(() => {
  const items = capabilityTemplates.value.filter(
    (item) => item.capability_key !== "navigation"
  );
  return items.length > 0 ? items : capabilityTemplates.value;
});

let pollTimer: number | undefined;
let controller: AbortController | undefined;

function nextPointName(): string {
  const usedNames = new Set(
    mapPoints.value.map((point) => point.name.trim().toLowerCase())
  );
  let candidate = 1;
  while (usedNames.has(`p${candidate}`.toLowerCase())) {
    candidate += 1;
  }
  return `P${candidate}`;
}

const activeMap = computed(() => workspace.value?.map ?? null);
const sceneRobots = computed(() => workspace.value?.robots ?? []);
const mapPoints = computed(() => workspace.value?.points ?? []);
const activeScene = computed(
  () => scenes.value.find((item) => item.id === selectedSceneId.value) ?? null
);
const selectedInspectRobot = computed(
  () =>
    sceneRobots.value.find((robot) => robot.id === selectedInspectRobotId.value) ??
    null
);
const selectedPoint = computed(
  () => mapPoints.value.find((point) => point.id === selectedPointId.value) ?? null
);
const pointContextStyle = computed(() => {
  const menu = pointContextMenu.value;
  if (!menu) {
    return {};
  }
  return {
    left: `${Math.max(8, Math.min(menu.clientX, window.innerWidth - 216))}px`,
    top: `${Math.max(8, Math.min(menu.clientY, window.innerHeight - 164))}px`
  };
});

const inspectorMode = computed<"pointEdit" | "robot" | "bind" | "guide">(
  () => {
    if (showPointPanel.value && editingPoint.value) {
      return "pointEdit";
    }
    if (selectedInspectRobot.value) {
      return "robot";
    }
    if (showBindPanel.value) {
      return "bind";
    }
    return "guide";
  }
);

/** Keep drawer open while editing / binding / inspecting a robot / navigating. */
const inspectorPinned = computed(
  () =>
    inspectorMode.value === "pointEdit" ||
    inspectorMode.value === "bind" ||
    inspectorMode.value === "robot" ||
    toolMode.value === "navigate" ||
    toolMode.value === "place"
);

const inspectorOpen = computed(
  () => inspectorPinned.value || inspectorHovered.value
);

function onInspectorEnter(): void {
  inspectorHovered.value = true;
}

function onInspectorLeave(): void {
  inspectorHovered.value = false;
}

/** Drag only while the position editor is open for that point. */
const draggablePointId = computed(() => {
  if (
    showPointPanel.value &&
    editingPoint.value &&
    pointEditMode.value !== "actions"
  ) {
    return editingPoint.value.id;
  }
  return "";
});

const guideStep = computed(() => {
  if (!selectedSceneId.value) {
    return "scene";
  }
  if (!activeMap.value) {
    return "map";
  }
  if (sceneRobots.value.length === 0) {
    return "bind";
  }
  return "ready";
});

function robotPoseHint(robot: {
  connection_state: string;
  drawable: boolean;
  pose: { stale?: boolean } | null;
  current_scene_id?: string | null;
}): string {
  if (
    robot.connection_state !== "ONLINE" &&
    robot.connection_state !== "CONNECTED"
  ) {
    return "未连接 rosbridge";
  }
  if (!robot.current_scene_id) {
    return "未绑定本场景";
  }
  if (!robot.pose) {
    return "等待定位话题";
  }
  if (robot.pose.stale) {
    return "位姿超时";
  }
  if (!robot.drawable) {
    return "坐标未对齐地图";
  }
  return "地图上可见";
}

function robotLabel(id: string | null): string {
  if (!id) {
    return "未指定";
  }
  return robots.value.find((robot) => robot.id === id)?.name ?? id.slice(0, 8);
}

function summarizeActions(point: MapPoint): string {
  if (!point.actions.length) {
    return "未配置动作";
  }
  return point.actions
    .map(
      (action) =>
        `${robotLabel(action.robot_id)}→${action.action_name || action.capability_key || "?"}`
    )
    .join("；");
}

async function refreshScenes(): Promise<void> {
  const result = await listScenes();
  scenes.value = result.items;
  if (!selectedSceneId.value && scenes.value.length > 0) {
    selectedSceneId.value = scenes.value[0].id;
  }
}

async function refreshRobots(): Promise<void> {
  const result = await listRobots();
  robots.value = result.items;
}

async function refreshCapabilities(): Promise<void> {
  const result = await listCapabilityTemplates();
  capabilityTemplates.value = result.items;
}

async function refreshMapVersions(): Promise<void> {
  if (!selectedSceneId.value) {
    mapVersions.value = [];
    selectedMapVersionId.value = "";
    return;
  }
  const result = await listSceneMaps(selectedSceneId.value);
  mapVersions.value = result.items;
  const activeId = workspace.value?.scene.active_map_version_id;
  if (activeId && mapVersions.value.some((map) => map.id === activeId)) {
    selectedMapVersionId.value = activeId;
  } else if (mapVersions.value.length > 0) {
    selectedMapVersionId.value = mapVersions.value[0].id;
  } else {
    selectedMapVersionId.value = "";
  }
}

function defaultsFromSchema(
  schema: Record<string, unknown> | null | undefined
): Record<string, unknown> {
  const properties =
    schema && typeof schema === "object"
      ? ((schema as { properties?: Record<string, { default?: unknown }> })
          .properties ?? {})
      : {};
  const values: Record<string, unknown> = {};
  for (const [key, field] of Object.entries(properties)) {
    if (field && Object.prototype.hasOwnProperty.call(field, "default")) {
      values[key] = field.default;
    }
  }
  return values;
}

function findCapabilityById(id: string): CapabilityTemplate | undefined {
  if (!id) {
    return undefined;
  }
  return capabilityTemplates.value.find((item) => item.id === id);
}

function templateOfAction(
  action: MapPointAction
): CapabilityTemplate | undefined {
  const byId = findCapabilityById(action.capability_definition_id ?? "");
  if (byId) {
    return byId;
  }
  if (!action.capability_key) {
    return undefined;
  }
  return capabilityTemplates.value.find(
    (item) => item.capability_key === action.capability_key
  );
}

function capabilityOptionLabel(item: CapabilityTemplate): string {
  return `${item.capability_key} · ${item.operation_kind} · ${item.endpoint_name}`;
}

function makeEmptyAction(sequenceNo: number): MapPointAction {
  const template = businessCapabilities.value[0];
  return {
    sequence_no: sequenceNo,
    action_name: `动作 ${sequenceNo}`,
    parallel_group: null,
    capability_definition_id: template?.id ?? "",
    capability_key: template?.capability_key ?? "",
    motion_ownership: template?.motion_ownership ?? "DISPATCHER",
    robot_selector_type: "FIXED",
    robot_id: robots.value[0]?.id ?? "",
    robot_group: "",
    runtime_variable: "",
    parameters: defaultsFromSchema(template?.parameter_schema),
    precondition: null,
    failure_policy: "FAIL",
    retry_count: 0,
    timeout_ms: template?.timeout_ms ?? 30000,
    success_event_name: "",
    post_navigation_station_id: null
  };
}

function applyCapabilityTemplate(
  action: MapPointAction,
  template: CapabilityTemplate
): void {
  action.capability_definition_id = template.id;
  action.capability_key = template.capability_key;
  action.motion_ownership = template.motion_ownership;
  action.timeout_ms = template.timeout_ms;
  action.parameters = defaultsFromSchema(template.parameter_schema);
  action.post_navigation_station_id = null;
}

function onCapabilitySelect(action: MapPointAction, capabilityId: string): void {
  const template = findCapabilityById(capabilityId);
  if (!template) {
    return;
  }
  if (
    template.id === action.capability_definition_id &&
    template.capability_key === action.capability_key
  ) {
    return;
  }
  applyCapabilityTemplate(action, template);
}

function validateActionParameters(action: MapPointAction, index: number): string {
  const template = templateOfAction(action);
  if (!template) {
    return `第 ${index + 1} 条请选择能力模板`;
  }
  const schema = (template.parameter_schema ?? {}) as {
    required?: string[];
    properties?: Record<string, unknown>;
  };
  const required = schema.required ?? [];
  for (const key of required) {
    const value = action.parameters?.[key];
    if (value === undefined || value === null || value === "") {
      const title =
        (schema.properties?.[key] as { title?: string } | undefined)?.title ||
        key;
      return `第 ${index + 1} 条请填写参数「${title}」`;
    }
  }
  return "";
}

async function refreshWorkspace(): Promise<void> {
  if (!selectedSceneId.value) {
    workspace.value = null;
    mapVersions.value = [];
    return;
  }
  controller?.abort();
  controller = new AbortController();
  const signal = controller.signal;
  try {
    const next = await getWorkspace(selectedSceneId.value, signal);
    if (signal.aborted) {
      return;
    }
    if (!Array.isArray(next.points)) {
      next.points = [];
    }
    // Keep the open point editor intact. Replacing workspace/points here
    // re-renders native <select>s and they jump back to the first option.
    if (showPointPanel.value && workspace.value) {
      workspace.value.robots = next.robots;
      return;
    }
    workspace.value = next;
    if (!showBindPanel.value) {
      selectedRobotIds.value = next.robots.map((robot) => robot.id);
    }
    if (!selectedNavRobotId.value && next.robots.length > 0) {
      selectedNavRobotId.value = next.robots[0].id;
    }
    if (
      selectedPointId.value &&
      !next.points.some((point) => point.id === selectedPointId.value)
    ) {
      selectedPointId.value = "";
      showPointPanel.value = false;
      editingPoint.value = null;
    }
    await refreshMapVersions();
  } catch (error) {
    if (error instanceof DOMException && error.name === "AbortError") {
      return;
    }
    throw error;
  }
}

function clonePoint(point: MapPoint): MapPoint {
  return JSON.parse(JSON.stringify(point)) as MapPoint;
}

async function handleActiveMapChange(): Promise<void> {
  if (!selectedSceneId.value || !selectedMapVersionId.value) {
    return;
  }
  if (selectedMapVersionId.value === workspace.value?.scene.active_map_version_id) {
    return;
  }
  const map = mapVersions.value.find(
    (item) => item.id === selectedMapVersionId.value
  );
  if (map?.status === "ARCHIVED") {
    errorMessage.value =
      "已归档地图不能设为活动地图；请选择 READY 版本或删除/归档其他版本";
    selectedMapVersionId.value =
      workspace.value?.scene.active_map_version_id ?? "";
    return;
  }
  try {
    await setActiveMap(selectedSceneId.value, selectedMapVersionId.value);
    await refreshWorkspace();
    statusMessage.value = map
      ? `已切换活动地图 v${map.version}（${map.width}×${map.height}）`
      : "已切换活动地图";
    errorMessage.value = "";
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "切换地图失败";
  }
}

async function bootstrap(): Promise<void> {
  loading.value = true;
  errorMessage.value = "";
  try {
    await Promise.all([
      refreshScenes(),
      refreshRobots(),
      refreshCapabilities()
    ]);
    await refreshWorkspace();
  } catch (error) {
    errorMessage.value = error instanceof Error ? error.message : "加载失败";
  } finally {
    loading.value = false;
  }
}

async function handleCreateScene(): Promise<void> {
  if (!newSceneName.value.trim()) {
    return;
  }
  try {
    const scene = await createScene(newSceneName.value.trim());
    newSceneName.value = "";
    await refreshScenes();
    selectedSceneId.value = scene.id;
    statusMessage.value = `已创建场景 ${scene.name}`;
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "创建场景失败";
  }
}

async function handleDeleteScene(): Promise<void> {
  if (!selectedSceneId.value) {
    return;
  }
  const scene = scenes.value.find((item) => item.id === selectedSceneId.value);
  const name = scene?.name ?? selectedSceneId.value;
  if (
    !window.confirm(
      `确认删除场景「${name}」？将解除机器人归属并删除该场景下全部地图版本。`
    )
  ) {
    return;
  }
  try {
    loading.value = true;
    await deleteScene(selectedSceneId.value);
    selectedSceneId.value = "";
    selectedPointId.value = "";
    showPointPanel.value = false;
    editingPoint.value = null;
    workspace.value = null;
    mapVersions.value = [];
    await refreshScenes();
    if (scenes.value.length > 0) {
      selectedSceneId.value = scenes.value[0].id;
      await refreshWorkspace();
    }
    statusMessage.value = `已删除场景 ${name}`;
    errorMessage.value = "";
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "删除场景失败";
  } finally {
    loading.value = false;
  }
}

async function handleDeleteMap(): Promise<void> {
  if (!selectedMapVersionId.value) {
    return;
  }
  const map = mapVersions.value.find(
    (item) => item.id === selectedMapVersionId.value
  );
  const label = map
    ? `v${map.version}（${map.width}×${map.height}）`
    : selectedMapVersionId.value;
  if (
    !window.confirm(
      `确认移除地图 ${label}？未被引用则删除；当前活动地图或仍有点位/机器人引用时只会归档。`
    )
  ) {
    return;
  }
  try {
    loading.value = true;
    const result = await deleteMapVersion(selectedMapVersionId.value);
    selectedPointId.value = "";
    showPointPanel.value = false;
    editingPoint.value = null;
    await refreshWorkspace();
    statusMessage.value =
      result.action === "archived"
        ? `地图 ${label} 已被引用，已归档（不可再设为活动地图）`
        : `已删除地图 ${label}`;
    errorMessage.value = "";
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "删除地图失败";
  } finally {
    loading.value = false;
  }
}

function onYamlSelected(event: Event): void {
  const input = event.target as HTMLInputElement;
  yamlFile.value = input.files?.[0] ?? null;
  statusMessage.value = yamlFile.value
    ? `已选择 YAML：${yamlFile.value.name}`
    : "";
  errorMessage.value = "";
}

function onPgmSelected(event: Event): void {
  const input = event.target as HTMLInputElement;
  pgmFile.value = input.files?.[0] ?? null;
  statusMessage.value = pgmFile.value
    ? `已选择 PGM：${pgmFile.value.name}`
    : "";
  errorMessage.value = "";
}

async function handleImport(): Promise<void> {
  if (!yamlFile.value || !pgmFile.value) {
    errorMessage.value =
      "请同时选择 .yaml/.yml 与 .pgm（可从任意目录选择，不必放在工程内）";
    statusMessage.value = "";
    return;
  }
  const form = new FormData();
  if (selectedSceneId.value) {
    form.append("scene_id", selectedSceneId.value);
  } else if (newSceneName.value.trim()) {
    form.append("scene_name", newSceneName.value.trim());
  } else {
    errorMessage.value = "请先选择或创建场景";
    statusMessage.value = "";
    return;
  }
  form.append("yaml", yamlFile.value, yamlFile.value.name);
  form.append("pgm", pgmFile.value, pgmFile.value.name);
  try {
    loading.value = true;
    errorMessage.value = "";
    statusMessage.value = "正在导入地图…";
    const imported = await importMap(form);
    selectedSceneId.value = imported.scene.id;
    if (imported.map?.id) {
      await setActiveMap(imported.scene.id, imported.map.id);
    }
    await refreshScenes();
    await refreshWorkspace();
    statusMessage.value =
      `地图导入成功：${yamlFile.value.name} + ${pgmFile.value.name}` +
      `（v${imported.map.version}, ${imported.map.width}×${imported.map.height}）`;
    errorMessage.value = "";
  } catch (error) {
    statusMessage.value = "";
    errorMessage.value = error instanceof Error ? error.message : "导入失败";
  } finally {
    loading.value = false;
  }
}

async function handleSaveRobots(): Promise<void> {
  if (!selectedSceneId.value) {
    return;
  }
  try {
    await updateSceneRobots(selectedSceneId.value, selectedRobotIds.value);
    showBindPanel.value = false;
    await refreshWorkspace();
    statusMessage.value =
      selectedRobotIds.value.length > 0
        ? `已绑定 ${selectedRobotIds.value.length} 台机器人到本场景，等待位姿…`
        : "已清空场景机器人绑定";
    errorMessage.value = "";
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "更新归属失败";
  }
}

async function handleUnbindRobot(robotId: string): Promise<void> {
  if (!selectedSceneId.value) {
    return;
  }
  const robotName =
    sceneRobots.value.find((robot) => robot.id === robotId)?.name ?? "机器人";
  const remaining = sceneRobots.value
    .map((robot) => robot.id)
    .filter((id) => id !== robotId);
  try {
    await updateSceneRobots(selectedSceneId.value, remaining);
    if (selectedInspectRobotId.value === robotId) {
      selectedInspectRobotId.value = "";
    }
    if (selectedNavRobotId.value === robotId) {
      selectedNavRobotId.value = remaining[0] ?? "";
    }
    selectedRobotIds.value = remaining;
    await refreshWorkspace();
    statusMessage.value =
      remaining.length > 0
        ? `已从本场景解绑 ${robotName}`
        : `已解绑 ${robotName}，本场景已无在场机器人`;
    errorMessage.value = "";
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "解绑失败";
  }
}

async function onClickPixel(payload: { x: number; y: number }): Promise<void> {
  pointContextMenu.value = null;
  if (!activeMap.value || !selectedSceneId.value) {
    return;
  }
  try {
    const world = await worldFromPixel(
      activeMap.value.id,
      payload.x,
      payload.y,
      (yawDegrees.value * Math.PI) / 180
    );
    if (toolMode.value === "navigate") {
      goalPixel.value = payload;
      goalWorld.value = world;
      statusMessage.value = "已选导航目标，右侧确认发送";
      return;
    }
    if (toolMode.value !== "place") {
      return;
    }
    goalPixel.value = payload;
    goalWorld.value = world;
    if (!pendingPointName.value.trim()) {
      pendingPointName.value = nextPointName();
    }
    statusMessage.value = "已选位置，右侧确认后才会添加点位";
    errorMessage.value = "";
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "地图点击处理失败";
  }
}

function onSelectPoint(pointId: string): void {
  pointContextMenu.value = null;
  if (!pointId) {
    selectedPointId.value = "";
    return;
  }
  // Single click: highlight only. Config panel opens on double-click.
  selectedPointId.value = pointId;
  if (showPointPanel.value) {
    return;
  }
  editingPoint.value = null;
  selectedInspectRobotId.value = "";
  showBindPanel.value = false;
  statusMessage.value = "已选中点位 · 双击打开配置";
}

function clearPlacementPreview(): void {
  if (toolMode.value === "place") {
    goalPixel.value = null;
    goalWorld.value = null;
  }
}

function setTool(mode: "browse" | "place" | "navigate"): void {
  toolMode.value = mode;
  showBindPanel.value = false;
  goalPixel.value = null;
  goalWorld.value = null;
  if (mode === "place") {
    pendingPointName.value = nextPointName();
    statusMessage.value = "添加点位：在地图上单击选位置，右侧确认后才会创建";
  } else if (mode === "navigate") {
    statusMessage.value = "导航：在地图上单击目标点";
  } else {
    statusMessage.value = "";
  }
}

async function confirmPlacePoint(): Promise<void> {
  if (
    !selectedSceneId.value ||
    !activeMap.value ||
    !goalWorld.value ||
    toolMode.value !== "place"
  ) {
    errorMessage.value = "请先在地图上单击选择点位位置";
    return;
  }
  const name = pendingPointName.value.trim() || nextPointName();
  try {
    creatingPoint.value = true;
    const created = await createMapPoint({
      scene_id: selectedSceneId.value,
      map_version_id: activeMap.value.id,
      name,
      x: goalWorld.value.x,
      y: goalWorld.value.y,
      yaw: (yawDegrees.value * Math.PI) / 180
    });
    selectedPointId.value = created.id;
    showPointPanel.value = false;
    editingPoint.value = null;
    selectedInspectRobotId.value = "";
    goalPixel.value = null;
    goalWorld.value = null;
    await refreshWorkspace();
    pendingPointName.value = nextPointName();
    statusMessage.value = `已添加 ${created.name} · 可继续点地图添加下一个，或 Esc 退出`;
    errorMessage.value = "";
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "添加点位失败";
  } finally {
    creatingPoint.value = false;
  }
}

function openBindPanel(): void {
  toolMode.value = "browse";
  showBindPanel.value = true;
  showPointPanel.value = false;
  editingPoint.value = null;
  selectedInspectRobotId.value = "";
  selectedPointId.value = "";
  statusMessage.value = "勾选机器人并保存，即可绑定到当前地图场景";
}

function closePointPanel(): void {
  showPointPanel.value = false;
  editingPoint.value = null;
  draftActions.value = [];
  panelError.value = "";
  statusMessage.value = "";
}

function onPointContextMenu(payload: {
  pointId: string;
  clientX: number;
  clientY: number;
}): void {
  const point = mapPoints.value.find((item) => item.id === payload.pointId);
  if (!point) {
    return;
  }
  selectedPointId.value = point.id;
  selectedInspectRobotId.value = "";
  showBindPanel.value = false;
  pointContextMenu.value = payload;
  statusMessage.value = `已选中「${point.name}」`;
  errorMessage.value = "";
}

function closePointContextMenu(): void {
  pointContextMenu.value = null;
}

function focusPointOnMap(point: MapPoint): void {
  if (!activeMap.value) {
    return;
  }
  const pixel =
    point.pixel_x != null && point.pixel_y != null
      ? { x: point.pixel_x, y: point.pixel_y }
      : worldToPixel(activeMap.value, {
          x: point.x,
          y: point.y,
          yaw: point.yaw
        });
  if (!pixel) {
    return;
  }
  mapCanvasRef.value?.centerOn(pixel.x, pixel.y);
}

function openPointPanel(
  pointId: string,
  section: "position" | "actions" | "all" = "all"
): void {
  const point = mapPoints.value.find((item) => item.id === pointId);
  if (!point) {
    errorMessage.value = "未找到点位，请刷新后重试";
    return;
  }
  selectedPointId.value = pointId;
  pointContextMenu.value = null;
  pointEditMode.value = section;
  editingPoint.value = clonePoint(point);
  draftName.value = point.name;
  draftX.value = Number(point.x.toFixed(4));
  draftY.value = Number(point.y.toFixed(4));
  draftYaw.value = Math.round((point.yaw * 180) / Math.PI);
  draftNotes.value = point.notes ?? "";
  draftActions.value = point.actions.map((action) => ({
    ...action,
    action_name:
      action.action_name?.trim() || action.capability_key || `动作 ${action.sequence_no}`,
    robot_id: action.robot_id ?? "",
    capability_definition_id: action.capability_definition_id ?? "",
    parameters: { ...(action.parameters ?? {}) }
  }));
  panelError.value = "";
  selectedInspectRobotId.value = "";
  showBindPanel.value = false;
  showPointPanel.value = true;
  statusMessage.value =
    section === "position"
      ? `正在修改「${point.name}」的位置`
      : section === "actions"
        ? `正在配置「${point.name}」的动作`
        : `「${point.name}」完整配置已打开`;
  errorMessage.value = "";
  void nextTick(() => focusPointOnMap(point));
}

function publishPointNavigation(pointId: string): void {
  const point = mapPoints.value.find((item) => item.id === pointId);
  if (!point || !activeMap.value || !selectedSceneId.value) {
    errorMessage.value = "点位或活动地图不可用，请刷新后重试";
    closePointContextMenu();
    return;
  }
  if (point.map_version_id !== activeMap.value.id) {
    errorMessage.value = "点位不属于当前活动地图，禁止发布导航";
    closePointContextMenu();
    return;
  }
  if (sceneRobots.value.length === 0) {
    errorMessage.value = "当前场景没有已绑定机器人，无法发布导航";
    closePointContextMenu();
    return;
  }
  if (!sceneRobots.value.some((robot) => robot.id === selectedNavRobotId.value)) {
    selectedNavRobotId.value = sceneRobots.value[0].id;
  }
  const pixel =
    point.pixel_x != null && point.pixel_y != null
      ? { x: point.pixel_x, y: point.pixel_y }
      : worldToPixel(activeMap.value, point);
  if (!pixel || ("inside" in pixel && !pixel.inside)) {
    errorMessage.value = "点位坐标超出当前地图范围，禁止发布导航";
    closePointContextMenu();
    return;
  }
  selectedPointId.value = point.id;
  goalPixel.value = { x: pixel.x, y: pixel.y };
  goalWorld.value = { x: point.x, y: point.y, yaw: point.yaw };
  yawDegrees.value = Math.round((point.yaw * 180) / Math.PI);
  toolMode.value = "navigate";
  closePointContextMenu();
  statusMessage.value = `导航目标已设为点位「${point.name}」，请确认机器人和地图版本`;
  errorMessage.value = "";
  showConfirm.value = true;
}

function selectInspectRobot(robotId: string): void {
  selectedInspectRobotId.value = robotId;
  selectedNavRobotId.value = robotId;
  showPointPanel.value = false;
  editingPoint.value = null;
  selectedPointId.value = "";
  showBindPanel.value = false;
  statusMessage.value = "";
  errorMessage.value = "";
}

function clearInspector(): void {
  selectedInspectRobotId.value = "";
  closePointPanel();
}

async function savePointPanel(): Promise<void> {
  if (!editingPoint.value) {
    return;
  }
  if (!draftName.value.trim()) {
    panelError.value = "点位名称不能为空";
    return;
  }
  if (!Number.isFinite(draftX.value) || !Number.isFinite(draftY.value)) {
    panelError.value = "坐标 x/y 必须是有效数字（地图世界坐标，单位 m）";
    return;
  }
  if (!Number.isFinite(draftYaw.value)) {
    panelError.value = "yaw 必须是有效数字";
    return;
  }
  if (activeMap.value) {
    const pixel = worldToPixel(activeMap.value, {
      x: draftX.value,
      y: draftY.value,
      yaw: (draftYaw.value * Math.PI) / 180
    });
    if (!pixel?.inside) {
      panelError.value = "点位坐标超出当前地图范围，请修改 x/y";
      return;
    }
  }
  const filledActions = draftActions.value.filter(
    (action) => action.robot_id || action.capability_definition_id
  );
  for (const [index, action] of filledActions.entries()) {
    if (!action.action_name.trim()) {
      panelError.value = `第 ${index + 1} 条请填写动作名称`;
      return;
    }
    if (!action.robot_id) {
      panelError.value = `第 ${index + 1} 条请选择机器人`;
      return;
    }
    if (!action.capability_definition_id && !action.capability_key) {
      panelError.value = `第 ${index + 1} 条请选择能力模板`;
      return;
    }
    const paramError = validateActionParameters(action, index);
    if (paramError) {
      panelError.value = paramError;
      return;
    }
  }
  panelError.value = "";
  await handleSavePointConfig({
    name: draftName.value.trim(),
    x: draftX.value,
    y: draftY.value,
    yaw: (draftYaw.value * Math.PI) / 180,
    notes: draftNotes.value,
    tags: editingPoint.value.tags ?? [],
    actions: filledActions.map((action, index) => {
      const template = capabilityTemplates.value.find(
        (item) => item.id === action.capability_definition_id
      );
      return {
        ...action,
        sequence_no: index + 1,
        action_name: action.action_name.trim(),
        capability_key: template?.capability_key || action.capability_key,
        capability_definition_id:
          template?.id || action.capability_definition_id || null,
        robot_id: action.robot_id || null,
        post_navigation_station_id: action.post_navigation_station_id || null
      };
    })
  });
}

async function onMovePoint(payload: {
  id: string;
  pixelX: number;
  pixelY: number;
}): Promise<void> {
  if (!activeMap.value) {
    return;
  }
  const point = mapPoints.value.find((item) => item.id === payload.id);
  if (!point) {
    return;
  }
  try {
    const world = await worldFromPixel(
      activeMap.value.id,
      payload.pixelX,
      payload.pixelY,
      point.yaw
    );
    await updateMapPoint(point.id, {
      name: point.name,
      x: world.x,
      y: world.y,
      yaw: point.yaw,
      tags: point.tags,
      notes: point.notes
    });
    await refreshWorkspace();
    statusMessage.value = `已移动点位 ${point.name}`;
    errorMessage.value = "";
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "移动点位失败";
    await refreshWorkspace();
  }
}

async function handleSavePointConfig(payload: {
  name: string;
  x: number;
  y: number;
  yaw: number;
  notes: string;
  tags: string[];
  actions: MapPointAction[];
}): Promise<void> {
  if (!editingPoint.value) {
    return;
  }
  try {
    savingPoint.value = true;
    const pointId = editingPoint.value.id;
    await updateMapPoint(pointId, {
      name: payload.name,
      x: payload.x,
      y: payload.y,
      yaw: payload.yaw,
      tags: payload.tags,
      notes: payload.notes
    });
    await replaceMapPointActions(
      pointId,
      payload.actions.map((action, index) => ({
        sequence_no: index + 1,
        action_name: action.action_name,
        parallel_group: action.parallel_group,
        capability_definition_id: action.capability_definition_id,
        capability_key: action.capability_key,
        robot_selector_type: action.robot_selector_type,
        robot_id: action.robot_id,
        robot_group: action.robot_group,
        runtime_variable: action.runtime_variable,
        parameters: action.parameters,
        failure_policy: action.failure_policy,
        retry_count: action.retry_count,
        timeout_ms: action.timeout_ms,
        success_event_name: action.success_event_name,
        post_navigation_station_id: action.post_navigation_station_id
      }))
    );
    await refreshWorkspace();
    const saved = mapPoints.value.find((point) => point.id === pointId);
    closePointPanel();
    selectedPointId.value = pointId;
    statusMessage.value =
      `已保存点位「${payload.name}」` +
      (payload.actions.length ? ` · ${payload.actions.length} 个动作` : "") +
      " · 已跳转到该位置";
    errorMessage.value = "";
    await nextTick();
    if (saved) {
      focusPointOnMap(saved);
    } else if (activeMap.value) {
      const pixel = worldToPixel(activeMap.value, {
        x: payload.x,
        y: payload.y,
        yaw: payload.yaw
      });
      if (pixel) {
        mapCanvasRef.value?.centerOn(pixel.x, pixel.y);
      }
    }
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "保存点位配置失败";
  } finally {
    savingPoint.value = false;
  }
}

async function handleDeletePoint(pointId: string): Promise<void> {
  const point =
    mapPoints.value.find((item) => item.id === pointId) ??
    (editingPoint.value?.id === pointId ? editingPoint.value : null);
  if (!point) {
    errorMessage.value = "未找到要删除的点位";
    return;
  }
  if (!window.confirm(`确认删除点位「${point.name}」？`)) {
    return;
  }
  try {
    const result = await deleteMapPoint(point.id);
    if (selectedPointId.value === point.id) {
      selectedPointId.value = "";
    }
    if (editingPoint.value?.id === point.id) {
      showPointPanel.value = false;
      editingPoint.value = null;
    }
    await refreshWorkspace();
    statusMessage.value =
      result.action === "archived"
        ? `点位 ${point.name} 仍被引用，已归档`
        : `已删除点位 ${point.name}`;
    errorMessage.value = "";
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "删除点位失败";
  }
}

function openConfirm(): void {
  if (!goalWorld.value || !selectedNavRobotId.value || !activeMap.value) {
    errorMessage.value = "请点「导航」后在地图上单击目标点，并选择机器人";
    return;
  }
  showConfirm.value = true;
}

async function confirmNavigate(): Promise<void> {
  if (
    !goalWorld.value ||
    !selectedNavRobotId.value ||
    !activeMap.value ||
    !selectedSceneId.value
  ) {
    return;
  }
  if (
    !Number.isFinite(distanceTolerance.value) ||
    distanceTolerance.value <= 0 ||
    !Number.isFinite(headingTolerance.value) ||
    headingTolerance.value <= 0
  ) {
    errorMessage.value = "距离容差和朝向容差必须大于 0";
    return;
  }
  try {
    const result = await sendNavigationGoal({
      robot_id: selectedNavRobotId.value,
      scene_id: selectedSceneId.value,
      map_version_id: activeMap.value.id,
      x: goalWorld.value.x,
      y: goalWorld.value.y,
      yaw: (yawDegrees.value * Math.PI) / 180,
      distance_tolerance: distanceTolerance.value,
      heading_tolerance: headingTolerance.value
    });
    showConfirm.value = false;
    statusMessage.value = `导航命令已提交：${result.state} (${result.command_id})`;
    errorMessage.value = "";
  } catch (error) {
    errorMessage.value = error instanceof Error ? error.message : "发送失败";
  }
}

watch(selectedSceneId, () => {
  selectedPointId.value = "";
  selectedInspectRobotId.value = "";
  showPointPanel.value = false;
  editingPoint.value = null;
  void refreshWorkspace();
});

function stopPolling(): void {
  if (pollTimer !== undefined) {
    window.clearInterval(pollTimer);
    pollTimer = undefined;
  }
}

function startPolling(): void {
  stopPolling();
  pollTimer = window.setInterval(() => {
    if (document.visibilityState !== "visible") {
      return;
    }
    if (showPointPanel.value) {
      return;
    }
    void refreshWorkspace().catch(() => undefined);
  }, 3000);
}

function onVisibilityChange(): void {
  if (document.visibilityState === "visible") {
    void refreshWorkspace().catch(() => undefined);
  }
}

function onDocumentPointerDown(event: PointerEvent): void {
  const target = event.target;
  if (
    pointContextMenu.value &&
    target instanceof Element &&
    !target.closest(".point-context-menu")
  ) {
    closePointContextMenu();
  }
}

function onDocumentKeyDown(event: KeyboardEvent): void {
  if (event.key !== "Escape") {
    return;
  }
  closePointContextMenu();
  showConfirm.value = false;
  if (toolMode.value === "place" && goalPixel.value) {
    goalPixel.value = null;
    goalWorld.value = null;
    statusMessage.value = "已取消本次点位，可重新单击地图或再按 Esc 退出";
    return;
  }
  if (toolMode.value !== "browse") {
    toolMode.value = "browse";
    goalPixel.value = null;
    goalWorld.value = null;
    statusMessage.value = "已退出地图工具";
  }
}

onMounted(() => {
  void bootstrap();
  startPolling();
  document.addEventListener("visibilitychange", onVisibilityChange);
  document.addEventListener("pointerdown", onDocumentPointerDown);
  document.addEventListener("keydown", onDocumentKeyDown);
});

onBeforeUnmount(() => {
  document.removeEventListener("visibilitychange", onVisibilityChange);
  document.removeEventListener("pointerdown", onDocumentPointerDown);
  document.removeEventListener("keydown", onDocumentKeyDown);
  stopPolling();
  controller?.abort();
  controller = undefined;
  showLogs.value = false;
  showPointPanel.value = false;
  editingPoint.value = null;
  pointContextMenu.value = null;
  inspectorHovered.value = false;
});
</script>

<template>
  <section class="ops-stage sparse">
    <header class="ops-topbar slim">
      <div class="ops-map-title">
        <strong>{{ activeScene?.name || "地图场景" }}</strong>
        <span v-if="activeMap">v{{ activeMap.version }} · {{ activeMap.width }}×{{ activeMap.height }}</span>
        <span v-else class="muted">未导入地图</span>
      </div>
      <div class="ops-tools">
        <label>
          场景
          <select v-model="selectedSceneId">
            <option disabled value="">选择场景</option>
            <option v-for="scene in scenes" :key="scene.id" :value="scene.id">
              {{ scene.name }}
            </option>
          </select>
        </label>
        <label v-if="mapVersions.length">
          地图
          <select
            v-model="selectedMapVersionId"
            @change="handleActiveMapChange"
          >
            <option v-for="map in mapVersions" :key="map.id" :value="map.id">
              v{{ map.version }} · {{ map.status }}
            </option>
          </select>
        </label>
        <button
          class="ghost-button"
          type="button"
          @click="showMore = !showMore"
        >
          {{ showMore ? "收起" : "更多" }}
        </button>
        <button
          class="icon-button"
          type="button"
          title="适配窗口"
          @click="mapCanvasRef?.fitToView()"
        >
          <Maximize2 :size="16" />
        </button>
      </div>
    </header>

    <div v-if="showMore" class="ops-more-bar">
      <section class="more-group more-scene-group">
        <span class="more-group-title">场景管理</span>
        <div class="more-group-controls">
          <label class="more-scene-name">
            <span class="sr-only">新建场景</span>
            <input v-model="newSceneName" placeholder="新建场景名称" type="text" />
          </label>
          <button class="primary-button" type="button" @click="handleCreateScene">创建</button>
          <button
            class="danger-button"
            type="button"
            :disabled="!selectedSceneId || loading"
            @click="handleDeleteScene"
          >
            删场景
          </button>
          <button
            class="danger-button"
            type="button"
            :disabled="!selectedMapVersionId || loading"
            @click="handleDeleteMap"
          >
            删地图
          </button>
        </div>
      </section>

      <section class="more-group more-import-group">
        <span class="more-group-title">地图文件</span>
        <div class="more-group-controls">
          <label class="file-picker">
            <span class="file-picker-kind">YAML</span>
            <span class="file-picker-name">{{ yamlFile?.name || "选择文件" }}</span>
            <input type="file" accept=".yaml,.yml,text/yaml" @change="onYamlSelected" />
          </label>
          <label class="file-picker">
            <span class="file-picker-kind">PGM</span>
            <span class="file-picker-name">{{ pgmFile?.name || "选择文件" }}</span>
            <input type="file" accept=".pgm,image/x-portable-graymap" @change="onPgmSelected" />
          </label>
          <button class="primary-button import-map-button" type="button" :disabled="loading" @click="handleImport">
            <Upload :size="15" />
            导入地图
          </button>
        </div>
      </section>

      <section class="more-group more-layer-group">
        <span class="more-group-title">图层</span>
        <div class="more-group-controls layer-controls">
          <label class="layer-chip"><input v-model="layerPoints" type="checkbox" />点位</label>
          <label class="layer-chip"><input v-model="layerRobots" type="checkbox" />机器人</label>
          <label class="layer-chip"><input v-model="layerLabels" type="checkbox" />标签</label>
          <label class="layer-chip"><input v-model="layerGrid" type="checkbox" />网格</label>
        </div>
      </section>
    </div>

    <div v-if="statusMessage || errorMessage" class="ops-banner">
      <p v-if="statusMessage" class="status-ok">{{ statusMessage }}</p>
      <p v-if="errorMessage" class="status-error">{{ errorMessage }}</p>
    </div>

    <div class="ops-body">
      <div class="ops-canvas-wrap">
        <MapCanvas
          ref="mapCanvasRef"
          :map="activeMap"
          :robots="sceneRobots"
          :points="mapPoints"
          :selected-point-id="selectedPointId"
          :selected-robot-id="selectedInspectRobotId"
          :draggable-point-id="draggablePointId"
          :goal-pixel="goalPixel"
          :interaction-mode="toolMode"
          :show-points="layerPoints"
          :show-robots="layerRobots"
          :show-labels="layerLabels"
          :layer-grid="layerGrid"
          @click-pixel="onClickPixel"
          @select-point="onSelectPoint"
          @open-point="openPointPanel"
          @context-point="onPointContextMenu"
          @move-point="onMovePoint"
          @select-robot="selectInspectRobot"
        />

        <div class="canvas-toolbar">
          <button
            type="button"
            class="tool-chip"
            :class="{ active: showBindPanel }"
            @click="openBindPanel"
          >
            <Link2 :size="15" />
            绑定机器人
          </button>
          <button
            type="button"
            class="tool-chip"
            :class="{ active: toolMode === 'place' }"
            :disabled="!activeMap"
            @click="setTool(toolMode === 'place' ? 'browse' : 'place')"
          >
            <MapPinPlus :size="15" />
            添加点位
          </button>
          <button
            type="button"
            class="tool-chip"
            :class="{ active: toolMode === 'navigate' }"
            :disabled="!activeMap || sceneRobots.length === 0"
            @click="setTool(toolMode === 'navigate' ? 'browse' : 'navigate')"
          >
            <Navigation :size="15" />
            导航
          </button>
          <button
            type="button"
            class="tool-chip"
            :class="{ active: showLogs }"
            @click="showLogs = !showLogs"
          >
            <ScrollText :size="15" />
            日志
          </button>
        </div>

        <p v-if="toolMode === 'place'" class="canvas-hint">单击地图选择位置，右侧确认后添加 · Esc 取消/退出</p>
        <p v-else-if="toolMode === 'navigate'" class="canvas-hint">单击地图选择导航目标</p>
        <p v-else-if="guideStep === 'bind'" class="canvas-hint">先绑定机器人到本场景，才能显示位姿并导航</p>
      </div>

      <div
        class="inspector-dock"
        :class="{ open: inspectorOpen, pinned: inspectorPinned }"
        title="移到右边缘查看详情"
        @mouseenter="onInspectorEnter"
        @mouseleave="onInspectorLeave"
      >
        <aside class="ops-inspector">
          <div class="inspector-edge-tab" aria-hidden="true">详情</div>
        <!-- Point edit (double-click) -->
        <template v-if="inspectorMode === 'pointEdit' && editingPoint">
          <div class="inspector-hero compact">
            <h2>
              {{
                pointEditMode === "position"
                  ? "修改位置"
                  : pointEditMode === "actions"
                    ? "配置动作"
                    : "完整配置"
              }}
              · {{ editingPoint.name }}
            </h2>
            <p v-if="pointEditMode === 'position'">
              改坐标或拖动该点；保存后关闭本栏并跳到该点
            </p>
            <p v-else-if="pointEditMode === 'actions'">
              选择能力模板，并按模板填写本点位的执行参数
            </p>
            <p v-else>改信息/动作，或拖动该点改位置；保存后关闭本栏</p>
            <div class="inspector-hero-actions">
              <button
                type="button"
                class="danger-button"
                @click="handleDeletePoint(editingPoint.id)"
              >
                删除点位
              </button>
              <button type="button" class="ghost-button" @click="closePointPanel">
                <X :size="14" /> 关闭
              </button>
            </div>
          </div>
          <div id="point-config-root" class="inspector-scroll inspector-form">
            <template v-if="pointEditMode !== 'actions'">
              <label>
                名称
                <input v-model="draftName" />
              </label>
              <div class="coord-grid">
                <label>
                  x (m)
                  <input v-model.number="draftX" type="number" step="0.01" />
                </label>
                <label>
                  y (m)
                  <input v-model.number="draftY" type="number" step="0.01" />
                </label>
                <label>
                  yaw (deg)
                  <input v-model.number="draftYaw" type="number" step="1" />
                </label>
              </div>
              <p class="hint-text">坐标为地图世界系；超出当前地图范围时不能保存</p>
              <label>
                备注
                <input v-model="draftNotes" />
              </label>
            </template>
            <div v-if="pointEditMode !== 'position'" class="inspector-section">
              <div class="section-head">
                <h3>动作列表</h3>
                <button
                  type="button"
                  class="primary-button"
                  @click="draftActions.push(makeEmptyAction(draftActions.length + 1))"
                >
                  + 动作
                </button>
              </div>
              <div
                v-for="(action, index) in draftActions"
                :key="action.id || `${action.sequence_no}-${index}`"
                class="action-card"
              >
                <header>
                  <span>#{{ index + 1 }} · {{ action.action_name || "未命名动作" }}</span>
                  <button
                    type="button"
                    class="compact-danger"
                    @click="draftActions.splice(index, 1)"
                  >
                    删除
                  </button>
                </header>
                <label>
                  动作名称
                  <input
                    v-model="action.action_name"
                    maxlength="80"
                    placeholder="例如：抓取料箱、打开舱门"
                  />
                </label>
                <label>
                  执行机器人
                  <select v-model="action.robot_id">
                    <option disabled value="">请选择</option>
                    <option v-for="robot in robots" :key="robot.id" :value="robot.id">
                      {{ robot.name }}
                    </option>
                  </select>
                </label>
                <label>
                  能力模板
                  <select
                    :value="action.capability_definition_id"
                    @change="
                      onCapabilitySelect(
                        action,
                        ($event.target as HTMLSelectElement).value
                      )
                    "
                  >
                    <option disabled value="">请选择模板</option>
                    <option
                      v-for="item in businessCapabilities"
                      :key="item.id"
                      :value="item.id"
                    >
                      {{ capabilityOptionLabel(item) }}
                    </option>
                  </select>
                </label>
                <p v-if="templateOfAction(action)" class="hint-text">
                  {{ templateOfAction(action)?.operation_kind }}
                  · {{ templateOfAction(action)?.endpoint_name }}
                  · {{ templateOfAction(action)?.ros_message_type || "无类型" }}
                </p>
                <label>
                  超时(ms)
                  <input v-model.number="action.timeout_ms" type="number" min="1" />
                </label>
                <div class="param-block">
                  <h4>执行参数（按模板）</h4>
                  <SchemaForm
                    :key="action.capability_definition_id || `action-${index}`"
                    :model-value="action.parameters"
                    :schema="templateOfAction(action)?.parameter_schema"
                    @update:model-value="action.parameters = $event"
                  />
                </div>
              </div>
            </div>
          </div>
          <div class="inspector-footer">
            <span v-if="panelError" class="status-error">{{ panelError }}</span>
            <button type="button" class="ghost-button" @click="closePointPanel">取消</button>
            <button
              type="button"
              class="primary-button"
              :disabled="savingPoint"
              @click="savePointPanel"
            >
              保存配置
            </button>
          </div>
        </template>

        <!-- Robot -->
        <template v-else-if="inspectorMode === 'robot' && selectedInspectRobot">
          <div class="inspector-hero compact">
            <h2>{{ selectedInspectRobot.name }}</h2>
            <p>{{ robotPoseHint(selectedInspectRobot) }}</p>
          </div>
          <div class="inspector-scroll">
            <dl class="inspector-kv">
              <div>
                <dt>连接</dt>
                <dd>{{ selectedInspectRobot.connection_state }}</dd>
              </div>
              <div>
                <dt>定位</dt>
                <dd>{{ selectedInspectRobot.localization_status }}</dd>
              </div>
              <div v-if="selectedInspectRobot.pose">
                <dt>位姿</dt>
                <dd>
                  {{ selectedInspectRobot.pose.x.toFixed(2) }},
                  {{ selectedInspectRobot.pose.y.toFixed(2) }}
                </dd>
              </div>
            </dl>
            <div class="inspector-hero-actions">
              <button
                type="button"
                class="danger-button"
                @click="handleUnbindRobot(selectedInspectRobot.id)"
              >
                解绑
              </button>
              <button type="button" class="ghost-button" @click="clearInspector">关闭</button>
            </div>
          </div>
        </template>

        <!-- Bind robots -->
        <template v-else-if="inspectorMode === 'bind'">
          <div class="inspector-hero compact">
            <h2>绑定机器人</h2>
            <p>勾选后保存 → 归属当前场景地图，才会订阅位姿并画在图上</p>
          </div>
          <div class="inspector-scroll">
            <label v-for="robot in robots" :key="robot.id" class="robot-check">
              <input v-model="selectedRobotIds" type="checkbox" :value="robot.id" />
              <span>
                {{ robot.name }}
                <small>
                  {{ robot.host || "无 host" }} · {{ robot.connection_state }} ·
                  {{ robot.pose_topic || "无 pose topic" }}
                </small>
              </span>
            </label>
            <p v-if="robots.length === 0" class="hint-text">
              还没有机器人，请先到「机器人」页配置 rosbridge。
            </p>
            <div class="inspector-hero-actions" style="margin-top: 12px">
              <button class="primary-button" type="button" @click="handleSaveRobots">
                保存绑定
              </button>
              <button
                class="ghost-button"
                type="button"
                @click="showBindPanel = false"
              >
                取消
              </button>
            </div>
          </div>
        </template>

        <!-- Guide / ready -->
        <template v-else>
          <div class="inspector-hero compact">
            <h2>{{ activeScene?.name || "场景作业" }}</h2>
            <p>
              <template v-if="guideStep === 'scene'">创建或选择场景</template>
              <template v-else-if="guideStep === 'map'">导入地图后开始</template>
              <template v-else-if="guideStep === 'bind'">绑定机器人显示位姿</template>
              <template v-else>添加点位 · 双击配置 · 看日志排查</template>
            </p>
          </div>
          <div class="inspector-scroll">
            <ol class="guide-steps">
              <li :class="{ done: !!selectedSceneId, current: guideStep === 'scene' }">
                选择场景
              </li>
              <li :class="{ done: !!activeMap, current: guideStep === 'map' }">
                导入 / 选择地图
              </li>
              <li :class="{ done: sceneRobots.length > 0, current: guideStep === 'bind' }">
                绑定机器人到场景
                <button
                  v-if="activeMap"
                  type="button"
                  class="linkish"
                  @click="openBindPanel"
                >
                  去绑定
                </button>
              </li>
              <li :class="{ done: mapPoints.length > 0, current: guideStep === 'ready' }">
                添加点位并双击配置
                <button
                  v-if="activeMap"
                  type="button"
                  class="linkish"
                  @click="setTool('place')"
                >
                  去添加
                </button>
              </li>
            </ol>

            <div v-if="sceneRobots.length" class="inspector-section">
              <h3>在场机器人</h3>
              <ul class="compact-list">
                <li v-for="robot in sceneRobots" :key="robot.id">
                  <button class="linkish" type="button" @click="selectInspectRobot(robot.id)">
                    {{ robot.name }}
                    <small>{{ robotPoseHint(robot) }}</small>
                  </button>
                  <button
                    type="button"
                    class="compact-danger"
                    @click.stop="handleUnbindRobot(robot.id)"
                  >
                    解绑
                  </button>
                </li>
              </ul>
            </div>

            <div v-if="mapPoints.length" class="inspector-section">
              <h3>点位</h3>
              <ul class="compact-list">
                <li
                  v-for="point in mapPoints"
                  :key="point.id"
                  :class="{ active: point.id === selectedPointId }"
                >
                  <button class="linkish" type="button" @click="onSelectPoint(point.id)">
                    {{ point.name }}
                    <small>{{ summarizeActions(point) }}</small>
                  </button>
                  <button
                    type="button"
                    class="compact-danger"
                    @click.stop="handleDeletePoint(point.id)"
                  >
                    删除
                  </button>
                </li>
              </ul>
            </div>

            <div v-if="toolMode === 'place'" class="inspector-section">
              <h3>添加点位</h3>
              <p v-if="!goalWorld" class="hint-text">先在地图上单击选择位置</p>
              <label class="inspector-form">
                名称
                <input v-model="pendingPointName" type="text" />
              </label>
              <label class="inspector-form">
                yaw (deg)
                <input v-model.number="yawDegrees" type="number" step="1" />
              </label>
              <p v-if="goalWorld" class="coord-readout">
                x={{ goalWorld.x.toFixed(3) }}, y={{ goalWorld.y.toFixed(3) }}
              </p>
              <button
                class="primary-button"
                type="button"
                :disabled="!goalWorld || creatingPoint"
                @click="confirmPlacePoint"
              >
                确认添加
              </button>
              <button
                class="ghost-button"
                type="button"
                :disabled="!goalWorld"
                @click="clearPlacementPreview"
              >
                取消本次
              </button>
            </div>
            <div v-if="toolMode === 'navigate'" class="inspector-section">
              <h3>导航</h3>
              <label class="inspector-form">
                机器人
                <select v-model="selectedNavRobotId">
                  <option disabled value="">选择</option>
                  <option v-for="robot in sceneRobots" :key="robot.id" :value="robot.id">
                    {{ robot.name }}
                  </option>
                </select>
              </label>
              <label class="inspector-form">
                yaw (deg)
                <input v-model.number="yawDegrees" type="number" step="1" />
              </label>
              <p v-if="goalWorld" class="coord-readout">
                目标 x={{ goalWorld.x.toFixed(3) }}, y={{ goalWorld.y.toFixed(3) }}
              </p>
              <button class="primary-button" type="button" @click="openConfirm">
                确认发送导航
              </button>
            </div>
          </div>
        </template>
        </aside>
      </div>
    </div>

    <OpsLogPanel :open="showLogs" compact @close="showLogs = false" />

    <Teleport to="body">
      <div
        v-if="pointContextMenu"
        class="point-context-menu"
        :style="pointContextStyle"
        role="menu"
        aria-label="点位操作"
        @contextmenu.prevent
        @pointerdown.stop
      >
        <strong>{{ selectedPoint?.name || "点位" }}</strong>
        <button
          type="button"
          role="menuitem"
          @click="openPointPanel(pointContextMenu.pointId, 'position')"
        >
          修改位置
        </button>
        <button
          type="button"
          role="menuitem"
          @click="openPointPanel(pointContextMenu.pointId, 'actions')"
        >
          配置动作
        </button>
        <button
          type="button"
          role="menuitem"
          @click="publishPointNavigation(pointContextMenu.pointId)"
        >
          发布导航
        </button>
      </div>
    </Teleport>

    <div v-if="showConfirm" class="confirm-mask">
      <div class="confirm-dialog">
        <h3>确认发送导航目标</h3>
        <label class="confirm-field">
          机器人
          <select v-model="selectedNavRobotId">
            <option disabled value="">请选择机器人</option>
            <option v-for="robot in sceneRobots" :key="robot.id" :value="robot.id">
              {{ robot.name }} · {{ robot.connection_state }}
            </option>
          </select>
        </label>
        <p>地图：{{ activeScene?.name }} / v{{ activeMap?.version }}</p>
        <p>
          目标：x={{ goalWorld?.x.toFixed(3) }}, y={{ goalWorld?.y.toFixed(3) }},
          yaw={{ yawDegrees }}°
        </p>
        <div class="confirm-tolerance-grid">
          <label class="confirm-field">
            距离容差 (m)
            <input
              v-model.number="distanceTolerance"
              type="number"
              min="0.001"
              step="0.01"
            />
          </label>
          <label class="confirm-field">
            朝向容差 (rad)
            <input
              v-model.number="headingTolerance"
              type="number"
              min="0.001"
              step="0.01"
            />
          </label>
        </div>
        <div class="confirm-actions">
          <button type="button" class="ghost-button" @click="showConfirm = false">取消</button>
          <button
            class="primary-button"
            type="button"
            :disabled="!selectedNavRobotId"
            @click="confirmNavigate"
          >
            确认发送
          </button>
        </div>
      </div>
    </div>
  </section>
</template>
