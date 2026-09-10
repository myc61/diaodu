#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace dispatcher::ros {

// actionlib GoalStatus: PENDING=0 ACTIVE=1 PREEMPTED=2 SUCCEEDED=3
// ABORTED=4 REJECTED=5 PREEMPTING=6 RECALLING=7 RECALLED=8 LOST=9
//
// navigation/NavigationState: Idle=0 Active=1 Running=2 Arrived=3
// Canceling=4 Cancelled=5 Succeeded=6 Failed=7 Error=8 Aborted=9

inline std::optional<int> jsonInt(const nlohmann::json& value) {
  if (value.is_number()) {
    return static_cast<int>(value.get<double>());
  }
  if (value.is_object()) {
    if (value.contains("value")) {
      return jsonInt(value["value"]);
    }
    if (value.contains("status")) {
      return jsonInt(value["status"]);
    }
  }
  return std::nullopt;
}

inline std::string jsonGoalId(const nlohmann::json& value) {
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_object()) {
    return value.value("id", "");
  }
  return {};
}

inline std::string actionMessageGoalId(const nlohmann::json& message) {
  if (message.contains("status") && message["status"].is_object()) {
    const auto& status = message["status"];
    if (status.contains("goal_id")) {
      if (const auto id = jsonGoalId(status["goal_id"]); !id.empty()) {
        return id;
      }
    }
  }
  if (message.contains("goal_id")) {
    if (const auto id = jsonGoalId(message["goal_id"]); !id.empty()) {
      return id;
    }
  }
  if (message.contains("result") && message["result"].is_object() &&
      message["result"].contains("goal_id")) {
    if (const auto id = jsonGoalId(message["result"]["goal_id"]); !id.empty()) {
      return id;
    }
  }
  return {};
}

inline std::optional<int> actionlibGoalStatus(const nlohmann::json& message) {
  if (message.contains("status") && message["status"].is_object()) {
    return jsonInt(message["status"]);
  }
  // Flattened ActionResult (status=3 and state=6 at the same level).
  if (message.contains("status") && message["status"].is_number() &&
      message.contains("state")) {
    return jsonInt(message["status"]);
  }
  return std::nullopt;
}

inline std::optional<int> navigationStateCode(const nlohmann::json& message) {
  if (message.contains("result") && message["result"].is_object() &&
      message["result"].contains("state")) {
    return jsonInt(message["result"]["state"]);
  }
  if (message.contains("feedback") && message["feedback"].is_object() &&
      message["feedback"].contains("state")) {
    return jsonInt(message["feedback"]["state"]);
  }
  if (message.contains("state")) {
    return jsonInt(message["state"]);
  }
  return std::nullopt;
}

inline bool actionlibStatusFailed(int code) {
  return code == 2 || code == 4 || code == 5 || code == 8 || code == 9;
}

inline bool navigationStateFailed(int code) {
  return code == 5 || code == 7 || code == 8 || code == 9;
}

inline bool navigationStateSucceeded(int code) {
  return code == 6;
}

// zj_humanoid /result: NavigationState.Succeeded (6) ends the node.
// Actionlib GoalStatus 3 is SUCCEEDED, not Arrived. Do not finish on
// feedback Arrived(3); wait for this ActionResult.
inline std::optional<bool> actionResultSucceeded(
    const nlohmann::json& message, bool zj_navigation_status) {
  if (zj_navigation_status) {
    const auto nav = navigationStateCode(message);
    const auto lib = actionlibGoalStatus(message);
    if ((nav.has_value() && navigationStateFailed(*nav)) ||
        (lib.has_value() && actionlibStatusFailed(*lib))) {
      return false;
    }
    if (nav.has_value() && navigationStateSucceeded(*nav)) {
      return true;
    }
    return std::nullopt;
  }

  if (const auto code = actionlibGoalStatus(message); code.has_value()) {
    return *code == 3;
  }
  if (message.contains("status") && message["status"].is_number()) {
    return jsonInt(message["status"]) == 3;
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
  return false;
}

inline nlohmann::json actionMessageStamps(const nlohmann::json& message) {
  nlohmann::json out = nlohmann::json::object();
  const auto takeStamp = [](const nlohmann::json& parent, const char* key) {
    if (parent.contains(key) && parent[key].is_object() &&
        parent[key].contains("stamp")) {
      return parent[key]["stamp"];
    }
    return nlohmann::json();
  };
  if (message.contains("header") && message["header"].is_object() &&
      message["header"].contains("stamp")) {
    out["result_header_stamp"] = message["header"]["stamp"];
  }
  if (message.contains("result") && message["result"].is_object()) {
    auto inner = takeStamp(message["result"], "header");
    if (!inner.is_null() && !inner.empty()) {
      out["result_inner_stamp"] = inner;
    }
  }
  if (message.contains("status") && message["status"].is_object() &&
      message["status"].contains("goal_id") &&
      message["status"]["goal_id"].is_object() &&
      message["status"]["goal_id"].contains("stamp")) {
    out["goal_accepted_stamp"] = message["status"]["goal_id"]["stamp"];
  }
  if (message.contains("feedback") && message["feedback"].is_object()) {
    auto inner = takeStamp(message["feedback"], "header");
    if (!inner.is_null() && !inner.empty()) {
      out["feedback_stamp"] = inner;
    }
  }
  return out;
}

inline std::string actionResultError(const nlohmann::json& message) {
  if (message.contains("status") && message["status"].is_object()) {
    return message["status"].value("text", "");
  }
  if (message.contains("text") && message["text"].is_string()) {
    return message["text"].get<std::string>();
  }
  return message.value("error", "");
}

}  // namespace dispatcher::ros
