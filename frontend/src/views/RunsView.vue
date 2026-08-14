<script setup lang="ts">
import { Background } from "@vue-flow/background";
import { Controls } from "@vue-flow/controls";
import { MarkerType, VueFlow } from "@vue-flow/core";
import { Archive, Play, RefreshCw, Square, Trash2 } from "lucide-vue-next";
import { computed, onBeforeUnmount, onMounted, ref, watch } from "vue";
import { useRouter } from "vue-router";

import {
  cancelWorkflowRun,
  archiveWorkflowRun,
  cleanupWorkflowRunHistory,
  decideManualConfirmation,
  getWorkflowRun,
  injectWorkflowEvent,
  listWorkflowRuns,
  startWorkflowRun,
  type NodeRun,
  type CommandRun,
  type WorkflowRunDetail,
  type WorkflowRunSummary
} from "../api/runs";
import { listWorkflows } from "../api/workflows";
import type { WorkflowSummary } from "../types/workflow";

import "@vue-flow/core/dist/style.css";
import "@vue-flow/core/dist/theme-default.css";
import "@vue-flow/controls/dist/style.css";

const router = useRouter();
const runs = ref<WorkflowRunSummary[]>([]);
const workflows = ref<WorkflowSummary[]>([]);
const selectedWorkflowId = ref("");
const selectedRun = ref<WorkflowRunDetail | null>(null);
const eventName = ref("");
const eventTargetRunId = ref("");
const eventBusinessKey = ref("");
const manualBusinessKey = ref("");
const statusMessage = ref("");
const errorMessage = ref("");
const loading = ref(false);
const stateFilter = ref("ALL");
const showArchived = ref(false);
const cleanupDays = ref(30);
const confirmationNotes = ref<Record<string, string>>({});
const selectedAttempt = ref(1);
const followedRunId = ref("");
const followedLatestAttempt = ref(0);
let timer: number | undefined;

const visibleRuns = computed(() =>
  stateFilter.value === "ALL"
    ? runs.value
    : runs.value.filter((run) => run.state === stateFilter.value)
);

const linkedRuns = computed(() => {
  const selected = selectedRun.value;
  if (!selected) {
    return [];
  }
  const rootId = selected.root_run_id || selected.id;
  return runs.value.filter(
    (run) =>
      run.id === rootId ||
      run.root_run_id === rootId ||
      run.parent_run_id === selected.id ||
      run.id === selected.parent_run_id ||
      run.id === selected.source_run_id
  );
});

const availableAttempts = computed(() => {
  const values = new Set((selectedRun.value?.nodes ?? []).map((node) => node.attempt));
  return [...values].sort((left, right) => left - right);
});

const latestAttempt = computed(
  () => availableAttempts.value[availableAttempts.value.length - 1] ?? 1
);

const attemptNodeRuns = computed(() =>
  (selectedRun.value?.nodes ?? []).filter(
    (node) => node.attempt === selectedAttempt.value
  )
);

const nodeRunByKey = computed(
  () => new Map(attemptNodeRuns.value.map((node) => [node.node_key, node]))
);

const statePresentation: Record<
  string,
  { label: string; color: string; background: string }
> = {
  PENDING: { label: "未执行", color: "#8a9690", background: "#f3f5f4" },
  WAITING_RESOURCE: { label: "等待资源", color: "#b7791f", background: "#fff8dd" },
  RUNNING: { label: "执行中", color: "#1976d2", background: "#e8f2ff" },
  WAITING_EVENT: { label: "等待事件", color: "#b7791f", background: "#fff8dd" },
  PAUSED: { label: "等待人工", color: "#d97706", background: "#fff4d6" },
  SUCCEEDED: { label: "已成功", color: "#238636", background: "#eaf7ed" },
  FAILED: { label: "失败", color: "#c62828", background: "#fdecec" },
  CANCELLING: { label: "取消中", color: "#6b7280", background: "#f0f1f2" },
  CANCELLED: { label: "已取消", color: "#6b7280", background: "#f0f1f2" },
  RECOVERING: { label: "恢复中", color: "#7e57c2", background: "#f1ebff" }
};

function stateInfo(state: string) {
  return statePresentation[state] ?? statePresentation.PENDING;
}

const runtimeNodes = computed(() =>
  (selectedRun.value?.graph.nodes ?? []).map((raw) => {
    const id = String(raw.id ?? "");
    const data = (raw.data as Record<string, unknown> | undefined) ?? {};
    const run = nodeRunByKey.value.get(id);
    const state = run?.state ?? "PENDING";
    const info = stateInfo(state);
    const baseLabel = String(data.label ?? raw.type ?? id);
    return {
      id,
      type: raw.type === "START" ? "input" : raw.type === "END" ? "output" : "default",
      position: (raw.position as { x: number; y: number } | undefined) ?? { x: 0, y: 0 },
      label: `${baseLabel}\n${info.label}`,
      data: { ...data, runtime_state: state },
      class: `runtime-node runtime-node-${state.toLowerCase()}`,
      style: {
        border: `2px solid ${info.color}`,
        background: info.background,
        color: "#1f2925",
        whiteSpace: "pre-line",
        minWidth: "130px"
      }
    };
  })
);

const traversedEdgeIds = computed(() => {
  const ids = new Set<string>();
  for (const event of selectedRun.value?.events ?? []) {
    if (
      event.event_type === "workflow.edge.traversed" &&
      Number(event.payload.attempt ?? 1) === selectedAttempt.value
    ) {
      const edgeId = String(event.payload.edge_id ?? "");
      if (edgeId) {
        ids.add(edgeId);
      }
    }
  }
  return ids;
});

const runtimeEdges = computed(() =>
  (selectedRun.value?.graph.edges ?? []).map((raw) => {
    const id = String(raw.id ?? `${raw.source}-${raw.target}`);
    const traversed = traversedEdgeIds.value.has(id);
    const edgeKind = String(raw.edge_kind ?? "success");
    const color = traversed ? (edgeKind === "event" ? "#7e57c2" : "#238636") : "#aeb8b3";
    return {
      id,
      source: String(raw.source ?? ""),
      target: String(raw.target ?? ""),
      label: edgeKind === "event" ? `事件：${String(raw.event_name ?? "")}` : "成功",
      animated: traversed && selectedRun.value?.state === "RUNNING",
      class: traversed ? "runtime-edge-traversed" : "runtime-edge-pending",
      style: {
        stroke: color,
        strokeWidth: traversed ? 3 : 1.5,
        strokeDasharray: traversed ? "none" : "6 5"
      },
      markerEnd: { type: MarkerType.ArrowClosed, color }
    };
  })
);

watch(
  [() => selectedRun.value?.id ?? "", latestAttempt],
  ([runId, latest]) => {
    if (runId !== followedRunId.value || latest > followedLatestAttempt.value) {
      followedRunId.value = runId;
      followedLatestAttempt.value = latest;
      selectedAttempt.value = latest;
    }
  },
  { immediate: true }
);

async function refresh(): Promise<void> {
  loading.value = true;
  errorMessage.value = "";
  try {
    const [runResult, wfResult] = await Promise.all([
      listWorkflowRuns(showArchived.value ? "only" : "exclude"),
      listWorkflows()
    ]);
    runs.value = runResult.items;
    workflows.value = wfResult.items.filter((item) => item.published_version);
    if (!selectedWorkflowId.value && workflows.value.length > 0) {
      selectedWorkflowId.value = workflows.value[0].id;
    }
    if (selectedRun.value) {
      selectedRun.value = await getWorkflowRun(selectedRun.value.id);
    }
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "加载运行列表失败";
  } finally {
    loading.value = false;
  }
}

async function handleArchive(): Promise<void> {
  const run = selectedRun.value;
  if (!run) {
    return;
  }
  if (!["SUCCEEDED", "FAILED", "CANCELLED"].includes(run.state)) {
    errorMessage.value = "运行中的实例必须先取消，结束后才能归档";
    return;
  }
  try {
    await archiveWorkflowRun(run.id);
    selectedRun.value = null;
    statusMessage.value = "运行记录已归档，可通过“查看归档”找到";
    await refresh();
  } catch (error) {
    errorMessage.value = error instanceof Error ? error.message : "归档失败";
  }
}

async function handleCleanup(): Promise<void> {
  const days = Math.max(0, Math.trunc(cleanupDays.value || 0));
  if (!window.confirm(
    `永久删除已归档且归档时间超过 ${days} 天的运行记录？此操作不可恢复。`
  )) {
    return;
  }
  try {
    const result = await cleanupWorkflowRunHistory(days);
    selectedRun.value = null;
    statusMessage.value = `已永久清理 ${result.deleted} 条归档记录`;
    await refresh();
  } catch (error) {
    errorMessage.value = error instanceof Error ? error.message : "清理失败";
  }
}

async function handleStart(): Promise<void> {
  if (!selectedWorkflowId.value) {
    errorMessage.value = "请选择已发布流程";
    return;
  }
  try {
    loading.value = true;
    const detail = await startWorkflowRun(
      selectedWorkflowId.value,
      manualBusinessKey.value.trim()
        ? { business_key: manualBusinessKey.value.trim() }
        : {}
    );
    selectedRun.value = detail;
    statusMessage.value = `已启动运行 ${detail.id.slice(0, 8)} · ${detail.state}`;
    errorMessage.value = "";
    await refresh();
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "启动失败（需先发布流程）";
  } finally {
    loading.value = false;
  }
}

async function openRun(id: string): Promise<void> {
  try {
    selectedRun.value = await getWorkflowRun(id);
    errorMessage.value = "";
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "加载运行详情失败";
  }
}

async function handleCancel(): Promise<void> {
  if (!selectedRun.value) {
    return;
  }
  try {
    selectedRun.value = await cancelWorkflowRun(selectedRun.value.id);
    statusMessage.value = "运行已取消";
    await refresh();
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "取消失败";
  }
}

async function handleInjectEvent(): Promise<void> {
  if (!eventName.value.trim()) {
    errorMessage.value = "请输入事件名";
    return;
  }
  try {
    const result = await injectWorkflowEvent(
      eventName.value.trim(),
      {},
      {
        target_run_id: eventTargetRunId.value.trim() || undefined,
        business_key: eventBusinessKey.value.trim() || undefined
      }
    );
    statusMessage.value = `事件 ${result.event_name}：唤醒 ${result.woken_runs.length}，启动 ${result.started_runs.length}`;
    errorMessage.value = "";
    await refresh();
    if (result.started_runs[0]) {
      await openRun(result.started_runs[0]);
    }
  } catch (error) {
    errorMessage.value =
      error instanceof Error ? error.message : "注入事件失败";
  }
}

function isManualConfirmation(node: NodeRun): boolean {
  const input = node.input_data as { type?: string };
  return input?.type === "MANUAL_CONFIRM" && node.state === "PAUSED";
}

async function handleManualConfirmation(
  node: NodeRun,
  decision: "APPROVE" | "REJECT"
): Promise<void> {
  const run = selectedRun.value;
  if (!run) {
    return;
  }
  const action = decision === "APPROVE" ? "允许继续" : "拒绝并终止";
  if (!window.confirm(`${action}当前人工确认节点？`)) {
    return;
  }
  try {
    loading.value = true;
    selectedRun.value = await decideManualConfirmation(
      run.id,
      node.id,
      decision,
      confirmationNotes.value[node.id] ?? ""
    );
    statusMessage.value = `人工确认已${decision === "APPROVE" ? "通过，流程继续" : "拒绝，流程已失败"}`;
    errorMessage.value = "";
    await refresh();
  } catch (error) {
    errorMessage.value = error instanceof Error ? error.message : "人工确认操作失败";
  } finally {
    loading.value = false;
  }
}

function nodeLabel(node: NodeRun): string {
  const input = node.input_data as { type?: string; data?: { label?: string } };
  return `${input?.data?.label || input?.type || node.node_key} · ${node.state}`;
}

function commandsFor(node: NodeRun): CommandRun[] {
  return selectedRun.value?.commands.filter(
    (command) => command.node_run_id === node.id
  ) || [];
}

onMounted(() => {
  void refresh();
  timer = window.setInterval(() => {
    void refresh().catch(() => undefined);
  }, 3000);
});

onBeforeUnmount(() => {
  if (timer !== undefined) {
    window.clearInterval(timer);
  }
});
</script>

<template>
  <section class="runs-page">
    <header class="runs-toolbar">
      <label>
        已发布流程
        <select v-model="selectedWorkflowId">
          <option disabled value="">选择流程</option>
          <option v-for="item in workflows" :key="item.id" :value="item.id">
            {{ item.name }} · pub v{{ item.published_version }}
          </option>
        </select>
      </label>
      <button
        class="primary-button"
        type="button"
        :disabled="loading"
        @click="handleStart"
      >
        <Play :size="15" />
        人工启动
      </button>
      <label>
        业务键
        <input v-model="manualBusinessKey" placeholder="order-20260814-001" />
      </label>
      <button class="ghost-button" type="button" @click="refresh">
        <RefreshCw :size="15" :class="{ spinning: loading }" />
        刷新
      </button>
      <button class="ghost-button" type="button" @click="router.push('/workflows')">
        去编排
      </button>
      <label>
        状态筛选
        <select v-model="stateFilter">
          <option value="ALL">全部</option>
          <option value="RUNNING">运行中</option>
          <option value="SUCCEEDED">成功</option>
          <option value="FAILED">失败</option>
          <option value="CANCELLED">已取消</option>
        </select>
      </label>
      <label class="archive-toggle">
        <input v-model="showArchived" type="checkbox" @change="refresh" />
        查看归档
      </label>
      <label>
        归档保留天数
        <input v-model.number="cleanupDays" type="number" min="0" />
      </label>
      <button class="danger-button" type="button" @click="handleCleanup">
        <Trash2 :size="14" />
        工程师清理历史
      </button>
      <label>
        注入事件
        <input v-model="eventName" placeholder="point_b_completed" />
      </label>
      <label>
        目标实例（可选）
        <input v-model="eventTargetRunId" placeholder="精确唤醒 run_id" />
      </label>
      <label>
        事件业务键（可选）
        <input v-model="eventBusinessKey" placeholder="同业务实例" />
      </label>
      <button class="hot-button" type="button" @click="handleInjectEvent">
        发送事件
      </button>
    </header>

    <p v-if="statusMessage" class="status-ok">{{ statusMessage }}</p>
    <p v-if="errorMessage" class="status-error">{{ errorMessage }}</p>

    <div class="runs-body">
      <aside class="runs-list">
        <h3>运行实例</h3>
        <button
          v-for="run in visibleRuns"
          :key="run.id"
          type="button"
          class="run-item"
          :class="{ active: selectedRun?.id === run.id }"
          @click="openRun(run.id)"
        >
          <strong>{{ run.workflow_name }}</strong>
          <span>{{ run.state }} · {{ run.trigger_type }}</span>
          <small>{{ run.id.slice(0, 8) }} · v{{ run.workflow_version }}</small>
        </button>
        <p v-if="visibleRuns.length === 0" class="hint-text">暂无匹配运行</p>
      </aside>

      <section class="runs-detail">
        <template v-if="selectedRun">
          <header class="panel-header">
            <div>
              <h2>{{ selectedRun.workflow_name }}</h2>
              <span>
                {{ selectedRun.state }} · {{ selectedRun.trigger_type }} ·
                {{ selectedRun.id }}
              </span>
            </div>
            <button
              v-if="selectedRun.state === 'RUNNING'"
              class="danger-button"
              type="button"
              @click="handleCancel"
            >
              <Square :size="14" />
              取消
            </button>
            <button
              v-if="['SUCCEEDED', 'FAILED', 'CANCELLED'].includes(selectedRun.state)"
              class="ghost-button"
              type="button"
              @click="handleArchive"
            >
              <Archive :size="14" />
              归档记录
            </button>
          </header>

          <section class="run-linkage">
            <header>
              <strong>关联实例</strong>
              <span>
                root {{ (selectedRun.root_run_id || selectedRun.id).slice(0, 8) }}
                <template v-if="selectedRun.business_key">
                  · {{ selectedRun.business_key }}
                </template>
              </span>
            </header>
            <div class="run-link-list">
              <button
                v-for="run in linkedRuns"
                :key="run.id"
                type="button"
                :class="{ active: run.id === selectedRun.id }"
                @click="openRun(run.id)"
              >
                <strong>{{ run.workflow_name }}</strong>
                <span>{{ run.state }} · {{ run.trigger_type }}</span>
                <small>
                  {{ run.id.slice(0, 8) }}
                  <template v-if="run.parent_run_id"> · 子实例</template>
                  <template v-else-if="run.source_run_id"> · 事件启动</template>
                  <template v-else> · 根实例</template>
                </small>
              </button>
            </div>
          </section>

          <section class="runtime-graph-card">
            <header class="runtime-graph-toolbar">
              <div>
                <strong>流程执行图</strong>
                <small>新循环自动切换到最新轮次，颜色按本轮重新计算</small>
              </div>
              <label v-if="availableAttempts.length > 1">
                查看轮次
                <select v-model.number="selectedAttempt">
                  <option v-for="attempt in availableAttempts" :key="attempt" :value="attempt">
                    第 {{ attempt }} 轮
                  </option>
                </select>
              </label>
              <div class="runtime-legend">
                <span class="legend-pending">未执行</span>
                <span class="legend-running">执行/等待</span>
                <span class="legend-success">成功/已走边</span>
                <span class="legend-failed">失败/拒绝</span>
              </div>
            </header>
            <div class="runtime-flow">
              <VueFlow
                :nodes="runtimeNodes"
                :edges="runtimeEdges"
                fit-view-on-init
                :nodes-draggable="false"
                :nodes-connectable="false"
                :elements-selectable="false"
                :zoom-on-double-click="false"
              >
                <Background pattern-color="#d5ddd8" :gap="18" />
                <Controls :show-interactive="false" />
              </VueFlow>
            </div>
          </section>

          <ul class="node-run-list">
            <li v-for="node in attemptNodeRuns" :key="node.id">
              <div>
                <strong>{{ nodeLabel(node) }}</strong>
                <small v-if="node.assigned_robot_id">
                  robot {{ node.assigned_robot_id.slice(0, 8) }}
                </small>
              </div>
              <pre>{{ JSON.stringify(node.output_data || node.error_data || {}, null, 2) }}</pre>
              <div v-if="isManualConfirmation(node)" class="manual-confirm-card">
                <strong>{{ String(node.output_data?.prompt ?? "请确认是否继续执行") }}</strong>
                <label>
                  工程师备注（可选）
                  <textarea
                    v-model="confirmationNotes[node.id]"
                    maxlength="1000"
                    rows="2"
                    placeholder="例如：现场人员已离开危险区域"
                  ></textarea>
                </label>
                <div class="manual-confirm-actions">
                  <button
                    class="ok-button"
                    type="button"
                    :disabled="loading"
                    @click="handleManualConfirmation(node, 'APPROVE')"
                  >
                    允许继续
                  </button>
                  <button
                    class="danger-button"
                    type="button"
                    :disabled="loading"
                    @click="handleManualConfirmation(node, 'REJECT')"
                  >
                    拒绝并终止
                  </button>
                </div>
              </div>
              <div
                v-for="command in commandsFor(node)"
                :key="command.id"
                class="command-run"
              >
                <strong>{{ command.operation_kind }} · {{ command.state }}</strong>
                <span>{{ command.endpoint_name }}</span>
                <small>
                  command {{ command.command_id.slice(0, 8) }}
                  <template v-if="command.correlation_id">
                    · correlation {{ command.correlation_id }}
                  </template>
                </small>
                <pre>{{ JSON.stringify({
                  request: command.request_payload,
                  feedback: command.last_feedback,
                  result: command.result_payload,
                  error: command.error_data
                }, null, 2) }}</pre>
              </div>
            </li>
          </ul>
        </template>
        <div v-else class="empty-state">
          <strong>选择或启动一次运行</strong>
          <span>需先在流程编排中发布流程；事件触发可用「发送事件」</span>
        </div>
      </section>
    </div>
  </section>
</template>

<style scoped>
.runs-page {
  display: grid;
  gap: 10px;
}

.runs-toolbar {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  align-items: end;
  padding: 10px 12px;
  border: 1px solid var(--line);
  border-radius: 8px;
  background: #fff;
}

.runs-toolbar label {
  display: grid;
  gap: 3px;
  color: var(--muted);
  font-size: 11px;
}

.runs-toolbar input,
.runs-toolbar select {
  min-width: 140px;
  height: 32px;
  padding: 0 8px;
  border: 1px solid var(--line);
  border-radius: 6px;
}

.runs-toolbar .archive-toggle {
  display: flex;
  flex-direction: row;
  align-items: center;
  gap: 6px;
  min-height: 32px;
}

.runs-toolbar .archive-toggle input {
  width: 16px;
  min-width: 16px;
  height: 16px;
}

.runs-body {
  display: grid;
  grid-template-columns: 280px minmax(0, 1fr);
  gap: 10px;
  min-height: 480px;
}

.runs-list,
.runs-detail {
  border: 1px solid var(--line);
  border-radius: 8px;
  background: #fff;
  padding: 12px;
  min-height: 0;
  overflow: auto;
}

.run-item {
  display: grid;
  gap: 2px;
  width: 100%;
  margin-bottom: 6px;
  padding: 8px 10px;
  border: 1px solid var(--line);
  border-radius: 7px;
  background: #f7faf8;
  text-align: left;
}

.run-item.active {
  border-color: var(--accent);
  background: var(--accent-soft);
}

.run-item span,
.run-item small {
  color: var(--muted);
  font-size: 12px;
  font-family: var(--mono);
}

.node-run-list {
  list-style: none;
  margin: 0;
  padding: 0;
  display: grid;
  gap: 8px;
}

.runtime-graph-card {
  display: grid;
  gap: 8px;
  margin-bottom: 12px;
  border: 1px solid var(--line);
  border-radius: 8px;
  overflow: hidden;
  background: #fbfdfc;
}

.run-linkage {
  display: grid;
  gap: 7px;
  margin-bottom: 12px;
  padding: 9px 10px;
  border: 1px solid var(--line);
  border-radius: 8px;
  background: #f7faf8;
}

.run-linkage > header {
  display: flex;
  justify-content: space-between;
  gap: 8px;
}

.run-linkage > header span {
  color: var(--muted);
  font-family: var(--mono);
  font-size: 11px;
}

.run-link-list {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
}

.run-link-list button {
  display: grid;
  gap: 1px;
  min-width: 150px;
  padding: 6px 8px;
  border: 1px solid var(--line);
  border-radius: 6px;
  background: #fff;
  text-align: left;
}

.run-link-list button.active {
  border-color: var(--accent);
  background: var(--accent-soft);
}

.run-link-list span,
.run-link-list small {
  color: var(--muted);
  font-size: 11px;
}

.runtime-graph-toolbar {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
  padding: 8px 10px;
  border-bottom: 1px solid var(--line);
}

.runtime-graph-toolbar > div:first-child {
  display: grid;
  gap: 2px;
}

.runtime-graph-toolbar small {
  color: var(--muted);
}

.runtime-graph-toolbar label {
  display: flex;
  align-items: center;
  gap: 6px;
  color: var(--muted);
  font-size: 12px;
}

.runtime-legend {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  font-size: 11px;
}

.runtime-legend span {
  padding: 2px 7px;
  border: 1px solid currentColor;
  border-radius: 999px;
}

.legend-pending { color: #7b8781; }
.legend-running { color: #1976d2; }
.legend-success { color: #238636; }
.legend-failed { color: #c62828; }

.runtime-flow {
  height: 360px;
  min-height: 260px;
}

.runtime-flow :deep(.vue-flow__node) {
  border-radius: 8px;
  box-shadow: 0 2px 8px rgb(30 55 43 / 10%);
  font-size: 12px;
  line-height: 1.35;
}

.runtime-flow :deep(.runtime-node-running) {
  animation: runtime-pulse 1.4s ease-in-out infinite;
}

.runtime-flow :deep(.vue-flow__edge-textbg) {
  fill: #fff;
}

@keyframes runtime-pulse {
  50% { box-shadow: 0 0 0 5px rgb(25 118 210 / 14%); }
}

.manual-confirm-card {
  display: grid;
  gap: 8px;
  margin: 8px 0;
  padding: 10px;
  border: 1px solid #e2b85d;
  border-radius: 7px;
  background: #fff9e8;
}

.manual-confirm-card label {
  display: grid;
  gap: 4px;
  color: var(--muted);
  font-size: 12px;
}

.manual-confirm-card textarea {
  width: 100%;
  padding: 7px;
  border: 1px solid var(--line);
  border-radius: 6px;
  resize: vertical;
}

.manual-confirm-actions {
  display: flex;
  gap: 8px;
}

.node-run-list li {
  border: 1px solid var(--line);
  border-radius: 7px;
  padding: 8px 10px;
}

.node-run-list pre {
  margin: 6px 0 0;
  max-height: 140px;
  overflow: auto;
  font-size: 11px;
  background: #f3f6f4;
  padding: 6px;
  border-radius: 4px;
}

.command-run {
  display: grid;
  gap: 3px;
  margin-top: 8px;
  padding-top: 8px;
  border-top: 1px solid var(--line);
}

.command-run span,
.command-run small {
  color: var(--muted);
  font-family: var(--mono);
  font-size: 11px;
  overflow-wrap: anywhere;
}

@media (max-width: 900px) {
  .runs-body {
    grid-template-columns: 1fr;
  }
}
</style>
