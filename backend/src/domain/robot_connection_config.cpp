#include "dispatcher/domain/robot_connection_config.hpp"

#include <algorithm>
#include <utility>

namespace dispatcher::domain {
namespace {

std::string normalizedPath(const std::string& path) {
  if (path.empty()) {
    return "/";
  }
  return path.front() == '/' ? path : "/" + path;
}

std::string urlHost(const std::string& host) {
  if (host.find(':') != std::string::npos &&
      !(host.starts_with('[') && host.ends_with(']'))) {
    return "[" + host + "]";
  }
  return host;
}

bool endsWith(const std::string& value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

std::string RosbridgeEndpointConfig::websocketUrl() const {
  return std::string(secure ? "wss://" : "ws://") + urlHost(host) + ":" +
         std::to_string(port) + normalizedPath(path);
}

std::vector<std::string> RobotConnectionConfig::validationErrors() const {
  std::vector<std::string> errors;
  if (name.empty()) {
    errors.emplace_back("robot name is required");
  }
  if (ros_version != RosVersion::Ros1 || ros_distribution != "noetic") {
    errors.emplace_back("the first robot contract requires ROS 1 Noetic");
  }
  if (rosbridge.host.empty()) {
    errors.emplace_back("robot IP or host is required");
  }
  if (rosbridge.port == 0) {
    errors.emplace_back("rosbridge port must be greater than zero");
  }
  if (remote_access.protocol != RemoteAccessProtocol::None) {
    if (remote_access.port == 0) {
      errors.emplace_back("remote access port must be greater than zero");
    }
    if (remote_access.username.empty()) {
      errors.emplace_back("remote access username is required");
    }
    if (remote_access.credential_reference.empty()) {
      errors.emplace_back("remote access credential reference is required");
    }
    if (remote_access.known_hosts_reference.empty()) {
      errors.emplace_back("known_hosts reference is required");
    }
  }
  if (pose.has_value() &&
      (pose->topic.empty() || pose->message_type.empty() ||
       pose->mapping_key.empty())) {
    errors.emplace_back("configured pose topic requires type and mapping key");
  }
  return errors;
}

Ros1ActionlibEndpointConfig Ros1ActionlibEndpointConfig::fromGoalTopic(
    std::string goal_topic, std::string action_type) {
  Ros1ActionlibEndpointConfig config;
  config.goal_topic = std::move(goal_topic);
  config.action_type = std::move(action_type);
  config.server_name = endsWith(config.goal_topic, "/goal")
                           ? config.goal_topic.substr(
                                 0, config.goal_topic.size() - 5)
                           : config.goal_topic;
  config.cancel_topic = config.server_name + "/cancel";
  config.status_topic = config.server_name + "/status";
  config.feedback_topic = config.server_name + "/feedback";
  config.result_topic = config.server_name + "/result";
  return config;
}

bool Ros1ActionlibEndpointConfig::ready() const noexcept {
  return !server_name.empty() && !action_type.empty() && !goal_topic.empty() &&
         !cancel_topic.empty() && !status_topic.empty() &&
         !feedback_topic.empty() && !result_topic.empty();
}

std::string_view toString(RemoteAccessProtocol protocol) noexcept {
  switch (protocol) {
    case RemoteAccessProtocol::None:
      return "NONE";
    case RemoteAccessProtocol::Sftp:
      return "SFTP";
    case RemoteAccessProtocol::Ssh:
      return "SSH";
  }
  return "NONE";
}

}  // namespace dispatcher::domain
