#include "dispatcher/db/workspace_repository.hpp"

#include "dispatcher/domain/map_transform.hpp"
#include "dispatcher/domain/robot_connection_config.hpp"
#include "dispatcher/workflow/event_spec.hpp"

#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace dispatcher::db {
namespace {

std::optional<std::string> optionalText(const pqxx::field& field) {
  if (field.is_null()) {
    return std::nullopt;
  }
  return field.as<std::string>();
}

nlohmann::json defaultPoseMapping() {
  return nlohmann::json{
      {"x_path", "pose.pose.position.x"},
      {"y_path", "pose.pose.position.y"},
      {"quaternion_x_path", "pose.pose.orientation.x"},
      {"quaternion_y_path", "pose.pose.orientation.y"},
      {"quaternion_z_path", "pose.pose.orientation.z"},
      {"quaternion_w_path", "pose.pose.orientation.w"},
      {"frame_id_path", "header.frame_id"},
      {"timestamp_path", "header.stamp"},
  };
}

std::string buildRosbridgeUrl(const RobotUpsertRequest& request) {
  domain::RosbridgeEndpointConfig endpoint{
      .host = request.host,
      .port = static_cast<std::uint16_t>(request.rosbridge_port),
      .path = request.rosbridge_path.empty() ? "/" : request.rosbridge_path,
      .secure = request.rosbridge_tls,
  };
  return endpoint.websocketUrl();
}

std::string configurationStateFor(const RobotUpsertRequest& request) {
  if (request.host.empty() || request.rosbridge_port <= 0) {
    return "DRAFT";
  }
  if (request.pose_topic.empty() || request.pose_message_type.empty()) {
    return "DRAFT";
  }
  return "READY";
}

std::string joinTags(const std::vector<std::string>& tags) {
  std::ostringstream out;
  for (std::size_t i = 0; i < tags.size(); ++i) {
    if (i > 0) {
      out << ',';
    }
    out << tags[i];
  }
  return out.str();
}

std::vector<std::string> splitTags(const std::string& csv) {
  std::vector<std::string> tags;
  if (csv.empty()) {
    return tags;
  }
  std::stringstream stream(csv);
  std::string item;
  while (std::getline(stream, item, ',')) {
    if (!item.empty()) {
      tags.push_back(item);
    }
  }
  return tags;
}

void requireStationInsideMap(
    const pqxx::row& map,
    const StationUpsertRequest& request) {
  const domain::MapMetadata metadata{
      .width = static_cast<std::size_t>(map["width"].as<int>()),
      .height = static_cast<std::size_t>(map["height"].as<int>()),
      .resolution = map["resolution"].as<double>(),
      .origin_x = map["origin_x"].as<double>(),
      .origin_y = map["origin_y"].as<double>(),
      .origin_yaw = map["origin_yaw"].as<double>(),
  };
  const auto pixel = domain::worldToPixel(
      metadata,
      domain::WorldPose2D{
          .x = request.x,
          .y = request.y,
          .yaw = request.yaw,
      });
  if (!pixel.has_value() || !pixel->inside_map) {
    throw std::runtime_error("station coordinates are outside the map");
  }
}

StationRecord readStation(const pqxx::row& row) {
  return StationRecord{
      .id = row["id"].as<std::string>(),
      .scene_id = row["scene_id"].as<std::string>(),
      .map_version_id = row["map_version_id"].as<std::string>(),
      .name = row["name"].as<std::string>(),
      .x = row["x"].as<double>(),
      .y = row["y"].as<double>(),
      .yaw = row["yaw"].as<double>(),
      .tags = splitTags(row["tags_csv"].as<std::string>()),
      .notes = row["notes"].as<std::string>(),
      .status = row["status"].as<std::string>(),
      .metadata = nlohmann::json::parse(row["metadata"].as<std::string>()),
  };
}

StationActionRecord readStationAction(const pqxx::row& row) {
  StationActionRecord action{
      .id = row["id"].as<std::string>(),
      .station_id = row["station_id"].as<std::string>(),
      .sequence_no = row["sequence_no"].as<int>(),
      .action_name = row["action_name"].as<std::string>(),
      .capability_definition_id = optionalText(row["capability_definition_id"]),
      .capability_key = row["capability_key"].as<std::string>(),
      .motion_ownership = row["motion_ownership"].as<std::string>(),
      .robot_selector_type = row["robot_selector_type"].as<std::string>(),
      .robot_id = optionalText(row["robot_id"]),
      .robot_name = row["robot_name"].as<std::string>(),
      .robot_group = row["robot_group"].as<std::string>(),
      .runtime_variable = row["runtime_variable"].as<std::string>(),
      .parameters = nlohmann::json::parse(row["parameters"].as<std::string>()),
      .failure_policy = row["failure_policy"].as<std::string>(),
      .retry_count = row["retry_count"].as<int>(),
      .timeout_ms = row["timeout_ms"].as<int>(),
      .success_event_name = row["success_event_name"].as<std::string>(),
      .event_specs = nlohmann::json::array(),
      .post_navigation_station_id =
          optionalText(row["post_navigation_station_id"]),
  };
  if (!row["parallel_group"].is_null()) {
    action.parallel_group = row["parallel_group"].as<int>();
  }
  if (!row["precondition"].is_null()) {
    action.precondition =
        nlohmann::json::parse(row["precondition"].as<std::string>());
  }
  if (!row["event_specs"].is_null()) {
    action.event_specs =
        nlohmann::json::parse(row["event_specs"].as<std::string>());
  }
  return action;
}

constexpr const char* kStationSelect =
    "SELECT id::text AS id, scene_id::text AS scene_id, "
    "map_version_id::text AS map_version_id, name, x, y, yaw, "
    "COALESCE(array_to_string(tags, ','), '') AS tags_csv, notes, status, "
    "metadata::text AS metadata ";

constexpr const char* kStationActionSelect =
    "SELECT a.id::text AS id, a.station_id::text AS station_id, a.sequence_no, "
    "a.action_name, a.parallel_group, "
    "a.capability_definition_id::text AS capability_definition_id, "
    "a.capability_key, COALESCE(c.motion_ownership, 'DISPATCHER') AS motion_ownership, "
    "a.robot_selector_type, a.robot_id::text AS robot_id, "
    "COALESCE(r.name, '') AS robot_name, a.robot_group, "
    "a.runtime_variable, a.parameters::text AS parameters, "
    "a.precondition::text AS precondition, a.failure_policy, a.retry_count, "
    "a.timeout_ms, a.success_event_name, "
    "COALESCE(a.event_specs::text, '[]') AS event_specs, "
    "a.post_navigation_station_id::text AS post_navigation_station_id "
    "FROM dispatch.station_actions a "
    "LEFT JOIN dispatch.capability_definitions c "
    "  ON c.id = a.capability_definition_id "
    "LEFT JOIN dispatch.robots r ON r.id = a.robot_id ";

void validateUpsert(const RobotUpsertRequest& request) {
  domain::RobotConnectionConfig config{
      .name = request.name,
      .ros_version = request.ros_version == "ROS2" ? domain::RosVersion::Ros2
                                                   : domain::RosVersion::Ros1,
      .ros_distribution = request.ros_distribution,
      .ros_namespace = request.ros_namespace,
      .rosbridge =
          {
              .host = request.host,
              .port = static_cast<std::uint16_t>(
                  request.rosbridge_port > 0 ? request.rosbridge_port : 9090),
              .path = request.rosbridge_path.empty() ? "/"
                                                    : request.rosbridge_path,
              .secure = request.rosbridge_tls,
          },
  };
  if (!request.pose_topic.empty()) {
    config.pose = domain::PoseTopicConfig{
        .topic = request.pose_topic,
        .message_type = request.pose_message_type,
        .mapping_key = "default",
    };
  }
  const auto errors = config.validationErrors();
  if (!errors.empty()) {
    throw std::runtime_error(errors.front());
  }
  if (request.rosbridge_port <= 0 || request.rosbridge_port > 65535) {
    throw std::runtime_error("rosbridge port out of range");
  }
}

SceneRecord readScene(const pqxx::row& row) {
  return SceneRecord{
      .id = row["id"].as<std::string>(),
      .name = row["name"].as<std::string>(),
      .description = row["description"].as<std::string>(),
      .active_map_version_id = optionalText(row["active_map_version_id"]),
  };
}

MapVersionRecord readMap(const pqxx::row& row) {
  return MapVersionRecord{
      .id = row["id"].as<std::string>(),
      .scene_id = row["scene_id"].as<std::string>(),
      .version = row["version"].as<int>(),
      .status = row["status"].as<std::string>(),
      .pgm_path = row["pgm_path"].as<std::string>(),
      .yaml_path = row["yaml_path"].as<std::string>(),
      .preview_path = row["preview_path"].as<std::string>(),
      .sha256 = row["sha256"].as<std::string>(),
      .width = row["width"].as<int>(),
      .height = row["height"].as<int>(),
      .resolution = row["resolution"].as<double>(),
      .origin_x = row["origin_x"].as<double>(),
      .origin_y = row["origin_y"].as<double>(),
      .origin_yaw = row["origin_yaw"].as<double>(),
      .occupied_thresh = row["occupied_thresh"].as<double>(),
      .free_thresh = row["free_thresh"].as<double>(),
      .file_size_bytes = row["file_size_bytes"].as<std::uint64_t>(),
      .yaml_image = row["yaml_image"].as<std::string>(),
      .frame_id = row["frame_id"].as<std::string>(),
  };
}

nlohmann::json defaultEmptyGraph() {
  return nlohmann::json{
      {"nodes", nlohmann::json::array()},
      {"edges", nlohmann::json::array()}};
}

nlohmann::json normalizeGraph(const nlohmann::json& graph) {
  if (graph.empty() || graph.is_null()) {
    return defaultEmptyGraph();
  }
  return graph;
}

WorkflowSummaryRecord readWorkflowSummary(const pqxx::row& row) {
  WorkflowSummaryRecord summary{
      .id = row["id"].as<std::string>(),
      .name = row["name"].as<std::string>(),
      .description = row["description"].as<std::string>(),
      .scene_id = optionalText(row["scene_id"]),
      .trigger_type = row["trigger_type"].as<std::string>(),
      .trigger_config =
          nlohmann::json::parse(row["trigger_config"].as<std::string>()),
  };
  if (!row["draft_version"].is_null()) {
    summary.draft_version = row["draft_version"].as<int>();
  }
  if (!row["draft_status"].is_null()) {
    summary.draft_status = row["draft_status"].as<std::string>();
  }
  if (!row["published_version"].is_null()) {
    summary.published_version = row["published_version"].as<int>();
  }
  return summary;
}

constexpr const char* kWorkflowSummarySelect =
    "SELECT d.id::text AS id, d.name, d.description, "
    "d.scene_id::text AS scene_id, d.trigger_type, "
    "d.trigger_config::text AS trigger_config, "
    "draft.version AS draft_version, draft.status AS draft_status, "
    "published.version AS published_version ";

constexpr const char* kWorkflowFromJoin =
    "FROM dispatch.workflow_definitions d "
    "LEFT JOIN LATERAL ("
    "  SELECT version, status FROM dispatch.workflow_versions "
    "  WHERE workflow_definition_id = d.id AND status = 'DRAFT' "
    "  ORDER BY version DESC LIMIT 1"
    ") draft ON true "
    "LEFT JOIN LATERAL ("
    "  SELECT version FROM dispatch.workflow_versions "
    "  WHERE workflow_definition_id = d.id AND status = 'PUBLISHED' "
    "  ORDER BY version DESC LIMIT 1"
    ") published ON true ";

constexpr const char* kWorkflowDetailSelect =
    "SELECT d.id::text AS id, d.name, d.description, "
    "d.scene_id::text AS scene_id, d.trigger_type, "
    "d.trigger_config::text AS trigger_config, "
    "draft.id::text AS draft_version_id, "
    "draft.version AS draft_version, draft.status AS draft_status, "
    "draft.graph::text AS graph, "
    "published.version AS published_version, "
    "published.graph::text AS published_graph ";

constexpr const char* kWorkflowDetailFromJoin =
    "FROM dispatch.workflow_definitions d "
    "LEFT JOIN LATERAL ("
    "  SELECT id, version, status, graph FROM dispatch.workflow_versions "
    "  WHERE workflow_definition_id = d.id AND status = 'DRAFT' "
    "  ORDER BY version DESC LIMIT 1"
    ") draft ON true "
    "LEFT JOIN LATERAL ("
    "  SELECT version, graph FROM dispatch.workflow_versions "
    "  WHERE workflow_definition_id = d.id AND status = 'PUBLISHED' "
    "  ORDER BY version DESC LIMIT 1"
    ") published ON true ";

WorkflowDetailRecord readWorkflowDetailPreferDraft(const pqxx::row& row) {
  WorkflowDetailRecord detail{
      .summary = readWorkflowSummary(row),
  };
  if (!row["draft_version_id"].is_null()) {
    detail.draft_version_id = row["draft_version_id"].as<std::string>();
    detail.graph = nlohmann::json::parse(row["graph"].as<std::string>());
    return detail;
  }
  if (!row["published_graph"].is_null()) {
    detail.graph =
        nlohmann::json::parse(row["published_graph"].as<std::string>());
    return detail;
  }
  detail.graph = defaultEmptyGraph();
  return detail;
}

PublishedWorkflowRecord readPublishedWorkflow(const pqxx::row& row) {
  return PublishedWorkflowRecord{
      .definition_id = row["definition_id"].as<std::string>(),
      .name = row["name"].as<std::string>(),
      .description = row["description"].as<std::string>(),
      .scene_id = optionalText(row["scene_id"]),
      .trigger_type = row["trigger_type"].as<std::string>(),
      .trigger_config =
          nlohmann::json::parse(row["trigger_config"].as<std::string>()),
      .version_id = row["version_id"].as<std::string>(),
      .version = row["version"].as<int>(),
      .graph = nlohmann::json::parse(row["graph"].as<std::string>()),
  };
}

WorkflowRunRecord readWorkflowRun(const pqxx::row& row) {
  return WorkflowRunRecord{
      .id = row["id"].as<std::string>(),
      .workflow_version_id = row["workflow_version_id"].as<std::string>(),
      .workflow_definition_id = row["workflow_definition_id"].as<std::string>(),
      .workflow_name = row["workflow_name"].as<std::string>(),
      .workflow_version = row["workflow_version"].as<int>(),
      .trigger_type = row["trigger_type"].as<std::string>(),
      .trigger_metadata =
          nlohmann::json::parse(row["trigger_metadata"].as<std::string>()),
      .state = row["state"].as<std::string>(),
      .input_data = nlohmann::json::parse(row["input_data"].as<std::string>()),
      .context_data =
          nlohmann::json::parse(row["context_data"].as<std::string>()),
      .parent_run_id = optionalText(row["parent_run_id"]),
      .root_run_id = optionalText(row["root_run_id"]),
      .source_run_id = optionalText(row["source_run_id"]),
      .parent_node_run_id = optionalText(row["parent_node_run_id"]),
      .causation_event_id = optionalText(row["causation_event_id"]),
      .business_key = row["business_key"].as<std::string>(),
      .started_at = optionalText(row["started_at"]),
      .finished_at = optionalText(row["finished_at"]),
      .archived_at = optionalText(row["archived_at"]),
      .created_at = row["created_at"].as<std::string>(),
  };
}

RobotStartupProfileRecord readRobotStartupProfile(const pqxx::row& row) {
  return RobotStartupProfileRecord{
      .id = row["id"].as<std::string>(),
      .robot_id = row["robot_id"].as<std::string>(),
      .name = row["name"].as<std::string>(),
      .description = row["description"].as<std::string>(),
      .enabled = row["enabled"].as<bool>(),
      .ssh_port = row["ssh_port"].as<int>(),
      .ssh_username = row["ssh_username"].as<std::string>(),
      .credential_reference =
          row["credential_reference"].as<std::string>(),
      .known_hosts_reference =
          row["known_hosts_reference"].as<std::string>(),
      .steps = nlohmann::json::parse(row["steps"].as<std::string>()),
      .readiness_checks =
          nlohmann::json::parse(row["readiness_checks"].as<std::string>()),
      .stop_steps =
          nlohmann::json::parse(row["stop_steps"].as<std::string>()),
      .timeout_ms = row["timeout_ms"].as<int>(),
      .version = row["version"].as<int>(),
  };
}

NodeRunRecord readNodeRun(const pqxx::row& row) {
  NodeRunRecord node{
      .id = row["id"].as<std::string>(),
      .workflow_run_id = row["workflow_run_id"].as<std::string>(),
      .node_key = row["node_key"].as<std::string>(),
      .attempt = row["attempt"].as<int>(),
      .state = row["state"].as<std::string>(),
      .assigned_robot_id = optionalText(row["assigned_robot_id"]),
      .input_data = nlohmann::json::parse(row["input_data"].as<std::string>()),
      .output_data =
          nlohmann::json::parse(row["output_data"].as<std::string>()),
  };
  if (!row["error_data"].is_null()) {
    node.error_data =
        nlohmann::json::parse(row["error_data"].as<std::string>());
  }
  return node;
}

WorkflowEventRecord readWorkflowEvent(const pqxx::row& row) {
  return WorkflowEventRecord{
      .id = row["id"].as<std::string>(),
      .workflow_run_id = row["workflow_run_id"].as<std::string>(),
      .node_run_id = optionalText(row["node_run_id"]),
      .event_type = row["event_type"].as<std::string>(),
      .payload = nlohmann::json::parse(row["payload"].as<std::string>()),
      .occurred_at = row["occurred_at"].as<std::string>(),
  };
}

CapabilityTemplateRecord readCapabilityTemplate(const pqxx::row& row) {
  CapabilityTemplateRecord item{
      .id = row["id"].as<std::string>(),
      .profile_id = row["profile_id"].as<std::string>(),
      .profile_name = row["profile_name"].as<std::string>(),
      .capability_key = row["capability_key"].as<std::string>(),
      .operation_kind = row["operation_kind"].as<std::string>(),
      .endpoint_name = row["endpoint_name"].as<std::string>(),
      .ros_message_type = row["ros_message_type"].as<std::string>(),
      .motion_ownership = row["motion_ownership"].as<std::string>(),
      .blocking_type = row["blocking_type"].as<std::string>(),
      .timeout_ms = row["timeout_ms"].as<int>(),
      .parameter_schema =
          nlohmann::json::parse(row["parameter_schema"].as<std::string>()),
      .request_template =
          nlohmann::json::parse(row["request_template"].as<std::string>()),
      .feedback_mapping =
          nlohmann::json::parse(row["feedback_mapping"].as<std::string>()),
      .result_mapping =
          nlohmann::json::parse(row["result_mapping"].as<std::string>()),
      .retry_policy =
          nlohmann::json::parse(row["retry_policy"].as<std::string>()),
      .cancel_policy =
          nlohmann::json::parse(row["cancel_policy"].as<std::string>()),
      .resource_claims =
          nlohmann::json::parse(row["resource_claims"].as<std::string>()),
      .protocol_config =
          nlohmann::json::parse(row["protocol_config"].as<std::string>()),
      .event_specs = nlohmann::json::array(),
  };
  if (!row["success_condition"].is_null()) {
    item.success_condition =
        nlohmann::json::parse(row["success_condition"].as<std::string>());
  }
  if (!row["failure_condition"].is_null()) {
    item.failure_condition =
        nlohmann::json::parse(row["failure_condition"].as<std::string>());
  }
  if (!row["event_specs"].is_null()) {
    item.event_specs =
        nlohmann::json::parse(row["event_specs"].as<std::string>());
  }
  return item;
}

CommandRunRecord readCommandRun(const pqxx::row& row) {
  CommandRunRecord command{
      .id = row["id"].as<std::string>(),
      .command_id = row["command_id"].as<std::string>(),
      .workflow_run_id = row["workflow_run_id"].as<std::string>(),
      .node_run_id = row["node_run_id"].as<std::string>(),
      .robot_id = row["robot_id"].as<std::string>(),
      .capability_definition_id =
          optionalText(row["capability_definition_id"]),
      .operation_kind = row["operation_kind"].as<std::string>(),
      .endpoint_name = row["endpoint_name"].as<std::string>(),
      .correlation_id = row["correlation_id"].as<std::string>(),
      .state = row["state"].as<std::string>(),
      .request_payload =
          nlohmann::json::parse(row["request_payload"].as<std::string>()),
      .dispatched_at = optionalText(row["dispatched_at"]),
      .completed_at = optionalText(row["completed_at"]),
      .created_at = row["created_at"].as<std::string>(),
  };
  if (!row["last_feedback"].is_null()) {
    command.last_feedback =
        nlohmann::json::parse(row["last_feedback"].as<std::string>());
  }
  if (!row["result_payload"].is_null()) {
    command.result_payload =
        nlohmann::json::parse(row["result_payload"].as<std::string>());
  }
  if (!row["error_data"].is_null()) {
    command.error_data =
        nlohmann::json::parse(row["error_data"].as<std::string>());
  }
  return command;
}

constexpr const char* kPublishedWorkflowSelect =
    "SELECT d.id::text AS definition_id, d.name, d.description, "
    "d.scene_id::text AS scene_id, d.trigger_type, "
    "d.trigger_config::text AS trigger_config, "
    "v.id::text AS version_id, v.version, v.graph::text AS graph "
    "FROM dispatch.workflow_definitions d "
    "JOIN dispatch.workflow_versions v "
    "  ON v.workflow_definition_id = d.id AND v.status = 'PUBLISHED' ";

constexpr const char* kWorkflowRunSelect =
    "SELECT r.id::text AS id, r.workflow_version_id::text AS workflow_version_id, "
    "d.id::text AS workflow_definition_id, d.name AS workflow_name, "
    "v.version AS workflow_version, r.trigger_type, "
    "r.trigger_metadata::text AS trigger_metadata, r.state, "
    "r.input_data::text AS input_data, r.context_data::text AS context_data, "
    "r.parent_run_id::text AS parent_run_id, "
    "r.root_run_id::text AS root_run_id, "
    "r.source_run_id::text AS source_run_id, "
    "r.parent_node_run_id::text AS parent_node_run_id, "
    "r.causation_event_id::text AS causation_event_id, r.business_key, "
    "r.started_at::text AS started_at, r.finished_at::text AS finished_at, "
    "r.archived_at::text AS archived_at, "
    "r.created_at::text AS created_at ";

constexpr const char* kWorkflowRunFromJoin =
    "FROM dispatch.workflow_runs r "
    "JOIN dispatch.workflow_versions v ON v.id = r.workflow_version_id "
    "JOIN dispatch.workflow_definitions d ON d.id = v.workflow_definition_id ";

constexpr const char* kNodeRunSelect =
    "SELECT id::text AS id, workflow_run_id::text AS workflow_run_id, "
    "node_key, attempt, state, assigned_robot_id::text AS assigned_robot_id, "
    "input_data::text AS input_data, output_data::text AS output_data, "
    "error_data::text AS error_data "
    "FROM dispatch.node_runs ";

constexpr const char* kCapabilityTemplateSelect =
    "SELECT c.id::text AS id, c.profile_id::text AS profile_id, "
    "p.name AS profile_name, c.capability_key, c.operation_kind, "
    "c.endpoint_name, c.ros_message_type, "
    "COALESCE(c.motion_ownership, 'DISPATCHER') AS motion_ownership, "
    "c.blocking_type, c.timeout_ms, "
    "c.parameter_schema::text AS parameter_schema, "
    "c.request_template::text AS request_template, "
    "c.feedback_mapping::text AS feedback_mapping, "
    "c.result_mapping::text AS result_mapping, "
    "c.success_condition::text AS success_condition, "
    "c.failure_condition::text AS failure_condition, "
    "c.retry_policy::text AS retry_policy, "
    "c.cancel_policy::text AS cancel_policy, "
    "c.resource_claims::text AS resource_claims, "
    "c.protocol_config::text AS protocol_config, "
    "COALESCE(c.event_specs::text, '[]') AS event_specs "
    "FROM dispatch.capability_definitions c "
    "JOIN dispatch.capability_profiles p ON p.id = c.profile_id ";

constexpr const char* kCommandRunSelect =
    "SELECT id::text AS id, command_id::text AS command_id, "
    "workflow_run_id::text AS workflow_run_id, "
    "node_run_id::text AS node_run_id, robot_id::text AS robot_id, "
    "capability_definition_id::text AS capability_definition_id, "
    "operation_kind, endpoint_name, correlation_id, state, "
    "request_payload::text AS request_payload, "
    "last_feedback::text AS last_feedback, "
    "result_payload::text AS result_payload, error_data::text AS error_data, "
    "dispatched_at::text AS dispatched_at, completed_at::text AS completed_at, "
    "created_at::text AS created_at "
    "FROM dispatch.command_runs ";

constexpr const char* kRobotStartupProfileSelect =
    "SELECT id::text AS id, robot_id::text AS robot_id, name, description, "
    "enabled, ssh_port, ssh_username, credential_reference, "
    "known_hosts_reference, steps::text AS steps, "
    "readiness_checks::text AS readiness_checks, "
    "stop_steps::text AS stop_steps, timeout_ms, version "
    "FROM dispatch.robot_startup_profiles ";

}  // namespace

WorkspaceRepository::WorkspaceRepository(PgPool& pool) : pool_(pool) {}

std::vector<SceneRecord> WorkspaceRepository::listScenes() {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec(
      "SELECT id, name, description, active_map_version_id "
      "FROM dispatch.scenes ORDER BY name");
  tx.commit();
  std::vector<SceneRecord> scenes;
  scenes.reserve(rows.size());
  for (const auto& row : rows) {
    scenes.push_back(readScene(row));
  }
  return scenes;
}

std::optional<SceneRecord> WorkspaceRepository::getScene(const std::string& id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec_params(
      "SELECT id, name, description, active_map_version_id "
      "FROM dispatch.scenes WHERE id = $1::uuid",
      id);
  tx.commit();
  if (rows.empty()) {
    return std::nullopt;
  }
  return readScene(rows[0]);
}

SceneRecord WorkspaceRepository::createScene(
    const std::string& name,
    const std::string& description) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec_params(
      "INSERT INTO dispatch.scenes (name, description) "
      "VALUES ($1, $2) "
      "RETURNING id, name, description, active_map_version_id",
      name,
      description);
  tx.commit();
  return readScene(rows[0]);
}

void WorkspaceRepository::deleteScene(const std::string& id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto scenes = tx.exec_params(
      "SELECT id FROM dispatch.scenes WHERE id = $1::uuid",
      id);
  if (scenes.empty()) {
    throw std::runtime_error("scene not found");
  }

  tx.exec_params(
      "UPDATE dispatch.scene_robots SET valid_to = now() "
      "WHERE scene_id = $1::uuid AND valid_to IS NULL",
      id);
  tx.exec_params(
      "UPDATE dispatch.robots SET current_scene_id = NULL, "
      "current_map_version_id = NULL, localization_status = 'UNKNOWN', "
      "updated_at = now() "
      "WHERE current_scene_id = $1::uuid "
      "   OR current_map_version_id IN ("
      "        SELECT id FROM dispatch.map_versions WHERE scene_id = $1::uuid)",
      id);
  tx.exec_params(
      "UPDATE dispatch.scenes SET active_map_version_id = NULL, "
      "updated_at = now() WHERE id = $1::uuid",
      id);
  const auto result = tx.exec_params(
      "DELETE FROM dispatch.scenes WHERE id = $1::uuid RETURNING id",
      id);
  tx.commit();
  if (result.empty()) {
    throw std::runtime_error("scene not found");
  }
}

std::optional<MapVersionRecord> WorkspaceRepository::getMapVersion(
    const std::string& id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec_params(
      "SELECT id, scene_id, version, status, pgm_path, yaml_path, preview_path, "
      "sha256, width, height, resolution, origin_x, origin_y, origin_yaw, "
      "occupied_thresh, free_thresh, file_size_bytes, yaml_image, frame_id "
      "FROM dispatch.map_versions WHERE id = $1::uuid",
      id);
  tx.commit();
  if (rows.empty()) {
    return std::nullopt;
  }
  return readMap(rows[0]);
}

int WorkspaceRepository::nextMapVersion(const std::string& scene_id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec_params(
      "SELECT COALESCE(MAX(version), 0) + 1 AS next_version "
      "FROM dispatch.map_versions WHERE scene_id = $1::uuid",
      scene_id);
  tx.commit();
  return rows[0]["next_version"].as<int>();
}

MapVersionRecord WorkspaceRepository::insertMapVersion(
    const std::string& scene_id,
    int version,
    const maps::ImportedMapFiles& files) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec_params(
      "INSERT INTO dispatch.map_versions ("
      "scene_id, version, status, pgm_path, yaml_path, preview_path, sha256, "
      "width, height, resolution, origin_x, origin_y, origin_yaw, "
      "occupied_thresh, free_thresh, file_size_bytes, yaml_image, source_type"
      ") VALUES ("
      "$1::uuid, $2, 'READY', $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, "
      "$13, $14, $15, $16, 'UPLOAD'"
      ") RETURNING id, scene_id, version, status, pgm_path, yaml_path, "
      "preview_path, sha256, width, height, resolution, origin_x, origin_y, "
      "origin_yaw, occupied_thresh, free_thresh, file_size_bytes, yaml_image, "
      "frame_id",
      scene_id,
      version,
      files.pgm_path,
      files.yaml_path,
      files.preview_path,
      files.sha256,
      static_cast<int>(files.width),
      static_cast<int>(files.height),
      files.yaml.resolution,
      files.yaml.origin_x,
      files.yaml.origin_y,
      files.yaml.origin_yaw,
      files.yaml.occupied_thresh,
      files.yaml.free_thresh,
      static_cast<std::int64_t>(files.file_size_bytes),
      files.yaml.image);
  // Explicit import should become the scene's visible map immediately.
  tx.exec_params(
      "UPDATE dispatch.scenes SET active_map_version_id = $1::uuid, "
      "updated_at = now() WHERE id = $2::uuid",
      rows[0]["id"].as<std::string>(),
      scene_id);
  tx.commit();
  return readMap(rows[0]);
}

bool WorkspaceRepository::setActiveMap(
    const std::string& scene_id,
    const std::string& map_version_id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto maps = tx.exec_params(
      "SELECT id, status FROM dispatch.map_versions "
      "WHERE id = $1::uuid AND scene_id = $2::uuid",
      map_version_id,
      scene_id);
  if (maps.empty() || maps[0]["status"].as<std::string>() != "READY") {
    return false;
  }
  tx.exec_params(
      "UPDATE dispatch.scenes SET active_map_version_id = $1::uuid, "
      "updated_at = now() WHERE id = $2::uuid",
      map_version_id,
      scene_id);
  tx.commit();
  return true;
}

std::string WorkspaceRepository::removeOrArchiveMapVersion(
    const std::string& id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto maps = tx.exec_params(
      "SELECT id, status FROM dispatch.map_versions WHERE id = $1::uuid",
      id);
  if (maps.empty()) {
    throw std::runtime_error("map version not found");
  }

  const auto status = maps[0]["status"].as<std::string>();
  const auto active = tx.exec_params(
      "SELECT 1 FROM dispatch.scenes "
      "WHERE active_map_version_id = $1::uuid LIMIT 1",
      id);
  const auto stations = tx.exec_params(
      "SELECT 1 FROM dispatch.stations "
      "WHERE map_version_id = $1::uuid LIMIT 1",
      id);
  const auto robots = tx.exec_params(
      "SELECT 1 FROM dispatch.robots "
      "WHERE current_map_version_id = $1::uuid LIMIT 1",
      id);
  const bool referenced =
      !active.empty() || !stations.empty() || !robots.empty();
  // FAILED/IMPORTING are treated as drafts and may be hard-deleted.
  const bool draft_like = status == "FAILED" || status == "IMPORTING";

  if (referenced && !draft_like) {
    if (!active.empty()) {
      tx.exec_params(
          "UPDATE dispatch.scenes SET active_map_version_id = NULL, "
          "updated_at = now() WHERE active_map_version_id = $1::uuid",
          id);
    }
    if (!robots.empty()) {
      tx.exec_params(
          "UPDATE dispatch.robots SET current_map_version_id = NULL, "
          "localization_status = 'UNKNOWN', updated_at = now() "
          "WHERE current_map_version_id = $1::uuid",
          id);
    }
    tx.exec_params(
        "UPDATE dispatch.map_versions SET status = 'ARCHIVED' "
        "WHERE id = $1::uuid",
        id);
    tx.commit();
    return "archived";
  }

  if (!active.empty()) {
    tx.exec_params(
        "UPDATE dispatch.scenes SET active_map_version_id = NULL, "
        "updated_at = now() WHERE active_map_version_id = $1::uuid",
        id);
  }
  if (!robots.empty()) {
    tx.exec_params(
        "UPDATE dispatch.robots SET current_map_version_id = NULL, "
        "localization_status = 'UNKNOWN', updated_at = now() "
        "WHERE current_map_version_id = $1::uuid",
        id);
  }
  const auto deleted = tx.exec_params(
      "DELETE FROM dispatch.map_versions WHERE id = $1::uuid RETURNING id",
      id);
  tx.commit();
  if (deleted.empty()) {
    throw std::runtime_error("map version not found");
  }
  return "deleted";
}

std::vector<RobotRecord> WorkspaceRepository::listRobots() {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec(
      "SELECT r.id, r.name, r.enabled, r.robot_type, r.ros_version, "
      "r.current_scene_id, r.current_map_version_id, r.business_status, "
      "r.localization_status, "
      "c.state AS connection_state, c.configuration_state, c.rosbridge_url, "
      "c.host, c.rosbridge_port, c.rosbridge_path, c.rosbridge_tls, "
      "c.ros_distribution, c.ros_namespace, "
      "c.pose_topic, c.pose_message_type, c.pose_mapping, c.stale_timeout_ms, "
      "cd.endpoint_name AS nav_action, cd.ros_message_type AS nav_action_type "
      "FROM dispatch.robots r "
      "LEFT JOIN dispatch.robot_connections c ON c.robot_id = r.id "
      "LEFT JOIN dispatch.capability_definitions cd "
      "  ON cd.profile_id = r.capability_profile_id "
      " AND cd.capability_key = 'navigation' "
      "ORDER BY r.name");
  tx.commit();

  std::vector<RobotRecord> robots;
  robots.reserve(rows.size());
  for (const auto& row : rows) {
    RobotRecord robot{
        .id = row["id"].as<std::string>(),
        .name = row["name"].as<std::string>(),
        .enabled = row["enabled"].as<bool>(),
        .robot_type = row["robot_type"].as<std::string>(),
        .ros_version = row["ros_version"].as<std::string>(),
        .ros_distribution = row["ros_distribution"].is_null()
            ? "noetic"
            : row["ros_distribution"].as<std::string>(),
        .ros_namespace = row["ros_namespace"].is_null()
            ? ""
            : row["ros_namespace"].as<std::string>(),
        .current_scene_id = optionalText(row["current_scene_id"]),
        .current_map_version_id = optionalText(row["current_map_version_id"]),
        .business_status = row["business_status"].as<std::string>(),
        .localization_status = row["localization_status"].as<std::string>(),
        .connection_state = row["connection_state"].is_null()
            ? "DISCONNECTED"
            : row["connection_state"].as<std::string>(),
        .configuration_state = row["configuration_state"].is_null()
            ? "DRAFT"
            : row["configuration_state"].as<std::string>(),
        .rosbridge_url = row["rosbridge_url"].is_null()
            ? ""
            : row["rosbridge_url"].as<std::string>(),
        .host = optionalText(row["host"]),
        .rosbridge_port = row["rosbridge_port"].is_null()
            ? std::nullopt
            : std::optional<int>(row["rosbridge_port"].as<int>()),
        .rosbridge_path = optionalText(row["rosbridge_path"]),
        .rosbridge_tls = !row["rosbridge_tls"].is_null() &&
            row["rosbridge_tls"].as<bool>(),
        .pose_topic = optionalText(row["pose_topic"]),
        .pose_message_type = optionalText(row["pose_message_type"]),
        .stale_timeout_ms = row["stale_timeout_ms"].is_null()
            ? 3000
            : row["stale_timeout_ms"].as<int>(),
        .nav_action = optionalText(row["nav_action"]),
        .nav_action_type = optionalText(row["nav_action_type"]),
    };
    if (!row["pose_mapping"].is_null()) {
      robot.pose_mapping =
          nlohmann::json::parse(row["pose_mapping"].as<std::string>());
    }
    robots.push_back(std::move(robot));
  }
  return robots;
}

std::optional<RobotRecord> WorkspaceRepository::getRobot(const std::string& id) {
  const auto robots = listRobots();
  for (const auto& robot : robots) {
    if (robot.id == id) {
      return robot;
    }
  }
  return std::nullopt;
}

RobotRecord WorkspaceRepository::createRobot(const RobotUpsertRequest& request) {
  validateUpsert(request);
  auto upsert = request;
  if (upsert.pose_mapping.empty() || upsert.pose_mapping.is_null()) {
    upsert.pose_mapping = defaultPoseMapping();
  }
  if (upsert.pose_message_type.empty()) {
    upsert.pose_message_type = "geometry_msgs/PoseWithCovarianceStamped";
  }
  const auto url = buildRosbridgeUrl(upsert);
  const auto config_state = configurationStateFor(upsert);

  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto profile = tx.exec(
      "INSERT INTO dispatch.capability_profiles (name, description) "
      "VALUES ('zj_humanoid_noetic', 'zj_humanoid Noetic profile') "
      "ON CONFLICT (name) DO UPDATE SET description = EXCLUDED.description "
      "RETURNING id::text AS id");
  const auto profile_id = profile[0]["id"].as<std::string>();
  tx.exec_params(
      "INSERT INTO dispatch.capability_definitions ("
      "profile_id, capability_key, operation_kind, endpoint_name, "
      "ros_message_type, parameter_schema, request_template, timeout_ms, "
      "blocking_type, resource_claims, protocol_config"
      ") VALUES ("
      "$1::uuid, 'navigation', 'ACTION', "
      "'/zj_humanoid/navigation/navigation', "
      "'navigation/NavigationAction', '{}'::jsonb, '{}'::jsonb, 120000, "
      "'NAVIGATION', "
      "'[{\"name\":\"navigation\",\"access\":\"EXCLUSIVE\"}]'::jsonb, "
      "'{\"adapter\":\"ROS1_ACTIONLIB\","
      "\"goal_topic\":\"/zj_humanoid/navigation/navigation/goal\","
      "\"cancel_topic\":\"/zj_humanoid/navigation/navigation/cancel\","
      "\"status_topic\":\"/zj_humanoid/navigation/navigation/status\","
      "\"feedback_topic\":\"/zj_humanoid/navigation/navigation/feedback\","
      "\"result_topic\":\"/zj_humanoid/navigation/navigation/result\"}'::jsonb"
      ") ON CONFLICT (profile_id, capability_key) DO NOTHING",
      profile_id);

  const auto robot_rows = tx.exec_params(
      "INSERT INTO dispatch.robots ("
      "name, enabled, robot_type, ros_version, capability_profile_id, "
      "business_status, localization_status"
      ") VALUES ($1, $2, $3, $4, $5::uuid, 'IDLE', 'UNKNOWN') "
      "RETURNING id::text AS id",
      upsert.name,
      upsert.enabled,
      upsert.robot_type,
      upsert.ros_version,
      profile_id);
  const auto robot_id = robot_rows[0]["id"].as<std::string>();

  tx.exec_params(
      "INSERT INTO dispatch.robot_connections ("
      "robot_id, rosbridge_url, host, rosbridge_port, rosbridge_path, "
      "rosbridge_tls, ros_namespace, pose_topic, pose_message_type, "
      "pose_mapping, stale_timeout_ms, state, configuration_state, "
      "ros_distribution"
      ") VALUES ("
      "$1::uuid, $2, $3, $4, $5, $6, $7, NULLIF($8, ''), NULLIF($9, ''), "
      "$10::jsonb, $11, 'DISCONNECTED', $12, $13"
      ")",
      robot_id,
      url,
      upsert.host,
      upsert.rosbridge_port,
      upsert.rosbridge_path.empty() ? "/" : upsert.rosbridge_path,
      upsert.rosbridge_tls,
      upsert.ros_namespace,
      upsert.pose_topic,
      upsert.pose_message_type,
      upsert.pose_mapping.dump(),
      upsert.stale_timeout_ms,
      config_state,
      upsert.ros_distribution);
  tx.commit();

  auto created = getRobot(robot_id);
  if (!created.has_value()) {
    throw std::runtime_error("failed to load created robot");
  }
  return *created;
}

RobotRecord WorkspaceRepository::updateRobot(
    const std::string& id,
    const RobotUpsertRequest& request) {
  validateUpsert(request);
  auto upsert = request;
  if (upsert.pose_mapping.empty() || upsert.pose_mapping.is_null()) {
    upsert.pose_mapping = defaultPoseMapping();
  }
  if (upsert.pose_message_type.empty()) {
    upsert.pose_message_type = "geometry_msgs/PoseWithCovarianceStamped";
  }
  const auto url = buildRosbridgeUrl(upsert);
  const auto config_state = configurationStateFor(upsert);

  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto existing = tx.exec_params(
      "SELECT id FROM dispatch.robots WHERE id = $1::uuid",
      id);
  if (existing.empty()) {
    throw std::runtime_error("robot not found");
  }

  tx.exec_params(
      "UPDATE dispatch.robots SET name = $1, enabled = $2, robot_type = $3, "
      "ros_version = $4, updated_at = now() WHERE id = $5::uuid",
      upsert.name,
      upsert.enabled,
      upsert.robot_type,
      upsert.ros_version,
      id);
  tx.exec_params(
      "UPDATE dispatch.robot_connections SET "
      "rosbridge_url = $1, host = $2, rosbridge_port = $3, rosbridge_path = $4, "
      "rosbridge_tls = $5, ros_namespace = $6, pose_topic = NULLIF($7, ''), "
      "pose_message_type = NULLIF($8, ''), pose_mapping = $9::jsonb, "
      "stale_timeout_ms = $10, configuration_state = $11, "
      "ros_distribution = $12, updated_at = now() "
      "WHERE robot_id = $13::uuid",
      url,
      upsert.host,
      upsert.rosbridge_port,
      upsert.rosbridge_path.empty() ? "/" : upsert.rosbridge_path,
      upsert.rosbridge_tls,
      upsert.ros_namespace,
      upsert.pose_topic,
      upsert.pose_message_type,
      upsert.pose_mapping.dump(),
      upsert.stale_timeout_ms,
      config_state,
      upsert.ros_distribution,
      id);
  tx.commit();

  auto updated = getRobot(id);
  if (!updated.has_value()) {
    throw std::runtime_error("failed to load updated robot");
  }
  return *updated;
}

bool WorkspaceRepository::deleteRobot(const std::string& id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto active = tx.exec_params(
      "SELECT 1 FROM dispatch.scene_robots "
      "WHERE robot_id = $1::uuid AND valid_to IS NULL",
      id);
  if (!active.empty()) {
    throw std::runtime_error(
        "robot still belongs to an active scene; remove it from the scene first");
  }
  const auto result = tx.exec_params(
      "DELETE FROM dispatch.robots WHERE id = $1::uuid RETURNING id",
      id);
  tx.commit();
  return !result.empty();
}

std::vector<RobotStartupProfileRecord>
WorkspaceRepository::listRobotStartupProfiles(const std::string& robot_id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec_params(
      std::string(kRobotStartupProfileSelect) +
          "WHERE robot_id = $1::uuid AND version = ("
          "SELECT MAX(latest.version) FROM dispatch.robot_startup_profiles latest "
          "WHERE latest.robot_id = robot_startup_profiles.robot_id "
          "AND latest.name = robot_startup_profiles.name"
          ") ORDER BY name",
      robot_id);
  tx.commit();
  std::vector<RobotStartupProfileRecord> profiles;
  profiles.reserve(rows.size());
  for (const auto& row : rows) {
    profiles.push_back(readRobotStartupProfile(row));
  }
  return profiles;
}

std::optional<RobotStartupProfileRecord>
WorkspaceRepository::getRobotStartupProfile(const std::string& id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec_params(
      std::string(kRobotStartupProfileSelect) + "WHERE id = $1::uuid",
      id);
  tx.commit();
  if (rows.empty()) {
    return std::nullopt;
  }
  return readRobotStartupProfile(rows[0]);
}

RobotStartupProfileRecord WorkspaceRepository::createRobotStartupProfile(
    const std::string& robot_id,
    const RobotStartupProfileUpsert& request) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec_params(
      "INSERT INTO dispatch.robot_startup_profiles ("
      "robot_id, name, description, enabled, ssh_port, ssh_username, "
      "credential_reference, known_hosts_reference, steps, readiness_checks, "
      "stop_steps, timeout_ms"
      ") VALUES ("
      "$1::uuid, $2, $3, $4, $5, $6, $7, $8, $9::jsonb, $10::jsonb, "
      "$11::jsonb, $12"
      ") RETURNING id::text AS id",
      robot_id,
      request.name,
      request.description,
      request.enabled,
      request.ssh_port,
      request.ssh_username,
      request.credential_reference,
      request.known_hosts_reference,
      request.steps.dump(),
      request.readiness_checks.dump(),
      request.stop_steps.dump(),
      request.timeout_ms);
  tx.commit();
  auto created = getRobotStartupProfile(rows[0]["id"].as<std::string>());
  if (!created.has_value()) {
    throw std::runtime_error("failed to load created startup profile");
  }
  return *created;
}

RobotStartupProfileRecord WorkspaceRepository::updateRobotStartupProfile(
    const std::string& robot_id,
    const std::string& id,
    const RobotStartupProfileUpsert& request) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec_params(
      "INSERT INTO dispatch.robot_startup_profiles ("
      "robot_id, name, description, enabled, ssh_port, ssh_username, "
      "credential_reference, known_hosts_reference, steps, readiness_checks, "
      "stop_steps, timeout_ms, version"
      ") SELECT source.robot_id, $1, $2, $3, $4, $5, $6, $7, $8::jsonb, "
      "$9::jsonb, $10::jsonb, $11, COALESCE(("
      "SELECT MAX(latest.version) + 1 "
      "FROM dispatch.robot_startup_profiles latest "
      "WHERE latest.robot_id = source.robot_id AND latest.name = $1"
      "), 1) "
      "FROM dispatch.robot_startup_profiles source "
      "WHERE source.id = $12::uuid AND source.robot_id = $13::uuid "
      "RETURNING id::text AS id",
      request.name,
      request.description,
      request.enabled,
      request.ssh_port,
      request.ssh_username,
      request.credential_reference,
      request.known_hosts_reference,
      request.steps.dump(),
      request.readiness_checks.dump(),
      request.stop_steps.dump(),
      request.timeout_ms,
      id,
      robot_id);
  tx.commit();
  if (rows.empty()) {
    throw std::runtime_error("startup profile not found");
  }
  auto updated =
      getRobotStartupProfile(rows[0]["id"].as<std::string>());
  if (!updated.has_value()) {
    throw std::runtime_error("failed to load updated startup profile");
  }
  return *updated;
}

bool WorkspaceRepository::deleteRobotStartupProfile(
    const std::string& robot_id, const std::string& id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto referenced = tx.exec_params(
      "SELECT 1 FROM dispatch.workflow_versions v "
      "WHERE v.status = 'PUBLISHED' AND EXISTS ("
      "SELECT 1 FROM jsonb_array_elements(COALESCE(v.graph->'nodes', '[]'::jsonb)) node "
      "WHERE node->>'type' IN ('ROBOT_STARTUP', 'ROBOT_SSH') "
      "AND node #>> '{data,startup_profile_id}' = $1"
      ") OR EXISTS ("
      "SELECT 1 FROM dispatch.capability_definitions c "
      "WHERE c.operation_kind = 'SSH' "
      "AND c.protocol_config->>'ssh_profile_id' = $1"
      ") LIMIT 1",
      id);
  if (!referenced.empty()) {
    throw std::runtime_error(
        "startup profile is referenced by a published workflow");
  }
  const auto rows = tx.exec_params(
      "DELETE FROM dispatch.robot_startup_profiles "
      "WHERE id = $1::uuid AND robot_id = $2::uuid RETURNING id",
      id,
      robot_id);
  tx.commit();
  return !rows.empty();
}

std::vector<MapVersionRecord> WorkspaceRepository::listMapVersions(
    const std::string& scene_id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec_params(
      "SELECT id, scene_id, version, status, pgm_path, yaml_path, preview_path, "
      "sha256, width, height, resolution, origin_x, origin_y, origin_yaw, "
      "occupied_thresh, free_thresh, file_size_bytes, yaml_image, frame_id "
      "FROM dispatch.map_versions WHERE scene_id = $1::uuid "
      "ORDER BY version DESC",
      scene_id);
  tx.commit();
  std::vector<MapVersionRecord> maps;
  maps.reserve(rows.size());
  for (const auto& row : rows) {
    maps.push_back(readMap(row));
  }
  return maps;
}

std::vector<std::string> WorkspaceRepository::listSceneRobotIds(
    const std::string& scene_id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec_params(
      "SELECT robot_id::text AS robot_id FROM dispatch.scene_robots "
      "WHERE scene_id = $1::uuid AND valid_to IS NULL "
      "ORDER BY valid_from",
      scene_id);
  tx.commit();
  std::vector<std::string> ids;
  ids.reserve(rows.size());
  for (const auto& row : rows) {
    ids.push_back(row["robot_id"].as<std::string>());
  }
  return ids;
}

void WorkspaceRepository::replaceSceneRobots(
    const std::string& scene_id,
    const std::vector<std::string>& robot_ids) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);

  for (const auto& robot_id : robot_ids) {
    const auto conflict = tx.exec_params(
        "SELECT scene_id::text AS scene_id FROM dispatch.scene_robots "
        "WHERE robot_id = $1::uuid AND valid_to IS NULL AND scene_id <> $2::uuid",
        robot_id,
        scene_id);
    if (!conflict.empty()) {
      throw std::runtime_error(
          "robot already belongs to another active scene: " + robot_id);
    }
  }

  tx.exec_params(
      "UPDATE dispatch.scene_robots SET valid_to = now() "
      "WHERE scene_id = $1::uuid AND valid_to IS NULL",
      scene_id);

  const auto scene = tx.exec_params(
      "SELECT active_map_version_id::text AS map_id FROM dispatch.scenes "
      "WHERE id = $1::uuid",
      scene_id);
  const auto map_id = scene[0]["map_id"].is_null()
      ? std::optional<std::string>{}
      : std::optional<std::string>{scene[0]["map_id"].as<std::string>()};

  for (const auto& robot_id : robot_ids) {
    tx.exec_params(
        "INSERT INTO dispatch.scene_robots (scene_id, robot_id) "
        "VALUES ($1::uuid, $2::uuid)",
        scene_id,
        robot_id);
    if (map_id.has_value()) {
      tx.exec_params(
          "UPDATE dispatch.robots SET current_scene_id = $1::uuid, "
          "current_map_version_id = $2::uuid, "
          "localization_status = 'UNLOCALIZED', updated_at = now() "
          "WHERE id = $3::uuid",
          scene_id,
          *map_id,
          robot_id);
    } else {
      tx.exec_params(
          "UPDATE dispatch.robots SET current_scene_id = $1::uuid, "
          "current_map_version_id = NULL, "
          "localization_status = 'UNLOCALIZED', updated_at = now() "
          "WHERE id = $2::uuid",
          scene_id,
          robot_id);
    }
  }

  tx.exec_params(
      "UPDATE dispatch.robots SET current_scene_id = NULL, "
      "current_map_version_id = NULL, localization_status = 'UNKNOWN', "
      "updated_at = now() "
      "WHERE current_scene_id = $1::uuid AND id NOT IN ("
      "  SELECT robot_id FROM dispatch.scene_robots "
      "  WHERE scene_id = $1::uuid AND valid_to IS NULL"
      ")",
      scene_id);
  tx.commit();
}

NavigationGoalRecord WorkspaceRepository::enqueueNavigationGoal(
    const std::string& robot_id,
    const std::string& scene_id,
    const std::string& map_version_id,
    double x,
    double y,
    double yaw,
    double distance_tolerance,
    double heading_tolerance) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto membership = tx.exec_params(
      "SELECT 1 FROM dispatch.scene_robots "
      "WHERE scene_id = $1::uuid AND robot_id = $2::uuid AND valid_to IS NULL",
      scene_id,
      robot_id);
  if (membership.empty()) {
    throw std::runtime_error("robot is not a member of the scene");
  }
  const auto scene = tx.exec_params(
      "SELECT active_map_version_id::text AS map_id FROM dispatch.scenes "
      "WHERE id = $1::uuid",
      scene_id);
  if (scene.empty() || scene[0]["map_id"].is_null() ||
      scene[0]["map_id"].as<std::string>() != map_version_id) {
    throw std::runtime_error("map version is not the scene active map");
  }

  nlohmann::json payload{
      {"scene_id", scene_id},
      {"map_version_id", map_version_id},
      {"x", x},
      {"y", y},
      {"yaw", yaw},
      {"distance_tolerance", distance_tolerance},
      {"heading_tolerance", heading_tolerance},
  };

  const auto rows = tx.exec_params(
      "INSERT INTO dispatch.command_outbox ("
      "command_id, target_type, target_id, operation_kind, payload, state"
      ") VALUES ("
      "gen_random_uuid(), 'ROBOT', $1::uuid, 'NAVIGATION', $2::jsonb, 'PENDING'"
      ") RETURNING id::text AS outbox_id, command_id::text AS command_id, state",
      robot_id,
      payload.dump());
  tx.commit();
  return NavigationGoalRecord{
      .command_id = rows[0]["command_id"].as<std::string>(),
      .outbox_id = rows[0]["outbox_id"].as<std::string>(),
      .state = rows[0]["state"].as<std::string>(),
  };
}

void WorkspaceRepository::updateOutboxState(
    const std::string& outbox_id,
    const std::string& state,
    const std::optional<nlohmann::json>& last_error) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  if (last_error.has_value()) {
    tx.exec_params(
        "UPDATE dispatch.command_outbox SET state = $1, last_error = $2::jsonb, "
        "updated_at = now(), sent_at = CASE WHEN $1 = 'SENT' THEN now() ELSE sent_at END "
        "WHERE id = $3::uuid",
        state,
        last_error->dump(),
        outbox_id);
  } else {
    tx.exec_params(
        "UPDATE dispatch.command_outbox SET state = $1, updated_at = now(), "
        "sent_at = CASE WHEN $1 = 'SENT' THEN now() ELSE sent_at END "
        "WHERE id = $2::uuid",
        state,
        outbox_id);
  }
  tx.commit();
}

void WorkspaceRepository::updateRobotConnectionState(
    const std::string& robot_id,
    const std::string& connection_state) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  tx.exec_params(
      "UPDATE dispatch.robot_connections SET state = $2, updated_at = now() "
      "WHERE robot_id = $1::uuid",
      robot_id,
      connection_state);
  if (connection_state != "ONLINE") {
    tx.exec_params(
        "UPDATE dispatch.robots SET localization_status = 'UNKNOWN', "
        "updated_at = now() WHERE id = $1::uuid",
        robot_id);
  }
  tx.commit();
}

std::optional<nlohmann::json> WorkspaceRepository::getRobotInterfaceCache(
    const std::string& robot_id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec_params(
      "SELECT metadata->'interface_cache' AS cache "
      "FROM dispatch.robots WHERE id = $1::uuid",
      robot_id);
  tx.commit();
  if (rows.empty() || rows[0]["cache"].is_null()) {
    return std::nullopt;
  }
  return nlohmann::json::parse(rows[0]["cache"].as<std::string>());
}

void WorkspaceRepository::setRobotInterfaceCache(
    const std::string& robot_id,
    const nlohmann::json& cache) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  tx.exec_params(
      "UPDATE dispatch.robots SET "
      "metadata = jsonb_set("
      "  COALESCE(metadata, '{}'::jsonb),"
      "  '{interface_cache}',"
      "  $2::jsonb,"
      "  true"
      "), updated_at = now() "
      "WHERE id = $1::uuid",
      robot_id,
      cache.dump());
  tx.commit();
}

void WorkspaceRepository::updateRobotPoseCache(
      const std::string& robot_id,
      const nlohmann::json& pose,
      const std::string& localization_status) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  tx.exec_params(
      "UPDATE dispatch.robots SET last_pose = $1::jsonb, last_pose_at = now(), "
      "localization_status = $2, updated_at = now() WHERE id = $3::uuid",
      pose.dump(),
      localization_status,
      robot_id);
  tx.commit();
}

std::vector<StationRecord> WorkspaceRepository::listStations(
    const std::string& scene_id,
    const std::optional<std::string>& map_version_id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  pqxx::result rows;
  if (map_version_id.has_value()) {
    rows = tx.exec_params(
        std::string(kStationSelect) +
            "FROM dispatch.stations "
            "WHERE scene_id = $1::uuid AND map_version_id = $2::uuid "
            "AND status = 'ACTIVE' ORDER BY name",
        scene_id,
        *map_version_id);
  } else {
    rows = tx.exec_params(
        std::string(kStationSelect) +
            "FROM dispatch.stations "
            "WHERE scene_id = $1::uuid AND status = 'ACTIVE' ORDER BY name",
        scene_id);
  }
  tx.commit();
  std::vector<StationRecord> stations;
  stations.reserve(rows.size());
  for (const auto& row : rows) {
    stations.push_back(readStation(row));
  }
  return stations;
}

std::optional<StationRecord> WorkspaceRepository::getStation(
    const std::string& id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec_params(
      std::string(kStationSelect) +
          "FROM dispatch.stations WHERE id = $1::uuid",
      id);
  tx.commit();
  if (rows.empty()) {
    return std::nullopt;
  }
  return readStation(rows[0]);
}

StationRecord WorkspaceRepository::createStation(
    const StationUpsertRequest& request) {
  if (request.name.empty()) {
    throw std::runtime_error("name is required");
  }
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto maps = tx.exec_params(
      "SELECT id, scene_id, width, height, resolution, origin_x, origin_y, "
      "origin_yaw FROM dispatch.map_versions "
      "WHERE id = $1::uuid AND scene_id = $2::uuid AND status = 'READY'",
      request.map_version_id,
      request.scene_id);
  if (maps.empty()) {
    throw std::runtime_error("map version not found or not READY for scene");
  }
  requireStationInsideMap(maps[0], request);
  const auto rows = tx.exec_params(
      "INSERT INTO dispatch.stations ("
      "scene_id, map_version_id, name, x, y, yaw, tags, notes, metadata"
      ") VALUES ("
      "$1::uuid, $2::uuid, $3, $4, $5, $6, "
      "string_to_array($7, ','), $8, $9::jsonb"
      ") RETURNING id::text AS id, scene_id::text AS scene_id, "
      "map_version_id::text AS map_version_id, name, x, y, yaw, "
      "COALESCE(array_to_string(tags, ','), '') AS tags_csv, notes, status, "
      "metadata::text AS metadata",
      request.scene_id,
      request.map_version_id,
      request.name,
      request.x,
      request.y,
      request.yaw,
      joinTags(request.tags),
      request.notes,
      request.metadata.dump());
  tx.commit();
  return readStation(rows[0]);
}

StationRecord WorkspaceRepository::updateStation(
    const std::string& id,
    const StationUpsertRequest& request) {
  if (request.name.empty()) {
    throw std::runtime_error("name is required");
  }
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto maps = tx.exec_params(
      "SELECT m.width, m.height, m.resolution, m.origin_x, m.origin_y, "
      "m.origin_yaw FROM dispatch.stations s "
      "JOIN dispatch.map_versions m ON m.id = s.map_version_id "
      "WHERE s.id = $1::uuid AND s.status = 'ACTIVE'",
      id);
  if (maps.empty()) {
    throw std::runtime_error("station not found");
  }
  requireStationInsideMap(maps[0], request);
  const auto rows = tx.exec_params(
      "UPDATE dispatch.stations SET "
      "name = $2, x = $3, y = $4, yaw = $5, "
      "tags = string_to_array($6, ','), notes = $7, metadata = $8::jsonb, "
      "updated_at = now() "
      "WHERE id = $1::uuid AND status = 'ACTIVE' "
      "RETURNING id::text AS id, scene_id::text AS scene_id, "
      "map_version_id::text AS map_version_id, name, x, y, yaw, "
      "COALESCE(array_to_string(tags, ','), '') AS tags_csv, notes, status, "
      "metadata::text AS metadata",
      id,
      request.name,
      request.x,
      request.y,
      request.yaw,
      joinTags(request.tags),
      request.notes,
      request.metadata.dump());
  tx.commit();
  if (rows.empty()) {
    throw std::runtime_error("station not found");
  }
  return readStation(rows[0]);
}

std::string WorkspaceRepository::removeOrArchiveStation(const std::string& id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto referenced = tx.exec_params(
      "SELECT 1 FROM dispatch.station_actions "
      "WHERE post_navigation_station_id = $1::uuid LIMIT 1",
      id);
  if (!referenced.empty()) {
    tx.exec_params(
        "UPDATE dispatch.stations SET status = 'ARCHIVED', updated_at = now() "
        "WHERE id = $1::uuid",
        id);
    tx.commit();
    return "archived";
  }
  const auto deleted = tx.exec_params(
      "DELETE FROM dispatch.stations WHERE id = $1::uuid RETURNING id",
      id);
  tx.commit();
  if (deleted.empty()) {
    throw std::runtime_error("station not found");
  }
  return "deleted";
}

std::vector<StationActionRecord> WorkspaceRepository::listStationActions(
    const std::string& station_id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec_params(
      std::string(kStationActionSelect) +
          "WHERE a.station_id = $1::uuid ORDER BY a.sequence_no",
      station_id);
  tx.commit();
  std::vector<StationActionRecord> actions;
  actions.reserve(rows.size());
  for (const auto& row : rows) {
    actions.push_back(readStationAction(row));
  }
  return actions;
}

std::optional<StationActionRecord> WorkspaceRepository::getStationAction(
    const std::string& id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec_params(
      std::string(kStationActionSelect) + "WHERE a.id = $1::uuid",
      id);
  tx.commit();
  if (rows.empty()) {
    return std::nullopt;
  }
  return readStationAction(rows[0]);
}

std::vector<StationActionRecord> WorkspaceRepository::replaceStationActions(
    const std::string& station_id,
    const std::vector<StationActionUpsert>& actions) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto stations = tx.exec_params(
      "SELECT id, scene_id FROM dispatch.stations "
      "WHERE id = $1::uuid AND status = 'ACTIVE'",
      station_id);
  if (stations.empty()) {
    throw std::runtime_error("station not found");
  }
  const auto scene_id = stations[0]["scene_id"].as<std::string>();

  tx.exec_params(
      "DELETE FROM dispatch.station_actions WHERE station_id = $1::uuid",
      station_id);

  for (const auto& action : actions) {
    if (action.action_name.empty()) {
      throw std::runtime_error("action_name is required");
    }
    if (action.capability_key.empty()) {
      throw std::runtime_error("capability_key is required");
    }
    if (action.sequence_no <= 0) {
      throw std::runtime_error("sequence_no must be positive");
    }
    if (action.timeout_ms <= 0) {
      throw std::runtime_error("timeout_ms must be positive");
    }
    if (action.retry_count < 0) {
      throw std::runtime_error("retry_count must be >= 0");
    }

    std::string capability_id;
    std::string motion_ownership = "DISPATCHER";
    std::string operation_kind;
    if (action.capability_definition_id.has_value()) {
      const auto caps = tx.exec_params(
          "SELECT id::text AS id, motion_ownership, capability_key, "
          "operation_kind "
          "FROM dispatch.capability_definitions WHERE id = $1::uuid",
          *action.capability_definition_id);
      if (caps.empty()) {
        throw std::runtime_error("capability definition not found");
      }
      capability_id = caps[0]["id"].as<std::string>();
      motion_ownership = caps[0]["motion_ownership"].as<std::string>();
      operation_kind = caps[0]["operation_kind"].as<std::string>();
      if (caps[0]["capability_key"].as<std::string>() != action.capability_key) {
        throw std::runtime_error("capability_key does not match definition");
      }
    } else {
      const auto caps = tx.exec_params(
          "SELECT id::text AS id, motion_ownership, operation_kind "
          "FROM dispatch.capability_definitions "
          "WHERE capability_key = $1 ORDER BY created_at LIMIT 1",
          action.capability_key);
      if (!caps.empty()) {
        capability_id = caps[0]["id"].as<std::string>();
        motion_ownership = caps[0]["motion_ownership"].as<std::string>();
        operation_kind = caps[0]["operation_kind"].as<std::string>();
      }
    }

    if (action.post_navigation_station_id.has_value()) {
      if (motion_ownership == "ROBOT_INTERNAL") {
        throw std::runtime_error(
            "ROBOT_INTERNAL capability cannot set post_navigation_station_id");
      }
      const auto targets = tx.exec_params(
          "SELECT id FROM dispatch.stations "
          "WHERE id = $1::uuid AND scene_id = $2::uuid AND status = 'ACTIVE'",
          *action.post_navigation_station_id,
          scene_id);
      if (targets.empty()) {
        throw std::runtime_error("post_navigation_station_id not found in scene");
      }
    }

    if (action.robot_id.has_value()) {
      const auto robots = tx.exec_params(
          "SELECT id FROM dispatch.robots WHERE id = $1::uuid",
          *action.robot_id);
      if (robots.empty()) {
        throw std::runtime_error("robot_id not found");
      }
    }

    const auto precondition = action.precondition.is_null()
                                  ? std::optional<std::string>{}
                                  : std::optional<std::string>{
                                        action.precondition.dump()};

    if (!action.event_specs.is_array()) {
      throw std::runtime_error("event_specs must be an array");
    }
    if (operation_kind.empty()) {
      (void)workflow::parseEventSpecs(action.event_specs);
    } else {
      workflow::validateEventSpecsForOperation(
          operation_kind, action.event_specs);
    }

    if (action.parallel_group.has_value()) {
      tx.exec_params(
          "INSERT INTO dispatch.station_actions ("
          "station_id, sequence_no, action_name, parallel_group, "
          "capability_definition_id, "
          "capability_key, robot_selector_type, robot_id, robot_group, "
          "runtime_variable, parameters, precondition, failure_policy, "
          "retry_count, timeout_ms, success_event_name, event_specs, "
          "post_navigation_station_id"
          ") VALUES ("
          "$1::uuid, $2, $3, $4, "
          "NULLIF($5, '')::uuid, $6, $7, NULLIF($8, '')::uuid, $9, $10, "
          "$11::jsonb, $12::jsonb, $13, $14, $15, $16, $17::jsonb, "
          "NULLIF($18, '')::uuid)",
          station_id,
          action.sequence_no,
          action.action_name,
          *action.parallel_group,
          capability_id,
          action.capability_key,
          action.robot_selector_type,
          action.robot_id.value_or(""),
          action.robot_group,
          action.runtime_variable,
          action.parameters.dump(),
          precondition.value_or("null"),
          action.failure_policy,
          action.retry_count,
          action.timeout_ms,
          action.success_event_name,
          action.event_specs.dump(),
          action.post_navigation_station_id.value_or(""));
    } else {
      tx.exec_params(
          "INSERT INTO dispatch.station_actions ("
          "station_id, sequence_no, action_name, parallel_group, "
          "capability_definition_id, "
          "capability_key, robot_selector_type, robot_id, robot_group, "
          "runtime_variable, parameters, precondition, failure_policy, "
          "retry_count, timeout_ms, success_event_name, event_specs, "
          "post_navigation_station_id"
          ") VALUES ("
          "$1::uuid, $2, $3, NULL, "
          "NULLIF($4, '')::uuid, $5, $6, NULLIF($7, '')::uuid, $8, $9, "
          "$10::jsonb, $11::jsonb, $12, $13, $14, $15, $16::jsonb, "
          "NULLIF($17, '')::uuid)",
          station_id,
          action.sequence_no,
          action.action_name,
          capability_id,
          action.capability_key,
          action.robot_selector_type,
          action.robot_id.value_or(""),
          action.robot_group,
          action.runtime_variable,
          action.parameters.dump(),
          precondition.value_or("null"),
          action.failure_policy,
          action.retry_count,
          action.timeout_ms,
          action.success_event_name,
          action.event_specs.dump(),
          action.post_navigation_station_id.value_or(""));
    }
  }

  const auto rows = tx.exec_params(
      std::string(kStationActionSelect) +
          "WHERE a.station_id = $1::uuid ORDER BY a.sequence_no",
      station_id);
  tx.commit();
  std::vector<StationActionRecord> saved;
  saved.reserve(rows.size());
  for (const auto& row : rows) {
    saved.push_back(readStationAction(row));
  }
  return saved;
}

std::vector<CapabilityTemplateRecord>
WorkspaceRepository::listCapabilityTemplates() {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec(
      std::string(kCapabilityTemplateSelect) +
      "ORDER BY p.name, c.capability_key");
  tx.commit();
  std::vector<CapabilityTemplateRecord> items;
  items.reserve(rows.size());
  for (const auto& row : rows) {
    items.push_back(readCapabilityTemplate(row));
  }
  return items;
}

std::optional<CapabilityTemplateRecord> WorkspaceRepository::getCapabilityTemplate(
    const std::string& id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec_params(
      std::string(kCapabilityTemplateSelect) + "WHERE c.id = $1::uuid",
      id);
  tx.commit();
  if (rows.empty()) {
    return std::nullopt;
  }
  return readCapabilityTemplate(rows[0]);
}

std::vector<CapabilityProfileRecord> WorkspaceRepository::listCapabilityProfiles() {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec(
      "SELECT id::text AS id, name, description "
      "FROM dispatch.capability_profiles ORDER BY name");
  tx.commit();
  std::vector<CapabilityProfileRecord> items;
  items.reserve(rows.size());
  for (const auto& row : rows) {
    items.push_back(CapabilityProfileRecord{
        .id = row["id"].as<std::string>(),
        .name = row["name"].as<std::string>(),
        .description = row["description"].as<std::string>(),
    });
  }
  return items;
}

namespace {

void validateCapabilityUpsert(const CapabilityUpsertRequest& request) {
  if (request.profile_id.empty()) {
    throw std::runtime_error("profile_id is required");
  }
  if (request.capability_key.empty()) {
    throw std::runtime_error("capability_key is required");
  }
  if (request.endpoint_name.empty()) {
    throw std::runtime_error("endpoint_name is required");
  }
  static const std::unordered_set<std::string> kinds{
      "TOPIC", "SERVICE", "ACTION", "CONFIG", "SSH"};
  if (!kinds.contains(request.operation_kind)) {
    throw std::runtime_error(
        "operation_kind must be TOPIC, SERVICE, ACTION, CONFIG or SSH");
  }
  static const std::unordered_set<std::string> ownership{
      "DISPATCHER", "ROBOT_INTERNAL"};
  if (!ownership.contains(request.motion_ownership)) {
    throw std::runtime_error(
        "motion_ownership must be DISPATCHER or ROBOT_INTERNAL");
  }
  static const std::unordered_set<std::string> blocking{
      "HARD", "SOFT", "NONE", "NAVIGATION"};
  if (!blocking.contains(request.blocking_type)) {
    throw std::runtime_error(
        "blocking_type must be HARD, SOFT, NONE or NAVIGATION");
  }
  if (request.timeout_ms <= 0) {
    throw std::runtime_error("timeout_ms must be > 0");
  }
  if (!request.event_specs.is_array()) {
    throw std::runtime_error("event_specs must be an array");
  }
  workflow::validateEventSpecsForOperation(
      request.operation_kind, request.event_specs);
}

}  // namespace

CapabilityTemplateRecord WorkspaceRepository::createCapabilityTemplate(
    const CapabilityUpsertRequest& request) {
  validateCapabilityUpsert(request);
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto profile = tx.exec_params(
      "SELECT id FROM dispatch.capability_profiles WHERE id = $1::uuid",
      request.profile_id);
  if (profile.empty()) {
    throw std::runtime_error("capability profile not found");
  }
  const auto rows = tx.exec_params(
      "INSERT INTO dispatch.capability_definitions ("
      "profile_id, capability_key, operation_kind, endpoint_name, "
      "ros_message_type, parameter_schema, request_template, "
      "feedback_mapping, result_mapping, success_condition, failure_condition, "
      "timeout_ms, retry_policy, cancel_policy, blocking_type, "
      "resource_claims, protocol_config, motion_ownership, event_specs"
      ") VALUES ("
      "$1::uuid, $2, $3, $4, $5, $6::jsonb, $7::jsonb, $8::jsonb, $9::jsonb, "
      "NULLIF($10, '')::jsonb, NULLIF($11, '')::jsonb, $12, $13::jsonb, "
      "$14::jsonb, $15, $16::jsonb, $17::jsonb, $18, $19::jsonb"
      ") RETURNING id::text AS id",
      request.profile_id,
      request.capability_key,
      request.operation_kind,
      request.endpoint_name,
      request.ros_message_type,
      request.parameter_schema.dump(),
      request.request_template.dump(),
      request.feedback_mapping.dump(),
      request.result_mapping.dump(),
      request.success_condition.is_null() ? ""
                                         : request.success_condition.dump(),
      request.failure_condition.is_null() ? ""
                                         : request.failure_condition.dump(),
      request.timeout_ms,
      request.retry_policy.dump(),
      request.cancel_policy.dump(),
      request.blocking_type,
      request.resource_claims.dump(),
      request.protocol_config.dump(),
      request.motion_ownership,
      request.event_specs.dump());
  const auto id = rows[0]["id"].as<std::string>();
  tx.commit();
  auto created = getCapabilityTemplate(id);
  if (!created.has_value()) {
    throw std::runtime_error("failed to load created capability template");
  }
  return *created;
}

CapabilityTemplateRecord WorkspaceRepository::updateCapabilityTemplate(
    const std::string& id,
    const CapabilityUpsertRequest& request) {
  validateCapabilityUpsert(request);
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto existing = tx.exec_params(
      "SELECT id FROM dispatch.capability_definitions WHERE id = $1::uuid",
      id);
  if (existing.empty()) {
    throw std::runtime_error("capability template not found");
  }
  const auto profile = tx.exec_params(
      "SELECT id FROM dispatch.capability_profiles WHERE id = $1::uuid",
      request.profile_id);
  if (profile.empty()) {
    throw std::runtime_error("capability profile not found");
  }
  tx.exec_params(
      "UPDATE dispatch.capability_definitions SET "
      "profile_id = $2::uuid, "
      "capability_key = $3, "
      "operation_kind = $4, "
      "endpoint_name = $5, "
      "ros_message_type = $6, "
      "parameter_schema = $7::jsonb, "
      "request_template = $8::jsonb, "
      "feedback_mapping = $9::jsonb, "
      "result_mapping = $10::jsonb, "
      "success_condition = NULLIF($11, '')::jsonb, "
      "failure_condition = NULLIF($12, '')::jsonb, "
      "timeout_ms = $13, "
      "retry_policy = $14::jsonb, "
      "cancel_policy = $15::jsonb, "
      "blocking_type = $16, "
      "resource_claims = $17::jsonb, "
      "protocol_config = $18::jsonb, "
      "motion_ownership = $19, "
      "event_specs = $20::jsonb, "
      "updated_at = now() "
      "WHERE id = $1::uuid",
      id,
      request.profile_id,
      request.capability_key,
      request.operation_kind,
      request.endpoint_name,
      request.ros_message_type,
      request.parameter_schema.dump(),
      request.request_template.dump(),
      request.feedback_mapping.dump(),
      request.result_mapping.dump(),
      request.success_condition.is_null() ? ""
                                         : request.success_condition.dump(),
      request.failure_condition.is_null() ? ""
                                         : request.failure_condition.dump(),
      request.timeout_ms,
      request.retry_policy.dump(),
      request.cancel_policy.dump(),
      request.blocking_type,
      request.resource_claims.dump(),
      request.protocol_config.dump(),
      request.motion_ownership,
      request.event_specs.dump());
  tx.commit();
  auto updated = getCapabilityTemplate(id);
  if (!updated.has_value()) {
    throw std::runtime_error("capability template not found");
  }
  return *updated;
}

bool WorkspaceRepository::deleteCapabilityTemplate(const std::string& id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto refs = tx.exec_params(
      "SELECT COUNT(*)::int AS count FROM dispatch.station_actions "
      "WHERE capability_definition_id = $1::uuid",
      id);
  if (!refs.empty() && refs[0]["count"].as<int>() > 0) {
    throw std::runtime_error(
        "capability is referenced by station actions; unbind first");
  }
  const auto workflow_refs = tx.exec_params(
      "SELECT COUNT(*)::int AS count "
      "FROM dispatch.workflow_versions v "
      "WHERE EXISTS ("
      "  SELECT 1 "
      "  FROM jsonb_array_elements(COALESCE(v.graph->'nodes', '[]'::jsonb)) n "
      "  WHERE n->>'type' = 'ROBOT_CAPABILITY' "
      "    AND n->'data'->>'capability_definition_id' = $1"
      ")",
      id);
  if (!workflow_refs.empty() && workflow_refs[0]["count"].as<int>() > 0) {
    throw std::runtime_error(
        "capability is referenced by workflow versions; remove nodes first");
  }
  const auto result = tx.exec_params(
      "DELETE FROM dispatch.capability_definitions WHERE id = $1::uuid "
      "RETURNING id",
      id);
  tx.commit();
  return !result.empty();
}

std::vector<WorkflowSummaryRecord> WorkspaceRepository::listWorkflows(
    const std::optional<std::string>& scene_id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  pqxx::result rows;
  if (scene_id.has_value()) {
    rows = tx.exec_params(
        std::string(kWorkflowSummarySelect) + kWorkflowFromJoin +
            "WHERE d.scene_id = $1::uuid ORDER BY d.name",
        *scene_id);
  } else {
    rows = tx.exec(
        std::string(kWorkflowSummarySelect) + kWorkflowFromJoin +
        "ORDER BY d.name");
  }
  tx.commit();
  std::vector<WorkflowSummaryRecord> workflows;
  workflows.reserve(rows.size());
  for (const auto& row : rows) {
    workflows.push_back(readWorkflowSummary(row));
  }
  return workflows;
}

std::optional<WorkflowDetailRecord> WorkspaceRepository::getWorkflow(
    const std::string& id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec_params(
      std::string(kWorkflowDetailSelect) + kWorkflowDetailFromJoin +
          "WHERE d.id = $1::uuid",
      id);
  tx.commit();
  if (rows.empty()) {
    return std::nullopt;
  }
  return readWorkflowDetailPreferDraft(rows[0]);
}

WorkflowDetailRecord WorkspaceRepository::createWorkflow(
    const std::string& name,
    const std::string& description,
    const std::optional<std::string>& scene_id,
    const std::string& trigger_type,
    const nlohmann::json& trigger_config,
    const nlohmann::json& graph) {
  const auto normalized_graph = normalizeGraph(graph);
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto defs = tx.exec_params(
      "INSERT INTO dispatch.workflow_definitions ("
      "name, description, scene_id, trigger_type, trigger_config"
      ") VALUES ($1, $2, NULLIF($3, '')::uuid, $4, $5::jsonb) "
      "RETURNING id::text AS id",
      name,
      description,
      scene_id.value_or(""),
      trigger_type,
      trigger_config.dump());
  const auto workflow_id = defs[0]["id"].as<std::string>();
  tx.exec_params(
      "INSERT INTO dispatch.workflow_versions ("
      "workflow_definition_id, version, status, graph"
      ") VALUES ($1::uuid, 1, 'DRAFT', $2::jsonb)",
      workflow_id,
      normalized_graph.dump());
  tx.commit();

  auto created = getWorkflow(workflow_id);
  if (!created.has_value()) {
    throw std::runtime_error("failed to load created workflow");
  }
  return *created;
}

WorkflowDetailRecord WorkspaceRepository::updateWorkflow(
    const std::string& id,
    const std::string& name,
    const std::string& description,
    const std::optional<std::string>& scene_id,
    const std::string& trigger_type,
    const nlohmann::json& trigger_config,
    const nlohmann::json& graph) {
  const auto normalized_graph = normalizeGraph(graph);
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto defs = tx.exec_params(
      "UPDATE dispatch.workflow_definitions SET "
      "name = $2, description = $3, scene_id = NULLIF($4, '')::uuid, "
      "trigger_type = $5, trigger_config = $6::jsonb, updated_at = now() "
      "WHERE id = $1::uuid RETURNING id::text AS id",
      id,
      name,
      description,
      scene_id.value_or(""),
      trigger_type,
      trigger_config.dump());
  if (defs.empty()) {
    throw std::runtime_error("workflow not found");
  }

  const auto updated = tx.exec_params(
      "UPDATE dispatch.workflow_versions SET graph = $2::jsonb "
      "WHERE id = ("
      "  SELECT id FROM dispatch.workflow_versions "
      "  WHERE workflow_definition_id = $1::uuid AND status = 'DRAFT' "
      "  ORDER BY version DESC LIMIT 1"
      ") RETURNING id::text AS id",
      id,
      normalized_graph.dump());
  if (updated.empty()) {
    tx.exec_params(
        "INSERT INTO dispatch.workflow_versions ("
        "workflow_definition_id, version, status, graph"
        ") SELECT $1::uuid, COALESCE(("
        "  SELECT MAX(version) FROM dispatch.workflow_versions "
        "  WHERE workflow_definition_id = $1::uuid"
        "), 0) + 1, 'DRAFT', $2::jsonb",
        id,
        normalized_graph.dump());
  }
  tx.commit();

  auto detail = getWorkflow(id);
  if (!detail.has_value()) {
    throw std::runtime_error("failed to load updated workflow");
  }
  return *detail;
}

WorkflowDetailRecord WorkspaceRepository::publishWorkflow(
    const std::string& id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto defs = tx.exec_params(
      "SELECT id FROM dispatch.workflow_definitions WHERE id = $1::uuid",
      id);
  if (defs.empty()) {
    throw std::runtime_error("workflow not found");
  }

  const auto drafts = tx.exec_params(
      "SELECT id::text AS id, version, graph::text AS graph "
      "FROM dispatch.workflow_versions "
      "WHERE workflow_definition_id = $1::uuid AND status = 'DRAFT' "
      "ORDER BY version DESC LIMIT 1",
      id);
  if (drafts.empty()) {
    throw std::runtime_error("no draft version to publish");
  }

  const auto draft_id = drafts[0]["id"].as<std::string>();
  const auto draft_version = drafts[0]["version"].as<int>();
  const auto graph = drafts[0]["graph"].as<std::string>();

  tx.exec_params(
      "UPDATE dispatch.workflow_versions SET status = 'PUBLISHED', "
      "published_at = now() WHERE id = $1::uuid",
      draft_id);
  tx.exec_params(
      "INSERT INTO dispatch.workflow_versions ("
      "workflow_definition_id, version, status, graph"
      ") VALUES ($1::uuid, $2, 'DRAFT', $3::jsonb)",
      id,
      draft_version + 1,
      graph);
  tx.exec_params(
      "UPDATE dispatch.workflow_definitions SET updated_at = now() "
      "WHERE id = $1::uuid",
      id);
  tx.commit();

  auto detail = getWorkflow(id);
  if (!detail.has_value()) {
    throw std::runtime_error("failed to load published workflow");
  }
  return *detail;
}

bool WorkspaceRepository::deleteWorkflow(const std::string& id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto result = tx.exec_params(
      "DELETE FROM dispatch.workflow_definitions WHERE id = $1::uuid "
      "RETURNING id",
      id);
  tx.commit();
  return !result.empty();
}

std::optional<PublishedWorkflowRecord>
WorkspaceRepository::getLatestPublishedWorkflow(
    const std::string& definition_id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec_params(
      std::string(kPublishedWorkflowSelect) +
          "WHERE d.id = $1::uuid ORDER BY v.version DESC LIMIT 1",
      definition_id);
  tx.commit();
  if (rows.empty()) {
    return std::nullopt;
  }
  return readPublishedWorkflow(rows[0]);
}

std::vector<PublishedWorkflowRecord>
WorkspaceRepository::listPublishedWorkflowsByEvent(
    const std::string& event_name,
    const std::optional<std::string>& definition_id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec_params(
      std::string(kPublishedWorkflowSelect) +
          "WHERE d.trigger_type IN ('EVENT', 'MANUAL_OR_EVENT') "
          "AND d.trigger_config->>'event_name' = $1 "
          "AND ($2 = '' OR d.id = NULLIF($2, '')::uuid) "
          "AND v.version = ("
          "SELECT MAX(v2.version) FROM dispatch.workflow_versions v2 "
          "WHERE v2.workflow_definition_id = d.id "
          "AND v2.status = 'PUBLISHED'"
          ") ORDER BY d.name",
      event_name,
      definition_id.value_or(""));
  tx.commit();
  std::vector<PublishedWorkflowRecord> workflows;
  workflows.reserve(rows.size());
  for (const auto& row : rows) {
    workflows.push_back(readPublishedWorkflow(row));
  }
  return workflows;
}

WorkflowRunDetail WorkspaceRepository::createWorkflowRun(
    const std::string& definition_id,
    const std::string& trigger_type,
    const nlohmann::json& trigger_metadata,
    const nlohmann::json& input_data,
    const WorkflowRunLinkage& linkage) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);

  const auto published = tx.exec_params(
      std::string(kPublishedWorkflowSelect) +
          "WHERE d.id = $1::uuid ORDER BY v.version DESC LIMIT 1",
      definition_id);
  if (published.empty()) {
    throw std::runtime_error("no published workflow version");
  }

  const auto version_id = published[0]["version_id"].as<std::string>();
  const auto graph =
      nlohmann::json::parse(published[0]["graph"].as<std::string>());

  const auto runs = tx.exec_params(
      "INSERT INTO dispatch.workflow_runs ("
      "workflow_version_id, trigger_type, trigger_metadata, state, "
      "input_data, context_data, parent_run_id, root_run_id, source_run_id, "
      "parent_node_run_id, causation_event_id, business_key, started_at"
      ") VALUES ("
      "$1::uuid, $2, $3::jsonb, 'RUNNING', $4::jsonb, "
      "'{\"active\":[]}'::jsonb, NULLIF($5, '')::uuid, "
      "NULLIF($6, '')::uuid, NULLIF($7, '')::uuid, NULLIF($8, '')::uuid, "
      "NULLIF($9, '')::uuid, $10, now()"
      ") RETURNING id::text AS id",
      version_id,
      trigger_type,
      trigger_metadata.dump(),
      input_data.dump(),
      linkage.parent_run_id.value_or(""),
      linkage.root_run_id.value_or(""),
      linkage.source_run_id.value_or(""),
      linkage.parent_node_run_id.value_or(""),
      linkage.causation_event_id.value_or(""),
      linkage.business_key);
  const auto run_id = runs[0]["id"].as<std::string>();
  if (!linkage.root_run_id.has_value()) {
    tx.exec_params(
        "UPDATE dispatch.workflow_runs SET root_run_id = id "
        "WHERE id = $1::uuid",
        run_id);
  }

  if (graph.contains("nodes") && graph["nodes"].is_array()) {
    for (const auto& node : graph["nodes"]) {
      if (!node.contains("id") || !node["id"].is_string()) {
        continue;
      }
      const auto node_key = node["id"].get<std::string>();
      tx.exec_params(
          "INSERT INTO dispatch.node_runs ("
          "workflow_run_id, node_key, state, input_data"
          ") VALUES ($1::uuid, $2, 'PENDING', $3::jsonb)",
          run_id,
          node_key,
          node.dump());
    }
  }

  tx.commit();

  auto detail = getWorkflowRun(run_id);
  if (!detail.has_value()) {
    throw std::runtime_error("failed to load created workflow run");
  }
  return *detail;
}

std::vector<WorkflowRunRecord> WorkspaceRepository::listWorkflowRuns(
    int limit,
    const std::string& archived) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  std::string archive_filter = "WHERE r.archived_at IS NULL ";
  if (archived == "only") {
    archive_filter = "WHERE r.archived_at IS NOT NULL ";
  } else if (archived == "include") {
    archive_filter.clear();
  }
  const auto rows = tx.exec_params(
      std::string(kWorkflowRunSelect) + kWorkflowRunFromJoin + archive_filter +
          "ORDER BY r.created_at DESC LIMIT $1",
      std::clamp(limit, 1, 500));
  tx.commit();
  std::vector<WorkflowRunRecord> runs;
  runs.reserve(rows.size());
  for (const auto& row : rows) {
    runs.push_back(readWorkflowRun(row));
  }
  return runs;
}

WorkflowRunDetail WorkspaceRepository::createNextWorkflowCycle(
    const std::string& run_id,
    int cycle) {
  if (cycle <= 1) {
    throw std::runtime_error("workflow cycle must be > 1");
  }
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec_params(
      "SELECT v.graph::text AS graph FROM dispatch.workflow_runs r "
      "JOIN dispatch.workflow_versions v ON v.id = r.workflow_version_id "
      "WHERE r.id = $1::uuid AND r.state = 'RUNNING' FOR UPDATE OF r",
      run_id);
  if (rows.empty()) {
    throw std::runtime_error("running workflow run not found");
  }
  const auto graph = nlohmann::json::parse(rows[0]["graph"].as<std::string>());
  for (const auto& node : graph.value("nodes", nlohmann::json::array())) {
    if (!node.contains("id") || !node["id"].is_string()) {
      continue;
    }
    tx.exec_params(
        "INSERT INTO dispatch.node_runs ("
        "workflow_run_id, node_key, attempt, state, input_data"
        ") VALUES ($1::uuid, $2, $3, 'PENDING', $4::jsonb)",
        run_id,
        node["id"].get<std::string>(),
        cycle,
        node.dump());
  }
  tx.exec_params(
      "UPDATE dispatch.workflow_runs SET "
      "context_data = jsonb_set(context_data - 'next_cycle_scheduled', "
      "'{current_cycle}', to_jsonb($1::integer), true), updated_at = now() "
      "WHERE id = $2::uuid",
      cycle,
      run_id);
  tx.commit();
  auto detail = getWorkflowRun(run_id);
  if (!detail.has_value()) {
    throw std::runtime_error("failed to load next workflow cycle");
  }
  return *detail;
}

bool WorkspaceRepository::archiveWorkflowRun(const std::string& run_id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto result = tx.exec_params(
      "UPDATE dispatch.workflow_runs SET archived_at = now(), "
      "updated_at = now() WHERE id = $1::uuid "
      "AND state IN ('SUCCEEDED', 'FAILED', 'CANCELLED') "
      "AND archived_at IS NULL",
      run_id);
  tx.commit();
  return result.affected_rows() > 0;
}

int WorkspaceRepository::cleanupArchivedWorkflowRuns(int older_than_days) {
  if (older_than_days < 0) {
    throw std::runtime_error("older_than_days must be >= 0");
  }
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto result = tx.exec_params(
      "DELETE FROM dispatch.workflow_runs WHERE archived_at IS NOT NULL "
      "AND state IN ('SUCCEEDED', 'FAILED', 'CANCELLED') "
      "AND archived_at <= now() - ($1::integer * interval '1 day')",
      older_than_days);
  const auto count = static_cast<int>(result.affected_rows());
  tx.commit();
  return count;
}

std::optional<WorkflowRunDetail> WorkspaceRepository::getWorkflowRun(
    const std::string& run_id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec_params(
      std::string(kWorkflowRunSelect) + ", v.graph::text AS graph " +
          kWorkflowRunFromJoin + "WHERE r.id = $1::uuid",
      run_id);
  if (rows.empty()) {
    tx.commit();
    return std::nullopt;
  }

  const auto node_rows = tx.exec_params(
      std::string(kNodeRunSelect) +
          "WHERE workflow_run_id = $1::uuid ORDER BY created_at",
      run_id);
  const auto command_rows = tx.exec_params(
      std::string(kCommandRunSelect) +
      "WHERE workflow_run_id = $1::uuid ORDER BY created_at",
      run_id);
  const auto event_rows = tx.exec_params(
      "SELECT id::text AS id, workflow_run_id::text AS workflow_run_id, "
      "node_run_id::text AS node_run_id, event_type, payload::text AS payload, "
      "occurred_at::text AS occurred_at FROM dispatch.workflow_events "
      "WHERE workflow_run_id = $1::uuid ORDER BY occurred_at, id",
      run_id);
  tx.commit();

  WorkflowRunDetail detail{
      .run = readWorkflowRun(rows[0]),
      .graph = nlohmann::json::parse(rows[0]["graph"].as<std::string>()),
  };
  detail.nodes.reserve(node_rows.size());
  for (const auto& row : node_rows) {
    detail.nodes.push_back(readNodeRun(row));
  }
  detail.commands.reserve(command_rows.size());
  for (const auto& row : command_rows) {
    detail.commands.push_back(readCommandRun(row));
  }
  detail.events.reserve(event_rows.size());
  for (const auto& row : event_rows) {
    detail.events.push_back(readWorkflowEvent(row));
  }
  return detail;
}

void WorkspaceRepository::updateWorkflowRunState(
    const std::string& run_id,
    const std::string& state,
    const nlohmann::json& context_data) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  tx.exec_params(
      "UPDATE dispatch.workflow_runs SET state = $1, context_data = $2::jsonb, "
      "finished_at = CASE WHEN $1 IN ('SUCCEEDED', 'FAILED', 'CANCELLED') "
      "THEN COALESCE(finished_at, now()) ELSE NULL END, "
      "updated_at = now() WHERE id = $3::uuid",
      state,
      context_data.dump(),
      run_id);
  tx.commit();
}

void WorkspaceRepository::updateNodeRun(
    const std::string& node_run_id,
    const std::string& state,
    const std::optional<std::string>& assigned_robot_id,
    const nlohmann::json& output_data,
    const nlohmann::json& error_data) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  if (error_data.is_null()) {
    tx.exec_params(
        "UPDATE dispatch.node_runs SET state = $1, "
        "assigned_robot_id = NULLIF($2, '')::uuid, output_data = $3::jsonb, "
        "error_data = NULL, updated_at = now() WHERE id = $4::uuid",
        state,
        assigned_robot_id.value_or(""),
        output_data.dump(),
        node_run_id);
  } else {
    tx.exec_params(
        "UPDATE dispatch.node_runs SET state = $1, "
        "assigned_robot_id = NULLIF($2, '')::uuid, output_data = $3::jsonb, "
        "error_data = $4::jsonb, updated_at = now() WHERE id = $5::uuid",
        state,
        assigned_robot_id.value_or(""),
        output_data.dump(),
        error_data.dump(),
        node_run_id);
  }
  tx.commit();
}

bool WorkspaceRepository::transitionNodeRun(
    const std::string& node_run_id,
    const std::string& expected_state,
    const std::string& next_state,
    const std::optional<std::string>& assigned_robot_id,
    const nlohmann::json& output_data,
    const nlohmann::json& error_data) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  pqxx::result rows;
  if (error_data.is_null()) {
    rows = tx.exec_params(
        "UPDATE dispatch.node_runs SET state = $1, "
        "assigned_robot_id = NULLIF($2, '')::uuid, output_data = $3::jsonb, "
        "error_data = NULL, updated_at = now() "
        "WHERE id = $4::uuid AND state = $5 RETURNING id",
        next_state,
        assigned_robot_id.value_or(""),
        output_data.dump(),
        node_run_id,
        expected_state);
  } else {
    rows = tx.exec_params(
        "UPDATE dispatch.node_runs SET state = $1, "
        "assigned_robot_id = NULLIF($2, '')::uuid, output_data = $3::jsonb, "
        "error_data = $4::jsonb, updated_at = now() "
        "WHERE id = $5::uuid AND state = $6 RETURNING id",
        next_state,
        assigned_robot_id.value_or(""),
        output_data.dump(),
        error_data.dump(),
        node_run_id,
        expected_state);
  }
  tx.commit();
  return !rows.empty();
}

CommandRunRecord WorkspaceRepository::createCommandRun(
    const std::string& workflow_run_id,
    const std::string& node_run_id,
    const std::string& robot_id,
    const std::optional<std::string>& capability_definition_id,
    const std::string& operation_kind,
    const std::string& endpoint_name,
    const nlohmann::json& request_payload,
    const std::optional<std::string>& command_id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec_params(
      "INSERT INTO dispatch.command_runs ("
      "command_id, workflow_run_id, node_run_id, robot_id, "
      "capability_definition_id, operation_kind, endpoint_name, "
      "request_payload, state"
      ") VALUES ("
      "COALESCE(NULLIF($1, '')::uuid, gen_random_uuid()), $2::uuid, "
      "$3::uuid, $4::uuid, NULLIF($5, '')::uuid, $6, $7, $8::jsonb, "
      "'CREATED'"
      ") RETURNING id::text AS id, command_id::text AS command_id",
      command_id.value_or(""),
      workflow_run_id,
      node_run_id,
      robot_id,
      capability_definition_id.value_or(""),
      operation_kind,
      endpoint_name,
      request_payload.dump());
  tx.commit();
  auto detail = getWorkflowRun(workflow_run_id);
  if (detail.has_value()) {
    for (const auto& command : detail->commands) {
      if (command.id == rows[0]["id"].as<std::string>()) {
        return command;
      }
    }
  }
  throw std::runtime_error("failed to load created command run");
}

void WorkspaceRepository::markCommandRunDispatched(
    const std::string& command_run_id,
    const std::string& correlation_id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  tx.exec_params(
      "UPDATE dispatch.command_runs SET state = 'ACTIVE', "
      "correlation_id = $1, dispatched_at = now(), updated_at = now() "
      "WHERE id = $2::uuid AND state IN ('CREATED', 'DISPATCHING')",
      correlation_id,
      command_run_id);
  tx.commit();
}

void WorkspaceRepository::updateCommandRunFeedback(
    const std::string& command_run_id,
    const nlohmann::json& feedback) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  tx.exec_params(
      "UPDATE dispatch.command_runs SET last_feedback = $1::jsonb, "
      "updated_at = now() WHERE id = $2::uuid "
      "AND state = 'ACTIVE'",
      feedback.dump(),
      command_run_id);
  tx.commit();
}

void WorkspaceRepository::completeCommandRun(
    const std::string& command_run_id,
    const std::string& state,
    const nlohmann::json& result_payload,
    const nlohmann::json& error_data) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  if (error_data.is_null()) {
    tx.exec_params(
        "UPDATE dispatch.command_runs SET state = $1, "
        "result_payload = $2::jsonb, error_data = NULL, "
        "completed_at = now(), updated_at = now() WHERE id = $3::uuid",
        state,
        result_payload.dump(),
        command_run_id);
  } else {
    tx.exec_params(
        "UPDATE dispatch.command_runs SET state = $1, "
        "result_payload = $2::jsonb, error_data = $3::jsonb, "
        "completed_at = now(), updated_at = now() WHERE id = $4::uuid",
        state,
        result_payload.dump(),
        error_data.dump(),
        command_run_id);
  }
  tx.commit();
}

std::string WorkspaceRepository::insertWorkflowEvent(
    const std::string& run_id,
    const std::optional<std::string>& node_run_id,
    const std::string& event_type,
    const nlohmann::json& payload,
    const std::optional<std::string>& deduplication_key) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec_params(
      "INSERT INTO dispatch.workflow_events ("
      "workflow_run_id, node_run_id, event_type, payload, deduplication_key"
      ") VALUES ("
      "$1::uuid, NULLIF($2, '')::uuid, $3, $4::jsonb, NULLIF($5, ''))"
      " RETURNING id::text AS id",
      run_id,
      node_run_id.value_or(""),
      event_type,
      payload.dump(),
      deduplication_key.value_or(""));
  tx.commit();
  return rows[0]["id"].as<std::string>();
}

std::string WorkspaceRepository::insertWorkflowSignal(
    const WorkflowSignalEnvelope& envelope) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec_params(
      "INSERT INTO dispatch.workflow_signals ("
      "event_name, source_run_id, source_node_run_id, target_run_id, "
      "target_workflow_definition_id, business_key, deduplication_key, payload"
      ") VALUES ("
      "$1, NULLIF($2, '')::uuid, NULLIF($3, '')::uuid, "
      "NULLIF($4, '')::uuid, NULLIF($5, '')::uuid, $6, NULLIF($7, ''), "
      "$8::jsonb"
      ") RETURNING id::text AS id",
      envelope.event_name,
      envelope.source_run_id.value_or(""),
      envelope.source_node_run_id.value_or(""),
      envelope.target_run_id.value_or(""),
      envelope.target_workflow_definition_id.value_or(""),
      envelope.business_key,
      envelope.deduplication_key.value_or(""),
      envelope.payload.dump());
  tx.commit();
  return rows[0]["id"].as<std::string>();
}

std::vector<WorkflowEventRecord> WorkspaceRepository::listWorkflowEvents(
    const std::string& run_id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec_params(
      "SELECT id::text AS id, workflow_run_id::text AS workflow_run_id, "
      "node_run_id::text AS node_run_id, event_type, payload::text AS payload, "
      "occurred_at::text AS occurred_at FROM dispatch.workflow_events "
      "WHERE workflow_run_id = $1::uuid ORDER BY occurred_at, id",
      run_id);
  tx.commit();
  std::vector<WorkflowEventRecord> events;
  events.reserve(rows.size());
  for (const auto& row : rows) {
    events.push_back(readWorkflowEvent(row));
  }
  return events;
}

std::vector<NodeRunRecord> WorkspaceRepository::listWaitingEventNodeRuns(
    const std::string& event_name,
    const std::optional<std::string>& target_run_id,
    const std::string& business_key,
    const std::optional<std::string>& target_workflow_definition_id) {
  auto connection = pool_.acquire();
  pqxx::work tx(*connection);
  const auto rows = tx.exec_params(
      std::string(kNodeRunSelect) +
          "WHERE state = 'WAITING_EVENT' AND ("
          "(input_data #>> '{data,event_name}') = $1 OR "
          "(input_data->>'event_name') = $1 OR "
          "(output_data->>'waiting_event') = $1"
          ") AND ($2 = '' OR workflow_run_id = NULLIF($2, '')::uuid) "
          "AND ($3 = '' OR EXISTS ("
          "SELECT 1 FROM dispatch.workflow_runs wr "
          "WHERE wr.id = workflow_run_id AND wr.business_key = $3"
          ")) AND ($4 = '' OR EXISTS ("
          "SELECT 1 FROM dispatch.workflow_runs wr "
          "JOIN dispatch.workflow_versions wv "
          "ON wv.id = wr.workflow_version_id "
          "WHERE wr.id = workflow_run_id "
          "AND wv.workflow_definition_id = NULLIF($4, '')::uuid"
          ")) ORDER BY created_at",
      event_name,
      target_run_id.value_or(""),
      business_key,
      target_workflow_definition_id.value_or(""));
  tx.commit();
  std::vector<NodeRunRecord> nodes;
  nodes.reserve(rows.size());
  for (const auto& row : rows) {
    nodes.push_back(readNodeRun(row));
  }
  return nodes;
}

}  // namespace dispatcher::db
