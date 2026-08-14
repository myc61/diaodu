<script setup lang="ts">
import {
  NAlert,
  NButton,
  NCard,
  NForm,
  NFormItem,
  NInput,
  NInputNumber,
  NSelect,
  NSpace,
  NTag
} from "naive-ui";
import { computed, onMounted, reactive, ref, watch } from "vue";

import {
  createCapabilityTemplate,
  deleteCapabilityTemplate,
  listCapabilityProfiles,
  listCapabilityTemplates,
  testCapabilityTemplate,
  updateCapabilityTemplate,
  type CapabilityProfile,
  type CapabilityTestResult,
  type CapabilityUpsertPayload
} from "../api/capabilities";
import {
  getRobotInterfaces,
  listRobotConfigs,
  listRobotStartupProfiles,
  resolveRobotInterfaceSchema,
  scanRobotInterfaces,
  type RobotInterfaceCatalog
} from "../api/robots";
import SchemaForm from "../components/SchemaForm.vue";
import type { RobotConfig } from "../types/robot";
import type { RobotStartupProfile } from "../types/workflow";
import type { CapabilityTemplate, EventSpec } from "../types/workspace";

type EventSpecForm = {
  event_name: string;
  source: "RESULT" | "FEEDBACK";
  enabled: boolean;
  field: string;
  op: string;
  valueText: string;
  max_firings: number;
  cooldown_ms: number;
  emit_on_node: boolean;
  start_workflows: boolean;
};

const items = ref<CapabilityTemplate[]>([]);
const profiles = ref<CapabilityProfile[]>([]);
const robots = ref<RobotConfig[]>([]);
const startupProfiles = ref<RobotStartupProfile[]>([]);
const selectedId = ref("");
const isCreating = ref(false);
const loading = ref(false);
const testing = ref(false);
const scanning = ref(false);
const showAdvanced = ref(false);
const statusMessage = ref("");
const errorMessage = ref("");
const testRobotId = ref("");
const scanRobotId = ref("");
const interfaceCatalog = ref<RobotInterfaceCatalog | null>(null);
const testParameters = ref<Record<string, unknown>>({});
const testResult = ref<CapabilityTestResult | null>(null);
const schemaText = ref(
  '{\n  "type": "object",\n  "properties": {}\n}'
);

const form = reactive<CapabilityUpsertPayload>({
  profile_id: "",
  capability_key: "",
  operation_kind: "SERVICE",
  endpoint_name: "",
  ros_message_type: "",
  motion_ownership: "ROBOT_INTERNAL",
  blocking_type: "NONE",
  timeout_ms: 30000,
  parameter_schema: { type: "object", properties: {} },
  request_template: {},
  feedback_mapping: {},
  result_mapping: {},
  success_condition: null,
  failure_condition: null,
  retry_policy: {},
  cancel_policy: {},
  resource_claims: [],
  protocol_config: { adapter: "ROS1_SERVICE" },
  event_specs: []
});

const eventSpecs = ref<EventSpecForm[]>([]);

const isSystemNavigation = computed(
  () => form.capability_key.trim() === "navigation"
);

const eventSourceOptions = computed(() => {
  if (form.operation_kind === "ACTION") {
    return [
      { label: "RESULT（最终结果）", value: "RESULT" },
      { label: "FEEDBACK（过程反馈）", value: "FEEDBACK" }
    ];
  }
  if (form.operation_kind === "SERVICE") {
    return [{ label: "RESULT（服务响应）", value: "RESULT" }];
  }
  if (form.operation_kind === "SSH") {
    return [{ label: "RESULT（执行结果）", value: "RESULT" }];
  }
  return [{ label: "RESULT（发布结果）", value: "RESULT" }];
});

const eventSourceHint = computed(() => {
  if (form.operation_kind === "ACTION") {
    return "Action 可根据过程反馈或最终结果产生事件。";
  }
  if (form.operation_kind === "SERVICE") {
    return "Service 只有一次响应，不存在 Feedback。";
  }
  if (form.operation_kind === "SSH") {
    return "SSH 执行完成或 readiness 通过后产生 RESULT 事件。";
  }
  return "这里表示 Topic 发布结果；订阅 Topic 消息属于独立事件源。";
});

const editSubtitle = computed(() => {
  if (isSystemNavigation.value) {
    return "系统导航能力：用于编排中的导航任务";
  }
  if (form.operation_kind === "SSH") {
    return "受控 SSH 能力：选择绑定机器人与 SSH 执行方案，执行已保存的脚本或结构化命令";
  }
  return "选扫描结果中的服务/Action/Topic 后，会按 rosapi 请求定义自动生成执行参数 Schema";
});

const eventOpOptions = [
  { label: "ROS 调用成功", value: "ros_success" },
  { label: "等于", value: "eq" },
  { label: "不等于", value: "neq" },
  { label: "大于", value: "gt" },
  { label: "大于等于", value: "gte" },
  { label: "小于", value: "lt" },
  { label: "小于等于", value: "lte" },
  { label: "包含", value: "contains" },
  { label: "字段存在", value: "exists" },
  { label: "值为真", value: "truthy" }
];

function emptyEventSpec(): EventSpecForm {
  return {
    event_name: "",
    source: "RESULT",
    enabled: true,
    field: "",
    op: "ros_success",
    valueText: "",
    max_firings: 1,
    cooldown_ms: 0,
    emit_on_node: true,
    start_workflows: true
  };
}

function parseValueText(text: string): unknown {
  const trimmed = text.trim();
  if (!trimmed) {
    return null;
  }
  try {
    return JSON.parse(trimmed) as unknown;
  } catch {
    return trimmed;
  }
}

function toEventSpecPayload(): EventSpec[] {
  return eventSpecs.value
    .filter((item) => item.event_name.trim())
    .map((item) => {
      const when: EventSpec["when"] = { op: item.op };
      if (item.field.trim()) {
        when.field = item.field.trim();
      }
      if (
        item.op !== "ros_success" &&
        item.op !== "exists" &&
        item.op !== "truthy"
      ) {
        when.value = parseValueText(item.valueText);
      }
      return {
        event_name: item.event_name.trim(),
        source: item.source,
        enabled: item.enabled,
        when,
        max_firings: item.max_firings,
        cooldown_ms: item.cooldown_ms,
        emit_on_node: item.emit_on_node,
        start_workflows: item.start_workflows
      };
    });
}

function loadEventSpecs(specs: EventSpec[] | undefined): void {
  if (!Array.isArray(specs) || specs.length === 0) {
    eventSpecs.value = [];
    return;
  }
  eventSpecs.value = specs.map((item) => ({
    event_name: item.event_name ?? "",
    source: item.source === "FEEDBACK" ? "FEEDBACK" : "RESULT",
    enabled: item.enabled !== false,
    field: item.when?.field ?? "",
    op: item.when?.op ?? "ros_success",
    valueText:
      item.when?.value === undefined || item.when?.value === null
        ? ""
        : typeof item.when.value === "string"
          ? item.when.value
          : JSON.stringify(item.when.value),
    max_firings: item.max_firings ?? 1,
    cooldown_ms: item.cooldown_ms ?? 0,
    emit_on_node: item.emit_on_node !== false,
    start_workflows: item.start_workflows !== false
  }));
}

const kindOptions = [
  { label: "调用 Service", value: "SERVICE" },
  { label: "调用 Action", value: "ACTION" },
  { label: "发布 Topic", value: "TOPIC" },
  { label: "受控 SSH", value: "SSH" }
];

const profileOptions = computed(() =>
  profiles.value.map((item) => ({ label: item.name, value: item.id }))
);

const robotOptions = computed(() =>
  robots.value.map((item) => ({
    label: `${item.name} · ${item.connection_state}`,
    value: item.id
  }))
);

const scanRobotOptions = computed(() =>
  robots.value.map((item) => ({
    label: `${item.name} · ${item.connection_state}`,
    value: item.id
  }))
);

const sshRobotId = computed({
  get: () => String(form.protocol_config?.ssh_robot_id ?? ""),
  set: (value: string) => {
    form.protocol_config = {
      ...(form.protocol_config ?? {}),
      adapter: "CONTROLLED_SSH",
      ssh_robot_id: value,
      ssh_profile_id: ""
    };
    form.endpoint_name = "";
    if (value) {
      testRobotId.value = value;
    }
  }
});

const sshProfileId = computed({
  get: () => String(form.protocol_config?.ssh_profile_id ?? ""),
  set: (value: string) => {
    const profile = startupProfiles.value.find((item) => item.id === value);
    form.protocol_config = {
      ...(form.protocol_config ?? {}),
      adapter: "CONTROLLED_SSH",
      ssh_profile_id: value
    };
    form.endpoint_name = profile?.name ?? "";
  }
});

const sshProfileOptions = computed(() =>
  startupProfiles.value
    .filter((item) => item.enabled && item.robot_id === sshRobotId.value)
    .map((item) => ({
      label: `${item.name} · v${item.version}`,
      value: item.id
    }))
);

const endpointOptions = computed(() => {
  const catalog = interfaceCatalog.value;
  if (!catalog) {
    return [] as Array<{ label: string; value: string; type: string }>;
  }
  const kind = form.operation_kind;
  const list =
    kind === "ACTION"
      ? catalog.actions
      : kind === "TOPIC"
        ? catalog.topics
        : catalog.services;
  return list
    .filter((item) => item.name)
    .map((item) => ({
      label: item.message_type
        ? `${item.name}  ·  ${item.message_type}`
        : item.name,
      value: item.name,
      type: item.message_type || ""
    }));
});

const scanSummary = computed(() => {
  const catalog = interfaceCatalog.value;
  if (!catalog) {
    return "尚未扫描；连上 rosbridge 后会自动缓存，也可手动扫描";
  }
  const counts = catalog.counts ?? {
    topics: catalog.topics.length,
    services: catalog.services.length,
    actions: catalog.actions.length
  };
  const when = catalog.scanned_at || "未知时间";
  const source = catalog.live
    ? "实时完整"
    : catalog.error
      ? "部分实时 / 失败项沿用缓存"
      : "缓存";
  return `${source} · ${when} · topic ${counts.topics} / service ${counts.services} / action ${counts.actions}`;
});

const endpointPlaceholder = computed(() => {
  if (form.operation_kind === "SSH") {
    return "选择 SSH 执行方案";
  }
  if (form.operation_kind === "ACTION") {
    return "/zj_humanoid/navigation/navigation";
  }
  if (form.operation_kind === "TOPIC") {
    return "/some/topic";
  }
  return "/zj_humanoid/task/pick_and_place";
});

const rosTypePlaceholder = computed(() => {
  if (form.operation_kind === "SSH") {
    return "SSH";
  }
  if (form.operation_kind === "ACTION") {
    return "navigation/NavigationAction";
  }
  if (form.operation_kind === "TOPIC") {
    return "std_msgs/String";
  }
  return "std_srvs/Trigger";
});

function adapterForKind(kind: string): string {
  if (kind === "SSH") {
    return "CONTROLLED_SSH";
  }
  if (kind === "ACTION") {
    return "ROS1_ACTIONLIB";
  }
  if (kind === "TOPIC") {
    return "ROS1_TOPIC";
  }
  return "ROS1_SERVICE";
}

function applyKindDefaults(kind: string, forceTypeHint: boolean): void {
  form.protocol_config = { adapter: adapterForKind(kind) };
  // Business robot capabilities: just call ROS; navigation is a separate system capability.
  if (!isSystemNavigation.value) {
    form.motion_ownership = "ROBOT_INTERNAL";
    form.blocking_type = "NONE";
  }
  if (forceTypeHint && !form.ros_message_type.trim()) {
    form.ros_message_type = String(rosTypePlaceholder.value);
  }
  if (kind === "SSH") {
    form.ros_message_type = "SSH";
    form.endpoint_name = "";
    form.parameter_schema = { type: "object", properties: {} };
    form.request_template = {};
    schemaText.value = JSON.stringify(form.parameter_schema, null, 2);
  }
}

function parseJsonObject(text: string, label: string): Record<string, unknown> {
  const value = JSON.parse(text || "{}") as unknown;
  if (value === null || typeof value !== "object" || Array.isArray(value)) {
    throw new Error(`${label} 必须是 JSON 对象`);
  }
  return value as Record<string, unknown>;
}

function resetForm(): void {
  form.profile_id = profiles.value[0]?.id ?? "";
  form.capability_key = "";
  form.operation_kind = "SERVICE";
  form.endpoint_name = "";
  form.ros_message_type = "std_srvs/Trigger";
  form.motion_ownership = "ROBOT_INTERNAL";
  form.blocking_type = "NONE";
  form.timeout_ms = 30000;
  form.parameter_schema = { type: "object", properties: {} };
  form.request_template = {};
  form.feedback_mapping = {};
  form.result_mapping = {};
  form.success_condition = null;
  form.failure_condition = null;
  form.retry_policy = {};
  form.cancel_policy = {};
  form.resource_claims = [];
  form.protocol_config = { adapter: "ROS1_SERVICE" };
  form.event_specs = [];
  eventSpecs.value = [];
  schemaText.value = '{\n  "type": "object",\n  "properties": {}\n}';
  showAdvanced.value = false;
  testParameters.value = {};
  testResult.value = null;
}

function fillForm(item: CapabilityTemplate): void {
  form.profile_id = item.profile_id;
  form.capability_key = item.capability_key;
  form.operation_kind = item.operation_kind;
  form.endpoint_name = item.endpoint_name;
  form.ros_message_type = item.ros_message_type;
  form.motion_ownership = item.motion_ownership;
  form.blocking_type = item.blocking_type;
  form.timeout_ms = item.timeout_ms;
  form.parameter_schema = item.parameter_schema ?? {
    type: "object",
    properties: {}
  };
  form.request_template = item.request_template ?? {};
  form.feedback_mapping = item.feedback_mapping ?? {};
  form.result_mapping = item.result_mapping ?? {};
  form.success_condition = item.success_condition;
  form.failure_condition = item.failure_condition;
  form.retry_policy = item.retry_policy ?? {};
  form.cancel_policy = item.cancel_policy ?? {};
  form.resource_claims = item.resource_claims ?? [];
  form.protocol_config = item.protocol_config?.adapter
    ? item.protocol_config
    : { adapter: adapterForKind(item.operation_kind), ...(item.protocol_config ?? {}) };
  if (form.operation_kind === "SSH") {
    testRobotId.value = String(form.protocol_config?.ssh_robot_id ?? "");
  }
  form.event_specs = item.event_specs ?? [];
  loadEventSpecs(item.event_specs);
  schemaText.value = JSON.stringify(form.parameter_schema, null, 2);
  showAdvanced.value = false;
  testParameters.value = {};
  testResult.value = null;
}

watch(
  () => form.operation_kind,
  (kind) => {
    applyKindDefaults(kind, isCreating.value);
    if (kind !== "ACTION") {
      for (const spec of eventSpecs.value) {
        spec.source = "RESULT";
      }
    }
  }
);

watch(schemaText, (text) => {
  try {
    form.parameter_schema = parseJsonObject(text, "参数 Schema");
  } catch {
    // Ignore while the user is still editing JSON.
  }
});

watch(scanRobotId, (id) => {
  if (id) {
    void loadInterfaceCatalog(false);
  } else {
    interfaceCatalog.value = null;
  }
});

async function loadInterfaceCatalog(live: boolean): Promise<void> {
  if (!scanRobotId.value) {
    return;
  }
  scanning.value = live;
  errorMessage.value = "";
  try {
    const catalog = live
      ? await scanRobotInterfaces(scanRobotId.value)
      : await getRobotInterfaces(scanRobotId.value);
    interfaceCatalog.value = catalog;
    if (live) {
      if (catalog.error) {
        errorMessage.value = `部分扫描完成：${catalog.error}`;
        statusMessage.value = "成功类别已刷新，失败类别继续使用上次缓存";
      } else {
        statusMessage.value = `已扫描并缓存到系统（${scanSummary.value}）`;
      }
    }
  } catch (error) {
    if (live) {
      errorMessage.value =
        error instanceof Error ? error.message : "扫描失败";
    } else {
      interfaceCatalog.value = null;
    }
  } finally {
    scanning.value = false;
  }
}

const schemaGenerating = ref(false);
const showSchemaJson = ref(false);

async function generateParamsFromEndpoint(
  name: string | null | undefined,
  resetEndpoint = false
): Promise<void> {
  if (!name) {
    return;
  }
  const option = endpointOptions.value.find((item) => item.value === name);
  if (resetEndpoint) {
    form.ros_message_type = option?.type ?? "";
  }

  // Never retain another endpoint's request fields after the selected name
  // changes or a regeneration fails.
  form.parameter_schema = { type: "object", properties: {} };
  schemaText.value = JSON.stringify(form.parameter_schema, null, 2);
  form.request_template = {};
  testParameters.value = {};

  if (!scanRobotId.value) {
    statusMessage.value =
      "已填写名称；请先选择扫描来源机器人，才能自动生成可填参数";
    return;
  }
  const robot = robots.value.find((item) => item.id === scanRobotId.value);
  if (!robot || robot.connection_state !== "ONLINE") {
    statusMessage.value =
      "机器人离线：无法生成参数表单；上线后点「按接口重新生成参数」";
    return;
  }

  schemaGenerating.value = true;
  errorMessage.value = "";
  try {
    const resolved = await resolveRobotInterfaceSchema(
      scanRobotId.value,
      form.operation_kind,
      name,
      { type: option?.type || form.ros_message_type || undefined }
    );
    if (resolved.endpoint_name && resolved.endpoint_name !== name) {
      form.endpoint_name = resolved.endpoint_name;
    }
    if (resolved.message_type) {
      form.ros_message_type = resolved.message_type;
    }
    const properties =
      resolved.parameter_schema &&
      typeof resolved.parameter_schema === "object" &&
      !Array.isArray(resolved.parameter_schema)
        ? ((resolved.parameter_schema as { properties?: Record<string, unknown> })
            .properties ?? {})
        : {};
    if (Object.keys(properties).length === 0) {
      if (resolved.empty_request && (resolved.typedef_count ?? 0) > 0) {
        form.parameter_schema = resolved.parameter_schema;
        schemaText.value = JSON.stringify(resolved.parameter_schema, null, 2);
        statusMessage.value = `已读取 ${resolved.message_type || name}：该接口请求为空，执行参数为 {}`;
        return;
      }
      errorMessage.value =
        resolved.error ||
        `rosapi 未返回请求定义（类型 ${resolved.message_type || "未知"}，typedefs=${resolved.typedef_count ?? 0}）；请确认接口类型或重新扫描`;
      return;
    }
    form.parameter_schema = resolved.parameter_schema;
    schemaText.value = JSON.stringify(resolved.parameter_schema, null, 2);
    // Empty template → point/test parameters are sent as the service request body.
    form.request_template = {};
    testParameters.value = {
      ...(resolved.request_defaults ?? {})
    };
    const canonical = resolved.endpoint_name || name;
    statusMessage.value = `已读取 ${resolved.message_type || canonical}，生成 ${Object.keys(properties).length} 个可填参数（与 rosbridge 工具同类）`;
  } catch (error) {
    errorMessage.value =
      error instanceof Error
        ? `生成参数失败：${error.message}`
        : "生成参数失败";
  } finally {
    schemaGenerating.value = false;
  }
}

async function onEndpointPicked(name: string | null): Promise<void> {
  await generateParamsFromEndpoint(name, true);
}

async function refresh(): Promise<void> {
  loading.value = true;
  errorMessage.value = "";
  try {
    const [capResult, profileResult, robotResult] = await Promise.all([
      listCapabilityTemplates(),
      listCapabilityProfiles(),
      listRobotConfigs()
    ]);
    items.value = capResult.items;
    profiles.value = profileResult.items;
    robots.value = robotResult.items;
    const startupResults = await Promise.all(
      robots.value.map((robot) => listRobotStartupProfiles(robot.id))
    );
    startupProfiles.value = startupResults.flatMap((result) => result.items);
    if (!testRobotId.value && robots.value.length > 0) {
      const online = robots.value.find((item) => item.connection_state === "ONLINE");
      testRobotId.value = (online ?? robots.value[0]).id;
    }
    if (!scanRobotId.value && robots.value.length > 0) {
      const online = robots.value.find((item) => item.connection_state === "ONLINE");
      scanRobotId.value = (online ?? robots.value[0]).id;
    } else if (scanRobotId.value) {
      void loadInterfaceCatalog(false);
    }
    if (!isCreating.value) {
      if (
        selectedId.value &&
        items.value.some((item) => item.id === selectedId.value)
      ) {
        fillForm(items.value.find((item) => item.id === selectedId.value)!);
      } else if (items.value.length > 0) {
        selectedId.value = items.value[0].id;
        fillForm(items.value[0]);
      } else {
        selectedId.value = "";
        isCreating.value = true;
        resetForm();
      }
    } else if (!form.profile_id && profiles.value[0]) {
      form.profile_id = profiles.value[0].id;
    }
  } catch (error) {
    errorMessage.value = error instanceof Error ? error.message : "加载失败";
  } finally {
    loading.value = false;
  }
}

function startCreate(): void {
  isCreating.value = true;
  selectedId.value = "";
  resetForm();
  statusMessage.value =
    "选择调用方式与名称，配置参数后保存；可在右侧对机器人做隔离测试";
  errorMessage.value = "";
}

function selectItem(id: string): void {
  isCreating.value = false;
  selectedId.value = id;
  const item = items.value.find((entry) => entry.id === id);
  if (item) {
    fillForm(item);
  }
  statusMessage.value = "";
  errorMessage.value = "";
}

function buildPayload(): CapabilityUpsertPayload {
  const kind = form.operation_kind;
  const key = form.capability_key.trim();
  const isNav = key === "navigation";
  return {
    ...form,
    capability_key: key,
    endpoint_name:
      kind === "SSH" ? form.endpoint_name.trim() || "SSH" : form.endpoint_name.trim(),
    ros_message_type: kind === "SSH" ? "SSH" : form.ros_message_type.trim(),
    operation_kind: kind,
    // Hidden from UI: business robot caps don't auto-insert navigation.
    motion_ownership: isNav ? "DISPATCHER" : "ROBOT_INTERNAL",
    blocking_type: isNav ? "NAVIGATION" : form.blocking_type || "NONE",
    parameter_schema: parseJsonObject(schemaText.value, "参数 Schema"),
    request_template: form.request_template ?? {},
    protocol_config: {
      ...(form.protocol_config ?? {}),
      adapter: adapterForKind(kind)
    },
    event_specs: toEventSpecPayload()
  };
}

async function save(): Promise<void> {
  if (!form.capability_key.trim() ||
      (form.operation_kind !== "SSH" && !form.endpoint_name.trim())) {
    errorMessage.value = "动作名与调用名称不能为空";
    return;
  }
  if (!form.profile_id) {
    errorMessage.value = "请选择能力档案";
    return;
  }
  if (
    (form.operation_kind === "SERVICE" || form.operation_kind === "ACTION") &&
    !form.ros_message_type.trim()
  ) {
    errorMessage.value = "SERVICE / ACTION 需要填写 ROS 类型";
    return;
  }
  if (form.operation_kind === "SSH" &&
      (!sshRobotId.value || !sshProfileId.value)) {
    errorMessage.value = "SSH 能力需要选择机器人和 SSH 执行方案";
    return;
  }
  loading.value = true;
  errorMessage.value = "";
  try {
    const payload = buildPayload();
    Object.assign(form, payload);
    const saved = isCreating.value
      ? await createCapabilityTemplate(payload)
      : await updateCapabilityTemplate(selectedId.value, payload);
    isCreating.value = false;
    selectedId.value = saved.id;
    await refresh();
    statusMessage.value = `已保存 ${saved.capability_key}（${saved.operation_kind}）`;
  } catch (error) {
    errorMessage.value = error instanceof Error ? error.message : "保存失败";
  } finally {
    loading.value = false;
  }
}

async function removeItem(): Promise<void> {
  if (isCreating.value || !selectedId.value) {
    return;
  }
  if (isSystemNavigation.value) {
    errorMessage.value = "系统导航能力不建议删除；编排导航依赖它";
    return;
  }
  if (!window.confirm(`确认删除能力 ${form.capability_key}？`)) {
    return;
  }
  loading.value = true;
  errorMessage.value = "";
  try {
    await deleteCapabilityTemplate(selectedId.value);
    selectedId.value = "";
    isCreating.value = false;
    await refresh();
    statusMessage.value = "能力已删除";
  } catch (error) {
    errorMessage.value = error instanceof Error ? error.message : "删除失败";
  } finally {
    loading.value = false;
  }
}

async function runTest(): Promise<void> {
  if (isCreating.value || !selectedId.value) {
    errorMessage.value = "请先保存能力再测试";
    return;
  }
  if (!testRobotId.value) {
    errorMessage.value = "请选择测试机器人";
    return;
  }
  if (
    form.operation_kind === "SSH" &&
    testRobotId.value !== sshRobotId.value
  ) {
    errorMessage.value = "SSH 能力只能对绑定机器人执行受控方案";
    return;
  }
  testing.value = true;
  errorMessage.value = "";
  testResult.value = null;
  try {
    // Keep schema in sync for the test form preview.
    form.parameter_schema = parseJsonObject(schemaText.value, "参数 Schema");
    testResult.value = await testCapabilityTemplate(selectedId.value, {
      robot_id: testRobotId.value,
      parameters: testParameters.value
    });
    statusMessage.value = testResult.value.success
      ? `测试成功（${testResult.value.elapsed_ms ?? "-"} ms）`
      : `测试失败：${testResult.value.error || "未知错误"}`;
  } catch (error) {
    errorMessage.value = error instanceof Error ? error.message : "测试失败";
  } finally {
    testing.value = false;
  }
}

onMounted(() => {
  void refresh();
});
</script>

<template>
  <section class="capability-page">
    <n-card class="list-panel" size="small">
      <template #header>
        <div class="panel-title-row">
          <div>
            <h2>机器人能力</h2>
            <span>配置动作名与调用方式（rosbridge / 受控 SSH），供地图点位与流程引用</span>
          </div>
          <n-button type="primary" size="small" @click="startCreate">
            新建
          </n-button>
        </div>
      </template>

      <n-space vertical :size="8">
        <button
          v-for="item in items"
          :key="item.id"
          class="list-item"
          :class="{ active: !isCreating && selectedId === item.id }"
          type="button"
          @click="selectItem(item.id)"
        >
          <strong>
            {{ item.capability_key }}
            <n-tag
              v-if="item.capability_key === 'navigation'"
              size="tiny"
              type="warning"
              :bordered="false"
              style="margin-left: 6px"
            >
              系统导航
            </n-tag>
          </strong>
          <n-space :size="6" align="center">
            <n-tag size="tiny" :bordered="false">{{ item.operation_kind }}</n-tag>
            <small>{{ item.endpoint_name }}</small>
          </n-space>
        </button>
        <p v-if="items.length === 0" class="empty-hint">暂无能力，请新建</p>
      </n-space>
    </n-card>

    <div class="main-panels">
      <n-card size="small">
        <template #header>
          <div class="panel-title-row">
            <div>
              <h2>{{ isCreating ? "新建能力" : "编辑能力" }}</h2>
              <span>
                {{ editSubtitle }}
              </span>
            </div>
            <n-space>
              <n-button
                v-if="!isCreating && !isSystemNavigation"
                type="error"
                secondary
                :disabled="loading"
                @click="removeItem"
              >
                删除
              </n-button>
              <n-button type="primary" :loading="loading" @click="save">
                保存
              </n-button>
            </n-space>
          </div>
        </template>

        <n-alert
          v-if="isSystemNavigation"
          type="info"
          :bordered="false"
          style="margin-bottom: 12px"
        >
          导航任务请用本能力；业务动作请另建 SERVICE / ACTION / TOPIC
          能力，不必再选「运动归属」。
        </n-alert>

        <n-alert
          v-if="form.operation_kind !== 'SSH'"
          type="default"
          :bordered="false"
          style="margin-bottom: 12px"
        >
          <div class="scan-row">
            <n-form-item label="扫描来源机器人（rosbridge）" class="scan-robot">
              <n-select
                v-model:value="scanRobotId"
                :options="scanRobotOptions"
                placeholder="选择机器人"
              />
            </n-form-item>
            <n-button
              type="primary"
              secondary
              :loading="scanning"
              :disabled="!scanRobotId"
              @click="loadInterfaceCatalog(true)"
            >
              扫描接口
            </n-button>
          </div>
          <p class="hint">{{ scanSummary }}</p>
          <p class="hint">
            能力保存后落库，与扫描缓存无关；机器人下线后已保存的能力仍可被点位/流程引用。
            设备侧 MQTT/HTTP 发现接口已预留，当前仅机器人 rosbridge。
          </p>
        </n-alert>

        <n-form label-placement="top">
          <div class="form-grid">
            <n-form-item label="动作名（capability_key）" required>
              <n-input
                v-model:value="form.capability_key"
                :disabled="isSystemNavigation && !isCreating"
                placeholder="pick_box"
              />
            </n-form-item>
            <n-form-item label="调用方式" required>
              <n-select
                v-model:value="form.operation_kind"
                :options="kindOptions"
                :disabled="isSystemNavigation"
              />
            </n-form-item>
            <n-form-item
              v-if="form.operation_kind !== 'SSH'"
              :label="
                form.operation_kind === 'ACTION'
                  ? 'Action 名'
                  : form.operation_kind === 'TOPIC'
                    ? 'Topic 名'
                    : 'Service 名'
              "
              required
            >
              <n-select
                v-model:value="form.endpoint_name"
                filterable
                tag
                clearable
                :options="endpointOptions"
                :placeholder="endpointPlaceholder"
                @update:value="onEndpointPicked"
              />
            </n-form-item>
            <n-form-item
              v-if="form.operation_kind !== 'SSH'"
              :label="
                form.operation_kind === 'TOPIC' ? '消息类型（可选）' : 'ROS 类型'
              "
              :required="form.operation_kind !== 'TOPIC'"
            >
              <n-input
                v-model:value="form.ros_message_type"
                :placeholder="rosTypePlaceholder"
              />
            </n-form-item>
            <n-form-item
              v-if="form.operation_kind === 'SSH'"
              label="绑定机器人"
              required
            >
              <n-select
                v-model:value="sshRobotId"
                :options="robotOptions"
                placeholder="选择配置了 IP 的机器人"
              />
            </n-form-item>
            <n-form-item
              v-if="form.operation_kind === 'SSH'"
              label="SSH 执行方案"
              required
            >
              <n-select
                v-model:value="sshProfileId"
                :options="sshProfileOptions"
                :disabled="!sshRobotId"
                placeholder="选择该机器人的脚本/命令方案"
              />
            </n-form-item>
            <n-form-item label="超时 (ms)">
              <n-input-number
                v-model:value="form.timeout_ms"
                :min="100"
                class="full-width"
              />
            </n-form-item>
            <n-form-item label="能力档案">
              <n-select v-model:value="form.profile_id" :options="profileOptions" />
            </n-form-item>
          </div>

          <div v-if="form.operation_kind !== 'SSH'" class="param-preview-block">
            <div class="panel-title-row">
              <div>
                <h3 class="section-title">执行参数（选接口后自动生成）</h3>
                <p class="hint">
                  从 rosapi 读取请求字段，地图点位/在线测试按此表单填写即可；复合字段用 JSON。
                </p>
              </div>
              <n-button
                size="small"
                secondary
                :loading="schemaGenerating"
                :disabled="!form.endpoint_name || !scanRobotId"
                @click="generateParamsFromEndpoint(form.endpoint_name)"
              >
                按接口重新生成参数
              </n-button>
            </div>
            <SchemaForm
              :model-value="testParameters"
              :schema="form.parameter_schema"
              @update:model-value="testParameters = $event"
            />
            <n-button
              text
              type="primary"
              style="margin-top: 8px"
              @click="showSchemaJson = !showSchemaJson"
            >
              {{ showSchemaJson ? "收起 Schema JSON" : "高级：编辑 Schema JSON" }}
            </n-button>
            <n-input
              v-if="showSchemaJson"
              v-model:value="schemaText"
              type="textarea"
              :autosize="{ minRows: 6, maxRows: 14 }"
              style="margin-top: 8px"
              placeholder='{"type":"object","properties":{}}'
            />
          </div>

          <div class="event-specs-block">
            <div class="panel-title-row">
              <div>
                <h3 class="section-title">事件定义（可编辑）</h3>
                <p class="hint">
                  {{ eventSourceHint }} 编排里用同名事件启动流程 / EVENT_WAIT / 事件边。
                </p>
              </div>
              <n-button size="small" @click="eventSpecs.push(emptyEventSpec())">
                添加事件
              </n-button>
            </div>
            <div
              v-for="(spec, index) in eventSpecs"
              :key="index"
              class="event-spec-card"
            >
              <div class="form-grid">
                <n-form-item label="事件名" required>
                  <n-input
                    v-model:value="spec.event_name"
                    placeholder="pick_grasped"
                  />
                </n-form-item>
                <n-form-item label="来源">
                  <n-select
                    v-model:value="spec.source"
                    :options="eventSourceOptions"
                  />
                </n-form-item>
                <n-form-item label="字段路径">
                  <n-input
                    v-model:value="spec.field"
                    placeholder="status 或 feedback.phase"
                    :disabled="spec.op === 'ros_success'"
                  />
                </n-form-item>
                <n-form-item label="比较">
                  <n-select v-model:value="spec.op" :options="eventOpOptions" />
                </n-form-item>
                <n-form-item label="比较值（JSON 或字符串）">
                  <n-input
                    v-model:value="spec.valueText"
                    placeholder='"grasped" 或 1'
                    :disabled="
                      spec.op === 'ros_success' ||
                      spec.op === 'exists' ||
                      spec.op === 'truthy'
                    "
                  />
                </n-form-item>
                <n-form-item label="最大触发次数（0=不限）">
                  <n-input-number
                    v-model:value="spec.max_firings"
                    :min="0"
                    class="full-width"
                  />
                </n-form-item>
                <n-form-item label="冷却 (ms)">
                  <n-input-number
                    v-model:value="spec.cooldown_ms"
                    :min="0"
                    class="full-width"
                  />
                </n-form-item>
              </div>
              <n-space>
                <n-button
                  size="tiny"
                  :type="spec.enabled ? 'success' : 'default'"
                  secondary
                  @click="spec.enabled = !spec.enabled"
                >
                  {{ spec.enabled ? "已启用" : "已禁用" }}
                </n-button>
                <n-button
                  size="tiny"
                  secondary
                  @click="spec.emit_on_node = !spec.emit_on_node"
                >
                  节点事件边: {{ spec.emit_on_node ? "开" : "关" }}
                </n-button>
                <n-button
                  size="tiny"
                  secondary
                  @click="spec.start_workflows = !spec.start_workflows"
                >
                  启动 EVENT 流程: {{ spec.start_workflows ? "开" : "关" }}
                </n-button>
                <n-button size="tiny" type="error" secondary @click="eventSpecs.splice(index, 1)">
                  删除
                </n-button>
              </n-space>
            </div>
            <p v-if="eventSpecs.length === 0" class="hint">
              未配置事件定义时，点位上的「完成事件名」仍可作为 RESULT/ros_success 兼容路径。
            </p>
          </div>

          <n-button text type="primary" @click="showAdvanced = !showAdvanced">
            {{ showAdvanced ? "收起高级项" : "高级项（请求模板等）" }}
          </n-button>
          <div v-if="showAdvanced" class="advanced-block">
            <n-form-item
              v-if="form.operation_kind !== 'SSH'"
              label="request_template（空对象则直接使用点位参数）"
            >
              <n-input
                :value="JSON.stringify(form.request_template ?? {}, null, 2)"
                type="textarea"
                :autosize="{ minRows: 3, maxRows: 8 }"
                @update:value="
                  (v) => {
                    try {
                      form.request_template = parseJsonObject(v, 'request_template');
                    } catch (error) {
                      errorMessage =
                        error instanceof Error ? error.message : '模板无效';
                    }
                  }
                "
              />
            </n-form-item>
            <p class="hint">
              适配器已按调用方式自动设置：{{ adapterForKind(form.operation_kind) }}。
              <template v-if="form.operation_kind === 'SSH'">
                主机取机器人配置中的 IP，命令来自所选受控 SSH 方案。
              </template>
              <template v-else>
                业务能力默认不自动插导航；编排导航请使用系统「navigation」能力。
              </template>
            </p>
          </div>
        </n-form>

        <n-alert
          v-if="statusMessage"
          type="success"
          :bordered="false"
          style="margin-top: 12px"
        >
          {{ statusMessage }}
        </n-alert>
        <n-alert
          v-if="errorMessage"
          type="error"
          :bordered="false"
          style="margin-top: 12px"
        >
          {{ errorMessage }}
        </n-alert>
      </n-card>

      <n-card size="small" title="在线测试（不推进流程）">
        <n-space vertical :size="12">
          <n-form-item
            :label="
              form.operation_kind === 'SSH'
                ? '测试机器人（SSH 绑定机器人）'
                : '测试机器人'
            "
          >
            <n-select
              v-model:value="testRobotId"
              :options="robotOptions"
              :disabled="form.operation_kind === 'SSH'"
              placeholder="选择 ONLINE 机器人"
            />
          </n-form-item>
          <div v-if="form.operation_kind !== 'SSH'">
            <div class="section-label">执行参数预览</div>
            <SchemaForm
              :model-value="testParameters"
              :schema="form.parameter_schema"
              @update:model-value="testParameters = $event"
            />
          </div>
          <n-alert
            v-else
            type="warning"
            :bordered="false"
          >
            测试将执行已保存的受控 SSH 方案（{{
              form.endpoint_name || sshProfileId || "未选择方案"
            }}），请确认目标机器人与命令无误。
          </n-alert>
          <n-button
            type="primary"
            :loading="testing"
            :disabled="isCreating"
            @click="runTest"
          >
            对选定机器人测试
          </n-button>
          <pre v-if="testResult" class="result-box">{{
            JSON.stringify(testResult, null, 2)
          }}</pre>
          <p class="hint">
            {{
              form.operation_kind === "SSH"
                ? "执行所选受控 SSH 方案，不创建流程运行记录。"
                : "仅通过该机器人的 rosbridge 调用，不创建流程运行记录。"
            }}
          </p>
        </n-space>
      </n-card>
    </div>
  </section>
</template>

<style scoped>
.capability-page {
  display: grid;
  grid-template-columns: minmax(240px, 300px) minmax(0, 1fr);
  gap: 16px;
  min-height: 0;
}

.main-panels {
  display: grid;
  gap: 16px;
}

.panel-title-row {
  display: flex;
  align-items: start;
  justify-content: space-between;
  gap: 12px;
  width: 100%;
}

.panel-title-row h2 {
  margin: 0;
  font-size: 16px;
  font-weight: 600;
}

.panel-title-row span {
  display: block;
  margin-top: 2px;
  color: #6a7a73;
  font-size: 12px;
}

.list-item {
  display: flex;
  flex-direction: column;
  gap: 6px;
  width: 100%;
  padding: 10px 12px;
  border: 1px solid #c8d1cb;
  border-radius: 8px;
  background: #fff;
  text-align: left;
}

.list-item.active {
  border-color: #1f6b5c;
  background: #d7ebe4;
}

.list-item small {
  color: #6a7a73;
  font-size: 12px;
}

.empty-hint,
.hint {
  margin: 0;
  color: #6a7a73;
  font-size: 13px;
}

.form-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 4px 16px;
}

.full-width {
  width: 100%;
}

.section-label {
  margin-bottom: 6px;
  color: #3d4d47;
  font-size: 13px;
}

.advanced-block {
  margin-top: 8px;
  padding: 12px;
  border: 1px dashed #c8d1cb;
  border-radius: 8px;
  background: #f8faf8;
}

.scan-row {
  display: flex;
  flex-wrap: wrap;
  align-items: end;
  gap: 12px;
}

.scan-robot {
  flex: 1 1 240px;
  margin-bottom: 0;
}

.section-title {
  margin: 0;
  font-size: 14px;
  font-weight: 600;
}

.event-specs-block,
.param-preview-block {
  margin: 12px 0 16px;
  padding: 12px;
  border: 1px solid #c8d1cb;
  border-radius: 8px;
  background: #f7faf8;
}

.event-spec-card {
  margin-top: 10px;
  padding: 10px 12px;
  border: 1px dashed #b7c4bc;
  border-radius: 8px;
  background: #fff;
}

.result-box {
  margin: 0;
  max-height: 280px;
  overflow: auto;
  padding: 10px 12px;
  border-radius: 8px;
  background: #15201c;
  color: #d7ebe4;
  font-size: 12px;
}

@media (max-width: 960px) {
  .capability-page,
  .form-grid {
    grid-template-columns: 1fr;
  }
}
</style>
