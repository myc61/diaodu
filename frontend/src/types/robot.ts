export interface RobotConfig {
  id: string;
  name: string;
  enabled: boolean;
  robot_type: string;
  ros_version: string;
  ros_distribution: string;
  ros_namespace: string;
  connection_state: string;
  configuration_state: string;
  business_status: string;
  localization_status: string;
  rosbridge_url: string;
  host: string | null;
  rosbridge_port: number | null;
  rosbridge_path: string | null;
  rosbridge_tls: boolean;
  pose_topic: string;
  pose_message_type: string;
  stale_timeout_ms: number;
  current_scene_id: string | null;
  current_map_version_id: string | null;
  pose_mapping: Record<string, string>;
  battery: {
    percentage: number;
    percent: number;
    voltage: number;
    present: boolean;
    updated_at: number;
  } | null;
}

export interface RobotUpsertPayload {
  name: string;
  enabled: boolean;
  robot_type: string;
  ros_version: string;
  ros_distribution: string;
  ros_namespace: string;
  host: string;
  rosbridge_port: number;
  rosbridge_path: string;
  rosbridge_tls: boolean;
  pose_topic: string;
  pose_message_type: string;
  stale_timeout_ms: number;
}
