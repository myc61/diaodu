export interface Scene {
  id: string;
  name: string;
  description: string;
  active_map_version_id: string | null;
}

export interface MapVersion {
  id: string;
  scene_id: string;
  version: number;
  status: string;
  width: number;
  height: number;
  resolution: number;
  origin: [number, number, number];
  occupied_thresh: number;
  free_thresh: number;
  sha256: string;
  file_size_bytes: number;
  yaml_image: string;
  frame_id: string;
  preview_url: string;
}

export interface RobotSummary {
  id: string;
  name: string;
  enabled: boolean;
  robot_type: string;
  ros_version: string;
  current_scene_id: string | null;
  connection_state: string;
  localization_status: string;
  host?: string;
  pose_topic?: string;
}

export interface RobotBattery {
  percentage: number;
  percent: number;
  voltage: number;
  present: boolean;
  updated_at: number;
}

export interface WorkspaceRobot {
  id: string;
  name: string;
  connection_state: string;
  localization_status: string;
  current_scene_id: string | null;
  current_map_version_id: string | null;
  drawable: boolean;
  battery: RobotBattery | null;
  pose: {
    x: number;
    y: number;
    yaw: number;
    pixel_x: number | null;
    pixel_y: number | null;
    pixel_yaw: number | null;
    stale: boolean;
    scene_map_matched: boolean;
    updated_at: number;
  } | null;
}

export interface EventWhen {
  field?: string;
  op: string;
  value?: unknown;
}

export interface EventSpec {
  event_name: string;
  source: "RESULT" | "FEEDBACK" | string;
  enabled?: boolean;
  when: EventWhen;
  max_firings?: number;
  cooldown_ms?: number;
  emit_on_node?: boolean;
  start_workflows?: boolean;
}

export interface MapPointAction {
  id?: string;
  station_id?: string;
  sequence_no: number;
  action_name: string;
  parallel_group: number | null;
  capability_definition_id: string | null | "";
  capability_key: string;
  motion_ownership: "DISPATCHER" | "ROBOT_INTERNAL" | string;
  robot_selector_type: "FIXED" | "GROUP" | "RUNTIME_VAR" | string;
  robot_id: string | null | "";
  robot_name?: string;
  robot_group: string;
  runtime_variable: string;
  parameters: Record<string, unknown>;
  precondition: unknown;
  failure_policy: "FAIL" | "CONTINUE" | "RETRY" | string;
  retry_count: number;
  timeout_ms: number;
  success_event_name: string;
  event_specs?: EventSpec[];
  post_navigation_station_id: string | null;
}

export interface MapPoint {
  id: string;
  scene_id: string;
  map_version_id: string;
  name: string;
  x: number;
  y: number;
  yaw: number;
  tags: string[];
  notes: string;
  status: string;
  metadata: Record<string, unknown>;
  pixel_x: number | null;
  pixel_y: number | null;
  pixel_yaw: number | null;
  actions: MapPointAction[];
}

export interface CapabilityTemplate {
  id: string;
  profile_id: string;
  profile_name: string;
  capability_key: string;
  operation_kind: string;
  endpoint_name: string;
  ros_message_type: string;
  motion_ownership: "DISPATCHER" | "ROBOT_INTERNAL" | string;
  blocking_type: string;
  timeout_ms: number;
  parameter_schema: Record<string, unknown>;
  request_template: Record<string, unknown>;
  feedback_mapping: Record<string, unknown>;
  result_mapping: Record<string, unknown>;
  success_condition: Record<string, unknown> | null;
  failure_condition: Record<string, unknown> | null;
  retry_policy: Record<string, unknown>;
  cancel_policy: Record<string, unknown>;
  resource_claims: unknown[];
  protocol_config: Record<string, unknown>;
  event_specs?: EventSpec[];
}

export interface SceneWorkspace {
  scene: Scene;
  map: MapVersion | null;
  robots: WorkspaceRobot[];
  points: MapPoint[];
}

export interface WorldPose {
  x: number;
  y: number;
  yaw: number;
}

export interface NavigationGoalResponse {
  command_id: string;
  outbox_id: string;
  state: string;
  confirmation: {
    robot_id: string;
    robot_name: string;
    scene_id: string;
    map_version_id: string;
    map_version: number;
    x: number;
    y: number;
    yaw: number;
    distance_tolerance: number;
    heading_tolerance: number;
  };
}
