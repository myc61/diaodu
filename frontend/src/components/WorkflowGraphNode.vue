<script setup lang="ts">
import { Handle, Position, type NodeProps } from "@vue-flow/core";
import { computed } from "vue";

const props = defineProps<NodeProps<Record<string, unknown>>>();

const nodeType = computed(() => String(props.data?.nodeType ?? "DEFAULT"));
const label = computed(() => String(props.data?.label ?? nodeType.value));
const runtimeState = computed(() =>
  props.data?.runtime_state ? String(props.data.runtime_state) : ""
);
const dispatchedAt = computed(() =>
  props.data?.dispatcher_dispatched_at
    ? String(props.data.dispatcher_dispatched_at)
    : ""
);
const decidedAt = computed(() =>
  props.data?.dispatcher_decided_at
    ? String(props.data.dispatcher_decided_at)
    : ""
);
const robotStamp = computed(() =>
  props.data?.robot_result_stamp ? String(props.data.robot_result_stamp) : ""
);
const isStart = computed(() => nodeType.value === "START");
const isEnd = computed(() => nodeType.value === "END");
const showTarget = computed(() => !isStart.value);
const showSuccess = computed(() => !isEnd.value);
const showFailure = computed(() => !isStart.value && !isEnd.value);
</script>

<template>
  <div
    class="dispatch-node"
    :class="{
      selected,
      start: isStart,
      end: isEnd,
      [`runtime-${runtimeState.toLowerCase()}`]: Boolean(runtimeState)
    }"
  >
    <Handle
      v-if="showTarget"
      id="in"
      type="target"
      :position="Position.Top"
      class="handle-in"
    />
    <div class="dispatch-kind">{{ nodeType }}</div>
    <div class="dispatch-label">{{ label }}</div>
    <div v-if="runtimeState" class="dispatch-runtime">{{ runtimeState }}</div>
    <div v-if="dispatchedAt || decidedAt || robotStamp" class="dispatch-timing">
      <span v-if="dispatchedAt">下发 {{ dispatchedAt }}</span>
      <span v-if="decidedAt">
        {{ runtimeState === "FAILED" ? "失败" : "成功" }} {{ decidedAt }}
      </span>
      <span v-if="robotStamp">机器人 {{ robotStamp }}</span>
    </div>
    <div v-if="showSuccess || showFailure" class="dispatch-ports">
      <span v-if="showSuccess" class="port-success">成功 ↓</span>
      <span v-if="showFailure" class="port-failure">失败 →</span>
    </div>
    <Handle
      v-if="showSuccess"
      id="success"
      type="source"
      :position="Position.Bottom"
      class="handle-success"
    />
    <Handle
      v-if="showFailure"
      id="failure"
      type="source"
      :position="Position.Right"
      class="handle-failure"
    />
  </div>
</template>

<style scoped>
.dispatch-node {
  position: relative;
  min-width: 176px;
  max-width: 248px;
  padding: 8px 12px 10px;
  border: 1px solid #9aada3;
  border-radius: 8px;
  background: #fff;
  box-shadow: 0 1px 2px rgb(31 41 37 / 6%);
}

.dispatch-node.selected {
  border-color: #238636;
  box-shadow: 0 0 0 2px rgb(35 134 54 / 22%);
}

.dispatch-node.start {
  background: #eaf7ed;
}

.dispatch-node.end {
  background: #eef3fb;
}

.dispatch-kind {
  color: #6b7a74;
  font-size: 10px;
  letter-spacing: 0.04em;
  text-transform: uppercase;
}

.dispatch-label {
  margin-top: 2px;
  color: #1f2925;
  font-size: 12px;
  font-weight: 600;
  line-height: 1.35;
  white-space: pre-line;
  word-break: break-word;
}

.dispatch-runtime {
  margin-top: 4px;
  color: #4d5c56;
  font-size: 11px;
  font-family: var(--mono, ui-monospace, monospace);
}

.dispatch-timing {
  display: grid;
  gap: 1px;
  margin-top: 5px;
  color: #4d5c56;
  font-size: 10px;
  font-family: var(--mono, ui-monospace, monospace);
  line-height: 1.35;
}

.dispatch-ports {
  display: flex;
  justify-content: space-between;
  gap: 8px;
  margin-top: 8px;
  font-size: 10px;
  font-weight: 600;
}

.port-success {
  color: #238636;
}

.port-failure {
  margin-left: auto;
  color: #b03a2e;
}

.dispatch-node.runtime-succeeded {
  border-color: #238636;
  background: #eaf7ed;
}

.dispatch-node.runtime-failed {
  border-color: #c62828;
  background: #fdecec;
}

.dispatch-node.runtime-running,
.dispatch-node.runtime-waiting_event,
.dispatch-node.runtime-waiting_resource,
.dispatch-node.runtime-paused {
  border-color: #1976d2;
  background: #e8f2ff;
}

:deep(.vue-flow__handle.handle-in),
:deep(.vue-flow__handle.handle-success),
:deep(.vue-flow__handle.handle-failure) {
  width: 11px;
  height: 11px;
  border: 2px solid #fff;
}

:deep(.vue-flow__handle.handle-in) {
  background: #6b7a74;
}

:deep(.vue-flow__handle.handle-success) {
  background: #238636;
}

:deep(.vue-flow__handle.handle-failure) {
  background: #b03a2e;
}
</style>
