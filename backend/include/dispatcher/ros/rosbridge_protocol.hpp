#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace dispatcher::ros {

using Json = nlohmann::json;

struct SubscribeOptions {
  std::int64_t throttle_rate_ms{0};
  std::int64_t queue_length{0};
  std::string compression{"none"};
};

class RosbridgeMessageFactory {
 public:
  [[nodiscard]] static Json subscribe(
      const std::string& id,
      const std::string& topic,
      const std::string& type,
      const SubscribeOptions& options = {});

  [[nodiscard]] static Json unsubscribe(
      const std::string& id,
      const std::string& topic);

  [[nodiscard]] static Json advertise(
      const std::string& topic,
      const std::string& type);

  [[nodiscard]] static Json publish(
      const std::string& topic,
      const Json& message,
      const std::optional<std::string>& type = std::nullopt);

  [[nodiscard]] static Json callService(
      const std::string& id,
      const std::string& service,
      const Json& args,
      std::optional<double> timeout_seconds = std::nullopt);

  [[nodiscard]] static Json sendActionGoal(
      const std::string& id,
      const std::string& action,
      const std::string& action_type,
      const Json& args,
      bool request_feedback = true);

  [[nodiscard]] static Json cancelActionGoal(
      const std::string& id,
      const std::string& action);
};

}  // namespace dispatcher::ros

