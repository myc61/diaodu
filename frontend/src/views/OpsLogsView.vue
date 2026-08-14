<script setup lang="ts">
import {
  NAlert,
  NButton,
  NCard,
  NDataTable,
  NInput,
  NSpace,
  NTag,
  type DataTableColumns
} from "naive-ui";
import { computed, h, onBeforeUnmount, onMounted, ref } from "vue";

import { listOpsLogs, type OpsLogEntry } from "../api/opsLogs";

const items = ref<OpsLogEntry[]>([]);
const errorMessage = ref("");
const filter = ref("");
const loading = ref(false);
let timer: number | undefined;

function formatTime(at: number): string {
  return new Date(at).toLocaleString("zh-CN", { hour12: false });
}

function levelType(level: string): "success" | "warning" | "error" | "info" | "default" {
  const value = level.toLowerCase();
  if (value === "error") {
    return "error";
  }
  if (value === "warn" || value === "warning") {
    return "warning";
  }
  if (value === "info") {
    return "info";
  }
  if (value === "ok" || value === "success") {
    return "success";
  }
  return "default";
}

async function refresh(): Promise<void> {
  loading.value = true;
  try {
    const result = await listOpsLogs({ limit: 200 });
    items.value = result.items;
    errorMessage.value = "";
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "加载日志失败";
  } finally {
    loading.value = false;
  }
}

const visible = computed(() => {
  const q = filter.value.trim().toLowerCase();
  if (!q) {
    return items.value;
  }
  return items.value.filter(
    (item) =>
      item.message.toLowerCase().includes(q) ||
      item.source.toLowerCase().includes(q) ||
      item.level.toLowerCase().includes(q)
  );
});

const columns = computed<DataTableColumns<OpsLogEntry>>(() => [
  {
    title: "时间",
    key: "at",
    width: 180,
    render: (row) => formatTime(row.at)
  },
  {
    title: "级别",
    key: "level",
    width: 100,
    render: (row) =>
      h(
        NTag,
        {
          size: "small",
          type: levelType(row.level),
          bordered: false
        },
        { default: () => row.level }
      )
  },
  {
    title: "来源",
    key: "source",
    width: 160
  },
  {
    title: "消息",
    key: "message",
    ellipsis: { tooltip: true }
  }
]);

onMounted(() => {
  void refresh();
  timer = window.setInterval(() => {
    if (document.visibilityState !== "visible") {
      return;
    }
    void refresh();
  }, 3000);
});

onBeforeUnmount(() => {
  if (timer !== undefined) {
    window.clearInterval(timer);
  }
});
</script>

<template>
  <section class="ops-logs-page">
    <n-card size="small">
      <template #header>
        <n-space justify="space-between" align="center" style="width: 100%">
          <div>
            <h2 class="logs-title">操作记录</h2>
            <p class="logs-sub">
              调度进程内存日志：rosbridge、场景绑定、点位与导航（重启后清空）
            </p>
          </div>
          <n-space>
            <n-input
              v-model:value="filter"
              type="text"
              clearable
              placeholder="过滤 source / 消息"
              style="width: 220px"
            />
            <n-button :loading="loading" @click="refresh">刷新</n-button>
          </n-space>
        </n-space>
      </template>

      <n-alert
        v-if="errorMessage"
        type="error"
        :bordered="false"
        style="margin-bottom: 12px"
      >
        {{ errorMessage }}
      </n-alert>

      <n-data-table
        :columns="columns"
        :data="visible"
        :loading="loading"
        :bordered="false"
        :single-line="false"
        size="small"
        :max-height="560"
        :row-key="(row: OpsLogEntry) => row.id"
      />
    </n-card>
  </section>
</template>

<style scoped>
.ops-logs-page {
  min-height: 0;
}

.logs-title {
  margin: 0;
  font-size: 16px;
  font-weight: 600;
}

.logs-sub {
  margin: 4px 0 0;
  color: #6a7a73;
  font-size: 12px;
}
</style>
