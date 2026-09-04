#include "dispatcher/ros/robot_runtime.hpp"

#include "dispatcher/domain/map_transform.hpp"
#include "dispatcher/ops/ops_log.hpp"
#include "dispatcher/ros/pose_mapper.hpp"
#include "dispatcher/ros/ros_typedef_schema.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <future>
#include <optional>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dispatcher::ros {
namespace {

// navigation/NavigationActionGoal for zj_humanoid (ROS1 actionlib).
nlohmann::json makeNavigationActionGoal(
    const std::string& goal_id,
    double x,
    double y,
    double yaw,
    const NavigationGoalOptions& options) {
  const auto orientation = domain::yawToQuaternion(yaw);
  return nlohmann::json{
      {"header", {{"frame_id", "map"}}},
      {"goal_id",
       {{"stamp", {{"secs", 0}, {"nsecs", 0}}}, {"id", goal_id}}},
      {"goal",
       {{"header", {{"frame_id", "map"}}},
        {"task_type", {{"value", 0}}},  // Routine
        {"waypoints",
         nlohmann::json::array(
             {{{"pose",
                {{"position", {{"x", x}, {"y", y}, {"z", 0.0}}},
                 {"orientation",
                  {{"x", 0.0},
                   {"y", 0.0},
                   {"z", orientation.z},
                   {"w", orientation.w}}}}},
               {"distance_tolerance", options.distance_tolerance},
               {"heading_tolerance", options.heading_tolerance}}})},
        {"translation", {{"enable", false}, {"heading", 0.0}}}}},
  };
}

std::string actionGoalMessageType(const std::string& action_type) {
  if (action_type.size() >= 6 &&
      action_type.compare(action_type.size() - 6, 6, "Action") == 0) {
    return action_type + "Goal";
  }
  return action_type;
}

std::string actionGoalTopic(const std::string& server_name) {
  if (server_name.size() >= 5 &&
      server_name.compare(server_name.size() - 5, 5, "/goal") == 0) {
    return server_name;
  }
  return server_name + "/goal";
}

std::string actionTopic(const std::string& server_name, const char* suffix) {
  const auto goal_topic = actionGoalTopic(server_name);
  return goal_topic.substr(0, goal_topic.size() - 5) + suffix;
}

std::string actionEnvelopeType(
    const std::string& action_type, const char* suffix) {
  if (action_type.size() >= 6 &&
      action_type.compare(action_type.size() - 6, 6, "Action") == 0) {
    return action_type + suffix;
  }
  return action_type;
}

std::string actionMessageGoalId(const nlohmann::json& message) {
  if (message.contains("status") && message["status"].is_object()) {
    const auto& status = message["status"];
    if (status.contains("goal_id") && status["goal_id"].is_object()) {
      return status["goal_id"].value("id", "");
    }
  }
  if (message.contains("goal_id") && message["goal_id"].is_object()) {
    return message["goal_id"].value("id", "");
  }
  return {};
}

std::optional<int> actionStatusCode(const nlohmann::json& message) {
  auto readCode = [](const nlohmann::json& object) -> std::optional<int> {
    if (object.contains("status") && object["status"].is_number_integer()) {
      return object["status"].get<int>();
    }
    if (object.contains("value") && object["value"].is_number_integer()) {
      return object["value"].get<int>();
    }
    return std::nullopt;
  };

  if (message.contains("status") && message["status"].is_object()) {
    if (const auto code = readCode(message["status"]); code.has_value()) {
      return code;
    }
  }
  if (message.contains("result") && message["result"].is_object()) {
    const auto& result = message["result"];
    if (result.contains("status") && result["status"].is_object()) {
      if (const auto code = readCode(result["status"]); code.has_value()) {
        return code;
      }
    }
    if (const auto code = readCode(result); code.has_value()) {
      return code;
    }
  }
  return readCode(message);
}

// zj_humanoid navigation/Status: Idle=0 Active=1 Running=2 Arrived=3
// Canceling=4 Cancelled=5 Succeeded=6 Failed=7 Error=8 Aborted=9
// Generic ROS1 actionlib: SUCCEEDED=3.
std::optional<bool> actionResultSucceeded(
    const nlohmann::json& message, bool zj_navigation_status) {
  const auto code = actionStatusCode(message);
  if (zj_navigation_status) {
    if (code.has_value()) {
      if (*code == 6) {
        return true;
      }
      if (*code == 5 || *code == 7 || *code == 8 || *code == 9) {
        return false;
      }
      return std::nullopt;
    }
  } else if (code.has_value()) {
    return *code == 3;
  }
  if (message.contains("result") && message["result"].is_object()) {
    const auto& result = message["result"];
    if (result.contains("success") && result["success"].is_boolean()) {
      return result["success"].get<bool>();
    }
  }
  if (message.contains("success") && message["success"].is_boolean()) {
    return message["success"].get<bool>();
  }
  return zj_navigation_status ? std::nullopt : std::optional<bool>{false};
}

std::string actionResultError(const nlohmann::json& message) {
  if (message.contains("status") && message["status"].is_object()) {
    return message["status"].value("text", "");
  }
  return message.value("error", "");
}

nlohmann::json makeGenericActionGoal(
    const std::string& goal_id, const nlohmann::json& goal) {
  return nlohmann::json{
      {"header", {{"frame_id", ""}}},
      {"goal_id",
       {{"stamp", {{"secs", 0}, {"nsecs", 0}}}, {"id", goal_id}}},
      {"goal", goal},
  };
}

std::string utcNowIso() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&time, &tm);
  char buffer[32];
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
    return {};
  }
  return buffer;
}

std::optional<ServiceResponse> callServiceSync(
    RosbridgeSession& session,
    std::recursive_mutex& session_mutex,
    const std::string& service,
    const nlohmann::json& args,
    double timeout_seconds) {
  auto promise = std::make_shared<std::promise<ServiceResponse>>();
  auto future = promise->get_future();
  std::string correlation_id;
  {
    std::lock_guard lock(session_mutex);
    correlation_id = session.callService(
        service,
        args,
        timeout_seconds,
        [promise](const ServiceResponse& response) {
          try {
            promise->set_value(response);
          } catch (...) {
          }
        });
  }
  if (correlation_id.empty()) {
    return std::nullopt;
  }
  const auto wait_for = std::chrono::milliseconds(
      static_cast<int>(timeout_seconds * 1000.0) + 800);
  if (future.wait_for(wait_for) != std::future_status::ready) {
    std::lock_guard lock(session_mutex);
    (void)session.forgetServiceCall(correlation_id);
    return std::nullopt;
  }
  return future.get();
}

std::string stripSuffix(const std::string& value, const std::string& suffix) {
  if (value.size() >= suffix.size() &&
      value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0) {
    return value.substr(0, value.size() - suffix.size());
  }
  return {};
}

bool isServiceResponse(std::string_view payload) {
  try {
    const auto message = nlohmann::json::parse(payload);
    return message.is_object() && message.value("op", "") == "service_response";
  } catch (...) {
    return false;
  }
}

}  // namespace

RobotRuntime::RobotRuntime(
    concurrency::ExecutionRuntime& execution,
    db::WorkspaceRepository& repository,
    PoseCache& pose_cache)
    : execution_(execution),
      repository_(repository),
      pose_cache_(pose_cache) {}

PoseFieldMapping RobotRuntime::mappingFromJson(
    const nlohmann::json& json) const {
  PoseFieldMapping mapping;
  mapping.x_path = json.value("x_path", "");
  mapping.y_path = json.value("y_path", "");
  if (json.contains("yaw_path")) {
    mapping.yaw_path = json.at("yaw_path").get<std::string>();
  }
  if (json.contains("quaternion_x_path")) {
    mapping.quaternion_x_path =
        json.at("quaternion_x_path").get<std::string>();
  }
  if (json.contains("quaternion_y_path")) {
    mapping.quaternion_y_path =
        json.at("quaternion_y_path").get<std::string>();
  }
  if (json.contains("quaternion_z_path")) {
    mapping.quaternion_z_path =
        json.at("quaternion_z_path").get<std::string>();
  }
  if (json.contains("quaternion_w_path")) {
    mapping.quaternion_w_path =
        json.at("quaternion_w_path").get<std::string>();
  }
  if (json.contains("timestamp_path")) {
    mapping.timestamp_path = json.at("timestamp_path").get<std::string>();
  }
  if (json.contains("frame_id_path")) {
    mapping.frame_id_path = json.at("frame_id_path").get<std::string>();
  }
  return mapping;
}

void RobotRuntime::ensureRobot(const db::RobotRecord& robot) {
  if (!robot.host.has_value() || !robot.rosbridge_port.has_value() ||
      !robot.pose_topic.has_value() || !robot.pose_message_type.has_value()) {
    ops::OpsLog::instance().warn(
        "ros",
        "skip connect (incomplete config): " + robot.name,
        {{"robot_id", robot.id},
         {"host", robot.host.value_or("")},
         {"pose_topic", robot.pose_topic.value_or("")}});
    return;
  }

  {
    std::lock_guard lock(mutex_);
    const auto it = sessions_.find(robot.id);
    if (it != sessions_.end()) {
      const auto& current = it->second->config;
      const bool same_endpoint =
          current.host == robot.host &&
          current.rosbridge_port == robot.rosbridge_port &&
          current.rosbridge_path == robot.rosbridge_path &&
          current.rosbridge_tls == robot.rosbridge_tls &&
          current.pose_topic == robot.pose_topic;
      if (same_endpoint) {
        it->second->config = robot;
        return;
      }
    }
  }
  dropRobot(robot.id);

  auto session_state = std::make_shared<RobotSession>();
  session_state->config = robot;
  auto transport = std::make_shared<BeastTransport>(
      *robot.host,
      static_cast<std::uint16_t>(*robot.rosbridge_port),
      robot.rosbridge_path.value_or("/"),
      robot.rosbridge_tls);
  auto session = std::make_shared<RosbridgeSession>(transport);
  session_state->transport = transport;
  session_state->session = session;

  const auto robot_id = robot.id;
  const auto mapping = mappingFromJson(robot.pose_mapping);
  const auto stale_timeout =
      std::chrono::milliseconds(robot.stale_timeout_ms);
  const auto pose_topic = *robot.pose_topic;
  const auto pose_type = *robot.pose_message_type;

  ops::OpsLog::instance().info(
      "ros",
      "connecting rosbridge: " + robot.name,
      {{"robot_id", robot.id},
       {"host", *robot.host},
       {"port", *robot.rosbridge_port},
       {"path", robot.rosbridge_path.value_or("/")},
       {"pose_topic", pose_topic}});

  transport->start(
      [session, session_state, robot_id, this](std::string_view payload) {
        auto handle_message = [session, session_state,
                               payload = std::string(payload)] {
          std::lock_guard lock(session_state->session_mutex);
          session->handleIncoming(payload);
        };
        // Service responses are control-plane traffic. Do not queue them
        // behind a high-rate pose topic on the per-robot data strand.
        if (isServiceResponse(payload)) {
          execution_.postParallel(std::move(handle_message));
        } else {
          execution_.postRobot(robot_id, std::move(handle_message));
        }
      },
      [session, session_state, robot_id, robot, this](
          bool connected, std::string_view reason) {
        execution_.postRobot(
            robot_id,
            [session, session_state, connected, robot,
             reason = std::string(reason), this] {
              {
                std::lock_guard lock(session_state->session_mutex);
                if (connected) {
                  session->onTransportConnected();
                  // DB check constraint uses ONLINE (not CONNECTED).
                  repository_.updateRobotConnectionState(robot.id, "ONLINE");
                  ops::OpsLog::instance().info(
                      "ros",
                      "rosbridge connected: " + robot.name,
                      {{"robot_id", robot.id},
                       {"host", robot.host.value_or("")},
                       {"pose_topic", robot.pose_topic.value_or("")},
                       {"reason", reason}});
                } else {
                  session->onTransportClosed();
                  repository_.updateRobotConnectionState(
                      robot.id, "DISCONNECTED");
                  ops::OpsLog::instance().warn(
                      "ros",
                      "rosbridge disconnected: " + robot.name,
                      {{"robot_id", robot.id},
                       {"host", robot.host.value_or("")},
                       {"reason", reason}});
                }
              }
              // Scan on blocking pool — never wait on the robot strand
              // (incoming service responses are also serialized there).
              if (connected) {
                execution_.postBlocking([this, robot_id = robot.id] {
                  try {
                    auto catalog = discoverInterfaces(robot_id, 8000);
                    if (catalog.error.empty() || !catalog.topics.empty() ||
                        !catalog.services.empty() || !catalog.actions.empty()) {
                      if (const auto cached =
                              repository_.getRobotInterfaceCache(robot_id);
                          cached.has_value()) {
                        catalog.preserveFailedCategoriesFrom(
                            interfaces::InterfaceCatalog::fromJson(*cached));
                      }
                      catalog.live = false;
                      repository_.setRobotInterfaceCache(
                          robot_id, catalog.toJson());
                      ops::OpsLog::instance().info(
                          "ros",
                          "interface cache refreshed",
                          {{"robot_id", robot_id},
                           {"topics", catalog.topics.size()},
                           {"services", catalog.services.size()},
                           {"actions", catalog.actions.size()}});
                    } else if (!catalog.error.empty()) {
                      ops::OpsLog::instance().warn(
                          "ros",
                          "interface scan failed: " + catalog.error,
                          {{"robot_id", robot_id}});
                    }
                  } catch (const std::exception& ex) {
                    ops::OpsLog::instance().warn(
                        "ros",
                        std::string("interface scan exception: ") + ex.what(),
                        {{"robot_id", robot_id}});
                  }
                });
              }
            });
      });

  execution_.postRobot(robot_id, [session, session_state, pose_topic, pose_type,
                                  mapping, stale_timeout, this] {
    std::lock_guard lock(session_state->session_mutex);
    (void)session->subscribe(
        pose_topic,
        pose_type,
        {.throttle_rate_ms = 100, .queue_length = 1},
        [session_state, mapping, stale_timeout, this](
            std::string_view, const Json& message) {
          db::RobotRecord robot;
          {
            std::lock_guard lock(mutex_);
            robot = session_state->config;
          }
          const auto received_at = std::chrono::system_clock::now();
          auto parsed = PoseMapper::parse(
              message,
              mapping,
              robot.id,
              robot.current_scene_id.value_or(""),
              robot.current_map_version_id.value_or(""),
              "map",
              received_at);
          if (!parsed.valid) {
            ops::OpsLog::instance().warn(
                "ros",
                "pose parse failed: " + robot.name,
                {{"robot_id", robot.id}, {"error", parsed.error}});
            return;
          }
          // When ROS stamp drifts far from wall clock, freshness follows receive time.
          const auto skew = parsed.pose.timestamp > received_at
                                ? parsed.pose.timestamp - received_at
                                : received_at - parsed.pose.timestamp;
          if (skew > std::chrono::seconds(60)) {
            parsed.pose.timestamp = received_at;
          }
          const bool fresh =
              PoseMapper::isFresh(parsed.pose, stale_timeout, received_at);
          const bool matched =
              robot.current_scene_id.has_value() &&
              robot.current_map_version_id.has_value() &&
              parsed.pose.scene_id == *robot.current_scene_id &&
              parsed.pose.map_version_id == *robot.current_map_version_id;
          CachedPose cached{
              .pose = parsed.pose,
              .stale = !fresh,
              .scene_map_matched = matched,
              .updated_at = received_at,
          };
          // For configured pose topics that omit scene/map fields, treat the
          // robot's current affiliation as the match source of truth.
          if (parsed.pose.scene_id.empty() && robot.current_scene_id.has_value()) {
            cached.pose.scene_id = *robot.current_scene_id;
            cached.pose.map_version_id =
                robot.current_map_version_id.value_or("");
            cached.scene_map_matched = true;
          }
          pose_cache_.upsert(cached);
          nlohmann::json pose_json{
              {"x", cached.pose.x},
              {"y", cached.pose.y},
              {"yaw", cached.pose.yaw},
              {"frame_id", cached.pose.frame_id},
              {"scene_id", cached.pose.scene_id},
              {"map_version_id", cached.pose.map_version_id},
              {"stale", cached.stale},
          };
          repository_.updateRobotPoseCache(
              robot.id,
              pose_json,
              cached.stale ? "STALE" : "LOCALIZED");
        });
  });

  {
    std::lock_guard lock(mutex_);
    sessions_[robot.id] = std::move(session_state);
  }
}

void RobotRuntime::dropRobot(const std::string& robot_id) {
  std::shared_ptr<RobotSession> session_state;
  {
    std::lock_guard lock(mutex_);
    const auto it = sessions_.find(robot_id);
    if (it == sessions_.end()) {
      return;
    }
    session_state = it->second;
    sessions_.erase(it);
  }
  if (session_state != nullptr && session_state->transport != nullptr) {
    session_state->transport->stop();
  }
}

void RobotRuntime::refreshConnections() {
  const auto robots = repository_.listRobots();
  std::unordered_set<std::string> wanted;
  for (const auto& robot : robots) {
    const bool should_connect =
        robot.enabled && robot.configuration_state == "READY" &&
        robot.host.has_value() && !robot.host->empty() &&
        robot.pose_topic.has_value() && !robot.pose_topic->empty();
    if (!should_connect) {
      continue;
    }
    wanted.insert(robot.id);
    try {
      ensureRobot(robot);
    } catch (const std::exception& ex) {
      ops::OpsLog::instance().error(
          "ros",
          "ensureRobot failed: " + robot.name,
          {{"robot_id", robot.id}, {"error", ex.what()}});
    } catch (...) {
      ops::OpsLog::instance().error(
          "ros",
          "ensureRobot failed: " + robot.name,
          {{"robot_id", robot.id}, {"error", "unknown"}});
    }
  }

  std::vector<std::string> stale;
  {
    std::lock_guard lock(mutex_);
    for (const auto& [robot_id, _] : sessions_) {
      if (!wanted.contains(robot_id)) {
        stale.push_back(robot_id);
      }
    }
  }
  for (const auto& robot_id : stale) {
    dropRobot(robot_id);
  }
}

bool RobotRuntime::sendNavigationGoal(
    const db::RobotRecord& robot,
    const db::NavigationGoalRecord& goal,
    double x,
    double y,
    double yaw,
    const NavigationGoalOptions& options) {
  return sendNavigationGoalTracked(
             robot, goal, x, y, yaw, options, RosCommandHandler{})
      .accepted;
}

RosDispatchResult RobotRuntime::sendNavigationGoalTracked(
    const db::RobotRecord& robot,
    const db::NavigationGoalRecord& goal,
    double x,
    double y,
    double yaw,
    const NavigationGoalOptions& options,
    RosCommandHandler handler) {
  ensureRobot(robot);
  std::shared_ptr<RobotSession> session_state;
  {
    std::lock_guard lock(mutex_);
    const auto it = sessions_.find(robot.id);
    if (it == sessions_.end()) {
      return {.error = "robot session not found"};
    }
    session_state = it->second;
  }
  if (session_state->session == nullptr ||
      !robot.nav_action.has_value() || !robot.nav_action_type.has_value()) {
    return {.error = "navigation action is not configured"};
  }

  if (!session_state->transport || !session_state->transport->connected()) {
    return {.error = "robot rosbridge is offline"};
  }

  const auto msg = makeNavigationActionGoal(
      goal.command_id, x, y, yaw, options);
  auto dispatched = publishRos1ActionGoal(
      session_state,
      *robot.nav_action,
      *robot.nav_action_type,
      goal.command_id,
      msg,
      std::move(handler),
      true);
  if (!dispatched.accepted) {
    ops::OpsLog::instance().error(
        "nav",
        "navigation goal publish failed: " + robot.name,
        {{"robot_id", robot.id},
         {"topic", actionGoalTopic(*robot.nav_action)},
         {"type", actionGoalMessageType(*robot.nav_action_type)},
         {"error", dispatched.error}});
    return dispatched;
  }
  repository_.updateOutboxState(goal.outbox_id, "SENT");
  ops::OpsLog::instance().info(
      "nav",
      "navigation goal sent: " + robot.name,
      {{"robot_id", robot.id},
       {"command_id", goal.command_id},
       {"topic", actionGoalTopic(*robot.nav_action)},
       {"x", x},
       {"y", y},
       {"yaw", yaw},
       {"distance_tolerance", options.distance_tolerance},
       {"heading_tolerance", options.heading_tolerance}});
  return dispatched;
}

RosDispatchResult RobotRuntime::publishRos1ActionGoal(
    const std::shared_ptr<RobotSession>& session_state,
    const std::string& action_name,
    const std::string& action_type,
    const std::string& goal_id,
    const nlohmann::json& action_goal,
    RosCommandHandler handler,
    bool zj_navigation_status) {
  if (session_state == nullptr || session_state->session == nullptr) {
    return {.error = "robot session is unavailable"};
  }

  struct SubscriptionState {
    std::string feedback_id;
    std::string result_id;
  };
  auto subscriptions = std::make_shared<SubscriptionState>();

  std::lock_guard lock(session_state->session_mutex);
  if (handler) {
    subscriptions->feedback_id = session_state->session->subscribe(
        actionTopic(action_name, "/feedback"),
        actionEnvelopeType(action_type, "Feedback"),
        {},
        [goal_id, handler](std::string_view, const Json& message) {
          if (actionMessageGoalId(message) != goal_id) {
            return;
          }
          handler(RosCommandEvent{
              .kind = RosCommandEvent::Kind::Feedback,
              .correlation_id = goal_id,
              .success = true,
              .values = message,
          });
        });
    subscriptions->result_id = session_state->session->subscribe(
        actionTopic(action_name, "/result"),
        actionEnvelopeType(action_type, "Result"),
        {},
        [session_state, subscriptions, goal_id, handler, zj_navigation_status](
            std::string_view, const Json& message) {
          if (actionMessageGoalId(message) != goal_id) {
            return;
          }
          const auto outcome =
              actionResultSucceeded(message, zj_navigation_status);
          if (!outcome.has_value()) {
            return;
          }
          const bool success = *outcome;
          const auto error = actionResultError(message);
          if (!subscriptions->feedback_id.empty()) {
            (void)session_state->session->unsubscribe(
                subscriptions->feedback_id);
          }
          if (!subscriptions->result_id.empty()) {
            (void)session_state->session->unsubscribe(
                subscriptions->result_id);
          }
          handler(RosCommandEvent{
              .kind = RosCommandEvent::Kind::Result,
              .correlation_id = goal_id,
              .success = success,
              .values = message,
              .error = error,
          });
        });
  }

  const auto goal_topic = actionGoalTopic(action_name);
  const auto goal_type = actionGoalMessageType(action_type);
  if (!session_state->session->advertise(goal_topic, goal_type) ||
      !session_state->session->publish(goal_topic, action_goal, goal_type)) {
    if (!subscriptions->feedback_id.empty()) {
      (void)session_state->session->unsubscribe(subscriptions->feedback_id);
    }
    if (!subscriptions->result_id.empty()) {
      (void)session_state->session->unsubscribe(subscriptions->result_id);
    }
    return {.error = "failed to publish ROS1 actionlib goal"};
  }
  return {.accepted = true, .correlation_id = goal_id};
}

RosDispatchResult RobotRuntime::dispatchCapability(
    const db::RobotRecord& robot,
    const std::string& operation_kind,
    const std::string& endpoint_name,
    const std::string& ros_message_type,
    const nlohmann::json& parameters,
    int timeout_ms,
    const nlohmann::json& protocol_config,
    RosCommandHandler handler) {
  if (endpoint_name.empty()) {
    return {.error = "capability endpoint is empty"};
  }
  ensureRobot(robot);
  std::shared_ptr<RobotSession> session_state;
  {
    std::lock_guard lock(mutex_);
    const auto it = sessions_.find(robot.id);
    if (it == sessions_.end()) {
      return {.error = "robot session not found"};
    }
    session_state = it->second;
  }
  if (session_state->session == nullptr || !session_state->transport ||
      !session_state->transport->connected()) {
    return {.error = "robot rosbridge is offline"};
  }

  const std::string kind = operation_kind;
  if (kind == "service" || kind == "SERVICE") {
    std::lock_guard lock(session_state->session_mutex);
    const auto correlation_id = session_state->session->callService(
        endpoint_name,
        parameters,
        timeout_ms > 0
            ? std::optional<double>(static_cast<double>(timeout_ms) / 1000.0)
            : std::nullopt,
        [handler = std::move(handler)](const ServiceResponse& response) {
          if (handler) {
            handler(RosCommandEvent{
                .kind = RosCommandEvent::Kind::Result,
                .correlation_id = response.id,
                .success = response.success,
                .values = response.values,
                .error = response.error,
            });
          }
        });
    if (correlation_id.empty()) {
      return {.error = "failed to send rosbridge service request"};
    }
    return {.accepted = true, .correlation_id = correlation_id};
  }
  if (kind == "action" || kind == "ACTION") {
    if (ros_message_type.empty()) {
      return {.error = "action ROS type is empty"};
    }
    if (protocol_config.value("adapter", "") == "ROS1_ACTIONLIB") {
      const auto goal_id = protocol_config.value("goal_id", "");
      if (goal_id.empty()) {
        return {.error = "ROS1 actionlib goal_id is required"};
      }
      return publishRos1ActionGoal(
          session_state,
          endpoint_name,
          ros_message_type,
          goal_id,
          makeGenericActionGoal(goal_id, parameters),
          std::move(handler));
    }
    std::lock_guard lock(session_state->session_mutex);
    const auto correlation_id = session_state->session->sendActionGoal(
        endpoint_name,
        ros_message_type,
        parameters,
        [handler = std::move(handler)](const ActionEvent& event) {
          if (handler) {
            handler(RosCommandEvent{
                .kind = event.kind == ActionEvent::Kind::Feedback
                    ? RosCommandEvent::Kind::Feedback
                    : RosCommandEvent::Kind::Result,
                .correlation_id = event.id,
                .success = event.success,
                .values = event.values,
                .error = event.error,
            });
          }
        },
        true);
    if (correlation_id.empty()) {
      return {.error = "failed to send rosbridge action goal"};
    }
    return {.accepted = true, .correlation_id = correlation_id};
  }
  if (kind == "topic" || kind == "TOPIC") {
    std::lock_guard lock(session_state->session_mutex);
    if (!ros_message_type.empty() &&
        !session_state->session->advertise(endpoint_name, ros_message_type)) {
      return {.error = "failed to advertise capability topic"};
    }
    if (!session_state->session->publish(
            endpoint_name,
            parameters,
            ros_message_type.empty()
                ? std::nullopt
                : std::optional<std::string>(ros_message_type))) {
      return {.error = "failed to publish capability topic"};
    }
    return {.accepted = true, .correlation_id = endpoint_name};
  }
  return {.error = "unsupported capability operation kind: " + kind};
}

interfaces::InterfaceCatalog RobotRuntime::discoverInterfaces(
    const std::string& robot_id,
    int timeout_ms) {
  interfaces::InterfaceCatalog catalog;
  catalog.entity_kind = "robot";
  catalog.entity_id = robot_id;
  catalog.transport =
      interfaces::transportKindName(interfaces::TransportKind::Rosbridge);
  catalog.scanned_at = utcNowIso();
  catalog.live = true;

  std::shared_ptr<RobotSession> session_state;
  {
    std::lock_guard lock(mutex_);
    const auto it = sessions_.find(robot_id);
    if (it == sessions_.end()) {
      catalog.error = "robot session not found";
      catalog.live = false;
      return catalog;
    }
    session_state = it->second;
  }
  if (session_state->session == nullptr || !session_state->transport ||
      !session_state->transport->connected()) {
    catalog.error = "robot rosbridge is offline";
    catalog.live = false;
    return catalog;
  }
  std::lock_guard discovery_lock(session_state->discovery_mutex);

  const double timeout_seconds =
      timeout_ms > 0 ? static_cast<double>(timeout_ms) / 1000.0 : 8.0;

  auto scanOnce = [&] {
    catalog.scanned_at = utcNowIso();
    catalog.topics.clear();
    catalog.services.clear();
    catalog.actions.clear();
    catalog.topic_error.clear();
    catalog.service_error.clear();
    catalog.action_error.clear();
    catalog.error.clear();

    // Scan services first. A topic failure must never prevent service
    // discovery (the capability editor primarily consumes this list).
    const auto services_response = callServiceSync(
        *session_state->session,
        session_state->session_mutex,
        "/rosapi/services",
        nlohmann::json::object(),
        timeout_seconds);
    const bool services_timed_out = !services_response.has_value();
    if (services_timed_out) {
      catalog.service_error = "rosapi /rosapi/services timed out";
    } else if (!services_response->success) {
      catalog.service_error = services_response->error.empty()
          ? "rosapi /rosapi/services failed"
          : services_response->error;
    } else {
      const auto services =
          services_response->values.value("services", nlohmann::json::array());
      catalog.services.reserve(services.size());
      for (const auto& item : services) {
        if (!item.is_string()) {
          continue;
        }
        catalog.services.push_back(interfaces::DiscoveredEndpoint{
            .name = item.get<std::string>(),
            .message_type = "",
            .operation_kind = "SERVICE",
            .transport = catalog.transport,
        });
      }
    }

    const auto topics_response = callServiceSync(
        *session_state->session,
        session_state->session_mutex,
        "/rosapi/topics",
        nlohmann::json::object(),
        timeout_seconds);
    const bool topics_timed_out = !topics_response.has_value();
    if (topics_timed_out) {
      catalog.topic_error = "rosapi /rosapi/topics timed out";
      catalog.action_error =
          "action discovery unavailable because topic scan timed out";
    } else if (!topics_response->success) {
      catalog.topic_error = topics_response->error.empty()
          ? "rosapi /rosapi/topics failed"
          : topics_response->error;
      catalog.action_error =
          "action discovery unavailable because topic scan failed";
    } else {
      const auto& topic_values = topics_response->values;
      const auto topics =
          topic_values.value("topics", nlohmann::json::array());
      const auto types = topic_values.value("types", nlohmann::json::array());
      catalog.topics.reserve(topics.size());
      for (std::size_t i = 0; i < topics.size(); ++i) {
        if (!topics[i].is_string()) {
          continue;
        }
        const std::string name = topics[i].get<std::string>();
        std::string message_type;
        if (i < types.size() && types[i].is_string()) {
          message_type = types[i].get<std::string>();
        }
        catalog.topics.push_back(interfaces::DiscoveredEndpoint{
            .name = name,
            .message_type = message_type,
            .operation_kind = "TOPIC",
            .transport = catalog.transport,
        });

        // ROS1 actionlib: */goal + *ActionGoal -> action server name/type.
        if (const auto action_name = stripSuffix(name, "/goal");
            !action_name.empty()) {
          if (const auto action_type = stripSuffix(message_type, "Goal");
              !action_type.empty() && action_type.size() >= 6 &&
              action_type.compare(
                  action_type.size() - 6, 6, "Action") == 0) {
            catalog.actions.push_back(interfaces::DiscoveredEndpoint{
                .name = action_name,
                .message_type = action_type,
                .operation_kind = "ACTION",
                .transport = catalog.transport,
            });
          }
        }
      }
    }

    if (!catalog.service_error.empty()) {
      catalog.error = catalog.service_error;
    }
    if (!catalog.topic_error.empty()) {
      if (!catalog.error.empty()) {
        catalog.error += "; ";
      }
      catalog.error += catalog.topic_error;
    }
    catalog.live = catalog.error.empty();
    return std::pair{services_timed_out, topics_timed_out};
  };

  auto [services_timed_out, topics_timed_out] = scanOnce();
  if (services_timed_out && topics_timed_out) {
    // The TCP connection may be half-open (for example after Wi-Fi resume).
    // Force the transport worker out of its blocking read and wait briefly for
    // its existing reconnect loop before retrying this scan once.
    repository_.updateRobotConnectionState(robot_id, "DISCONNECTED");
    session_state->transport->requestReconnect();
    const auto reconnect_deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(std::min(timeout_ms > 0 ? timeout_ms : 8000, 8000));
    while (std::chrono::steady_clock::now() < reconnect_deadline) {
      bool online = false;
      {
        std::lock_guard lock(session_state->session_mutex);
        online = session_state->transport->connected() &&
            session_state->session->state() ==
                domain::ConnectionState::Online;
      }
      if (online) {
        (void)scanOnce();
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  return catalog;
}

std::string RobotRuntime::resolveInterfaceType(
    const std::string& robot_id,
    const std::string& operation_kind,
    const std::string& endpoint_name,
    int timeout_ms) {
  if (endpoint_name.empty()) {
    return {};
  }
  std::shared_ptr<RobotSession> session_state;
  {
    std::lock_guard lock(mutex_);
    const auto it = sessions_.find(robot_id);
    if (it == sessions_.end()) {
      return {};
    }
    session_state = it->second;
  }
  if (session_state->session == nullptr || !session_state->transport ||
      !session_state->transport->connected()) {
    return {};
  }

  const double timeout_seconds =
      timeout_ms > 0 ? static_cast<double>(timeout_ms) / 1000.0 : 5.0;
  const std::string kind = operation_kind;
  std::string service;
  nlohmann::json args;
  if (kind == "SERVICE" || kind == "service") {
    service = "/rosapi/service_type";
    args = {{"service", endpoint_name}};
  } else if (kind == "TOPIC" || kind == "topic") {
    service = "/rosapi/topic_type";
    args = {{"topic", endpoint_name}};
  } else {
    return {};
  }

  const auto response = callServiceSync(
      *session_state->session,
      session_state->session_mutex,
      service,
      args,
      timeout_seconds);
  if (!response.has_value() || !response->success) {
    return {};
  }
  return response->values.value("type", "");
}

namespace {

std::string toLowerAscii(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

// Resolve canonical endpoint + type from robots.metadata.interface_cache when
// rosapi is case-sensitive and the UI name casing differs (movel vs moveL).
bool lookupCachedEndpoint(
    db::WorkspaceRepository& repository,
    const std::string& robot_id,
    const std::string& kind,
    const std::string& endpoint_name,
    std::string& canonical_name,
    std::string& cached_type) {
  const auto cache = repository.getRobotInterfaceCache(robot_id);
  if (!cache.has_value()) {
    return false;
  }
  const char* list_key = "services";
  if (kind == "TOPIC" || kind == "topic") {
    list_key = "topics";
  } else if (kind == "ACTION" || kind == "action") {
    list_key = "actions";
  }
  const auto items = cache->value(list_key, nlohmann::json::array());
  if (!items.is_array()) {
    return false;
  }
  const auto needle = toLowerAscii(endpoint_name);
  for (const auto& item : items) {
    if (!item.is_object()) {
      continue;
    }
    const auto name = item.value("name", "");
    if (name.empty()) {
      continue;
    }
    if (name == endpoint_name || toLowerAscii(name) == needle) {
      canonical_name = name;
      cached_type = item.value("message_type", "");
      return true;
    }
  }
  return false;
}

}  // namespace

RobotRuntime::InterfaceSchemaResolution RobotRuntime::resolveInterfaceSchema(
    const std::string& robot_id,
    const std::string& operation_kind,
    const std::string& endpoint_name,
    const std::string& known_type,
    int timeout_ms) {
  InterfaceSchemaResolution result;
  if (endpoint_name.empty()) {
    result.error = "endpoint name is empty";
    return result;
  }
  result.endpoint_name = endpoint_name;

  std::shared_ptr<RobotSession> session_state;
  {
    std::lock_guard lock(mutex_);
    const auto it = sessions_.find(robot_id);
    if (it == sessions_.end()) {
      result.error = "robot session not found";
      return result;
    }
    session_state = it->second;
  }
  if (session_state->session == nullptr || !session_state->transport ||
      !session_state->transport->connected()) {
    result.error = "robot rosbridge is offline";
    return result;
  }

  const double timeout_seconds =
      timeout_ms > 0 ? static_cast<double>(timeout_ms) / 1000.0 : 8.0;
  const std::string kind = operation_kind;

  std::string cached_name;
  std::string cached_type;
  const bool have_cache = lookupCachedEndpoint(
      repository_, robot_id, kind, endpoint_name, cached_name, cached_type);
  if (have_cache) {
    result.endpoint_name = cached_name;
    if (known_type.empty() && !cached_type.empty()) {
      result.message_type = cached_type;
    }
  }

  std::string details_service;
  nlohmann::json details_args;
  if (kind == "SERVICE" || kind == "service") {
    auto resolve_type = [&](const std::string& service_name) {
      const auto type_response = callServiceSync(
          *session_state->session,
          session_state->session_mutex,
          "/rosapi/service_type",
          nlohmann::json{{"service", service_name}},
          timeout_seconds);
      if (!type_response.has_value() || !type_response->success) {
        return std::string{};
      }
      return type_response->values.value("type", "");
    };
    // A schema refresh must query the live graph first. The scan/edited type
    // is only a fallback because nodes may restart with a different type.
    result.message_type = resolve_type(result.endpoint_name);
    if (!result.message_type.empty()) {
      result.type_source = "rosapi/service_type";
    }
    // Case mismatch: moveL → movel via cache, then re-query type.
    if (result.message_type.empty() && have_cache &&
        cached_name != endpoint_name) {
      result.endpoint_name = cached_name;
      result.message_type = resolve_type(cached_name);
      if (!result.message_type.empty()) {
        result.type_source = "rosapi/service_type(canonical)";
      }
    }
    if (result.message_type.empty() && !known_type.empty()) {
      result.message_type = known_type;
      result.type_source = "request_fallback";
    }
    if (result.message_type.empty() && !cached_type.empty()) {
      result.message_type = cached_type;
      result.type_source = "scan_cache_fallback";
    }
    if (result.message_type.empty()) {
      result.error =
          "empty service type (check exact service name casing from scan list)";
      return result;
    }
    details_service = "/rosapi/service_request_details";
    details_args = {{"type", result.message_type}};
  } else if (kind == "TOPIC" || kind == "topic") {
    const auto type_response = callServiceSync(
        *session_state->session,
        session_state->session_mutex,
        "/rosapi/topic_type",
        nlohmann::json{{"topic", result.endpoint_name}},
        timeout_seconds);
    if (type_response.has_value() && type_response->success) {
      result.message_type = type_response->values.value("type", "");
      if (!result.message_type.empty()) {
        result.type_source = "rosapi/topic_type";
      }
    }
    if (result.message_type.empty() && !known_type.empty()) {
      result.message_type = known_type;
      result.type_source = "request_fallback";
    }
    if (result.message_type.empty() && have_cache && !cached_type.empty()) {
      result.message_type = cached_type;
      result.endpoint_name = cached_name;
      result.type_source = "scan_cache_fallback";
    }
    if (result.message_type.empty()) {
      result.error = "empty topic type";
      return result;
    }
    details_service = "/rosapi/message_details";
    details_args = {{"type", result.message_type}};
  } else if (kind == "ACTION" || kind == "action") {
    if (!known_type.empty()) {
      result.message_type = known_type;
      result.type_source = "request";
    }
    if (result.message_type.empty() && have_cache) {
      result.message_type = cached_type;
      result.endpoint_name = cached_name;
      if (!result.message_type.empty()) {
        result.type_source = "scan_cache";
      }
    }
    if (result.message_type.empty()) {
      const auto type_response = callServiceSync(
          *session_state->session,
          session_state->session_mutex,
          "/rosapi/action_type",
          nlohmann::json{{"action", result.endpoint_name}},
          timeout_seconds);
      if (type_response.has_value() && type_response->success) {
        result.message_type = type_response->values.value("type", "");
        if (!result.message_type.empty()) {
          result.type_source = "rosapi/action_type";
        }
      }
    }
    if (result.message_type.empty()) {
      result.error =
          "ACTION schema requires known action type (ROS1: select from scan "
          "cache or fill ROS type first)";
      return result;
    }
    details_service = "/rosapi/message_details";
    details_args = {{"type", result.message_type + "Goal"}};
  } else {
    result.error = "unsupported operation kind";
    return result;
  }

  const auto details_response = callServiceSync(
      *session_state->session,
      session_state->session_mutex,
      details_service,
      details_args,
      timeout_seconds);
  if (!details_response.has_value() || !details_response->success) {
    result.error = "rosapi typedef details failed: " + details_service;
    return result;
  }

  const auto typedefs =
      details_response->values.value("typedefs", nlohmann::json::array());
  result.typedef_count = typedefs.is_array() ? typedefs.size() : 0;
  auto schema = schemaFromRosapiTypedefs(typedefs);
  result.root_type = schema.root_type;
  if (!schema.error.empty() && schema.parameter_schema
                                    .value("properties", nlohmann::json::object())
                                    .empty()) {
    result.error = schema.error;
    return result;
  }

  // For ACTION Goal envelopes, prefer nested `goal` fields as the capability
  // parameters (matches how dispatchCapability sends action args).
  if ((kind == "ACTION" || kind == "action") &&
      schema.parameter_schema.contains("properties") &&
      schema.parameter_schema["properties"].contains("goal") &&
      schema.parameter_schema["properties"]["goal"].is_object()) {
    const auto& goal = schema.parameter_schema["properties"]["goal"];
    if (goal.contains("properties")) {
      result.parameter_schema = {
          {"type", "object"},
          {"properties", goal["properties"]},
      };
      result.request_defaults = goal.value("default", nlohmann::json::object());
      result.empty_request = result.parameter_schema["properties"].empty();
      return result;
    }
  }

  result.parameter_schema = std::move(schema.parameter_schema);
  result.request_defaults = std::move(schema.request_defaults);
  result.empty_request = result.parameter_schema
                             .value("properties", nlohmann::json::object())
                             .empty();
  return result;
}

}  // namespace dispatcher::ros
