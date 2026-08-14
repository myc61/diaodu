<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref } from "vue";

import { listOpsLogs, type OpsLogEntry } from "../api/opsLogs";

const props = withDefaults(
  defineProps<{
    open: boolean;
    compact?: boolean;
  }>(),
  { compact: false }
);

const emit = defineEmits<{
  close: [];
}>();

const items = ref<OpsLogEntry[]>([]);
const errorMessage = ref("");
let timer: number | undefined;

function formatTime(at: number): string {
  const date = new Date(at);
  return date.toLocaleTimeString("zh-CN", { hour12: false });
}

async function refresh(): Promise<void> {
  try {
    const result = await listOpsLogs({ limit: props.compact ? 40 : 120 });
    items.value = result.items;
    errorMessage.value = "";
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "加载日志失败";
  }
}

onMounted(() => {
  void refresh();
  timer = window.setInterval(() => {
    void refresh();
  }, 2000);
});

onBeforeUnmount(() => {
  if (timer !== undefined) {
    window.clearInterval(timer);
  }
});
</script>

<template>
  <aside v-if="open" class="ops-log-panel" :class="{ compact }">
    <header>
      <div>
        <strong>运行日志</strong>
        <span>rosbridge · 绑定 · 点位 · 导航</span>
      </div>
      <button type="button" class="ghost-button" @click="emit('close')">关闭</button>
    </header>
    <p v-if="errorMessage" class="status-error">{{ errorMessage }}</p>
    <ul>
      <li v-for="entry in items" :key="entry.id" :data-level="entry.level">
        <time>{{ formatTime(entry.at) }}</time>
        <em>{{ entry.source }}</em>
        <span>{{ entry.message }}</span>
      </li>
      <li v-if="items.length === 0" class="empty">暂无日志。连接机器人或绑定场景后会出现记录。</li>
    </ul>
  </aside>
</template>
