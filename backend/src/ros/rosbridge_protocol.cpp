#include "dispatcher/ros/rosbridge_protocol.hpp"

namespace dispatcher::ros {

Json RosbridgeMessageFactory::subscribe(
    const std::string& id,
    const std::string& topic,
    const std::string& type,
    const SubscribeOptions& options) {
  return Json{
      {"op", "subscribe"},
      {"id", id},
      {"topic", topic},
      {"type", type},
      {"throttle_rate", options.throttle_rate_ms},
      {"queue_length", options.queue_length},
      {"compression", options.compression},
  };
}

Json RosbridgeMessageFactory::unsubscribe(
    const std::string& id,
    const std::string& topic) {
  return Json{
      {"op", "unsubscribe"},
      {"id", id},
      {"topic", topic},
  };
}

Json RosbridgeMessageFactory::advertise(
    const std::string& topic,
    const std::string& type) {
  return Json{
      {"op", "advertise"},
      {"topic", topic},
      {"type", type},
  };
}

Json RosbridgeMessageFactory::publish(
    const std::string& topic,
    const Json& message,
    const std::optional<std::string>& type) {
  Json result{
      {"op", "publish"},
      {"topic", topic},
      {"msg", message},
  };
  if (type.has_value()) {
    result["type"] = *type;
  }
  return result;
}

Json RosbridgeMessageFactory::callService(
    const std::string& id,
    const std::string& service,
    const Json& args,
    std::optional<double> timeout_seconds) {
  Json result{
      {"op", "call_service"},
      {"id", id},
      {"service", service},
      {"args", args},
  };
  if (timeout_seconds.has_value()) {
    result["timeout"] = *timeout_seconds;
  }
  return result;
}

Json RosbridgeMessageFactory::sendActionGoal(
    const std::string& id,
    const std::string& action,
    const std::string& action_type,
    const Json& args,
    bool request_feedback) {
  return Json{
      {"op", "send_action_goal"},
      {"id", id},
      {"action", action},
      {"action_type", action_type},
      {"args", args},
      {"feedback", request_feedback},
  };
}

Json RosbridgeMessageFactory::cancelActionGoal(
    const std::string& id,
    const std::string& action) {
  return Json{
      {"op", "cancel_action_goal"},
      {"id", id},
      {"action", action},
  };
}

}  // namespace dispatcher::ros

