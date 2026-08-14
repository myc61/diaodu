import type {
  CapabilityTemplate,
  MapPoint,
  MapPointAction,
  NavigationGoalResponse,
  RobotSummary,
  Scene,
  SceneWorkspace,
  WorldPose
} from "../types/workspace";

async function requestJson<T>(
  path: string,
  init?: RequestInit,
  signal?: AbortSignal
): Promise<T> {
  const response = await fetch(path, {
    ...init,
    headers: {
      Accept: "application/json",
      ...(init?.headers ?? {})
    },
    signal
  });
  if (!response.ok) {
    let detail = response.statusText;
    try {
      const body = (await response.json()) as { error?: string };
      if (body.error) {
        detail = body.error;
      }
    } catch {
      // ignore parse errors
    }
    throw new Error(detail || `request failed: ${response.status}`);
  }
  return (await response.json()) as T;
}

export function listScenes(signal?: AbortSignal): Promise<{ items: Scene[] }> {
  return requestJson("/api/v1/scenes", undefined, signal);
}

export function createScene(
  name: string,
  description = "",
  signal?: AbortSignal
): Promise<Scene> {
  return requestJson(
    "/api/v1/scenes",
    {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ name, description })
    },
    signal
  );
}

export function deleteScene(
  sceneId: string,
  signal?: AbortSignal
): Promise<{ deleted: boolean; id: string }> {
  return requestJson(
    `/api/v1/scenes/${sceneId}`,
    { method: "DELETE" },
    signal
  );
}

export function deleteMapVersion(
  mapId: string,
  signal?: AbortSignal
): Promise<{ id: string; action: "deleted" | "archived" }> {
  return requestJson(`/api/v1/maps/${mapId}`, { method: "DELETE" }, signal);
}

export function listRobots(
  signal?: AbortSignal
): Promise<{ items: RobotSummary[] }> {
  return requestJson("/api/v1/robots", undefined, signal);
}

export function getWorkspace(
  sceneId: string,
  signal?: AbortSignal
): Promise<SceneWorkspace> {
  return requestJson(`/api/v1/scenes/${sceneId}/workspace`, undefined, signal);
}

export function listSceneMaps(
  sceneId: string,
  signal?: AbortSignal
): Promise<{ items: import("../types/workspace").MapVersion[] }> {
  return requestJson(`/api/v1/scenes/${sceneId}/maps`, undefined, signal);
}

export function setActiveMap(
  sceneId: string,
  mapVersionId: string,
  signal?: AbortSignal
): Promise<Scene> {
  return requestJson(
    `/api/v1/scenes/${sceneId}/active-map`,
    {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ map_version_id: mapVersionId })
    },
    signal
  );
}

export function updateSceneRobots(
  sceneId: string,
  robotIds: string[],
  signal?: AbortSignal
): Promise<{ scene_id: string; robot_ids: string[] }> {
  return requestJson(
    `/api/v1/scenes/${sceneId}/robots`,
    {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ robot_ids: robotIds })
    },
    signal
  );
}

export async function importMap(
  form: FormData,
  signal?: AbortSignal
): Promise<{ scene: Scene; map: { id: string; version: number; width: number; height: number } }> {
  const response = await fetch("/api/v1/maps/import", {
    method: "POST",
    body: form,
    signal
  });
  if (!response.ok) {
    let detail = response.statusText;
    try {
      const body = (await response.json()) as { error?: string };
      if (body.error) {
        detail = body.error;
      }
    } catch {
      // ignore
    }
    throw new Error(detail || `import failed: ${response.status}`);
  }
  return response.json() as Promise<{
    scene: Scene;
    map: { id: string; version: number; width: number; height: number };
  }>;
}

export function worldFromPixel(
  mapId: string,
  pixelX: number,
  pixelY: number,
  yaw = 0,
  signal?: AbortSignal
): Promise<WorldPose> {
  return requestJson(
    `/api/v1/maps/${mapId}/world-from-pixel`,
    {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ pixel_x: pixelX, pixel_y: pixelY, yaw })
    },
    signal
  );
}

export function sendNavigationGoal(
  body: {
    robot_id: string;
    scene_id: string;
    map_version_id: string;
    x: number;
    y: number;
    yaw: number;
    distance_tolerance: number;
    heading_tolerance: number;
  },
  signal?: AbortSignal
): Promise<NavigationGoalResponse> {
  return requestJson(
    "/api/v1/navigation/goals",
    {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body)
    },
    signal
  );
}

export function listCapabilityTemplates(
  signal?: AbortSignal
): Promise<{ items: CapabilityTemplate[] }> {
  return requestJson("/api/v1/capability-templates", undefined, signal);
}

export function createMapPoint(
  body: {
    scene_id: string;
    map_version_id: string;
    name: string;
    x: number;
    y: number;
    yaw: number;
    tags?: string[];
    notes?: string;
  },
  signal?: AbortSignal
): Promise<MapPoint> {
  return requestJson(
    "/api/v1/map-points",
    {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body)
    },
    signal
  );
}

export function updateMapPoint(
  id: string,
  body: {
    name: string;
    x: number;
    y: number;
    yaw: number;
    tags?: string[];
    notes?: string;
  },
  signal?: AbortSignal
): Promise<MapPoint> {
  return requestJson(
    `/api/v1/map-points/${id}`,
    {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body)
    },
    signal
  );
}

export function deleteMapPoint(
  id: string,
  signal?: AbortSignal
): Promise<{ id: string; action: "deleted" | "archived" }> {
  return requestJson(`/api/v1/map-points/${id}`, { method: "DELETE" }, signal);
}

export function replaceMapPointActions(
  id: string,
  actions: Array<Partial<MapPointAction> & { capability_key: string; sequence_no: number }>,
  signal?: AbortSignal
): Promise<{ station_id: string; items: MapPointAction[] }> {
  return requestJson(
    `/api/v1/map-points/${id}/actions`,
    {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ actions })
    },
    signal
  );
}
