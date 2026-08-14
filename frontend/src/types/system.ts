export interface SystemHealth {
  status: "ok" | "degraded" | "unavailable";
  service: string;
  version: string;
}

export interface SystemModules {
  robots: boolean;
  devices: boolean;
  maps: boolean;
  workflows: boolean;
  robot_config: boolean;
}

export interface SystemSummary {
  profile: string;
  authentication: boolean;
  modules: SystemModules;
}

