<script setup lang="ts">
import { NConfigProvider } from "naive-ui";
import {
  Activity,
  Bell,
  Bot,
  Boxes,
  CircleGauge,
  Cpu,
  Map,
  Network,
  ScrollText,
  Workflow,
  Wrench
} from "lucide-vue-next";
import { computed } from "vue";
import { useRoute, useRouter } from "vue-router";

import { naiveThemeOverrides } from "./plugins/naive";

const router = useRouter();
const route = useRoute();

const navigation = [
  { label: "总览", icon: CircleGauge, path: "/" },
  { label: "机器人", icon: Bot, path: "/robots" },
  { label: "设备", icon: Cpu, path: "/devices" },
  { label: "地图场景", icon: Map, path: "/scenes" },
  { label: "流程编排", icon: Workflow, path: "/workflows" },
  { label: "运行监控", icon: Activity, path: "/runs" },
  { label: "能力模板", icon: Boxes, path: "/capabilities" },
  { label: "配置修改", icon: Wrench, path: "/config" },
  { label: "告警", icon: Bell, path: "/alerts" },
  { label: "操作记录", icon: ScrollText, path: "/operations" }
];

const activeTitle = computed(() => String(route.meta.title ?? "总览"));
const immersive = computed(() => Boolean(route.meta.immersive));

function isActive(path: string): boolean {
  if (path === "/") {
    return route.path === "/";
  }
  return route.path === path || route.path.startsWith(`${path}/`);
}

function go(path: string): void {
  if (route.path === path) {
    return;
  }
  void router.push(path);
}
</script>

<template>
  <n-config-provider
    class="naive-root"
    :theme-overrides="naiveThemeOverrides"
  >
    <div class="app-shell" :class="{ immersive }">
      <aside class="icon-rail" aria-label="主导航">
        <button
          class="rail-brand"
          type="button"
          title="调度系统"
          @click="go('/')"
        >
          <Network :size="18" />
        </button>

        <nav class="rail-nav">
          <button
            v-for="item in navigation"
            :key="item.path"
            class="rail-item"
            :class="{ active: isActive(item.path) }"
            type="button"
            :title="item.label"
            @click="go(item.path)"
          >
            <component :is="item.icon" :size="18" />
            <span>{{ item.label }}</span>
          </button>
        </nav>

        <div class="rail-foot">std</div>
      </aside>

      <div class="app-main">
        <header v-if="!immersive" class="page-chrome">
          <div>
            <p class="chrome-kicker">现场调度</p>
            <h1>{{ activeTitle }}</h1>
          </div>
        </header>
        <main class="page-body" :class="{ flush: immersive }">
          <router-view :key="route.path" />
        </main>
      </div>
    </div>
  </n-config-provider>
</template>

<style>
/* Must be unscoped: Naive root sits above the shell and must pass height. */
.naive-root {
  display: block;
  height: 100%;
  min-height: 100%;
}
</style>
