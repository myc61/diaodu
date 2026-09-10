#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace dispatcher::workflow {

inline std::string normalizedEdgeKind(const nlohmann::json& edge) {
  std::string kind = edge.value("edge_kind", edge.value("label", "success"));
  if (kind.rfind("event:", 0) == 0) {
    return "event";
  }
  if (kind == "SUCCESS" || kind == "success") {
    return "success";
  }
  if (kind == "EVENT" || kind == "event") {
    return "event";
  }
  if (kind == "FAILURE" || kind == "failure") {
    return "failure";
  }
  return kind;
}

inline std::string edgeEventName(const nlohmann::json& edge) {
  return edge.value(
      "event_name",
      edge.value("data", nlohmann::json::object()).value("event_name", ""));
}

struct JoinIncoming {
  std::string id;
  std::string source;
  std::string kind;
  std::string event_name;
};

inline std::vector<JoinIncoming> joinIncomingEdges(
    const nlohmann::json& edges, const std::string& target_key) {
  std::vector<JoinIncoming> incoming;
  if (!edges.is_array()) {
    return incoming;
  }
  for (const auto& edge : edges) {
    if (edge.value("target", "") != target_key) {
      continue;
    }
    const auto kind = normalizedEdgeKind(edge);
    if (kind != "success" && kind != "event") {
      continue;
    }
    incoming.push_back(JoinIncoming{
        .id = edge.value("id", ""),
        .source = edge.value("source", ""),
        .kind = kind,
        .event_name = edgeEventName(edge),
    });
  }
  return incoming;
}

inline bool traversedMatches(
    const nlohmann::json& payload,
    const JoinIncoming& incoming,
    const std::string& target_key,
    int cycle) {
  if (payload.value("target", "") != target_key) {
    return false;
  }
  if (payload.value("attempt", 1) != cycle) {
    return false;
  }
  if (!incoming.id.empty() && payload.value("edge_id", "") == incoming.id) {
    return true;
  }
  if (payload.value("source", "") != incoming.source) {
    return false;
  }
  if (payload.value("edge_kind", "") != incoming.kind) {
    return false;
  }
  if (incoming.kind == "event" &&
      payload.value("event_name", "") != incoming.event_name) {
    return false;
  }
  return true;
}

inline bool incomingEdgeSatisfied(
    const JoinIncoming& incoming,
    const std::string& target_key,
    int cycle,
    const std::unordered_map<std::string, std::string>& latest_states,
    const std::vector<nlohmann::json>& traversed_payloads) {
  if (incoming.kind == "success") {
    const auto it = latest_states.find(incoming.source);
    return it != latest_states.end() && it->second == "SUCCEEDED";
  }
  if (incoming.kind == "event") {
    for (const auto& payload : traversed_payloads) {
      if (traversedMatches(payload, incoming, target_key, cycle)) {
        return true;
      }
    }
    return false;
  }
  return true;
}

struct JoinCheck {
  bool ready{true};
  std::vector<std::string> waiting_on;
};

inline JoinCheck evaluateJoin(
    const nlohmann::json& edges,
    const std::string& target_key,
    int cycle,
    const std::unordered_map<std::string, std::string>& latest_states,
    const std::vector<nlohmann::json>& traversed_payloads) {
  JoinCheck result;
  for (const auto& incoming : joinIncomingEdges(edges, target_key)) {
    if (incomingEdgeSatisfied(
            incoming, target_key, cycle, latest_states, traversed_payloads)) {
      continue;
    }
    result.ready = false;
    if (incoming.kind == "event") {
      result.waiting_on.push_back(incoming.source + ":" + incoming.event_name);
    } else {
      result.waiting_on.push_back(incoming.source);
    }
  }
  return result;
}

}  // namespace dispatcher::workflow
