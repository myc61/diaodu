#include "dispatcher/ros/rosbridge_session.hpp"

#include <algorithm>
#include <limits>
#include <vector>

namespace dispatcher::ros {

RosbridgeSession::RosbridgeSession(
    std::shared_ptr<IRosbridgeTransport> transport,
    ReconnectPolicy reconnect_policy)
    : transport_(std::move(transport)), reconnect_policy_(reconnect_policy) {
  if (reconnect_policy_.initial_delay.count() < 0) {
    reconnect_policy_.initial_delay = std::chrono::milliseconds{0};
  }
  if (reconnect_policy_.max_delay < reconnect_policy_.initial_delay) {
    reconnect_policy_.max_delay = reconnect_policy_.initial_delay;
  }
}

void RosbridgeSession::setTransport(
    std::shared_ptr<IRosbridgeTransport> transport) {
  transport_ = std::move(transport);
}

domain::ConnectionState RosbridgeSession::state() const noexcept {
  return state_machine_.state();
}

std::uint32_t RosbridgeSession::reconnectAttempt() const noexcept {
  return reconnect_attempt_;
}

RosbridgeSession::Clock::time_point RosbridgeSession::nextReconnectAt()
    const noexcept {
  return next_reconnect_at_;
}

void RosbridgeSession::onTransportConnected() {
  if (state_machine_.state() == domain::ConnectionState::Disabled) {
    return;
  }
  if (state_machine_.state() == domain::ConnectionState::Disconnected) {
    state_machine_.transitionTo(domain::ConnectionState::Connecting);
  }
  state_machine_.transitionTo(domain::ConnectionState::Online);
  reconnect_attempt_ = 0;
  restoreSubscriptions();
}

void RosbridgeSession::onTransportClosed(Clock::time_point now) {
  if (state_machine_.state() == domain::ConnectionState::Disabled) {
    return;
  }
  if (state_machine_.state() == domain::ConnectionState::Disconnected) {
    state_machine_.transitionTo(domain::ConnectionState::Connecting);
  }
  state_machine_.transitionTo(domain::ConnectionState::Reconnecting);
  if (reconnect_policy_.max_attempts != 0 &&
      reconnect_attempt_ >= reconnect_policy_.max_attempts) {
    state_machine_.transitionTo(domain::ConnectionState::Disconnected);
    return;
  }
  next_reconnect_at_ = now + delayForAttempt(reconnect_attempt_);
  ++reconnect_attempt_;
}

bool RosbridgeSession::tick(Clock::time_point now) {
  if (state_machine_.state() != domain::ConnectionState::Reconnecting ||
      now < next_reconnect_at_) {
    return false;
  }
  return state_machine_.transitionTo(domain::ConnectionState::Connecting);
}

void RosbridgeSession::disable() {
  if (transport_ != nullptr) {
    transport_->close();
  }
  state_machine_.transitionTo(domain::ConnectionState::Disabled);
}

void RosbridgeSession::enable() {
  if (state_machine_.state() == domain::ConnectionState::Disabled) {
    state_machine_.transitionTo(domain::ConnectionState::Disconnected);
    reconnect_attempt_ = 0;
  }
}

std::string RosbridgeSession::subscribe(
    const std::string& topic,
    const std::string& type,
    const SubscribeOptions& options,
    TopicHandler handler) {
  const auto id = makeId("sub");
  subscriptions_.emplace(
      id, Subscription{topic, type, options, std::move(handler)});
  if (state() == domain::ConnectionState::Online) {
    send(RosbridgeMessageFactory::subscribe(id, topic, type, options));
  }
  return id;
}

bool RosbridgeSession::unsubscribe(const std::string& subscription_id) {
  const auto it = subscriptions_.find(subscription_id);
  if (it == subscriptions_.end()) {
    return false;
  }
  if (state() == domain::ConnectionState::Online) {
    send(RosbridgeMessageFactory::unsubscribe(subscription_id, it->second.topic));
  }
  subscriptions_.erase(it);
  return true;
}

std::string RosbridgeSession::callService(
    const std::string& service,
    const Json& args,
    std::optional<double> timeout_seconds,
    ServiceHandler handler) {
  const auto id = makeId("service");
  pending_services_.emplace(id, PendingService{service, std::move(handler)});
  if (state() == domain::ConnectionState::Online &&
      !send(RosbridgeMessageFactory::callService(
          id, service, args, timeout_seconds))) {
    pending_services_.erase(id);
    return {};
  }
  if (state() != domain::ConnectionState::Online) {
    pending_services_.erase(id);
    return {};
  }
  return id;
}

bool RosbridgeSession::forgetServiceCall(const std::string& correlation_id) {
  return pending_services_.erase(correlation_id) > 0;
}

std::string RosbridgeSession::sendActionGoal(
    const std::string& action,
    const std::string& action_type,
    const Json& args,
    ActionHandler handler,
    bool request_feedback) {
  const auto id = makeId("action");
  pending_actions_.emplace(id, PendingAction{action, std::move(handler)});
  if (state() == domain::ConnectionState::Online &&
      !send(RosbridgeMessageFactory::sendActionGoal(
          id, action, action_type, args, request_feedback))) {
    pending_actions_.erase(id);
    return {};
  }
  if (state() != domain::ConnectionState::Online) {
    pending_actions_.erase(id);
    return {};
  }
  return id;
}

bool RosbridgeSession::cancelActionGoal(
    const std::string& goal_id, const std::string& action) {
  if (pending_actions_.find(goal_id) == pending_actions_.end()) {
    return false;
  }
  return state() == domain::ConnectionState::Online &&
         send(RosbridgeMessageFactory::cancelActionGoal(goal_id, action));
}

bool RosbridgeSession::advertise(
    const std::string& topic, const std::string& type) {
  return state() == domain::ConnectionState::Online &&
         send(RosbridgeMessageFactory::advertise(topic, type));
}

bool RosbridgeSession::publish(
    const std::string& topic,
    const Json& message,
    const std::optional<std::string>& type) {
  return state() == domain::ConnectionState::Online &&
         send(RosbridgeMessageFactory::publish(topic, message, type));
}

bool RosbridgeSession::handleIncoming(std::string_view payload) {
  Json message;
  try {
    message = Json::parse(payload);
  } catch (...) {
    return false;
  }
  if (!message.is_object() || !message.contains("op") ||
      !message.at("op").is_string()) {
    return false;
  }

  const auto op = message.at("op").get<std::string>();
  if (op == "publish") {
    if (!message.contains("topic") || !message.contains("msg") ||
        !message.at("topic").is_string()) {
      return false;
    }
    const auto topic = message.at("topic").get<std::string>();
    std::vector<TopicHandler> handlers;
    for (const auto& [id, subscription] : subscriptions_) {
      (void)id;
      if (subscription.topic == topic && subscription.handler) {
        handlers.push_back(subscription.handler);
      }
    }
    for (const auto& handler : handlers) {
      handler(topic, message.at("msg"));
    }
    return true;
  }

  if (op == "service_response") {
    if (!message.contains("id") || !message.at("id").is_string()) {
      return false;
    }
    const auto id = message.at("id").get<std::string>();
    const auto it = pending_services_.find(id);
    if (it == pending_services_.end()) {
      return false;
    }
    const auto success = message.contains("result") &&
                         message.at("result").is_boolean() &&
                         message.at("result").get<bool>();
    const auto values = message.contains("values")
                            ? message.at("values")
                            : Json::object();
    const auto error = message.contains("error") &&
                               message.at("error").is_string()
                           ? message.at("error").get<std::string>()
                           : std::string{};
    auto handler = it->second.handler;
    ServiceResponse response{
        .id = id,
        .service = it->second.service,
        .success = success,
        .values = values,
        .error = error,
    };
    pending_services_.erase(it);
    if (handler) {
      handler(response);
    }
    return true;
  }

  const bool is_feedback = op == "action_feedback";
  const bool is_result = op == "action_result";
  if (is_feedback || is_result) {
    if (!message.contains("id") || !message.at("id").is_string()) {
      return false;
    }
    const auto id = message.at("id").get<std::string>();
    const auto it = pending_actions_.find(id);
    if (it == pending_actions_.end()) {
      return false;
    }
    const auto success = message.contains("result") &&
                         message.at("result").is_boolean()
                         ? message.at("result").get<bool>()
                         : is_result;
    const auto values = message.contains("values")
                            ? message.at("values")
                            : Json::object();
    const auto error = message.contains("error") &&
                               message.at("error").is_string()
                           ? message.at("error").get<std::string>()
                           : std::string{};
    auto handler = it->second.handler;
    ActionEvent event{
        .id = id,
        .action = it->second.action,
        .kind = is_feedback ? ActionEvent::Kind::Feedback
                            : ActionEvent::Kind::Result,
        .success = success,
        .values = values,
        .error = error,
    };
    if (is_result) {
      pending_actions_.erase(it);
    }
    if (handler) {
      handler(event);
    }
    return true;
  }

  return false;
}

bool RosbridgeSession::send(const Json& message) {
  return transport_ != nullptr && transport_->sendText(message.dump());
}

std::string RosbridgeSession::makeId(std::string_view prefix) {
  return std::string(prefix) + "-" + std::to_string(++id_counter_);
}

void RosbridgeSession::restoreSubscriptions() {
  for (const auto& [id, subscription] : subscriptions_) {
    send(RosbridgeMessageFactory::subscribe(
        id, subscription.topic, subscription.type, subscription.options));
  }
}

std::chrono::milliseconds RosbridgeSession::delayForAttempt(
    std::uint32_t attempt) const {
  const auto initial = reconnect_policy_.initial_delay.count();
  const auto max_delay = reconnect_policy_.max_delay.count();
  if (initial == 0 || max_delay == 0) {
    return std::chrono::milliseconds{0};
  }
  const auto exponent = std::min<std::uint32_t>(attempt, 30);
  const auto multiplier = static_cast<std::int64_t>(1ULL << exponent);
  if (initial > std::numeric_limits<std::int64_t>::max() / multiplier) {
    return reconnect_policy_.max_delay;
  }
  return std::chrono::milliseconds{std::min(max_delay, initial * multiplier)};
}

}  // namespace dispatcher::ros
