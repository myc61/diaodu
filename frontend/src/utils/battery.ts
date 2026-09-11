export function batteryPercentText(
  battery: { percent?: number } | null | undefined
): string {
  if (battery?.percent == null || !Number.isFinite(battery.percent)) {
    return "";
  }
  return `${Math.max(0, Math.min(100, Math.round(battery.percent)))}%`;
}

export function batteryTone(
  battery: { percent?: number } | null | undefined
): "ok" | "low" | "critical" | "unknown" {
  const percent = battery?.percent;
  if (percent == null || !Number.isFinite(percent)) {
    return "unknown";
  }
  if (percent <= 15) {
    return "critical";
  }
  if (percent <= 30) {
    return "low";
  }
  return "ok";
}
