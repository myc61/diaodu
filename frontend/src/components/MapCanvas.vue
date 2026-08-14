<script setup lang="ts">
import Konva from "konva";
import { onBeforeUnmount, onMounted, ref, watch } from "vue";

import type { MapPoint, MapVersion, WorkspaceRobot } from "../types/workspace";

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

let stage: Konva.Stage | null = null;
let mapLayer: Konva.Layer | null = null;
let overlayLayer: Konva.Layer | null = null;
let imageNode: Konva.Image | null = null;
let resizeObserver: ResizeObserver | undefined;
let pendingImage: HTMLImageElement | null = null;
let mounted = false;

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

function fitToView(): void {
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
        !robot.drawable ||
        !robot.pose ||
        robot.pose.pixel_x === null ||
        robot.pose.pixel_y === null
      ) {
        continue;
      }
      const selected = robot.id === props.selectedRobotId;
      const stale = Boolean(robot.pose.stale) || !robot.drawable;
      const size = selected ? 18 : 14;
      const fill = stale
        ? selected
          ? "#b08968"
          : "#7a8a86"
        : selected
          ? "#c45c26"
          : "#1f6b5c";
      const stroke = stale
        ? selected
          ? "#8a6a4a"
          : "#4d5c58"
        : selected
          ? "#8f3d12"
          : "#10352c";
      const group = new Konva.Group({
        x: robot.pose.pixel_x,
        y: robot.pose.pixel_y,
        rotation: ((robot.pose.pixel_yaw ?? 0) * 180) / Math.PI,
        listening: true,
        name: `robot:${robot.id}`,
        opacity: stale ? 0.72 : 1
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
      // Square body; short bar marks forward (+x after rotation).
      group.add(
        new Konva.Rect({
          x: -size / 2,
          y: -size / 2,
          width: size,
          height: size,
          fill,
          stroke,
          strokeWidth: selected ? 2 : 1,
          cornerRadius: 2
        })
      );
      group.add(
        new Konva.Rect({
          x: size / 2 - 2,
          y: -3,
          width: 8,
          height: 6,
          fill: selected ? "#fff4e8" : "#7fd0b5",
          listening: false
        })
      );
      if (props.showLabels) {
        group.add(
          new Konva.Text({
            text: robot.name,
            x: 10,
            y: -18,
            fontSize: 12,
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
        emit("selectRobot", robot.id);
      });
      overlayLayer.add(group);
    }
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
  if (pendingImage) {
    pendingImage.onload = null;
    pendingImage.onerror = null;
    pendingImage.src = "";
    pendingImage = null;
  }
}

function loadMapImage(): void {
  if (!mapLayer || !props.map) {
    return;
  }
  cancelPendingImage();
  mapLayer.destroyChildren();
  imageNode = null;
  const image = new window.Image();
  pendingImage = image;
  image.onload = () => {
    if (!mounted || pendingImage !== image || !mapLayer || !props.map) {
      return;
    }
    pendingImage = null;
    imageNode = new Konva.Image({
      image,
      width: props.map.width,
      height: props.map.height
    });
    mapLayer.add(imageNode);
    mapLayer.batchDraw();
    fitToView();
    redrawOverlays();
  };
  image.onerror = () => {
    if (pendingImage === image) {
      pendingImage = null;
    }
  };
  image.src = props.map.preview_url;
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
    loadMapImage();
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
    stage.size({ width, height });
    fitToView();
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
  () => props.map?.id,
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
        `${robot.id}:${robot.drawable ? 1 : 0}:${robot.pose?.pixel_x ?? ""}:${robot.pose?.pixel_y ?? ""}:${robot.pose?.pixel_yaw ?? ""}:${robot.pose?.stale ? 1 : 0}`
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
  </div>
</template>
