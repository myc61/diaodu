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
  NSwitch,
  NTag
} from "naive-ui";
import { computed, onMounted, reactive, ref } from "vue";

import {
  createRobotStartupProfile,
  createRobotConfig,
  deleteRobotStartupProfile,
  deleteRobotConfig,
  listRobotStartupProfiles,
  listRobotConfigs,
  updateRobotStartupProfile,
  updateRobotConfig
} from "../api/robots";
import type { RobotConfig, RobotUpsertPayload } from "../types/robot";
import type { RobotStartupProfile } from "../types/workflow";

const robots = ref<RobotConfig[]>([]);
const selectedId = ref<string>("");
const loading = ref(false);
const savingRobot = ref(false);
const deletingRobot = ref(false);
const savingStartupProfile = ref(false);
const deletingStartupProfile = ref(false);
const statusMessage = ref("");
const errorMessage = ref("");
const isCreating = ref(false);
const startupProfiles = ref<RobotStartupProfile[]>([]);
const selectedProfileId = ref("");
const busy = computed(
  () =>
    loading.value ||
    savingRobot.value ||
    deletingRobot.value ||
    savingStartupProfile.value ||
    deletingStartupProfile.value
);

const startupForm = reactive({
  name: "",
  description: "",
  enabled: true,
  ssh_port: 22,
  ssh_username: "naviai",
  credential_reference: "/etc/dispatcher/ssh/robot_ssh_key",
  known_hosts_reference: "/etc/dispatcher/ssh/robot_known_hosts",
  timeout_ms: 120000,
  steps_text: JSON.stringify(
    [
      {
        name: "启动机器人系统",
        script: "./start.sh",
        args: [],
        working_directory: "/home/naviai",
        environment: {},
        timeout_ms: 60000
      }
    ],
    null,
    2
  ),
  readiness_checks_text: "[]",
  stop_steps_text: "[]"
});

const form = reactive<RobotUpsertPayload>({
  name: "",
  enabled: true,
  robot_type: "zj_humanoid",
  ros_version: "ROS1",
  ros_distribution: "noetic",
  ros_namespace: "",
  host: "",
  rosbridge_port: 9090,
  rosbridge_path: "/",
  rosbridge_tls: false,
  pose_topic: "/zj_humanoid/navigation/odom_info",
  pose_message_type: "nav_msgs/Odometry",
  stale_timeout_ms: 3000
});

const selectedRobot = computed(
  () => robots.value.find((robot) => robot.id === selectedId.value) ?? null
);
const selectedStartupProfile = computed(
  () =>
    startupProfiles.value.find(
      (profile) => profile.id === selectedProfileId.value
    ) ?? null
);

const rosVersionOptions = [{ label: "ROS1（当前支持）", value: "ROS1" }];

function connectionType(state: string): "success" | "warning" | "error" | "default" {
  if (state === "ONLINE") {
    return "success";
  }
  if (state === "CONNECTING") {
    return "warning";
  }
  if (state === "ERROR" || state === "OFFLINE") {
    return "error";
  }
  return "default";
}

function resetForm(): void {
  form.name = "";
  form.enabled = true;
  form.robot_type = "zj_humanoid";
  form.ros_version = "ROS1";
  form.ros_distribution = "noetic";
  form.ros_namespace = "";
  form.host = "";
  form.rosbridge_port = 9090;
  form.rosbridge_path = "/";
  form.rosbridge_tls = false;
  form.pose_topic = "/zj_humanoid/navigation/odom_info";
  form.pose_message_type = "nav_msgs/Odometry";
  form.stale_timeout_ms = 3000;
}

function fillForm(robot: RobotConfig): void {
  form.name = robot.name;
  form.enabled = robot.enabled;
  form.robot_type = robot.robot_type;
  form.ros_version = robot.ros_version;
  form.ros_distribution = robot.ros_distribution || "noetic";
  form.ros_namespace = robot.ros_namespace || "";
  form.host = robot.host ?? "";
  form.rosbridge_port = robot.rosbridge_port ?? 9090;
  form.rosbridge_path = robot.rosbridge_path || "/";
  form.rosbridge_tls = robot.rosbridge_tls;
  form.pose_topic = robot.pose_topic || "";
  form.pose_message_type =
    robot.pose_message_type || "nav_msgs/Odometry";
  form.stale_timeout_ms = robot.stale_timeout_ms || 3000;
}

function resetStartupForm(): void {
  selectedProfileId.value = "";
  startupForm.name = "";
  startupForm.description = "";
  startupForm.enabled = true;
  startupForm.ssh_port = 22;
  startupForm.ssh_username = "naviai";
  startupForm.credential_reference = "/etc/dispatcher/ssh/robot_ssh_key";
  startupForm.known_hosts_reference = "/etc/dispatcher/ssh/robot_known_hosts";
  startupForm.timeout_ms = 120000;
  startupForm.steps_text = JSON.stringify(
    [{
      name: "启动机器人系统",
      script: "./start.sh",
      args: [],
      working_directory: "/home/naviai",
      environment: {},
      timeout_ms: 60000
    }],
    null,
    2
  );
  startupForm.readiness_checks_text = "[]";
  startupForm.stop_steps_text = "[]";
}

function fillStartupForm(profile: RobotStartupProfile): void {
  selectedProfileId.value = profile.id;
  startupForm.name = profile.name;
  startupForm.description = profile.description;
  startupForm.enabled = profile.enabled;
  startupForm.ssh_port = profile.ssh_port;
  startupForm.ssh_username = profile.ssh_username || "naviai";
  startupForm.credential_reference = "";
  startupForm.known_hosts_reference = "";
  startupForm.timeout_ms = profile.timeout_ms;
  startupForm.steps_text = JSON.stringify(profile.steps, null, 2);
  startupForm.readiness_checks_text = JSON.stringify(
    profile.readiness_checks,
    null,
    2
  );
  startupForm.stop_steps_text = JSON.stringify(profile.stop_steps, null, 2);
}

async function refreshStartupProfiles(robotId: string): Promise<void> {
  if (!robotId) {
    startupProfiles.value = [];
    resetStartupForm();
    return;
  }
  const result = await listRobotStartupProfiles(robotId);
  startupProfiles.value = result.items;
  const selected = result.items.find(
    (profile) => profile.id === selectedProfileId.value
  );
  if (selected) {
    fillStartupForm(selected);
  } else if (result.items[0]) {
    fillStartupForm(result.items[0]);
  } else {
    resetStartupForm();
  }
}

async function refresh(): Promise<void> {
  loading.value = true;
  errorMessage.value = "";
  try {
    const result = await listRobotConfigs();
    robots.value = result.items;
    if (!isCreating.value) {
      if (
        selectedId.value &&
        robots.value.some((robot) => robot.id === selectedId.value)
      ) {
        fillForm(
          robots.value.find((robot) => robot.id === selectedId.value)!
        );
      } else if (robots.value.length > 0) {
        selectedId.value = robots.value[0].id;
        fillForm(robots.value[0]);
      } else {
        selectedId.value = "";
        resetForm();
        isCreating.value = true;
      }
    }
    if (!isCreating.value && selectedId.value) {
      await refreshStartupProfiles(selectedId.value);
    } else {
      startupProfiles.value = [];
      resetStartupForm();
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
  startupProfiles.value = [];
  resetStartupForm();
  statusMessage.value = "填写名称与 IP 后保存以创建机器人";
  errorMessage.value = "";
}

async function selectRobot(id: string): Promise<void> {
  isCreating.value = false;
  selectedId.value = id;
  const robot = robots.value.find((item) => item.id === id);
  if (robot) {
    fillForm(robot);
  }
  await refreshStartupProfiles(id);
  statusMessage.value = "";
  errorMessage.value = "";
}

function parseArrayField(value: string, label: string): Array<Record<string, unknown>> {
  const parsed = JSON.parse(value) as unknown;
  if (!Array.isArray(parsed)) {
    throw new Error(label + "必须是 JSON 数组");
  }
  return parsed as Array<Record<string, unknown>>;
}

function appendSshStep(kind: "script" | "command"): void {
  try {
    const steps = parseArrayField(startupForm.steps_text, "SSH 执行步骤");
    steps.push(
      kind === "script"
        ? {
            name: "执行脚本",
            script: "./start.sh",
            args: [],
            working_directory: "/home/naviai",
            environment: {},
            mode: "wait",
            timeout_ms: 60000
          }
        : {
            name: "执行命令",
            command: ["roslaunch", "robot", "bringup.launch"],
            working_directory: "/home/naviai",
            environment: {},
            mode: "wait",
            timeout_ms: 60000
          }
    );
    startupForm.steps_text = JSON.stringify(steps, null, 2);
    errorMessage.value = "";
  } catch (error) {
    errorMessage.value = error instanceof Error ? error.message : "步骤 JSON 无效";
  }
}

async function saveStartupProfile(): Promise<void> {
  if (!selectedId.value || isCreating.value) {
    errorMessage.value = "请先保存并选择机器人";
    return;
  }
  try {
    savingStartupProfile.value = true;
    const payload = {
      name: startupForm.name.trim(),
      description: startupForm.description,
      enabled: startupForm.enabled,
      ssh_port: startupForm.ssh_port,
      ssh_username: startupForm.ssh_username.trim(),
      credential_reference: startupForm.credential_reference.trim(),
      known_hosts_reference: startupForm.known_hosts_reference.trim(),
      timeout_ms: startupForm.timeout_ms,
      steps: parseArrayField(startupForm.steps_text, "启动步骤"),
      readiness_checks: parseArrayField(
        startupForm.readiness_checks_text,
        "就绪检查"
      ),
      stop_steps: parseArrayField(startupForm.stop_steps_text, "停止步骤")
    };
    if (!payload.name) {
      throw new Error("启动方案名称不能为空");
    }
    const saved = selectedProfileId.value
      ? await updateRobotStartupProfile(
          selectedId.value,
          selectedProfileId.value,
          payload
        )
      : await createRobotStartupProfile(selectedId.value, payload);
    selectedProfileId.value = saved.id;
    await refreshStartupProfiles(selectedId.value);
    statusMessage.value = "启动方案已保存，可在流程编排中选择";
    errorMessage.value = "";
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "启动方案保存失败";
  } finally {
    savingStartupProfile.value = false;
  }
}

async function removeStartupProfile(): Promise<void> {
  if (!selectedId.value || !selectedProfileId.value) {
    return;
  }
  if (!window.confirm("确认删除当前启动方案？已发布流程可能仍引用该方案。")) {
    return;
  }
  try {
    deletingStartupProfile.value = true;
    await deleteRobotStartupProfile(
      selectedId.value,
      selectedProfileId.value
    );
    resetStartupForm();
    await refreshStartupProfiles(selectedId.value);
    statusMessage.value = "启动方案已删除";
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "启动方案删除失败";
  } finally {
    deletingStartupProfile.value = false;
  }
}

async function save(): Promise<void> {
  if (!form.name.trim()) {
    errorMessage.value = "机器人名称不能为空";
    return;
  }
  if (!form.host.trim()) {
    errorMessage.value = "请填写机器人 IP 或主机名";
    return;
  }
  if (form.ros_version !== "ROS1" || form.ros_distribution !== "noetic") {
    errorMessage.value = "当前首版契约仅支持 ROS1 Noetic";
    return;
  }
  savingRobot.value = true;
  errorMessage.value = "";
  try {
    const payload: RobotUpsertPayload = {
      ...form,
      name: form.name.trim(),
      host: form.host.trim(),
      rosbridge_path: form.rosbridge_path.trim() || "/",
      pose_topic: form.pose_topic.trim(),
      pose_message_type: form.pose_message_type.trim()
    };
    const saved = isCreating.value
      ? await createRobotConfig(payload)
      : await updateRobotConfig(selectedId.value, payload);
    isCreating.value = false;
    selectedId.value = saved.id;
    await refresh();
    statusMessage.value = `已保存 ${saved.name}（${saved.rosbridge_url}，${saved.configuration_state}）`;
  } catch (error) {
    errorMessage.value = error instanceof Error ? error.message : "保存失败";
  } finally {
    savingRobot.value = false;
  }
}

async function removeRobot(): Promise<void> {
  if (isCreating.value || !selectedId.value) {
    return;
  }
  if (!window.confirm(
      `确认删除机器人 ${form.name}？需先从地图场景解绑，且不能有进行中的流程。历史运行记录会保留。`
    )) {
    return;
  }
  deletingRobot.value = true;
  errorMessage.value = "";
  try {
    await deleteRobotConfig(selectedId.value);
    selectedId.value = "";
    isCreating.value = false;
    await refresh();
    statusMessage.value = "机器人已删除";
  } catch (error) {
    errorMessage.value = error instanceof Error ? error.message : "删除失败";
  } finally {
    deletingRobot.value = false;
  }
}

onMounted(() => {
  void refresh();
});
</script>

<template>
  <section class="robot-config-page">
    <n-card class="robot-list-panel" size="small" :bordered="true">
      <template #header>
        <div class="panel-title-row">
          <div>
            <h2>机器人</h2>
            <span>连接、启停与状态（非内部配置修改）</span>
          </div>
          <n-button type="primary" size="small" @click="startCreate">
            新建
          </n-button>
        </div>
      </template>

      <n-space vertical :size="8">
        <button
          v-for="robot in robots"
          :key="robot.id"
          class="robot-list-item"
          :class="{ active: !isCreating && selectedId === robot.id }"
          type="button"
          @click="selectRobot(robot.id)"
        >
          <strong>
            {{ robot.name }}{{ robot.enabled ? "" : "（已禁用）" }}
          </strong>
          <n-space :size="6" align="center">
            <n-tag
              size="tiny"
              :type="connectionType(robot.connection_state)"
              :bordered="false"
            >
              {{ robot.connection_state }}
            </n-tag>
            <small>
              {{ robot.host || "-" }}:{{ robot.rosbridge_port || "-" }} ·
              {{ robot.localization_status }}
            </small>
          </n-space>
        </button>
        <p v-if="robots.length === 0" class="empty-hint">暂无机器人，请新建</p>
      </n-space>
    </n-card>

    <n-card class="robot-form-panel" size="small" :bordered="true">
      <template #header>
        <div class="panel-title-row">
          <div>
            <h2>{{ isCreating ? "添加机器人连接" : "编辑机器人连接" }}</h2>
            <span>
              {{
                selectedRobot
                  ? `${selectedRobot.rosbridge_url || "尚未生成 URL"} · ${selectedRobot.configuration_state}`
                  : "填写名称与 IP 后由后端生成 ws/wss URL"
              }}
            </span>
          </div>
          <n-space>
            <n-button
              v-if="!isCreating"
              type="error"
              secondary
              :loading="deletingRobot"
              :disabled="busy"
              @click="removeRobot"
            >
              删除
            </n-button>
            <n-button
              type="primary"
              :loading="savingRobot"
              :disabled="busy"
              @click="save"
            >
              保存
            </n-button>
          </n-space>
        </div>
      </template>

      <n-form
        label-placement="top"
        require-mark-placement="right-hanging"
        @submit.prevent="save"
      >
        <div class="robot-form-grid">
          <n-form-item label="名称" required>
            <n-input v-model:value="form.name" placeholder="robot_c" />
          </n-form-item>
          <n-form-item label="型号">
            <n-input v-model:value="form.robot_type" placeholder="zj_humanoid" />
          </n-form-item>
          <n-form-item label="IP / 主机名" required>
            <n-input v-model:value="form.host" placeholder="192.168.1.20" />
          </n-form-item>
          <n-form-item label="rosbridge 端口">
            <n-input-number
              v-model:value="form.rosbridge_port"
              :min="1"
              :max="65535"
              class="full-width"
            />
          </n-form-item>
          <n-form-item label="WebSocket 路径">
            <n-input v-model:value="form.rosbridge_path" placeholder="/" />
          </n-form-item>
          <n-form-item label="启用 TLS (wss)">
            <n-switch v-model:value="form.rosbridge_tls" />
          </n-form-item>
          <n-form-item label="ROS 版本">
            <n-select
              v-model:value="form.ros_version"
              :options="rosVersionOptions"
            />
          </n-form-item>
          <n-form-item label="发行版">
            <n-input
              v-model:value="form.ros_distribution"
              placeholder="noetic"
            />
          </n-form-item>
          <n-form-item label="命名空间">
            <n-input v-model:value="form.ros_namespace" placeholder="可选" />
          </n-form-item>
          <n-form-item label="定位 topic">
            <n-input
              v-model:value="form.pose_topic"
              placeholder="/zj_humanoid/navigation/odom_info"
            />
          </n-form-item>
          <n-form-item label="定位消息类型">
            <n-input
              v-model:value="form.pose_message_type"
              placeholder="nav_msgs/Odometry"
            />
          </n-form-item>
          <n-form-item label="位姿超时 (ms)">
            <n-input-number
              v-model:value="form.stale_timeout_ms"
              :min="100"
              class="full-width"
            />
          </n-form-item>
          <n-form-item label="启用机器人">
            <n-switch v-model:value="form.enabled" />
          </n-form-item>
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

      <section v-if="!isCreating && selectedRobot" class="startup-section">
        <header class="panel-title-row">
          <div>
            <h2>受控 SSH 执行方案</h2>
            <span>主机使用机器人 IP；每台机器人可维护不同脚本、命令和启动顺序</span>
          </div>
          <n-space>
            <n-button size="small" @click="resetStartupForm">
              新建方案
            </n-button>
            <n-button
              v-if="selectedStartupProfile"
              size="small"
              type="error"
              secondary
              :loading="deletingStartupProfile"
              :disabled="busy"
              @click="removeStartupProfile"
            >
              删除方案
            </n-button>
            <n-button
              size="small"
              type="primary"
              :loading="savingStartupProfile"
              :disabled="busy"
              @click="saveStartupProfile"
            >
              保存方案
            </n-button>
          </n-space>
        </header>

        <div class="startup-profile-tabs">
          <button
            v-for="profile in startupProfiles"
            :key="profile.id"
            type="button"
            :class="{ active: selectedProfileId === profile.id }"
            @click="fillStartupForm(profile)"
          >
            {{ profile.name }} · v{{ profile.version }}
          </button>
        </div>

        <div class="robot-form-grid">
          <n-form-item label="方案名称" required>
            <n-input
              v-model:value="startupForm.name"
              placeholder="robot_bringup"
            />
          </n-form-item>
          <n-form-item label="SSH 用户" required>
            <n-input
              v-model:value="startupForm.ssh_username"
              placeholder="naviai"
            />
          </n-form-item>
          <n-form-item label="SSH 端口">
            <n-input-number
              v-model:value="startupForm.ssh_port"
              :min="1"
              :max="65535"
              class="full-width"
            />
          </n-form-item>
          <n-form-item label="总超时 (ms)">
            <n-input-number
              v-model:value="startupForm.timeout_ms"
              :min="1000"
              :max="3600000"
              class="full-width"
            />
          </n-form-item>
          <n-form-item label="SSH Key Secret 路径">
            <n-input
              v-model:value="startupForm.credential_reference"
              :placeholder="selectedStartupProfile?.credential_configured
                ? '已配置；留空沿用原路径'
                : '/etc/dispatcher/ssh/robot_ssh_key'"
            />
          </n-form-item>
          <n-form-item label="known_hosts 路径">
            <n-input
              v-model:value="startupForm.known_hosts_reference"
              :placeholder="selectedStartupProfile?.known_hosts_configured
                ? '已配置；留空沿用原路径'
                : '/etc/dispatcher/ssh/robot_known_hosts'"
            />
          </n-form-item>
          <n-form-item label="说明">
            <n-input
              v-model:value="startupForm.description"
              placeholder="启动程序、检查状态或执行维护命令"
            />
          </n-form-item>
          <n-form-item label="启用方案">
            <n-switch v-model:value="startupForm.enabled" />
          </n-form-item>
        </div>

        <n-space style="margin: 12px 0 8px">
          <n-button size="small" secondary @click="appendSshStep('script')">
            添加脚本步骤
          </n-button>
          <n-button size="small" secondary @click="appendSshStep('command')">
            添加命令步骤
          </n-button>
        </n-space>

        <div class="startup-json-grid">
          <label>
            SSH 执行步骤 JSON
            <textarea v-model="startupForm.steps_text" rows="14"></textarea>
          </label>
          <label>
            就绪检查 JSON
            <textarea
              v-model="startupForm.readiness_checks_text"
              rows="14"
            ></textarea>
          </label>
          <label>
            停止步骤 JSON
            <textarea
              v-model="startupForm.stop_steps_text"
              rows="14"
            ></textarea>
          </label>
        </div>
        <p class="empty-hint">
          script 支持 ./start.sh 或绝对路径；command 使用 [可执行文件, 参数...]。长期程序设 mode=detached 并填写绝对 log_path，再用就绪检查确认启动完成。不支持 sh -c、管道或重定向。
        </p>
      </section>
    </n-card>
  </section>
</template>

<style scoped>
.robot-config-page {
  display: grid;
  grid-template-columns: minmax(260px, 320px) minmax(0, 1fr);
  gap: 16px;
  min-height: 0;
  height: 100%;
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

.robot-list-item {
  display: flex;
  flex-direction: column;
  gap: 6px;
  width: 100%;
  padding: 10px 12px;
  border: 1px solid #c8d1cb;
  border-radius: 8px;
  background: #fff;
  text-align: left;
  transition: border-color 0.15s ease, background 0.15s ease;
}

.robot-list-item:hover {
  border-color: #1f6b5c;
}

.robot-list-item.active {
  border-color: #1f6b5c;
  background: #d7ebe4;
}

.robot-list-item strong {
  font-size: 14px;
}

.robot-list-item small {
  color: #6a7a73;
  font-size: 12px;
}

.empty-hint {
  margin: 8px 0 0;
  color: #6a7a73;
  font-size: 13px;
}

.robot-form-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 4px 16px;
}

.full-width {
  width: 100%;
}

.startup-section {
  display: grid;
  gap: 12px;
  margin-top: 18px;
  padding-top: 16px;
  border-top: 1px solid #c8d1cb;
}

.startup-profile-tabs {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
}

.startup-profile-tabs button {
  padding: 6px 9px;
  border: 1px solid #c8d1cb;
  border-radius: 6px;
  background: #f7faf8;
}

.startup-profile-tabs button.active {
  border-color: #1f6b5c;
  background: #d7ebe4;
}

.startup-json-grid {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 10px;
}

.startup-json-grid label {
  display: grid;
  gap: 5px;
  color: #6a7a73;
  font-size: 12px;
}

.startup-json-grid textarea {
  width: 100%;
  min-width: 0;
  padding: 8px;
  border: 1px solid #c8d1cb;
  border-radius: 6px;
  font-family: var(--mono);
  font-size: 11px;
  resize: vertical;
}

@media (max-width: 960px) {
  .robot-config-page {
    grid-template-columns: 1fr;
  }

  .robot-form-grid {
    grid-template-columns: 1fr;
  }

  .startup-json-grid {
    grid-template-columns: 1fr;
  }
}
</style>
