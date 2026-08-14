#pragma once

#include "dispatcher/domain/connection_state.hpp"
#include "dispatcher/ros/rosbridge_protocol.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace dispatcher::ros {

class IRosbridgeTransport {
 public:
  virtual ~IRosbridgeTransport() = default;
  virtual bool sendText(std::string_view payload) = 0;
  virtual void close() = 0;
};

struct ReconnectPolicy {
  std::chrono::milliseconds initial_delay{500};
  std::chrono::milliseconds max_delay{30'000};
  std::uint32_t max_attempts{0};
};

struct ServiceResponse {
  std::string id;
  std::string service;
  bool success{false};
  Json values;
  std::string error;
};

struct ActionEvent {
  enum class Kind { Feedback, Result };

  std::string id;
  std::string action;
  Kind kind{Kind::Feedback};
  bool success{false};
  Json values;
  std::string error;
};

class RosbridgeSession {
 public:
  using Clock = std::chrono::steady_clock;
  using TopicHandler = std::function<void(std::string_view, const Json&)>;
  using ServiceHandler = std::function<void(const ServiceResponse&)>;
  using ActionHandler = std::function<void(const ActionEvent&)>;

  explicit RosbridgeSession(
      std::shared_ptr<IRosbridgeTransport> transport = nullptr,
      ReconnectPolicy reconnect_policy = {});

  void setTransport(std::shared_ptr<IRosbridgeTransport> transport);
  [[nodiscard]] domain::ConnectionState state() const noexcept;
  [[nodiscard]] std::uint32_t reconnectAttempt() const noexcept;
  [[nodiscard]] Clock::time_point nextReconnectAt() const noexcept;

  void onTransportConnected();
  void onTransportClosed(Clock::time_point now = Clock::now());
  bool tick(Clock::time_point now = Clock::now());
  void disable();
  void enable();

  [[nodiscard]] std::string subscribe(
      const std::string& topic,
      const std::string& type,
      const SubscribeOptions& options,
      TopicHandler handler);
  bool unsubscribe(const std::string& subscription_id);

  [[nodiscard]] std::string callService(
      const std::string& service,
      const Json& args,
      std::optional<double> timeout_seconds,
      ServiceHandler handler);
  // Removes a timed-out local correlation without sending another ROS call.
  bool forgetServiceCall(const std::string& correlation_id);
  [[nodiscard]] std::string sendActionGoal(
      const std::string& action,
      const std::string& action_type,
      const Json& args,
      ActionHandler handler,
      bool request_feedback = true);
  bool cancelActionGoal(const std::string& goal_id, const std::string& action);

  // ROS1 actionlib / plain topics: advertise once then publish.
  bool advertise(const std::string& topic, const std::string& type);
  bool publish(
      const std::string& topic,
      const Json& message,
      const std::optional<std::string>& type = std::nullopt);

  bool handleIncoming(std::string_view payload);

 private:
  struct Subscription {
    std::string topic;
    std::string type;
    SubscribeOptions options;
    TopicHandler handler;
  };
  struct PendingService {
    std::string service;
    ServiceHandler handler;
  };
  struct PendingAction {
    std::string action;
    ActionHandler handler;
  };

  bool send(const Json& message);
  std::string makeId(std::string_view prefix);
  void restoreSubscriptions();
  std::chrono::milliseconds delayForAttempt(std::uint32_t attempt) const;

  std::shared_ptr<IRosbridgeTransport> transport_;
  domain::ConnectionStateMachine state_machine_;
  ReconnectPolicy reconnect_policy_;
  std::uint32_t reconnect_attempt_{0};
  Clock::time_point next_reconnect_at_{Clock::now()};
  std::uint64_t id_counter_{0};
  std::unordered_map<std::string, Subscription> subscriptions_;
  std::unordered_map<std::string, PendingService> pending_services_;
  std::unordered_map<std::string, PendingAction> pending_actions_;
};

}  // namespace dispatcher::ros
