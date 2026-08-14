#pragma once

#include "dispatcher/domain/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dispatcher::domain {

enum class RemoteAccessProtocol { None, Sftp, Ssh };

struct RosbridgeEndpointConfig {
  std::string host;
  std::uint16_t port{9090};
  std::string path{"/"};
  bool secure{false};

  [[nodiscard]] std::string websocketUrl() const;
};

struct RemoteAccessConfig {
  RemoteAccessProtocol protocol{RemoteAccessProtocol::None};
  std::uint16_t port{22};
  std::string username;
  std::string credential_reference;
  std::string known_hosts_reference;
};

struct PoseTopicConfig {
  std::string topic;
  std::string message_type;
  std::string mapping_key;
};

struct RobotConnectionConfig {
  std::string name;
  RosVersion ros_version{RosVersion::Ros1};
  std::string ros_distribution{"noetic"};
  std::string ros_namespace;
  RosbridgeEndpointConfig rosbridge;
  RemoteAccessConfig remote_access;
  std::optional<PoseTopicConfig> pose;

  [[nodiscard]] std::vector<std::string> validationErrors() const;
};

struct Ros1ActionlibEndpointConfig {
  std::string server_name;
  std::string action_type;
  std::string goal_topic;
  std::string cancel_topic;
  std::string status_topic;
  std::string feedback_topic;
  std::string result_topic;

  [[nodiscard]] static Ros1ActionlibEndpointConfig fromGoalTopic(
      std::string goal_topic,
      std::string action_type = {});
  [[nodiscard]] bool ready() const noexcept;
};

[[nodiscard]] std::string_view toString(RemoteAccessProtocol protocol) noexcept;

}  // namespace dispatcher::domain
