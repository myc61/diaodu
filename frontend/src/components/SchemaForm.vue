<script setup lang="ts">
import { computed } from "vue";

type JsonSchema = {
  type?: string;
  properties?: Record<
    string,
    {
      type?: string;
      title?: string;
      description?: string;
      default?: unknown;
      minimum?: number;
      maximum?: number;
      enum?: Array<string | number | boolean>;
    }
  >;
  required?: string[];
};

const props = defineProps<{
  schema: Record<string, unknown> | null | undefined;
  modelValue: Record<string, unknown>;
}>();

const emit = defineEmits<{
  "update:modelValue": [value: Record<string, unknown>];
}>();

const parsed = computed(() => (props.schema ?? {}) as JsonSchema);
const fields = computed(() => Object.entries(parsed.value.properties ?? {}));
const required = computed(() => new Set(parsed.value.required ?? []));

function setField(key: string, value: unknown): void {
  emit("update:modelValue", {
    ...props.modelValue,
    [key]: value
  });
}

function asNumber(value: string): number | "" {
  if (value === "" || value === "-") {
    return "";
  }
  const n = Number(value);
  return Number.isFinite(n) ? n : "";
}

function jsonText(value: unknown, fallback: unknown): string {
  return JSON.stringify(value ?? fallback, null, 2);
}

function setJsonField(key: string, text: string, fallback: unknown): void {
  try {
    setField(key, JSON.parse(text) as unknown);
  } catch {
    setField(key, fallback);
  }
}
</script>

<template>
  <div v-if="fields.length === 0" class="schema-empty">
    该能力无额外参数（或未提供 JSON Schema）。
  </div>
  <div v-else class="schema-form">
    <label v-for="[key, field] in fields" :key="key">
      <span>
        {{ field.title || key }}
        <em v-if="required.has(key)">*</em>
      </span>
      <select
        v-if="field.enum?.length"
        :value="String(modelValue[key] ?? field.default ?? '')"
        @change="
          setField(
            key,
            field.type === 'number' || field.type === 'integer'
              ? Number(($event.target as HTMLSelectElement).value)
              : ($event.target as HTMLSelectElement).value
          )
        "
      >
        <option value="" disabled>请选择</option>
        <option v-for="item in field.enum" :key="String(item)" :value="String(item)">
          {{ item }}
        </option>
      </select>
      <input
        v-else-if="field.type === 'boolean'"
        type="checkbox"
        :checked="Boolean(modelValue[key] ?? field.default ?? false)"
        @change="
          setField(key, ($event.target as HTMLInputElement).checked)
        "
      />
      <input
        v-else-if="field.type === 'number' || field.type === 'integer'"
        type="number"
        :step="field.type === 'integer' ? 1 : 'any'"
        :min="field.minimum"
        :max="field.maximum"
        :value="modelValue[key] ?? field.default ?? ''"
        @input="
          setField(
            key,
            asNumber(($event.target as HTMLInputElement).value)
          )
        "
      />
      <textarea
        v-else-if="field.type === 'object' || field.type === 'array'"
        rows="3"
        :value="
          jsonText(
            modelValue[key],
            field.default ?? (field.type === 'array' ? [] : {})
          )
        "
        @change="
          setJsonField(
            key,
            ($event.target as HTMLTextAreaElement).value,
            field.default ?? (field.type === 'array' ? [] : {})
          )
        "
      />
      <input
        v-else
        type="text"
        :value="String(modelValue[key] ?? field.default ?? '')"
        @input="setField(key, ($event.target as HTMLInputElement).value)"
      />
      <small v-if="field.description">{{ field.description }}</small>
    </label>
  </div>
</template>
