<script setup lang="ts">
import Konva from "konva";
import { onBeforeUnmount, onMounted, ref, watch } from "vue";

import type { MapPoint, MapVersion, WorkspaceRobot } from "../types/workspace";
import { batteryPercentText, batteryTone } from "../utils/battery";

const props = withDefaults(
  defineProps<{
    map: MapVersion | null;
    robots: WorkspaceRobot[];
    points: MapPoint[];
    selectedPointId: string;
    goalPixel: { x: number; y: number } | null;
    interactionMode: "browse" | "place" | "navigate";
    /** Only this point may be dragged (e.g. while editing position). */
    draggablePointId?: string;
    showPoints?: boolean;
    showRobots?: boolean;
    showLabels?: boolean;
    layerGrid?: boolean;
    selectedRobotId?: string;
  }>(),
  {
    draggablePointId: "",
    showPoints: true,
    showRobots: true,
    showLabels: true,
    layerGrid: true,
    selectedRobotId: ""
  }
);

const emit = defineEmits<{
  clickPixel: [payload: { x: number; y: number }];
  selectPoint: [pointId: string];
  openPoint: [pointId: string];
  contextPoint: [
    payload: { pointId: string; clientX: number; clientY: number }
  ];
  movePoint: [payload: { id: string; pixelX: number; pixelY: number }];
  selectRobot: [robotId: string];
}>();

const containerRef = ref<HTMLDivElement | null>(null);
const showGrid = ref(props.layerGrid);
const previewError = ref("");

let stage: Konva.Stage | null = null;
let mapLayer: Konva.Layer | null = null;
let overlayLayer: Konva.Layer | null = null;
let imageNode: Konva.Image | null = null;
let resizeObserver: ResizeObserver | undefined;
let pendingImage: HTMLImageElement | null = null;
let previewRetryTimer: number | undefined;
let previewAttempts = 0;
let mounted = false;
let viewAdjusted = false;

function containerSize(): { width: number; height: number } {
  const el = containerRef.value;
  if (!el) {
    return { width: 0, height: 0 };
  }
  return {
    width: el.clientWidth,
    height: el.clientHeight
  };
}

function applyFitToView(): void {
  if (!stage || !props.map) {
    return;
  }
  const width = stage.width();
  const height = stage.height();
  const padding = 32;
  const scale = Math.max(
    0.05,
    Math.min(
      (width - padding * 2) / props.map.width,
      (height - padding * 2) / props.map.height
    )
  );
  stage.scale({ x: scale, y: scale });
  stage.position({
    x: (width - props.map.width * scale) / 2,
    y: (height - props.map.height * scale) / 2
  });
  stage.batchDraw();
}

function fitToView(): void {
  viewAdjusted = false;
  applyFitToView();
}

/** Keep current zoom; pan so (pixelX, pixelY) is centered in the viewport. */
function centerOn(pixelX: number, pixelY: number): void {
  if (!stage) {
    return;
  }
  const scale = stage.scaleX();
  stage.position({
    x: stage.width() / 2 - pixelX * scale,
    y: stage.height() / 2 - pixelY * scale
  });
  stage.batchDraw();
}

function drawGrid(): void {
  if (!overlayLayer || !props.map || !showGrid.value) {
    return;
  }
  const step = Math.max(20, Math.round(1 / props.map.resolution));
  for (let x = 0; x <= props.map.width; x += step) {
    overlayLayer.add(
      new Konva.Line({
        points: [x, 0, x, props.map.height],
        stroke: "rgba(46, 125, 104, 0.18)",
        strokeWidth: 1,
        listening: false
      })
    );
  }
  for (let y = 0; y <= props.map.height; y += step) {
    overlayLayer.add(
      new Konva.Line({
        points: [0, y, props.map.width, y],
        stroke: "rgba(46, 125, 104, 0.18)",
        strokeWidth: 1,
        listening: false
      })
    );
  }
}

function redrawOverlays(): void {
  if (!overlayLayer) {
    return;
  }
  overlayLayer.destroyChildren();
  if (showGrid.value) {
    drawGrid();
  }

  if (props.showPoints) {
    for (const point of props.points) {
      if (point.pixel_x === null || point.pixel_y === null) {
        continue;
      }
      const selected = point.id === props.selectedPointId;
      const canDrag = props.draggablePointId === point.id;
      const group = new Konva.Group({
        x: point.pixel_x,
        y: point.pixel_y,
        rotation: ((point.pixel_yaw ?? 0) * 180) / Math.PI,
        draggable: canDrag,
        name: `point:${point.id}`
      });
      group.add(
        new Konva.Circle({
          radius: selected ? 5 : 3.5,
          fill: selected ? "#c45c26" : "#1f6f8b",
          stroke: "#0f2f3a",
          strokeWidth: selected ? 1.5 : 1
        })
      );
      group.add(
        new Konva.Line({
          points: [0, 0, 8, 0],
          stroke: selected ? "#fff4e8" : "#d7f0ff",
          strokeWidth: 1.5,
          listening: false
        })
      );
      if (props.showLabels) {
        group.add(
          new Konva.Text({
            text: point.name,
            x: 6,
            y: -14,
            fontSize: 11,
            fill: "#16343d",
            listening: false
          })
        );
      }
      group.on("mousedown", (event) => {
        event.cancelBubble = true;
      });
      group.on("click", (event) => {
        event.cancelBubble = true;
        event.evt.stopPropagation();
        emit("selectPoint", point.id);
      });
      group.on("dblclick dbltap", (event) => {
        event.cancelBubble = true;
        event.evt.preventDefault();
        event.evt.stopPropagation();
        // Stop any accidental drag that started on the second mousedown.
        group.stopDrag();
        emit("openPoint", point.id);
      });
      group.on("contextmenu", (event) => {
        event.cancelBubble = true;
        event.evt.preventDefault();
        event.evt.stopPropagation();
        emit("contextPoint", {
          pointId: point.id,
          clientX: event.evt.clientX,
          clientY: event.evt.clientY
        });
      });
      if (canDrag) {
        group.on("dragstart", () => {
          if (stage) {
            stage.draggable(false);
          }
        });
        group.on("dragend", () => {
          if (stage) {
            stage.draggable(true);
          }
          emit("movePoint", {
            id: point.id,
            pixelX: group.x(),
            pixelY: group.y()
          });
        });
      }
      overlayLayer.add(group);
    }
  }

  if (props.showRobots) {
    for (const robot of props.robots) {
      if (
        !robot.pose ||
        robot.pose.pixel_x === null ||
        robot.pose.pixel_y === null
      ) {
        continue;
      }
      const selected = robot.id === props.selectedRobotId;
      const stale = Boolean(robot.pose.stale) || !robot.drawable;
      const size = selected ? 18 : 14;
      const stroke = stale
        ? selected
          ? "#c45c26"
          : "#4d5c58"
        : selected
          ? "#c45c26"
          : "#1f6b5c";
      const group = new Konva.Group({
        x: robot.pose.pixel_x,
        y: robot.pose.pixel_y,
        rotation: ((robot.pose.pixel_yaw ?? 0) * 180) / Math.PI,
        listening: true,
        name: `robot:${robot.id}`,
        opacity: 1
      });
      if (selected) {
        group.add(
          new Konva.Rect({
            x: -size / 2 - 4,
            y: -size / 2 - 4,
            width: size + 8,
            height: size + 8,
            stroke: "#c45c26",
            strokeWidth: 2,
            fill: "transparent",
            listening: false
          })
        );
      }
      // Hollow square so map points under the robot stay visible.
      group.add(
        new Konva.Rect({
          x: -size / 2,
          y: -size / 2,
          width: size,
          height: size,
          fillEnabled: false,
          stroke,
          strokeWidth: selected ? 2.5 : 2,
          cornerRadius: 2
        })
      );
      group.add(
        new Konva.Line({
          points: [size / 2, 0, size / 2 + 8, 0],
          stroke,
          strokeWidth: selected ? 2.5 : 2,
          lineCap: "round",
          listening: false
        })
      );
      if (props.showLabels) {
        const battery = batteryPercentText(robot.battery);
        const tone = batteryTone(robot.battery);
        const batteryFill =
          tone === "critical"
            ? "#b42318"
            : tone === "low"
              ? "#b54708"
              : "#16343d";
        group.add(
          new Konva.Text({
            text: battery ? `${robot.name} ${battery}` : robot.name,
            x: 10,
            y: -18,
            fontSize: 12,
            fill: batteryFill,
            listening: false
          })
        );
      }
      group.on("mousedown", (event) => {
        event.cancelBubble = true;
      });
      group.on("click", (event) => {
        event.cancelBubble = true;
        event.evt.stopPropagation();
        emit("selectRobot", robot.id);
      });
      overlayLayer.add(group);
    }
  }

  if (props.goalPixel && props.interactionMode === "place") {
    overlayLayer.add(
      new Konva.Circle({
        x: props.goalPixel.x,
        y: props.goalPixel.y,
        radius: 9,
        stroke: "#1f6b5c",
        strokeWidth: 2,
        dash: [5, 4],
        fillEnabled: false
      })
    );
    overlayLayer.add(
      new Konva.Circle({
        x: props.goalPixel.x,
        y: props.goalPixel.y,
        radius: 2.5,
        fill: "#1f6b5c"
      })
    );
  }

  if (props.goalPixel && props.interactionMode === "navigate") {
    overlayLayer.add(
      new Konva.Line({
        points: [
          props.goalPixel.x - 10,
          props.goalPixel.y,
          props.goalPixel.x + 10,
          props.goalPixel.y
        ],
        stroke: "#c45c26",
        strokeWidth: 2
      })
    );
    overlayLayer.add(
      new Konva.Line({
        points: [
          props.goalPixel.x,
          props.goalPixel.y - 10,
          props.goalPixel.x,
          props.goalPixel.y + 10
        ],
        stroke: "#c45c26",
        strokeWidth: 2
      })
    );
    overlayLayer.add(
      new Konva.Circle({
        x: props.goalPixel.x,
        y: props.goalPixel.y,
        radius: 6,
        stroke: "#c45c26",
        strokeWidth: 2
      })
    );
  }
  overlayLayer.batchDraw();
}

function cancelPendingImage(): void {
  if (previewRetryTimer !== undefined) {
    window.clearTimeout(previewRetryTimer);
    previewRetryTimer = undefined;
  }
  if (pendingImage) {
    pendingImage.onload = null;
    pendingImage.onerror = null;
    pendingImage.src = "";
    pendingImage = null;
  }
}

function drawMapPlaceholder(): void {
  if (!mapLayer || !props.map) {
    return;
  }
  mapLayer.add(
    new Konva.Rect({
      width: props.map.width,
      height: props.map.height,
      fill: "#c5cfc9",
      listening: false
    })
  );
  mapLayer.batchDraw();
}

function loadMapImage(): void {
  if (!mapLayer || !props.map) {
    previewError.value = "";
    return;
  }
  cancelPendingImage();
  mapLayer.destroyChildren();
  imageNode = null;
  drawMapPlaceholder();
  if (!viewAdjusted) {
    applyFitToView();
  }
  redrawOverlays();
  const image = new window.Image();
  pendingImage = image;
  image.onload = () => {
    if (!mounted || pendingImage !== image || !mapLayer || !props.map) {
      return;
    }
    pendingImage = null;
    previewAttempts = 0;
    previewError.value = "";
    mapLayer.destroyChildren();
    imageNode = new Konva.Image({
      image,
      width: props.map.width,
      height: props.map.height
    });
    mapLayer.add(imageNode);
    mapLayer.batchDraw();
    if (!viewAdjusted) {
      applyFitToView();
    }
    redrawOverlays();
  };
  image.onerror = () => {
    if (pendingImage !== image) {
      return;
    }
    pendingImage = null;
    previewError.value = "地图底图加载失败，点位仍可用";
    if (!mounted || previewAttempts >= 5) {
      return;
    }
    previewAttempts += 1;
    previewRetryTimer = window.setTimeout(() => {
      if (mounted) {
        loadMapImage();
      }
    }, 800 * previewAttempts);
  };
  // Bust cached failed responses after a reconnect stall.
  image.src = `${props.map.preview_url}?t=${Date.now()}`;
}

function setupStage(): void {
  if (!containerRef.value || !mounted) {
    return;
  }
  const { width, height } = containerSize();
  // Avoid creating a 0×0 stage; wait for ResizeObserver / next layout pass.
  if (width < 2 || height < 2) {
    return;
  }
  cancelPendingImage();
  viewAdjusted = false;
  stage?.destroy();
  stage = new Konva.Stage({
    container: containerRef.value,
    width,
    height,
    draggable: true
  });
  mapLayer = new Konva.Layer();
  overlayLayer = new Konva.Layer();
  stage.add(mapLayer);
  stage.add(overlayLayer);

  stage.on("wheel", (event) => {
    event.evt.preventDefault();
    if (!stage) {
      return;
    }
    viewAdjusted = true;
    const oldScale = stage.scaleX();
    const pointer = stage.getPointerPosition();
    if (!pointer) {
      return;
    }
    const direction = event.evt.deltaY > 0 ? -1 : 1;
    const next = Math.min(
      8,
      Math.max(0.05, direction > 0 ? oldScale * 1.1 : oldScale / 1.1)
    );
    const mousePointTo = {
      x: (pointer.x - stage.x()) / oldScale,
      y: (pointer.y - stage.y()) / oldScale
    };
    stage.scale({ x: next, y: next });
    stage.position({
      x: pointer.x - mousePointTo.x * next,
      y: pointer.y - mousePointTo.y * next
    });
  });

  stage.on("dragend", () => {
    viewAdjusted = true;
  });

  stage.on("click", () => {
    if (!stage || !props.map) {
      return;
    }
    // Browse mode: pan/zoom only — placing points requires explicit Place tool.
    if (props.interactionMode === "browse") {
      emit("selectPoint", "");
      return;
    }
    const pointer = stage.getPointerPosition();
    if (!pointer) {
      return;
    }
    const transform = stage.getAbsoluteTransform().copy().invert();
    const local = transform.point(pointer);
    if (
      local.x < 0 ||
      local.y < 0 ||
      local.x >= props.map.width ||
      local.y >= props.map.height
    ) {
      return;
    }
    emit("clickPixel", { x: local.x, y: local.y });
  });

  if (props.map) {
    previewAttempts = 0;
    loadMapImage();
  } else {
    previewError.value = "";
  }
}

function syncStageSize(): void {
  if (!mounted || !containerRef.value) {
    return;
  }
  const { width, height } = containerSize();
  if (width < 2 || height < 2) {
    return;
  }
  if (!stage) {
    setupStage();
    return;
  }
  if (stage.width() !== width || stage.height() !== height) {
    if (viewAdjusted) {
      const scale = stage.scaleX();
      const oldWidth = stage.width();
      const oldHeight = stage.height();
      const pos = stage.position();
      const centerX = (oldWidth / 2 - pos.x) / scale;
      const centerY = (oldHeight / 2 - pos.y) / scale;
      stage.size({ width, height });
      stage.position({
        x: width / 2 - centerX * scale,
        y: height / 2 - centerY * scale
      });
      stage.batchDraw();
    } else {
      stage.size({ width, height });
      applyFitToView();
    }
  }
}

onMounted(() => {
  mounted = true;
  setupStage();
  if (containerRef.value) {
    resizeObserver = new ResizeObserver(() => {
      syncStageSize();
    });
    resizeObserver.observe(containerRef.value);
  }
  // Layout often settles after first paint when switching routes.
  requestAnimationFrame(() => {
    syncStageSize();
    requestAnimationFrame(() => syncStageSize());
  });
});

onBeforeUnmount(() => {
  mounted = false;
  cancelPendingImage();
  resizeObserver?.disconnect();
  resizeObserver = undefined;
  // Destroy Konva before Vue tears down siblings. Never put Vue-managed
  // nodes inside the Konva host — Stage mutates that DOM and breaks unmount.
  if (stage) {
    try {
      stage.destroy();
    } catch {
      // ignore double-destroy during fast route switches
    }
    stage = null;
  }
  mapLayer = null;
  overlayLayer = null;
  imageNode = null;
  if (containerRef.value) {
    containerRef.value.replaceChildren();
  }
});

watch(
  () => `${props.map?.id ?? ""}:${props.map?.preview_url ?? ""}`,
  () => {
    if (mounted) {
      setupStage();
      requestAnimationFrame(() => syncStageSize());
    }
  }
);

watch(
  () => props.layerGrid,
  (value) => {
    showGrid.value = value;
  }
);

function overlaySignature(): string {
  const robots = props.robots
    .map(
      (robot) =>
        `${robot.id}:${robot.drawable ? 1 : 0}:${robot.pose?.pixel_x ?? ""}:${robot.pose?.pixel_y ?? ""}:${robot.pose?.pixel_yaw ?? ""}:${robot.pose?.stale ? 1 : 0}:${robot.battery?.percent ?? ""}`
    )
    .join("|");
  const points = props.points
    .map(
      (point) =>
        `${point.id}:${point.pixel_x ?? ""}:${point.pixel_y ?? ""}:${point.pixel_yaw ?? ""}:${point.name}`
    )
    .join("|");
  const goal = props.goalPixel
    ? `${props.goalPixel.x},${props.goalPixel.y}`
    : "";
  return [
    robots,
    points,
    props.selectedPointId,
    props.selectedRobotId,
    props.draggablePointId,
    goal,
    props.interactionMode,
    props.showPoints,
    props.showRobots,
    props.showLabels,
    props.layerGrid,
    showGrid.value
  ].join(";");
}

watch(overlaySignature, () => redrawOverlays());

defineExpose({
  fitToView,
  centerOn,
  setShowGrid(value: boolean) {
    showGrid.value = value;
    redrawOverlays();
  }
});
</script>

<template>
  <div class="map-canvas">
    <!-- Konva-only host: must stay empty of Vue children -->
    <div
      ref="containerRef"
      class="map-canvas-host"
      @contextmenu.prevent
    />
    <div v-if="!map" class="map-empty">请导入地图以开始作业编排</div>
    <div v-else-if="previewError" class="map-preview-error">{{ previewError }}</div>
  </div>
</template>
