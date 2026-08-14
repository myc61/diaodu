#pragma once

#include "dispatcher/concurrency/execution_runtime.hpp"
#include "dispatcher/db/workspace_repository.hpp"
#include "dispatcher/interfaces/interface_catalog.hpp"
#include "dispatcher/ros/beast_transport.hpp"
#include "dispatcher/ros/pose_cache.hpp"
#include "dispatcher/ros/pose_mapper.hpp"
#include "dispatcher/ros/rosbridge_session.hpp"

#include <memory>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace dispatcher::ros {

struct NavigationGoalOptions {
  double distance_tolerance{0.15};
  double heading_tolerance{0.2};
};

struct RosCommandEvent {
  enum class Kind { Feedback, Result };

  Kind kind{Kind::Result};
  std::string correlation_id;
  bool success{false};
  nlohmann::json values = nlohmann::json::object();
  std::string error;
};

using RosCommandHandler = std::function<void(const RosCommandEvent&)>;

struct RosDispatchResult {
  bool accepted{false};
  std::string correlation_id;
  std::string error;
};

class RobotRuntime {
 public:
  RobotRuntime(
      concurrency::ExecutionRuntime& execution,
      db::WorkspaceRepository& repository,
      PoseCache& pose_cache);

  void refreshConnections();
  void dropRobot(const std::string& robot_id);
  bool sendNavigationGoal(
      const db::RobotRecord& robot,
      const db::NavigationGoalRecord& goal,
      double x,
      double y,
      double yaw,
      const NavigationGoalOptions& options = {});

  RosDispatchResult sendNavigationGoalTracked(
      const db::RobotRecord& robot,
      const db::NavigationGoalRecord& goal,
      double x,
      double y,
      double yaw,
      const NavigationGoalOptions& options,
      RosCommandHandler handler);

  RosDispatchResult dispatchCapability(
      const db::RobotRecord& robot,
      const std::string& operation_kind,
      const std::string& endpoint_name,
      const std::string& ros_message_type,
      const nlohmann::json& parameters,
      int timeout_ms,
      const nlohmann::json& protocol_config,
      RosCommandHandler handler);

  // Live rosapi scan via connected rosbridge. Does not require the robot
  // strand; safe to call from HTTP / blocking workers.
  [[nodiscard]] interfaces::InterfaceCatalog discoverInterfaces(
      const std::string& robot_id,
      int timeout_ms = 8000);

  // Resolve a single endpoint type via rosapi (SERVICE/TOPIC).
  [[nodiscard]] std::string resolveInterfaceType(
      const std::string& robot_id,
      const std::string& operation_kind,
      const std::string& endpoint_name,
      int timeout_ms = 5000);

  struct InterfaceSchemaResolution {
    std::string endpoint_name;  // canonical name (case-corrected when needed)
    std::string message_type;
    std::string type_source;
    std::string root_type;
    std::size_t typedef_count{0};
    bool empty_request{false};
    nlohmann::json parameter_schema = nlohmann::json::object({
        {"type", "object"},
        {"properties", nlohmann::json::object()},
    });
    nlohmann::json request_defaults = nlohmann::json::object();
    std::string error;
  };

  // Resolve ROS type + request/message field schema via rosapi typedefs.
  // known_type: optional pre-resolved ROS type (needed for ROS1 ACTION).
  [[nodiscard]] InterfaceSchemaResolution resolveInterfaceSchema(
      const std::string& robot_id,
      const std::string& operation_kind,
      const std::string& endpoint_name,
      const std::string& known_type = "",
      int timeout_ms = 8000);

 private:
  struct RobotSession {
    std::shared_ptr<BeastTransport> transport;
    std::shared_ptr<RosbridgeSession> session;
    db::RobotRecord config;
    std::recursive_mutex session_mutex;
    std::mutex discovery_mutex;
  };

  void ensureRobot(const db::RobotRecord& robot);
  PoseFieldMapping mappingFromJson(const nlohmann::json& json) const;
  RosDispatchResult publishRos1ActionGoal(
      const std::shared_ptr<RobotSession>& session_state,
      const std::string& action_name,
      const std::string& action_type,
      const std::string& goal_id,
      const nlohmann::json& action_goal,
      RosCommandHandler handler);

  concurrency::ExecutionRuntime& execution_;
  db::WorkspaceRepository& repository_;
  PoseCache& pose_cache_;
  std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<RobotSession>> sessions_;
};

}  // namespace dispatcher::ros
