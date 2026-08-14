#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace dispatcher::interfaces {

// How the endpoint is invoked at runtime.
// Robots default to ROSBRIDGE (rosapi discovery).
// Devices may use HTTP / MQTT / WEBSOCKET later — reserved only.
enum class TransportKind {
  Rosbridge,
  Http,
  Mqtt,
  Websocket,
};

[[nodiscard]] inline const char* transportKindName(TransportKind kind) {
  switch (kind) {
    case TransportKind::Rosbridge:
      return "ROSBRIDGE";
    case TransportKind::Http:
      return "HTTP";
    case TransportKind::Mqtt:
      return "MQTT";
    case TransportKind::Websocket:
      return "WEBSOCKET";
  }
  return "UNKNOWN";
}

struct DiscoveredEndpoint {
  std::string name;
  std::string message_type;
  std::string operation_kind;  // TOPIC | SERVICE | ACTION
  std::string transport{transportKindName(TransportKind::Rosbridge)};
};

struct InterfaceCatalog {
  std::string entity_kind{"robot"};  // robot | device
  std::string entity_id;
  std::string transport{transportKindName(TransportKind::Rosbridge)};
  std::string scanned_at;
  bool live{false};
  std::vector<DiscoveredEndpoint> topics;
  std::vector<DiscoveredEndpoint> services;
  std::vector<DiscoveredEndpoint> actions;
  std::string topic_error;
  std::string service_error;
  std::string action_error;
  std::string error;

  // Keep the last known data for categories that failed in the current scan.
  // Successful categories always replace their cached counterparts.
  void preserveFailedCategoriesFrom(const InterfaceCatalog& previous) {
    if (!topic_error.empty()) {
      topics = previous.topics;
    }
    if (!service_error.empty()) {
      services = previous.services;
    }
    if (!action_error.empty()) {
      actions = previous.actions;
    }
  }

  [[nodiscard]] nlohmann::json toJson() const {
    auto endpointsToJson =
        [](const std::vector<DiscoveredEndpoint>& items) {
          nlohmann::json arr = nlohmann::json::array();
          for (const auto& item : items) {
            arr.push_back({
                {"name", item.name},
                {"message_type", item.message_type},
                {"operation_kind", item.operation_kind},
                {"transport", item.transport},
            });
          }
          return arr;
        };
    return {
        {"entity_kind", entity_kind},
        {"entity_id", entity_id},
        {"transport", transport},
        {"scanned_at", scanned_at},
        {"live", live},
        {"topics", endpointsToJson(topics)},
        {"services", endpointsToJson(services)},
        {"actions", endpointsToJson(actions)},
        {"topic_error", topic_error},
        {"service_error", service_error},
        {"action_error", action_error},
        {"error", error},
        {"counts",
         {
             {"topics", topics.size()},
             {"services", services.size()},
             {"actions", actions.size()},
         }},
    };
  }

  static InterfaceCatalog fromJson(const nlohmann::json& json) {
    InterfaceCatalog catalog;
    catalog.entity_kind = json.value("entity_kind", "robot");
    catalog.entity_id = json.value("entity_id", "");
    catalog.transport =
        json.value("transport", transportKindName(TransportKind::Rosbridge));
    catalog.scanned_at = json.value("scanned_at", "");
    catalog.live = json.value("live", false);
    catalog.topic_error = json.value("topic_error", "");
    catalog.service_error = json.value("service_error", "");
    catalog.action_error = json.value("action_error", "");
    catalog.error = json.value("error", "");
    auto parseList = [](const nlohmann::json& arr,
                        const std::string& default_kind) {
      std::vector<DiscoveredEndpoint> out;
      if (!arr.is_array()) {
        return out;
      }
      out.reserve(arr.size());
      for (const auto& item : arr) {
        if (!item.is_object()) {
          continue;
        }
        out.push_back(DiscoveredEndpoint{
            .name = item.value("name", ""),
            .message_type = item.value("message_type", ""),
            .operation_kind = item.value("operation_kind", default_kind),
            .transport = item.value(
                "transport",
                transportKindName(TransportKind::Rosbridge)),
        });
      }
      return out;
    };
    catalog.topics = parseList(json.value("topics", nlohmann::json::array()), "TOPIC");
    catalog.services =
        parseList(json.value("services", nlohmann::json::array()), "SERVICE");
    catalog.actions =
        parseList(json.value("actions", nlohmann::json::array()), "ACTION");
    return catalog;
  }
};

// Extension point for non-rosbridge discovery (device MQTT/HTTP/WS).
// Robot discovery is implemented via RobotRuntime + rosapi.
class IInterfaceDiscoverer {
 public:
  virtual ~IInterfaceDiscoverer() = default;
  [[nodiscard]] virtual std::string transport() const = 0;
  [[nodiscard]] virtual InterfaceCatalog discover(
      const std::string& entity_id) = 0;
};

}  // namespace dispatcher::interfaces
