#include "dispatcher/workflow/event_spec.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace dispatcher::workflow {
namespace {

std::string upper(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return value;
}

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool asBool(const nlohmann::json& value) {
  if (value.is_boolean()) {
    return value.get<bool>();
  }
  if (value.is_number_integer()) {
    return value.get<std::int64_t>() != 0;
  }
  if (value.is_number_float()) {
    return value.get<double>() != 0.0;
  }
  if (value.is_string()) {
    const auto text = lower(value.get<std::string>());
    return text == "true" || text == "1" || text == "yes" || text == "on";
  }
  return !value.is_null();
}

bool jsonEquals(const nlohmann::json& left, const nlohmann::json& right) {
  if (left == right) {
    return true;
  }
  if (left.is_string() && !right.is_string()) {
    return left.get<std::string>() == right.dump();
  }
  if (!left.is_string() && right.is_string()) {
    return left.dump() == right.get<std::string>();
  }
  if (left.is_number() && right.is_number()) {
    return std::abs(left.get<double>() - right.get<double>()) < 1e-9;
  }
  return false;
}

bool compareNumbers(
    const nlohmann::json& left,
    const nlohmann::json& right,
    const std::string& op) {
  if (!left.is_number() || !right.is_number()) {
    return false;
  }
  const double a = left.get<double>();
  const double b = right.get<double>();
  if (op == "gt") {
    return a > b;
  }
  if (op == "gte") {
    return a >= b;
  }
  if (op == "lt") {
    return a < b;
  }
  if (op == "lte") {
    return a <= b;
  }
  return false;
}

}  // namespace

EventSpec parseEventSpec(const nlohmann::json& json) {
  if (!json.is_object()) {
    throw std::runtime_error("event_spec must be an object");
  }
  EventSpec spec;
  spec.event_name = json.value("event_name", "");
  if (spec.event_name.empty()) {
    throw std::runtime_error("event_spec.event_name is required");
  }
  spec.source = upper(json.value("source", "RESULT"));
  if (spec.source != "RESULT" && spec.source != "FEEDBACK") {
    throw std::runtime_error("event_spec.source must be RESULT or FEEDBACK");
  }
  spec.enabled = json.value("enabled", true);
  const auto max_firings = json.value("max_firings", std::int64_t{1});
  if (max_firings < 0) {
    throw std::runtime_error("event_spec.max_firings must be >= 0");
  }
  spec.max_firings = static_cast<std::size_t>(max_firings);
  spec.cooldown_ms = json.value("cooldown_ms", 0);
  if (spec.cooldown_ms < 0) {
    throw std::runtime_error("event_spec.cooldown_ms must be >= 0");
  }
  spec.emit_on_node = json.value("emit_on_node", true);
  spec.start_workflows = json.value("start_workflows", true);

  const auto when = json.value("when", nlohmann::json::object());
  if (!when.is_object()) {
    throw std::runtime_error("event_spec.when must be an object");
  }
  spec.when.field = when.value("field", "");
  spec.when.op = lower(when.value("op", "ros_success"));
  if (when.contains("value")) {
    spec.when.value = when.at("value");
  }
  static const std::vector<std::string> ops{
      "eq",
      "neq",
      "gt",
      "gte",
      "lt",
      "lte",
      "contains",
      "exists",
      "truthy",
      "ros_success"};
  if (std::find(ops.begin(), ops.end(), spec.when.op) == ops.end()) {
    throw std::runtime_error("unsupported event_spec.when.op: " + spec.when.op);
  }
  return spec;
}

std::vector<EventSpec> parseEventSpecs(const nlohmann::json& json) {
  std::vector<EventSpec> specs;
  if (json.is_null()) {
    return specs;
  }
  if (!json.is_array()) {
    throw std::runtime_error("event_specs must be an array");
  }
  specs.reserve(json.size());
  for (const auto& item : json) {
    specs.push_back(parseEventSpec(item));
  }
  return specs;
}

void validateEventSpecsForOperation(
    const std::string& operation_kind,
    const nlohmann::json& json) {
  const auto kind = upper(operation_kind);
  const auto specs = parseEventSpecs(json);
  for (const auto& spec : specs) {
    if (spec.source == "FEEDBACK" && kind != "ACTION") {
      throw std::runtime_error(
          "FEEDBACK event source is only valid for ACTION capabilities");
    }
    if (spec.source != "RESULT" && spec.source != "FEEDBACK") {
      throw std::runtime_error("unsupported event source: " + spec.source);
    }
  }
}

std::vector<EventSpec> resolveEventSpecs(
    const nlohmann::json& capability_specs,
    const nlohmann::json& action_specs,
    const std::string& success_event_name) {
  nlohmann::json source = action_specs;
  if (!source.is_array() || source.empty()) {
    source = capability_specs.is_array() ? capability_specs
                                         : nlohmann::json::array();
  }
  auto specs = parseEventSpecs(source);
  if (!success_event_name.empty()) {
    const bool exists = std::any_of(
        specs.begin(),
        specs.end(),
        [&](const EventSpec& spec) {
          return spec.event_name == success_event_name &&
              spec.source == "RESULT";
        });
    if (!exists) {
      EventSpec legacy;
      legacy.event_name = success_event_name;
      legacy.source = "RESULT";
      legacy.enabled = true;
      legacy.when.op = "ros_success";
      legacy.max_firings = 1;
      specs.push_back(std::move(legacy));
    }
  }
  return specs;
}

bool hasEmittableNodeEvent(
    const std::vector<EventSpec>& specs,
    std::string_view event_name) {
  return !event_name.empty() && std::any_of(
      specs.begin(),
      specs.end(),
      [event_name](const EventSpec& spec) {
        return spec.enabled && spec.emit_on_node &&
            spec.event_name == event_name;
      });
}

nlohmann::json lookupField(
    const nlohmann::json& root, const std::string& dotted_path) {
  if (dotted_path.empty()) {
    return root;
  }
  const nlohmann::json* current = &root;
  std::size_t start = 0;
  while (start <= dotted_path.size()) {
    const auto end = dotted_path.find('.', start);
    const auto key = dotted_path.substr(
        start,
        end == std::string::npos ? std::string::npos : end - start);
    if (key.empty() || !current->is_object() || !current->contains(key)) {
      return nullptr;
    }
    current = &current->at(key);
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return *current;
}

bool evaluateWhen(
    const EventWhen& when,
    const nlohmann::json& payload,
    bool ros_success) {
  if (when.op == "ros_success") {
    return ros_success;
  }
  const auto field = lookupField(payload, when.field);
  if (when.op == "exists") {
    return !field.is_null();
  }
  if (when.op == "truthy") {
    return !field.is_null() && asBool(field);
  }
  if (field.is_null()) {
    return false;
  }
  if (when.op == "eq") {
    return jsonEquals(field, when.value);
  }
  if (when.op == "neq") {
    return !jsonEquals(field, when.value);
  }
  if (when.op == "contains") {
    if (field.is_string() && when.value.is_string()) {
      return field.get<std::string>().find(when.value.get<std::string>()) !=
          std::string::npos;
    }
    if (field.is_array()) {
      for (const auto& item : field) {
        if (jsonEquals(item, when.value)) {
          return true;
        }
      }
    }
    return false;
  }
  return compareNumbers(field, when.value, when.op);
}

nlohmann::json eventSpecToJson(const EventSpec& spec) {
  nlohmann::json when{
      {"field", spec.when.field},
      {"op", spec.when.op},
  };
  if (!spec.when.value.is_null()) {
    when["value"] = spec.when.value;
  }
  return {
      {"event_name", spec.event_name},
      {"source", spec.source},
      {"enabled", spec.enabled},
      {"when", when},
      {"max_firings", spec.max_firings},
      {"cooldown_ms", spec.cooldown_ms},
      {"emit_on_node", spec.emit_on_node},
      {"start_workflows", spec.start_workflows},
  };
}

}  // namespace dispatcher::workflow
