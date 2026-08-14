import { createRouter, createWebHistory } from "vue-router";

export const router = createRouter({
  history: createWebHistory(),
  routes: [
    {
      path: "/",
      name: "overview",
      component: () => import("../views/OverviewView.vue"),
      meta: { title: "总览" }
    },
    {
      path: "/robots",
      name: "robots",
      component: () => import("../views/RobotManageView.vue"),
      meta: { title: "机器人" }
    },
    {
      path: "/devices",
      name: "devices",
      component: () => import("../views/PlaceholderView.vue"),
      meta: {
        title: "设备",
        description: "设备连接与能力配置尚未实现。"
      }
    },
    {
      path: "/scenes",
      name: "scenes",
      component: () => import("../views/SceneWorkspaceView.vue"),
      meta: { title: "地图场景", immersive: true }
    },
    {
      path: "/workflows",
      name: "workflows",
      component: () => import("../views/WorkflowEditorView.vue"),
      meta: { title: "流程编排" }
    },
    {
      path: "/runs",
      name: "runs",
      component: () => import("../views/RunsView.vue"),
      meta: { title: "运行监控" }
    },
    {
      path: "/capabilities",
      name: "capabilities",
      component: () => import("../views/CapabilitiesView.vue"),
      meta: { title: "能力模板" }
    },
    {
      path: "/config",
      name: "config",
      component: () => import("../views/PlaceholderView.vue"),
      meta: {
        title: "配置修改",
        description:
          "机器人内部配置（读取/差异预览/应用/回滚，FR-CFG）尚未实现。连接与 IP 请在「机器人」页管理。"
      }
    },
    // Legacy redirects from earlier duplicated menu labels
    { path: "/robot-config", redirect: "/config" },
    {
      path: "/alerts",
      name: "alerts",
      component: () => import("../views/PlaceholderView.vue"),
      meta: { title: "告警", description: "告警中心尚未实现。" }
    },
    {
      path: "/operations",
      name: "operations",
      component: () => import("../views/OpsLogsView.vue"),
      meta: { title: "操作记录" }
    }
  ]
});
