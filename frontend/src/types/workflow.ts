import type { EventSpec } from "./workspace";

export type WorkflowTriggerType =
  | "MANUAL"
  | "EVENT"
  | "CRON"
  | "MANUAL_OR_EVENT";

export type WorkflowNodeType =
  | "START"
  | "END"
  | "ROBOT"
  | "STATION"
  | "STATION_ACTION"
  | "ROBOT_CAPABILITY"
  | "NAVIGATION"
  | "DELAY"
  | "MANUAL_CONFIRM"
  | "EVENT_WAIT"
  | "ROBOT_STARTUP"
  | "ROBOT_SSH"
  | "SUBFLOW";

export interface WorkflowSummary {
  id: string;
  name: string;
  description: string;
  scene_id: string | null;
  trigger_type: WorkflowTriggerType;
  trigger_config: Record<string, unknown>;
  draft_version: number | null;
  draft_status: string;
  published_version: number | null;
}

export interface WorkflowGraph {
  nodes: Array<Record<string, unknown>>;
  edges: Array<Record<string, unknown>>;
  viewport?: { x: number; y: number; zoom: number };
  run_policy?: { loop_count: number; loop_delay_ms: number };
}

export interface WorkflowDetail extends WorkflowSummary {
  graph: WorkflowGraph;
  draft_version_id: string | null;
}

export interface OrchestrationAction {
  id: string;
  station_id: string;
  sequence_no: number;
  action_name: string;
  capability_key: string;
  capability_definition_id: string | null;
  motion_ownership: string;
  robot_selector_type: string;
  robot_id: string | null;
  robot_name: string;
  success_event_name: string;
  timeout_ms: number;
  event_specs?: EventSpec[];
}

export interface OrchestrationPoint {
  id: string;
  name: string;
  x: number;
  y: number;
  yaw: number;
  actions: OrchestrationAction[];
}

export interface OrchestrationRobot {
  id: string;
  name: string;
  connection_state: string;
  localization_status: string;
  robot_type: string;
}

export interface RobotStartupProfile {
  id: string;
  robot_id: string;
  name: string;
  description: string;
  enabled: boolean;
  ssh_port: number;
  ssh_username: string;
  credential_configured: boolean;
  known_hosts_configured: boolean;
  steps: Array<Record<string, unknown>>;
  readiness_checks: Array<Record<string, unknown>>;
  stop_steps: Array<Record<string, unknown>>;
  timeout_ms: number;
  version: number;
}

export type RobotStartupProfilePayload = Omit<
  RobotStartupProfile,
  | "id"
  | "robot_id"
  | "version"
  | "credential_configured"
  | "known_hosts_configured"
> & {
  credential_reference: string;
  known_hosts_reference: string;
};

export interface SuggestedEvent {
  event_name: string;
  source?: string;
  source_type: string;
  station_id: string;
  station_name: string;
  station_action_id: string;
  action_name?: string;
  capability_key: string;
  when?: { field?: string; op?: string; value?: unknown };
}

export interface OrchestrationAssets {
  scene: { id: string; name: string; active_map_version_id: string | null };
  map: { id: string; version: number } | null;
  robots: OrchestrationRobot[];
  points: OrchestrationPoint[];
  capabilities: Array<{
    id: string;
    capability_key: string;
    operation_kind: string;
    endpoint_name: string;
    ros_message_type: string;
    motion_ownership: string;
    blocking_type: string;
    timeout_ms: number;
    parameter_schema: Record<string, unknown>;
    request_template: Record<string, unknown>;
    event_specs?: EventSpec[];
  }>;
  startup_profiles: RobotStartupProfile[];
  suggested_events: SuggestedEvent[];
}
