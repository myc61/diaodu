<script setup lang="ts">
import {
  NAlert,
  NButton,
  NCard,
  NEmpty,
  NGi,
  NGrid,
  NSpace,
  NTag,
  NText
} from "naive-ui";
import { computed, onBeforeUnmount, onMounted, ref } from "vue";
import { useRouter } from "vue-router";

import { listRobotConfigs } from "../api/robots";
import { batteryPercentText } from "../utils/battery";
import { getSystemHealth, getSystemSummary } from "../api/system";
import type { RobotConfig } from "../types/robot";
import type { SystemHealth, SystemSummary } from "../types/system";

const router = useRouter();
const health = ref<SystemHealth | null>(null);
const summary = ref<SystemSummary | null>(null);
const robots = ref<RobotConfig[]>([]);
const loading = ref(false);
const lastUpdatedAt = ref<Date | null>(null);
const requestError = ref("");
let refreshTimer: number | undefined;
let controller: AbortController | undefined;

const healthLabel = computed(() => {
  if (loading.value && health.value === null) {
    return "检查中";
  }
  if (health.value?.status === "ok") {
    return "运行正常";
  }
  if (health.value?.status === "degraded") {
    return "服务降级";
  }
  return "后端离线";
});

const healthType = computed(() => {
  if (health.value?.status === "ok") {
    return "success" as const;
  }
  if (health.value?.status === "degraded") {
    return "warning" as const;
  }
  return "error" as const;
});

const enabledModuleCount = computed(() => {
  if (summary.value === null) {
    return 0;
  }
  return Object.values(summary.value.modules).filter(Boolean).length;
});

const onlineRobots = computed(
  () =>
    robots.value.filter((robot) => robot.connection_state === "ONLINE").length
);

const lastUpdatedLabel = computed(() => {
  if (lastUpdatedAt.value === null) {
    return "尚未同步";
  }
  return lastUpdatedAt.value.toLocaleTimeString("zh-CN", {
    hour12: false
  });
});

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

async function refreshSystemStatus(): Promise<void> {
  controller?.abort();
  controller = new AbortController();
  loading.value = true;
  requestError.value = "";

  try {
    const [nextHealth, nextSummary, nextRobots] = await Promise.all([
      getSystemHealth(controller.signal),
      getSystemSummary(controller.signal),
      listRobotConfigs(controller.signal)
    ]);
    health.value = nextHealth;
    summary.value = nextSummary;
    robots.value = nextRobots.items;
    lastUpdatedAt.value = new Date();
  } catch (error) {
    if (error instanceof DOMException && error.name === "AbortError") {
      return;
    }
    health.value = null;
    summary.value = null;
    robots.value = [];
    requestError.value = "无法连接调度后端";
  } finally {
    loading.value = false;
  }
}

onMounted(() => {
  void refreshSystemStatus();
  refreshTimer = window.setInterval(() => {
    if (document.visibilityState !== "visible") {
      return;
    }
    void refreshSystemStatus();
  }, 15_000);
});

onBeforeUnmount(() => {
  controller?.abort();
  if (refreshTimer !== undefined) {
    window.clearInterval(refreshTimer);
  }
});
</script>

<template>
  <section class="dashboard">
    <n-alert :type="healthType" :bordered="false" class="status-strip">
      <template #header>
        <n-space justify="space-between" align="center" style="width: 100%">
          <span>{{ healthLabel }}</span>
          <n-space align="center" :size="12">
            <n-text depth="3">后端 {{ health?.version ?? "--" }}</n-text>
            <n-text depth="3">{{ enabledModuleCount }}/5 模块启用</n-text>
            <n-text depth="3">同步于 {{ lastUpdatedLabel }}</n-text>
            <n-button
              size="tiny"
              quaternary
              :loading="loading"
              @click="refreshSystemStatus"
            >
              刷新
            </n-button>
          </n-space>
        </n-space>
      </template>
      {{ requestError || "调度核心与工程师工作台连接正常" }}
    </n-alert>

    <n-grid :cols="4" :x-gap="12" :y-gap="12" responsive="screen">
      <n-gi :span="1">
        <n-card size="small" title="在线机器人">
          <strong class="metric-value">
            {{ onlineRobots }}
            <small>/ {{ robots.length }}</small>
          </strong>
        </n-card>
      </n-gi>
      <n-gi :span="1">
        <n-card size="small" title="在线设备">
          <strong class="metric-value">0 <small>/ 0</small></strong>
        </n-card>
      </n-gi>
      <n-gi :span="1">
        <n-card size="small" title="运行流程">
          <strong class="metric-value">0</strong>
        </n-card>
      </n-gi>
      <n-gi :span="1">
        <n-card size="small" title="活动告警">
          <strong class="metric-value">0</strong>
        </n-card>
      </n-gi>
    </n-grid>

    <n-grid :cols="3" :x-gap="12" :y-gap="12" style="margin-top: 12px">
      <n-gi :span="1">
        <n-card size="small">
          <template #header>
            <n-space justify="space-between" style="width: 100%">
              <div>
                <div>机器人状态</div>
                <n-text depth="3" style="font-size: 12px">
                  连接、定位与业务状态
                </n-text>
              </div>
              <n-button
                text
                type="primary"
                @click="router.push('/robots')"
              >
                查看全部
              </n-button>
            </n-space>
          </template>
          <n-empty v-if="robots.length === 0" description="暂无机器人" />
          <n-space v-else vertical :size="10">
            <div
              v-for="robot in robots"
              :key="robot.id"
              class="overview-robot-row"
            >
              <strong>{{ robot.name }}</strong>
              <n-space :size="6" align="center">
                <n-tag
                  size="tiny"
                  :type="connectionType(robot.connection_state)"
                  :bordered="false"
                >
                  {{ robot.connection_state }}
                </n-tag>
                <n-text depth="3" style="font-size: 12px">
                  {{ robot.host || "-" }}:{{ robot.rosbridge_port || "-" }} ·
                  {{ robot.localization_status }}
                  <template v-if="batteryPercentText(robot.battery)">
                    · 电量 {{ batteryPercentText(robot.battery) }}
                  </template>
                </n-text>
              </n-space>
            </div>
          </n-space>
        </n-card>
      </n-gi>

      <n-gi :span="1">
        <n-card size="small">
          <template #header>
            <n-space justify="space-between" style="width: 100%">
              <div>
                <div>当前流程</div>
                <n-text depth="3" style="font-size: 12px">
                  执行、等待与恢复实例
                </n-text>
              </div>
              <n-button text type="primary" @click="router.push('/runs')">
                运行监控
              </n-button>
            </n-space>
          </template>
          <n-empty description="暂无运行实例" />
        </n-card>
      </n-gi>

      <n-gi :span="1">
        <n-card size="small" title="系统模块" subtitle="当前部署档案">
          <n-space vertical :size="8">
            <div
              v-for="(enabled, moduleName) in summary?.modules"
              :key="moduleName"
              class="module-row"
            >
              <span>{{ moduleName }}</span>
              <n-tag
                size="tiny"
                :type="enabled ? 'success' : 'default'"
                :bordered="false"
              >
                {{ enabled ? "启用" : "关闭" }}
              </n-tag>
            </div>
            <n-empty
              v-if="summary === null"
              description="等待后端状态"
              size="small"
            />
          </n-space>
        </n-card>
      </n-gi>
    </n-grid>
  </section>
</template>

<style scoped>
.dashboard {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.status-strip :deep(.n-alert-body__title) {
  width: 100%;
}

.metric-value {
  font-size: 28px;
  font-weight: 600;
  letter-spacing: -0.02em;
}

.metric-value small {
  color: #6a7a73;
  font-size: 14px;
  font-weight: 500;
}

.overview-robot-row,
.module-row {
  display: flex;
  flex-direction: column;
  gap: 4px;
  padding-bottom: 8px;
  border-bottom: 1px solid #e3ebe6;
}

.module-row {
  flex-direction: row;
  align-items: center;
  justify-content: space-between;
}

.overview-robot-row:last-child,
.module-row:last-child {
  border-bottom: 0;
  padding-bottom: 0;
}
</style>
