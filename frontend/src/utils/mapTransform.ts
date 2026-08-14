import type { MapVersion, WorldPose } from "../types/workspace";

function normalizeAngle(angle: number): number {
  const twoPi = Math.PI * 2;
  let value = ((angle + Math.PI) % twoPi + twoPi) % twoPi;
  return value - Math.PI;
}

export function worldToPixel(
  map: MapVersion,
  pose: WorldPose
): { x: number; y: number; yaw: number; inside: boolean } | null {
  const [originX, originY, originYaw] = map.origin;
  if (!(map.width > 0 && map.height > 0 && map.resolution > 0)) {
    return null;
  }
  const dx = pose.x - originX;
  const dy = pose.y - originY;
  const cosOrigin = Math.cos(originYaw);
  const sinOrigin = Math.sin(originYaw);
  const localX = cosOrigin * dx + sinOrigin * dy;
  const localY = -sinOrigin * dx + cosOrigin * dy;
  const x = localX / map.resolution;
  const y = map.height - 1 - localY / map.resolution;
  return {
    x,
    y,
    yaw: normalizeAngle(-(pose.yaw - originYaw)),
    inside: x >= 0 && y >= 0 && x < map.width && y < map.height
  };
}
