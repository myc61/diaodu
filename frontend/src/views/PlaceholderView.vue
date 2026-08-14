<script setup lang="ts">
import { computed } from "vue";
import { useRoute } from "vue-router";

const route = useRoute();
const title = computed(() => String(route.meta.title ?? "模块"));
const description = computed(
  () =>
    String(route.meta.description ?? "模块基础路由已就绪，功能尚未实现。")
);
</script>

<template>
  <section class="module-placeholder-page">
    <p class="chrome-kicker">模块占位</p>
    <h2>{{ title }}</h2>
    <p>{{ description }}</p>
  </section>
</template>
