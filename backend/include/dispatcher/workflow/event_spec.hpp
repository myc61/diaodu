#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace dispatcher::workflow {

struct EventWhen {
  std::string field;
  std::string op{"ros_success"};  // eq|neq|gt|gte|lt|lte|contains|exists|truthy|ros_success
  nlohmann::json value = nullptr;
};

struct EventSpec {
  std::string event_name;
  std::string source{"RESULT"};  // RESULT | FEEDBACK (ACTION only)
  bool enabled{true};
  EventWhen when{};
  std::size_t max_firings{1};
  int cooldown_ms{0};
  bool emit_on_node{true};
  bool start_workflows{true};
};

[[nodiscard]] EventSpec parseEventSpec(const nlohmann::json& json);
[[nodiscard]] std::vector<EventSpec> parseEventSpecs(const nlohmann::json& json);

// Enforce the response channels actually provided by each command adapter.
// SERVICE has one response, ACTION has feedback/result, and TOPIC currently
// represents outbound publish completion rather than an inbound subscription.
void validateEventSpecsForOperation(
    const std::string& operation_kind,
    const nlohmann::json& json);

// Resolve effective specs: action override if non-empty, else capability;
// always append legacy success_event_name as RESULT/ros_success when missing.
[[nodiscard]] std::vector<EventSpec> resolveEventSpecs(
    const nlohmann::json& capability_specs,
    const nlohmann::json& action_specs,
    const std::string& success_event_name);

[[nodiscard]] bool hasEmittableNodeEvent(
    const std::vector<EventSpec>& specs,
    std::string_view event_name);

[[nodiscard]] nlohmann::json lookupField(
    const nlohmann::json& root, const std::string& dotted_path);

[[nodiscard]] bool evaluateWhen(
    const EventWhen& when,
    const nlohmann::json& payload,
    bool ros_success);

[[nodiscard]] nlohmann::json eventSpecToJson(const EventSpec& spec);

}  // namespace dispatcher::workflow
