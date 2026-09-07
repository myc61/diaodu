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
  if (Object.is(props.modelValue[key], value)) {
    return;
  }
  emit("update:modelValue", {
    ...props.modelValue,
    [key]: value
  });
}

function coerceEnumValue(
  field: { type?: string; enum?: Array<string | number | boolean> },
  raw: string
): unknown {
  const match = (field.enum ?? []).find((item) => String(item) === raw);
  if (match !== undefined) {
    return match;
  }
  if (raw === "") {
    return "";
  }
  if (field.type === "number" || field.type === "integer") {
    const n = Number(raw);
    return Number.isFinite(n) ? n : raw;
  }
  if (field.type === "boolean") {
    return raw === "true";
  }
  return raw;
}

function enumSelectValue(
  key: string,
  field: { default?: unknown }
): string {
  const current = props.modelValue[key];
  if (current !== undefined && current !== null && current !== "") {
    return String(current);
  }
  if (field.default !== undefined && field.default !== null) {
    return String(field.default);
  }
  return "";
}

function onEnumChange(
  key: string,
  field: { type?: string; enum?: Array<string | number | boolean> },
  event: Event
): void {
  const raw = (event.target as HTMLSelectElement).value;
  // Native <select> can emit an empty change when the parent re-renders
  // and recreates options; do not wipe a value the user already chose.
  if (raw === "") {
    const current = props.modelValue[key];
    if (current !== undefined && current !== null && current !== "") {
      return;
    }
    setField(key, "");
    return;
  }
  setField(key, coerceEnumValue(field, raw));
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
        :value="enumSelectValue(key, field)"
        @change="onEnumChange(key, field, $event)"
      >
        <option value="" disabled>请选择</option>
        <option
          v-for="item in field.enum"
          :key="`${key}:${String(item)}`"
          :value="String(item)"
        >
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
