<script setup lang="ts">
import { computed, ref, watch } from "vue";

import SchemaForm from "./SchemaForm.vue";
import type {
  CapabilityTemplate,
  MapPoint,
  MapPointAction,
  RobotSummary
} from "../types/workspace";

const props = defineProps<{
  open: boolean;
  point: MapPoint | null;
  robots: RobotSummary[];
  points: MapPoint[];
  capabilities: CapabilityTemplate[];
  saving?: boolean;
}>();

const emit = defineEmits<{
  close: [];
  save: [
    payload: {
      name: string;
      yaw: number;
      notes: string;
      tags: string[];
      actions: MapPointAction[];
    }
  ];
  delete: [];
}>();

const name = ref("");
const yawDegrees = ref(0);
const notes = ref("");
const tagsText = ref("");
const actions = ref<MapPointAction[]>([]);
const localError = ref("");

const otherPoints = computed(() =>
  props.points.filter((item) => item.id !== props.point?.id)
);

const capabilityOptions = computed(() => {
  const business = props.capabilities.filter(
    (item) => item.capability_key !== "navigation"
  );
  return business.length > 0 ? business : props.capabilities;
});

function templateOf(action: MapPointAction): CapabilityTemplate | undefined {
  if (!action.capability_definition_id && !action.capability_key) {
    return undefined;
  }
  return props.capabilities.find(
    (item) =>
      item.id === action.capability_definition_id ||
      item.capability_key === action.capability_key
  );
}

function emptyAction(sequenceNo: number): MapPointAction {
  const template = capabilityOptions.value[0];
  return {
    sequence_no: sequenceNo,
    action_name: `动作 ${sequenceNo}`,
    parallel_group: null,
    capability_definition_id: template?.id ?? "",
    capability_key: template?.capability_key ?? "",
    motion_ownership: template?.motion_ownership ?? "DISPATCHER",
    robot_selector_type: "FIXED",
    robot_id: props.robots[0]?.id ?? "",
    robot_group: "",
    runtime_variable: "",
    parameters: {},
    precondition: null,
    failure_policy: "FAIL",
    retry_count: 0,
    timeout_ms: template?.timeout_ms ?? 30000,
    success_event_name: "",
    post_navigation_station_id: null
  };
}

function syncFromPoint(): void {
  if (!props.point) {
    return;
  }
  name.value = props.point.name;
  yawDegrees.value = Math.round((props.point.yaw * 180) / Math.PI);
  notes.value = props.point.notes ?? "";
  tagsText.value = (props.point.tags ?? []).join(", ");
  const source = Array.isArray(props.point.actions) ? props.point.actions : [];
  actions.value = source.map((action) => ({
    ...action,
    action_name:
      action.action_name?.trim() || action.capability_key || `动作 ${action.sequence_no}`,
    robot_id: action.robot_id ?? "",
    capability_definition_id: action.capability_definition_id ?? "",
    post_navigation_station_id: (action.post_navigation_station_id ||
      "") as string | null,
    parameters:
      action.parameters && typeof action.parameters === "object"
        ? { ...action.parameters }
        : {}
  }));
  if (actions.value.length === 0) {
    actions.value = [emptyAction(1)];
  }
  localError.value = "";
}

watch(
  () => [props.open, props.point?.id] as const,
  ([open]) => {
    if (open && props.point) {
      syncFromPoint();
    }
  },
  { immediate: true }
);

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

function onCapabilityChange(action: MapPointAction): void {
  const template = templateOf(action);
  if (!template) {
    return;
  }
  action.capability_definition_id = template.id;
  action.capability_key = template.capability_key;
  action.motion_ownership = template.motion_ownership;
  action.timeout_ms = template.timeout_ms;
  action.parameters = defaultsFromSchema(template.parameter_schema);
  action.post_navigation_station_id = null;
}

function addAction(): void {
  actions.value.push(emptyAction(actions.value.length + 1));
}

function removeAction(index: number): void {
  actions.value.splice(index, 1);
  actions.value.forEach((action, i) => {
    action.sequence_no = i + 1;
  });
}

function validate(): boolean {
  if (!name.value.trim()) {
    localError.value = "点位名称不能为空";
    return false;
  }
  for (const [index, action] of actions.value.entries()) {
    if (!action.action_name.trim()) {
      localError.value = `第 ${index + 1} 个动作请填写动作名称`;
      return false;
    }
    if (!action.capability_key && !action.capability_definition_id) {
      localError.value = `第 ${index + 1} 个动作未选择能力模板`;
      return false;
    }
    if (!action.robot_id) {
      localError.value = `第 ${index + 1} 个动作请选择执行机器人`;
      return false;
    }
    const template = templateOf(action);
    const schema = (template?.parameter_schema ?? {}) as {
      required?: string[];
      properties?: Record<string, { title?: string }>;
    };
    for (const key of schema.required ?? []) {
      const value = action.parameters?.[key];
      if (value === undefined || value === null || value === "") {
        localError.value = `第 ${index + 1} 个动作请填写参数「${
          schema.properties?.[key]?.title || key
        }」`;
        return false;
      }
    }
  }
  localError.value = "";
  return true;
}

function handleSave(): void {
  if (!validate() || !props.point) {
    return;
  }
  emit("save", {
    name: name.value.trim(),
    yaw: (yawDegrees.value * Math.PI) / 180,
    notes: notes.value,
    tags: tagsText.value
      .split(/[,，]/)
      .map((item) => item.trim())
      .filter(Boolean),
    actions: actions.value.map((action, index) => {
      const template = templateOf(action);
      return {
        ...action,
        sequence_no: index + 1,
        action_name: action.action_name.trim(),
        capability_key: template?.capability_key || action.capability_key,
        capability_definition_id:
          template?.id || action.capability_definition_id || null,
        robot_id: action.robot_id || null,
        post_navigation_station_id:
          (template?.motion_ownership || action.motion_ownership) ===
            "ROBOT_INTERNAL" || !action.post_navigation_station_id
            ? null
            : action.post_navigation_station_id
      };
    })
  });
}
</script>

<template>
  <Teleport to="body">
    <aside
      v-if="open && point"
      class="point-config-drawer"
      aria-label="点位配置"
    >
      <header class="point-drawer-header">
        <div>
          <h2>配置点位 · {{ point.name }}</h2>
          <p>选机器人 → 选能力 → 保存配置</p>
        </div>
        <div class="point-list-actions">
          <button type="button" class="danger-button" @click="emit('delete')">
            删除点位
          </button>
          <button type="button" class="ghost-button" @click="emit('close')">
            关闭
          </button>
        </div>
      </header>

      <div class="point-drawer-body">
        <section class="point-section">
          <h3>基本信息</h3>
          <div class="point-grid">
            <label>
              名称
              <input v-model="name" />
            </label>
            <label>
              yaw (deg)
              <input v-model.number="yawDegrees" type="number" step="1" />
            </label>
            <label class="span-2">
              备注
              <input v-model="notes" placeholder="用途说明" />
            </label>
          </div>
        </section>

        <section class="point-section">
          <div class="section-title-row">
            <h3>动作列表</h3>
            <button type="button" class="primary-button" @click="addAction">
              + 再加一条动作
            </button>
          </div>

          <p v-if="robots.length === 0" class="status-error">
            暂无机器人，请先在「机器人」页创建。
          </p>
          <p v-else-if="capabilityOptions.length === 0" class="status-error">
            暂无能力模板，无法配置动作。
          </p>

          <article
            v-for="(action, index) in actions"
            :key="`action-${index}`"
            class="action-block"
          >
            <header class="action-block-header">
              <strong>#{{ index + 1 }} · {{ action.action_name || "未命名动作" }}</strong>
              <button
                type="button"
                class="danger-button"
                @click="removeAction(index)"
              >
                删除这条
              </button>
            </header>

            <div class="point-grid">
              <label class="span-2">
                1. 动作名称
                <input
                  v-model="action.action_name"
                  maxlength="80"
                  placeholder="例如：抓取料箱、打开舱门"
                />
              </label>
              <label>
                2. 执行机器人
                <select v-model="action.robot_id">
                  <option disabled value="">请选择</option>
                  <option
                    v-for="robot in robots"
                    :key="robot.id"
                    :value="robot.id"
                  >
                    {{ robot.name }}
                  </option>
                </select>
              </label>
              <label>
                3. 关联能力模板
                <select
                  v-model="action.capability_definition_id"
                  @change="onCapabilityChange(action)"
                >
                  <option disabled value="">请选择</option>
                  <option
                    v-for="item in capabilityOptions"
                    :key="item.id"
                    :value="item.id"
                  >
                    {{ item.capability_key }} · {{ item.operation_kind }} ·
                    {{ item.endpoint_name }}
                  </option>
                </select>
              </label>
              <label>
                超时(ms)
                <input
                  v-model.number="action.timeout_ms"
                  type="number"
                  min="1"
                />
              </label>
              <label>
                重试
                <input
                  v-model.number="action.retry_count"
                  type="number"
                  min="0"
                />
              </label>
              <label class="span-2">
                完成事件名（可选，RESULT/ros_success 兼容）
                <input
                  v-model="action.success_event_name"
                  placeholder="point_a_done"
                />
              </label>
              <small class="span-2" style="color: #6a7a73">
                Feedback/字段级事件在「机器人能力」事件定义中配置；此处完成事件名会合并为
                RESULT/ros_success。
                <template v-if="(templateOf(action)?.event_specs?.length ?? 0) > 0">
                  当前能力已定义
                  {{ templateOf(action)?.event_specs?.length }} 条事件。
                </template>
              </small>
              <label
                v-if="action.motion_ownership !== 'ROBOT_INTERNAL'"
                class="span-2"
              >
                完成后导航到（可选）
                <select v-model="action.post_navigation_station_id">
                  <option value="">无</option>
                  <option
                    v-for="item in otherPoints"
                    :key="item.id"
                    :value="item.id"
                  >
                    {{ item.name }}
                  </option>
                </select>
              </label>
            </div>

            <div class="param-block">
              <h4>4. 动作参数</h4>
              <SchemaForm
                v-model="action.parameters"
                :schema="templateOf(action)?.parameter_schema"
              />
            </div>
          </article>
        </section>
      </div>

      <footer class="point-drawer-footer">
        <p v-if="localError" class="status-error">{{ localError }}</p>
        <div class="confirm-actions">
          <button type="button" class="ghost-button" @click="emit('close')">
            取消
          </button>
          <button
            type="button"
            class="primary-button"
            :disabled="saving"
            @click="handleSave"
          >
            保存配置
          </button>
        </div>
      </footer>
    </aside>
  </Teleport>
</template>
