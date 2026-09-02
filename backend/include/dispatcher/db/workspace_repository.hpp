#pragma once

#include "dispatcher/db/pg_pool.hpp"
#include "dispatcher/maps/map_import_service.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace dispatcher::db {

struct SceneRecord {
  std::string id;
  std::string name;
  std::string description;
  std::optional<std::string> active_map_version_id;
};

struct MapVersionRecord {
  std::string id;
  std::string scene_id;
  int version{0};
  std::string status;
  std::string pgm_path;
  std::string yaml_path;
  std::string preview_path;
  std::string sha256;
  int width{0};
  int height{0};
  double resolution{0.0};
  double origin_x{0.0};
  double origin_y{0.0};
  double origin_yaw{0.0};
  double occupied_thresh{0.65};
  double free_thresh{0.196};
  std::uint64_t file_size_bytes{0};
  std::string yaml_image;
  std::string frame_id{"map"};
};

struct RobotRecord {
  std::string id;
  std::string name;
  bool enabled{true};
  std::string robot_type;
  std::string ros_version;
  std::string ros_distribution{"noetic"};
  std::string ros_namespace;
  std::optional<std::string> current_scene_id;
  std::optional<std::string> current_map_version_id;
  std::string business_status{"UNKNOWN"};
  std::string localization_status{"UNKNOWN"};
  std::string connection_state{"DISCONNECTED"};
  std::string configuration_state{"DRAFT"};
  std::string rosbridge_url;
  std::optional<std::string> host;
  std::optional<int> rosbridge_port;
  std::optional<std::string> rosbridge_path;
  bool rosbridge_tls{false};
  std::optional<std::string> pose_topic;
  std::optional<std::string> pose_message_type;
  nlohmann::json pose_mapping = nlohmann::json::object();
  int stale_timeout_ms{3000};
  std::optional<std::string> nav_action;
  std::optional<std::string> nav_action_type;
};

struct RobotUpsertRequest {
  std::string name;
  bool enabled{true};
  std::string robot_type{"zj_humanoid"};
  std::string ros_version{"ROS1"};
  std::string ros_distribution{"noetic"};
  std::string ros_namespace;
  std::string host;
  int rosbridge_port{9090};
  std::string rosbridge_path{"/"};
  bool rosbridge_tls{false};
  std::string pose_topic;
  std::string pose_message_type{
      "geometry_msgs/PoseWithCovarianceStamped"};
  nlohmann::json pose_mapping = nlohmann::json::object();
  int stale_timeout_ms{3000};
};

struct RobotStartupProfileRecord {
  std::string id;
  std::string robot_id;
  std::string name;
  std::string description;
  bool enabled{true};
  int ssh_port{22};
  std::string ssh_username;
  std::string credential_reference;
  std::string known_hosts_reference;
  nlohmann::json steps = nlohmann::json::array();
  nlohmann::json readiness_checks = nlohmann::json::array();
  nlohmann::json stop_steps = nlohmann::json::array();
  int timeout_ms{120000};
  int version{1};
};

struct RobotStartupProfileUpsert {
  std::string name;
  std::string description;
  bool enabled{true};
  int ssh_port{22};
  std::string ssh_username;
  std::string credential_reference;
  std::string known_hosts_reference;
  nlohmann::json steps = nlohmann::json::array();
  nlohmann::json readiness_checks = nlohmann::json::array();
  nlohmann::json stop_steps = nlohmann::json::array();
  int timeout_ms{120000};
};

struct NavigationGoalRecord {
  std::string command_id;
  std::string outbox_id;
  std::string state;
};

struct StationRecord {
  std::string id;
  std::string scene_id;
  std::string map_version_id;
  std::string name;
  double x{0.0};
  double y{0.0};
  double yaw{0.0};
  std::vector<std::string> tags;
  std::string notes;
  std::string status{"ACTIVE"};
  nlohmann::json metadata = nlohmann::json::object();
};

struct StationActionRecord {
  std::string id;
  std::string station_id;
  int sequence_no{1};
  std::string action_name;
  std::optional<int> parallel_group;
  std::optional<std::string> capability_definition_id;
  std::string capability_key;
  std::string motion_ownership{"DISPATCHER"};
  std::string robot_selector_type{"FIXED"};
  std::optional<std::string> robot_id;
  std::string robot_name;
  std::string robot_group;
  std::string runtime_variable;
  nlohmann::json parameters = nlohmann::json::object();
  nlohmann::json precondition;
  std::string failure_policy{"FAIL"};
  int retry_count{0};
  int timeout_ms{30000};
  std::string success_event_name;
  nlohmann::json event_specs = nlohmann::json::array();
  std::optional<std::string> post_navigation_station_id;
};

struct CapabilityTemplateRecord {
  std::string id;
  std::string profile_id;
  std::string profile_name;
  std::string capability_key;
  std::string operation_kind;
  std::string endpoint_name;
  std::string ros_message_type;
  std::string motion_ownership{"DISPATCHER"};
  std::string blocking_type{"NONE"};
  int timeout_ms{30000};
  nlohmann::json parameter_schema = nlohmann::json::object();
  nlohmann::json request_template = nlohmann::json::object();
  nlohmann::json feedback_mapping = nlohmann::json::object();
  nlohmann::json result_mapping = nlohmann::json::object();
  nlohmann::json success_condition;
  nlohmann::json failure_condition;
  nlohmann::json retry_policy = nlohmann::json::object();
  nlohmann::json cancel_policy = nlohmann::json::object();
  nlohmann::json resource_claims = nlohmann::json::array();
  nlohmann::json protocol_config = nlohmann::json::object();
  nlohmann::json event_specs = nlohmann::json::array();
};

struct CapabilityProfileRecord {
  std::string id;
  std::string name;
  std::string description;
};

struct CapabilityUpsertRequest {
  std::string profile_id;
  std::string capability_key;
  std::string operation_kind{"SERVICE"};
  std::string endpoint_name;
  std::string ros_message_type;
  std::string motion_ownership{"DISPATCHER"};
  std::string blocking_type{"NONE"};
  int timeout_ms{30000};
  nlohmann::json parameter_schema = nlohmann::json::object();
  nlohmann::json request_template = nlohmann::json::object();
  nlohmann::json feedback_mapping = nlohmann::json::object();
  nlohmann::json result_mapping = nlohmann::json::object();
  nlohmann::json success_condition;
  nlohmann::json failure_condition;
  nlohmann::json retry_policy = nlohmann::json::object();
  nlohmann::json cancel_policy = nlohmann::json::object();
  nlohmann::json resource_claims = nlohmann::json::array();
  nlohmann::json protocol_config = nlohmann::json::object();
  nlohmann::json event_specs = nlohmann::json::array();
};

struct StationUpsertRequest {
  std::string scene_id;
  std::string map_version_id;
  std::string name;
  double x{0.0};
  double y{0.0};
  double yaw{0.0};
  std::vector<std::string> tags;
  std::string notes;
  nlohmann::json metadata = nlohmann::json::object();
};

struct StationActionUpsert {
  int sequence_no{1};
  std::string action_name;
  std::optional<int> parallel_group;
  std::string capability_key;
  std::optional<std::string> capability_definition_id;
  std::string robot_selector_type{"FIXED"};
  std::optional<std::string> robot_id;
  std::string robot_group;
  std::string runtime_variable;
  nlohmann::json parameters = nlohmann::json::object();
  nlohmann::json precondition;
  std::string failure_policy{"FAIL"};
  int retry_count{0};
  int timeout_ms{30000};
  std::string success_event_name;
  nlohmann::json event_specs = nlohmann::json::array();
  std::optional<std::string> post_navigation_station_id;
};

struct WorkflowSummaryRecord {
  std::string id;
  std::string name;
  std::string description;
  std::optional<std::string> scene_id;
  std::string trigger_type{"MANUAL"};
  nlohmann::json trigger_config = nlohmann::json::object();
  std::optional<int> draft_version;
  std::string draft_status{"DRAFT"};
  std::optional<int> published_version;
};

struct WorkflowDetailRecord {
  WorkflowSummaryRecord summary;
  nlohmann::json graph = nlohmann::json::object();
  std::optional<std::string> draft_version_id;
};

struct PublishedWorkflowRecord {
  std::string definition_id;
  std::string name;
  std::string description;
  std::optional<std::string> scene_id;
  std::string trigger_type;
  nlohmann::json trigger_config = nlohmann::json::object();
  std::string version_id;
  int version{0};
  nlohmann::json graph = nlohmann::json::object();
};

struct WorkflowRunRecord {
  std::string id;
  std::string workflow_version_id;
  std::string workflow_definition_id;
  std::string workflow_name;
  int workflow_version{0};
  std::string trigger_type;
  nlohmann::json trigger_metadata = nlohmann::json::object();
  std::string state;
  nlohmann::json input_data = nlohmann::json::object();
  nlohmann::json context_data = nlohmann::json::object();
  std::optional<std::string> parent_run_id;
  std::optional<std::string> root_run_id;
  std::optional<std::string> source_run_id;
  std::optional<std::string> parent_node_run_id;
  std::optional<std::string> causation_event_id;
  std::string business_key;
  std::optional<std::string> started_at;
  std::optional<std::string> finished_at;
  std::optional<std::string> archived_at;
  std::string created_at;
};

struct WorkflowRunLinkage {
  std::optional<std::string> parent_run_id;
  std::optional<std::string> root_run_id;
  std::optional<std::string> source_run_id;
  std::optional<std::string> parent_node_run_id;
  std::optional<std::string> causation_event_id;
  std::string business_key;
};

struct WorkflowSignalEnvelope {
  std::string event_name;
  std::optional<std::string> source_run_id;
  std::optional<std::string> source_node_run_id;
  std::optional<std::string> target_run_id;
  std::optional<std::string> target_workflow_definition_id;
  std::string business_key;
  std::optional<std::string> deduplication_key;
  nlohmann::json payload = nlohmann::json::object();
};

struct NodeRunRecord {
  std::string id;
  std::string workflow_run_id;
  std::string node_key;
  int attempt{1};
  std::string state;
  std::optional<std::string> assigned_robot_id;
  nlohmann::json input_data = nlohmann::json::object();
  nlohmann::json output_data = nlohmann::json::object();
  nlohmann::json error_data;
};

struct CommandRunRecord {
  std::string id;
  std::string command_id;
  std::string workflow_run_id;
  std::string node_run_id;
  std::string robot_id;
  std::optional<std::string> capability_definition_id;
  std::string operation_kind;
  std::string endpoint_name;
  std::string correlation_id;
  std::string state;
  nlohmann::json request_payload = nlohmann::json::object();
  nlohmann::json last_feedback;
  nlohmann::json result_payload;
  nlohmann::json error_data;
  std::optional<std::string> dispatched_at;
  std::optional<std::string> completed_at;
  std::string created_at;
};

struct WorkflowEventRecord {
  std::string id;
  std::string workflow_run_id;
  std::optional<std::string> node_run_id;
  std::string event_type;
  nlohmann::json payload = nlohmann::json::object();
  std::string occurred_at;
};

struct WorkflowRunDetail {
  WorkflowRunRecord run;
  nlohmann::json graph = nlohmann::json::object();
  std::vector<NodeRunRecord> nodes;
  std::vector<CommandRunRecord> commands;
  std::vector<WorkflowEventRecord> events;
};

class WorkspaceRepository {
 public:
  explicit WorkspaceRepository(PgPool& pool);

  [[nodiscard]] std::vector<SceneRecord> listScenes();
  [[nodiscard]] std::optional<SceneRecord> getScene(const std::string& id);
  [[nodiscard]] SceneRecord createScene(
      const std::string& name,
      const std::string& description);
  void deleteScene(const std::string& id);

  [[nodiscard]] std::optional<MapVersionRecord> getMapVersion(
      const std::string& id);
  [[nodiscard]] int nextMapVersion(const std::string& scene_id);
  [[nodiscard]] MapVersionRecord insertMapVersion(
      const std::string& scene_id,
      int version,
      const maps::ImportedMapFiles& files);
  bool setActiveMap(
      const std::string& scene_id,
      const std::string& map_version_id);
  // Deletes unreferenced maps; archives maps that are/were active or have stations.
  [[nodiscard]] std::string removeOrArchiveMapVersion(const std::string& id);

  [[nodiscard]] std::vector<RobotRecord> listRobots();
  [[nodiscard]] std::optional<RobotRecord> getRobot(const std::string& id);
  [[nodiscard]] RobotRecord createRobot(const RobotUpsertRequest& request);
  [[nodiscard]] RobotRecord updateRobot(
      const std::string& id,
      const RobotUpsertRequest& request);
  [[nodiscard]] bool robotHasActiveScene(const std::string& id);
  bool deleteRobot(const std::string& id);
  [[nodiscard]] std::vector<RobotStartupProfileRecord>
  listRobotStartupProfiles(const std::string& robot_id);
  [[nodiscard]] std::optional<RobotStartupProfileRecord>
  getRobotStartupProfile(const std::string& id);
  [[nodiscard]] RobotStartupProfileRecord createRobotStartupProfile(
      const std::string& robot_id,
      const RobotStartupProfileUpsert& request);
  [[nodiscard]] RobotStartupProfileRecord updateRobotStartupProfile(
      const std::string& robot_id,
      const std::string& id,
      const RobotStartupProfileUpsert& request);
  [[nodiscard]] bool deleteRobotStartupProfile(
      const std::string& robot_id, const std::string& id);
  [[nodiscard]] std::vector<MapVersionRecord> listMapVersions(
      const std::string& scene_id);
  [[nodiscard]] std::vector<std::string> listSceneRobotIds(
      const std::string& scene_id);
  void replaceSceneRobots(
      const std::string& scene_id,
      const std::vector<std::string>& robot_ids);

  [[nodiscard]] NavigationGoalRecord enqueueNavigationGoal(
      const std::string& robot_id,
      const std::string& scene_id,
      const std::string& map_version_id,
      double x,
      double y,
      double yaw,
      double distance_tolerance,
      double heading_tolerance);

  void updateOutboxState(
      const std::string& outbox_id,
      const std::string& state,
      const std::optional<nlohmann::json>& last_error = std::nullopt);

  void updateRobotPoseCache(
      const std::string& robot_id,
      const nlohmann::json& pose,
      const std::string& localization_status);

  void updateRobotConnectionState(
      const std::string& robot_id,
      const std::string& connection_state);

  // Persists last rosapi/interface scan under robots.metadata.interface_cache
  // so capability dropdowns survive robot offline.
  [[nodiscard]] std::optional<nlohmann::json> getRobotInterfaceCache(
      const std::string& robot_id);
  void setRobotInterfaceCache(
      const std::string& robot_id,
      const nlohmann::json& cache);

  [[nodiscard]] std::vector<StationRecord> listStations(
      const std::string& scene_id,
      const std::optional<std::string>& map_version_id = std::nullopt);
  [[nodiscard]] std::optional<StationRecord> getStation(const std::string& id);
  [[nodiscard]] StationRecord createStation(const StationUpsertRequest& request);
  [[nodiscard]] StationRecord updateStation(
      const std::string& id,
      const StationUpsertRequest& request);
  [[nodiscard]] std::string removeOrArchiveStation(const std::string& id);

  [[nodiscard]] std::vector<StationActionRecord> listStationActions(
      const std::string& station_id);
  [[nodiscard]] std::optional<StationActionRecord> getStationAction(
      const std::string& id);
  [[nodiscard]] std::vector<StationActionRecord> replaceStationActions(
      const std::string& station_id,
      const std::vector<StationActionUpsert>& actions);

  [[nodiscard]] std::vector<CapabilityTemplateRecord> listCapabilityTemplates();
  [[nodiscard]] std::optional<CapabilityTemplateRecord> getCapabilityTemplate(
      const std::string& id);
  [[nodiscard]] std::vector<CapabilityProfileRecord> listCapabilityProfiles();
  [[nodiscard]] CapabilityTemplateRecord createCapabilityTemplate(
      const CapabilityUpsertRequest& request);
  [[nodiscard]] CapabilityTemplateRecord updateCapabilityTemplate(
      const std::string& id,
      const CapabilityUpsertRequest& request);
  [[nodiscard]] bool deleteCapabilityTemplate(const std::string& id);

  [[nodiscard]] std::vector<WorkflowSummaryRecord> listWorkflows(
      const std::optional<std::string>& scene_id = std::nullopt);
  [[nodiscard]] std::optional<WorkflowDetailRecord> getWorkflow(
      const std::string& id);
  [[nodiscard]] WorkflowDetailRecord createWorkflow(
      const std::string& name,
      const std::string& description,
      const std::optional<std::string>& scene_id,
      const std::string& trigger_type,
      const nlohmann::json& trigger_config,
      const nlohmann::json& graph);
  [[nodiscard]] WorkflowDetailRecord updateWorkflow(
      const std::string& id,
      const std::string& name,
      const std::string& description,
      const std::optional<std::string>& scene_id,
      const std::string& trigger_type,
      const nlohmann::json& trigger_config,
      const nlohmann::json& graph);
  [[nodiscard]] WorkflowDetailRecord publishWorkflow(const std::string& id);
  bool deleteWorkflow(const std::string& id);

  [[nodiscard]] std::optional<PublishedWorkflowRecord> getLatestPublishedWorkflow(
      const std::string& definition_id);
  [[nodiscard]] std::vector<PublishedWorkflowRecord> listPublishedWorkflowsByEvent(
      const std::string& event_name,
      const std::optional<std::string>& definition_id = std::nullopt);

  [[nodiscard]] WorkflowRunDetail createWorkflowRun(
      const std::string& definition_id,
      const std::string& trigger_type,
      const nlohmann::json& trigger_metadata,
      const nlohmann::json& input_data,
      const WorkflowRunLinkage& linkage = {});

  [[nodiscard]] std::vector<WorkflowRunRecord> listWorkflowRuns(
      int limit = 50,
      const std::string& archived = "exclude");
  [[nodiscard]] std::optional<WorkflowRunDetail> getWorkflowRun(
      const std::string& run_id);

  void updateWorkflowRunState(
      const std::string& run_id,
      const std::string& state,
      const nlohmann::json& context_data);
  [[nodiscard]] WorkflowRunDetail createNextWorkflowCycle(
      const std::string& run_id,
      int cycle);
  [[nodiscard]] bool archiveWorkflowRun(const std::string& run_id);
  [[nodiscard]] int cleanupArchivedWorkflowRuns(int older_than_days);
  void updateNodeRun(
      const std::string& node_run_id,
      const std::string& state,
      const std::optional<std::string>& assigned_robot_id,
      const nlohmann::json& output_data,
      const nlohmann::json& error_data);
  [[nodiscard]] bool transitionNodeRun(
      const std::string& node_run_id,
      const std::string& expected_state,
      const std::string& next_state,
      const std::optional<std::string>& assigned_robot_id,
      const nlohmann::json& output_data,
      const nlohmann::json& error_data);

  [[nodiscard]] CommandRunRecord createCommandRun(
      const std::string& workflow_run_id,
      const std::string& node_run_id,
      const std::string& robot_id,
      const std::optional<std::string>& capability_definition_id,
      const std::string& operation_kind,
      const std::string& endpoint_name,
      const nlohmann::json& request_payload,
      const std::optional<std::string>& command_id = std::nullopt);
  void markCommandRunDispatched(
      const std::string& command_run_id,
      const std::string& correlation_id);
  void updateCommandRunFeedback(
      const std::string& command_run_id,
      const nlohmann::json& feedback);
  void completeCommandRun(
      const std::string& command_run_id,
      const std::string& state,
      const nlohmann::json& result_payload,
      const nlohmann::json& error_data = nlohmann::json());

  [[nodiscard]] std::string insertWorkflowEvent(
      const std::string& run_id,
      const std::optional<std::string>& node_run_id,
      const std::string& event_type,
      const nlohmann::json& payload,
      const std::optional<std::string>& deduplication_key = std::nullopt);
  [[nodiscard]] std::string insertWorkflowSignal(
      const WorkflowSignalEnvelope& envelope);
  [[nodiscard]] std::vector<WorkflowEventRecord> listWorkflowEvents(
      const std::string& run_id);

  [[nodiscard]] std::vector<NodeRunRecord> listWaitingEventNodeRuns(
      const std::string& event_name,
      const std::optional<std::string>& target_run_id = std::nullopt,
      const std::string& business_key = "",
      const std::optional<std::string>& target_workflow_definition_id =
          std::nullopt);

 private:
  PgPool& pool_;
};

}  // namespace dispatcher::db
