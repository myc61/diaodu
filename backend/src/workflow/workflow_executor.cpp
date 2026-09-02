#include "dispatcher/workflow/workflow_executor.hpp"
#include "dispatcher/ops/ops_log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string_view>

namespace dispatcher::workflow {
namespace {

std::string optionalString(const nlohmann::json& json, const char* key) {
  if (!json.contains(key) || json[key].is_null()) {
    return "";
  }
  if (json[key].is_string()) {
    return json[key].get<std::string>();
  }
  return json[key].dump();
}

const nlohmann::json* valueAtPath(
    const nlohmann::json& values, std::string_view path) {
  const nlohmann::json* current = &values;
  std::size_t offset = 0;
  while (offset <= path.size()) {
    const auto separator = path.find('.', offset);
    const auto part = path.substr(
        offset,
        separator == std::string_view::npos ? path.size() - offset
                                            : separator - offset);
    if (part.empty() || !current->is_object() ||
        !current->contains(std::string(part))) {
      return nullptr;
    }
    current = &current->at(std::string(part));
    if (separator == std::string_view::npos) {
      return current;
    }
    offset = separator + 1;
  }
  return nullptr;
}

nlohmann::json renderRequestTemplate(
    const nlohmann::json& request_template,
    const nlohmann::json& parameters) {
  if (request_template.is_null() ||
      (request_template.is_object() && request_template.empty())) {
    return parameters;
  }
  if (request_template.is_array()) {
    auto rendered = nlohmann::json::array();
    for (const auto& item : request_template) {
      rendered.push_back(renderRequestTemplate(item, parameters));
    }
    return rendered;
  }
  if (request_template.is_object()) {
    auto rendered = nlohmann::json::object();
    for (const auto& [key, value] : request_template.items()) {
      rendered[key] = renderRequestTemplate(value, parameters);
    }
    return rendered;
  }
  if (request_template.is_string()) {
    const auto text = request_template.get<std::string>();
    if (text.size() >= 4 && text.starts_with("${") && text.ends_with('}')) {
      const auto* value = valueAtPath(
          parameters, std::string_view(text).substr(2, text.size() - 3));
      if (value == nullptr) {
        throw std::runtime_error("request template parameter missing: " + text);
      }
      return *value;
    }
  }
  return request_template;
}

}  // namespace

WorkflowExecutor::WorkflowExecutor(
    db::WorkspaceRepository& repository,
    ros::RobotRuntime& robots,
    remote::ControlledSshExecutor& ssh_executor)
    : repository_(repository),
      robots_(robots),
      ssh_executor_(ssh_executor),
      callback_gate_(std::make_shared<CallbackGate>()) {
  callback_gate_->owner = this;
  timer_thread_ = std::thread([this] { timerLoop(); });
}

WorkflowExecutor::~WorkflowExecutor() {
  {
    std::lock_guard lock(callback_gate_->mutex);
    callback_gate_->owner = nullptr;
  }
  {
    std::lock_guard lock(timer_mutex_);
    timer_stopping_ = true;
    while (!scheduled_tasks_.empty()) {
      scheduled_tasks_.pop();
    }
  }
  timer_cv_.notify_all();
  if (timer_thread_.joinable()) {
    timer_thread_.join();
  }
}

void WorkflowExecutor::scheduleAfter(
    std::chrono::milliseconds delay, std::function<void()> task) {
  if (!task) {
    return;
  }
  {
    std::lock_guard lock(timer_mutex_);
    if (timer_stopping_) {
      return;
    }
    scheduled_tasks_.push(ScheduledTask{
        .due = std::chrono::steady_clock::now() + std::max(delay, std::chrono::milliseconds(0)),
        .sequence = ++timer_sequence_,
        .task = std::move(task),
    });
  }
  timer_cv_.notify_all();
}

void WorkflowExecutor::timerLoop() {
  std::unique_lock lock(timer_mutex_);
  while (!timer_stopping_) {
    if (scheduled_tasks_.empty()) {
      timer_cv_.wait(lock, [this] {
        return timer_stopping_ || !scheduled_tasks_.empty();
      });
      continue;
    }
    const auto due = scheduled_tasks_.top().due;
    if (timer_cv_.wait_until(lock, due) != std::cv_status::timeout) {
      continue;
    }
    if (scheduled_tasks_.empty() || scheduled_tasks_.top().due >
            std::chrono::steady_clock::now()) {
      continue;
    }
    auto task = std::move(scheduled_tasks_.top().task);
    scheduled_tasks_.pop();
    lock.unlock();
    try {
      task();
    } catch (const std::exception& ex) {
      ops::OpsLog::instance().error(
          "workflow", "scheduled workflow task failed", {{"error", ex.what()}});
    } catch (...) {
      ops::OpsLog::instance().error(
          "workflow", "scheduled workflow task failed", {{"error", "unknown"}});
    }
    lock.lock();
  }
}

std::string WorkflowExecutor::nodeType(const nlohmann::json& node) {
  return node.value("type", "");
}

nlohmann::json WorkflowExecutor::nodeData(const nlohmann::json& node) {
  return node.value("data", nlohmann::json::object());
}

db::NodeRunRecord* WorkflowExecutor::findNode(
    db::WorkflowRunDetail& detail, const std::string& node_key) {
  for (auto it = detail.nodes.rbegin(); it != detail.nodes.rend(); ++it) {
    if (it->node_key == node_key) {
      return &*it;
    }
  }
  return nullptr;
}

nlohmann::json WorkflowExecutor::findGraphNode(
    const db::WorkflowRunDetail& detail, const std::string& node_key) const {
  for (const auto& node : detail.graph.value("nodes", nlohmann::json::array())) {
    if (node.value("id", "") == node_key) {
      return node;
    }
  }
  return nlohmann::json::object();
}

db::WorkflowRunDetail WorkflowExecutor::startManual(
    const std::string& workflow_definition_id, const nlohmann::json& input) {
  db::WorkflowRunLinkage linkage{
      .business_key = input.value("business_key", ""),
  };
  auto detail = repository_.createWorkflowRun(
      workflow_definition_id,
      "MANUAL",
      nlohmann::json{{"business_key", linkage.business_key}},
      input,
      linkage);
  bootstrapFromStart(detail);
  auto refreshed = repository_.getWorkflowRun(detail.run.id);
  if (!refreshed.has_value()) {
    throw std::runtime_error("failed to reload workflow run");
  }
  return *refreshed;
}

void WorkflowExecutor::bootstrapFromStart(db::WorkflowRunDetail& detail) {
  std::string start_key;
  for (const auto& node : detail.graph.value("nodes", nlohmann::json::array())) {
    if (nodeType(node) == "START") {
      start_key = node.value("id", "");
      break;
    }
  }
  if (start_key.empty()) {
    repository_.updateWorkflowRunState(
        detail.run.id,
        "FAILED",
        nlohmann::json{{"error", "graph missing START"}});
    return;
  }
  completeNode(detail, start_key, nlohmann::json{{"bootstrapped", true}});
}

void WorkflowExecutor::activateNode(
    db::WorkflowRunDetail& detail, const std::string& node_key) {
  auto* node = findNode(detail, node_key);
  if (node == nullptr) {
    return;
  }
  if (node->state == "SUCCEEDED" || node->state == "FAILED" ||
      node->state == "CANCELLED") {
    return;
  }
  if (node->state == "WAITING_EVENT" || node->state == "PAUSED" ||
      node->state == "RUNNING") {
    return;
  }
  repository_.updateNodeRun(
      node->id,
      "RUNNING",
      node->assigned_robot_id,
      node->output_data,
      nlohmann::json());
  node->state = "RUNNING";
  executeNode(detail, *node);
}

void WorkflowExecutor::completeNode(
    db::WorkflowRunDetail& detail,
    const std::string& node_key,
    const nlohmann::json& output,
    bool follow_success_edges) {
  auto* node = findNode(detail, node_key);
  if (node == nullptr) {
    return;
  }
  repository_.updateNodeRun(
      node->id, "SUCCEEDED", node->assigned_robot_id, output, nlohmann::json());
  node->state = "SUCCEEDED";
  node->output_data = output;

  const auto graph_node = findGraphNode(detail, node_key);
  if (nodeType(graph_node) == "END") {
    finishOrRepeatWorkflow(detail);
    return;
  }
  if (follow_success_edges) {
    followEdges(detail, node_key, "success");
  }
  maybeFinishRun(detail);
}

void WorkflowExecutor::finishOrRepeatWorkflow(db::WorkflowRunDetail& detail) {
  const auto policy = detail.graph.value("run_policy", nlohmann::json::object());
  const int loop_count = std::clamp(policy.value("loop_count", 1), 1, 1000);
  const int loop_delay_ms = std::clamp(
      policy.value("loop_delay_ms", 0), 0, 24 * 60 * 60 * 1000);
  const int current_cycle = detail.run.context_data.value("current_cycle", 1);
  if (current_cycle >= loop_count) {
    detail.run.context_data["completed_cycles"] = current_cycle;
    repository_.updateWorkflowRunState(
        detail.run.id, "SUCCEEDED", detail.run.context_data);
    detail.run.state = "SUCCEEDED";
    notifyParentRun(detail, true);
    return;
  }
  if (detail.run.context_data.value("next_cycle_scheduled", false)) {
    return;
  }
  detail.run.context_data["current_cycle"] = current_cycle;
  detail.run.context_data["completed_cycles"] = current_cycle;
  detail.run.context_data["next_cycle_scheduled"] = true;
  detail.run.context_data["loop_count"] = loop_count;
  detail.run.context_data["loop_delay_ms"] = loop_delay_ms;
  repository_.updateWorkflowRunState(
      detail.run.id, "RUNNING", detail.run.context_data);
  const auto run_id = detail.run.id;
  scheduleAfter(
      std::chrono::milliseconds(loop_delay_ms),
      [this, run_id, next_cycle = current_cycle + 1] {
        auto detail = repository_.getWorkflowRun(run_id);
        if (!detail.has_value() || detail->run.state != "RUNNING") {
          return;
        }
        auto next = repository_.createNextWorkflowCycle(run_id, next_cycle);
        bootstrapFromStart(next);
      });
}

void WorkflowExecutor::failNode(
    db::WorkflowRunDetail& detail,
    const std::string& node_key,
    const std::string& message) {
  auto* node = findNode(detail, node_key);
  if (node == nullptr) {
    return;
  }
  const nlohmann::json error{{"error", message}};
  repository_.updateNodeRun(
      node->id, "FAILED", node->assigned_robot_id, node->output_data, error);
  node->state = "FAILED";
  if (followEdges(detail, node_key, "failure") > 0) {
    maybeFinishRun(detail);
    return;
  }
  repository_.updateWorkflowRunState(detail.run.id, "FAILED", detail.run.context_data);
  detail.run.state = "FAILED";
  notifyParentRun(detail, false, message);
}

void WorkflowExecutor::notifyParentRun(
    const db::WorkflowRunDetail& child,
    bool succeeded,
    const std::string& error) {
  if (!child.run.parent_run_id.has_value() ||
      !child.run.parent_node_run_id.has_value()) {
    return;
  }
  auto parent = repository_.getWorkflowRun(*child.run.parent_run_id);
  if (!parent.has_value() || parent->run.state != "RUNNING") {
    return;
  }
  auto node_it = std::find_if(
      parent->nodes.begin(),
      parent->nodes.end(),
      [&child](const db::NodeRunRecord& node) {
        return node.id == *child.run.parent_node_run_id;
      });
  if (node_it == parent->nodes.end() || node_it->state != "WAITING_EVENT") {
    return;
  }
  nlohmann::json output = node_it->output_data;
  output.update({
      {"waiting_subflow", false},
      {"child_run_id", child.run.id},
      {"child_state", succeeded ? "SUCCEEDED" : child.run.state},
  });
  if (succeeded) {
    if (!repository_.transitionNodeRun(
            node_it->id,
            "WAITING_EVENT",
            "SUCCEEDED",
            node_it->assigned_robot_id,
            output,
            nlohmann::json())) {
      return;
    }
    node_it->state = "SUCCEEDED";
    node_it->output_data = output;
    (void)repository_.insertWorkflowEvent(
        parent->run.id,
        node_it->id,
        "subflow.completed",
        {{"child_run_id", child.run.id}, {"child_state", "SUCCEEDED"}},
        "subflow-completed:" + child.run.id);
    followEdges(*parent, node_it->node_key, "success");
    maybeFinishRun(*parent);
    return;
  }
  const auto message =
      error.empty() ? "child workflow did not succeed" : error;
  if (!repository_.transitionNodeRun(
          node_it->id,
          "WAITING_EVENT",
          "FAILED",
          node_it->assigned_robot_id,
          output,
          nlohmann::json{
              {"error", message},
              {"child_run_id", child.run.id},
              {"child_state", child.run.state}})) {
    return;
  }
  node_it->state = "FAILED";
  (void)repository_.insertWorkflowEvent(
      parent->run.id,
      node_it->id,
      "subflow.failed",
      {{"child_run_id", child.run.id},
       {"child_state", child.run.state},
       {"error", message}},
      "subflow-failed:" + child.run.id);
  if (followEdges(*parent, node_it->node_key, "failure") > 0) {
    maybeFinishRun(*parent);
    return;
  }
  repository_.updateWorkflowRunState(
      parent->run.id, "FAILED", parent->run.context_data);
}

std::size_t WorkflowExecutor::followEdges(
    db::WorkflowRunDetail& detail,
    const std::string& source_key,
    const std::string& edge_kind,
    const std::string& event_name) {
  std::size_t followed = 0;
  for (const auto& edge :
       detail.graph.value("edges", nlohmann::json::array())) {
    if (edge.value("source", "") != source_key) {
      continue;
    }
    const auto kind = edge.value("edge_kind", edge.value("label", "success"));
    std::string normalized = kind;
    if (normalized.rfind("event:", 0) == 0) {
      normalized = "event";
    }
    if (edge_kind == "success") {
      if (normalized != "success" && normalized != "SUCCESS") {
        continue;
      }
    } else if (edge_kind == "event") {
      if (normalized != "event" && normalized != "EVENT") {
        continue;
      }
      const auto edge_event = edge.value(
          "event_name",
          edge.value("data", nlohmann::json::object()).value("event_name", ""));
      if (!event_name.empty() && edge_event != event_name) {
        continue;
      }
    } else if (edge_kind == "failure") {
      if (normalized != "failure" && normalized != "FAILURE") {
        continue;
      }
    } else {
      continue;
    }
    const auto edge_id = edge.value("id", "");
    const auto target_key = edge.value("target", "");
    const int cycle = detail.run.context_data.value("current_cycle", 1);
    (void)repository_.insertWorkflowEvent(
        detail.run.id,
        std::nullopt,
        "workflow.edge.traversed",
        {{"edge_id", edge_id},
         {"source", source_key},
         {"target", target_key},
         {"edge_kind", edge_kind},
         {"event_name", event_name},
         {"attempt", cycle}},
        std::nullopt);
    activateNode(detail, target_key);
    ++followed;
  }
  return followed;
}

void WorkflowExecutor::maybeFinishRun(db::WorkflowRunDetail& detail) {
  if (detail.run.state != "RUNNING") {
    return;
  }
  if (detail.run.context_data.value("next_cycle_scheduled", false)) {
    return;
  }
  bool any_open = false;
  bool any_end_done = false;
  for (const auto& node : detail.nodes) {
    const auto graph_node = findGraphNode(detail, node.node_key);
    if (nodeType(graph_node) == "END" && node.state == "SUCCEEDED") {
      any_end_done = true;
    }
    if (node.state == "PENDING" || node.state == "RUNNING" ||
        node.state == "WAITING_EVENT" || node.state == "WAITING_RESOURCE" ||
        node.state == "PAUSED") {
      any_open = true;
    }
  }
  if (any_end_done && !any_open) {
    repository_.updateWorkflowRunState(
        detail.run.id, "SUCCEEDED", detail.run.context_data);
    detail.run.state = "SUCCEEDED";
    notifyParentRun(detail, true);
  }
}

void WorkflowExecutor::emitAndPropagateEvent(
    db::WorkflowRunDetail& detail,
    const std::optional<std::string>& node_run_id,
    const std::string& event_name,
    const nlohmann::json& payload) {
  if (event_name.empty()) {
    return;
  }
  (void)repository_.insertWorkflowEvent(
      detail.run.id, node_run_id, event_name, payload, std::nullopt);
  if (node_run_id.has_value()) {
    for (const auto& node : detail.nodes) {
      if (node.id == *node_run_id) {
        followEdges(detail, node.node_key, "event", event_name);
        break;
      }
    }
  }
}

void WorkflowExecutor::clearEventGates(const std::string& command_run_id) {
  const auto prefix = command_run_id + "\x1f";
  for (auto it = event_gates_.begin(); it != event_gates_.end();) {
    if (it->first.rfind(prefix, 0) == 0) {
      it = event_gates_.erase(it);
    } else {
      ++it;
    }
  }
}

void WorkflowExecutor::evaluateAndEmitEventSpecs(
    db::WorkflowRunDetail& detail,
    const std::string& node_run_id,
    const std::string& command_run_id,
    const std::vector<EventSpec>& event_specs,
    const std::string& source,
    const nlohmann::json& payload,
    bool ros_success) {
  const auto now = FeedbackTriggerGate::Clock::now();
  for (const auto& spec : event_specs) {
    if (!spec.enabled || spec.source != source || spec.event_name.empty()) {
      continue;
    }
    if (!evaluateWhen(spec.when, payload, ros_success)) {
      // Keep rising-edge state in sync when condition is false.
      const auto key =
          command_run_id + "\x1f" + spec.source + "\x1f" + spec.event_name;
      auto& gate = event_gates_.try_emplace(
                        key,
                        FeedbackTriggerPolicy{
                            .max_firings = spec.max_firings,
                            .cooldown =
                                std::chrono::milliseconds(spec.cooldown_ms),
                        })
                       .first->second;
      (void)gate.evaluate(false, now);
      continue;
    }
    const auto key =
        command_run_id + "\x1f" + spec.source + "\x1f" + spec.event_name;
    auto& gate = event_gates_.try_emplace(
                      key,
                      FeedbackTriggerPolicy{
                          .max_firings = spec.max_firings,
                          .cooldown =
                              std::chrono::milliseconds(spec.cooldown_ms),
                      })
                     .first->second;
    if (!gate.evaluate(true, now)) {
      continue;
    }
    nlohmann::json event_payload{
        {"command_run_id", command_run_id},
        {"source", source},
        {"event_name", spec.event_name},
        {"values", payload},
    };
    if (spec.emit_on_node) {
      emitAndPropagateEvent(
          detail, node_run_id, spec.event_name, event_payload);
    }
    if (spec.start_workflows) {
      event_payload["source_run_id"] = detail.run.id;
      event_payload["source_node_run_id"] = node_run_id;
      (void)injectEvent(spec.event_name, event_payload);
    }
  }
}

ros::RosCommandHandler WorkflowExecutor::makeRosCommandHandler(
    const std::string& workflow_run_id,
    const std::string& node_run_id,
    const std::string& command_run_id,
    const std::vector<EventSpec>& event_specs) {
  const auto gate = callback_gate_;
  return [gate,
          workflow_run_id,
          node_run_id,
          command_run_id,
          event_specs](const ros::RosCommandEvent& event) {
    std::lock_guard lock(gate->mutex);
    if (gate->owner == nullptr) {
      return;
    }
    gate->owner->handleRosCommandEvent(
        workflow_run_id,
        node_run_id,
        command_run_id,
        event_specs,
        event);
  };
}

void WorkflowExecutor::handleRosCommandEvent(
    const std::string& workflow_run_id,
    const std::string& node_run_id,
    const std::string& command_run_id,
    const std::vector<EventSpec>& event_specs,
    const ros::RosCommandEvent& event) {
  if (event.kind == ros::RosCommandEvent::Kind::Feedback) {
    repository_.updateCommandRunFeedback(command_run_id, event.values);
    (void)repository_.insertWorkflowEvent(
        workflow_run_id,
        node_run_id,
        "node.feedback",
        nlohmann::json{
            {"command_run_id", command_run_id},
            {"correlation_id", event.correlation_id},
            {"values", event.values}},
        std::nullopt);
    auto detail = repository_.getWorkflowRun(workflow_run_id);
    if (detail.has_value() && detail->run.state == "RUNNING") {
      evaluateAndEmitEventSpecs(
          *detail,
          node_run_id,
          command_run_id,
          event_specs,
          "FEEDBACK",
          event.values,
          true);
    }
    return;
  }

  const auto command_state = event.success ? "SUCCEEDED" : "FAILED";
  const auto error = event.success
      ? nlohmann::json()
      : nlohmann::json{{"message", event.error.empty()
                                      ? "robot command failed"
                                      : event.error}};
  repository_.completeCommandRun(
      command_run_id, command_state, event.values, error);

  auto detail = repository_.getWorkflowRun(workflow_run_id);
  if (!detail.has_value() || detail->run.state != "RUNNING") {
    clearEventGates(command_run_id);
    return;
  }
  db::NodeRunRecord* node = nullptr;
  for (auto& candidate : detail->nodes) {
    if (candidate.id == node_run_id) {
      node = &candidate;
      break;
    }
  }
  if (node == nullptr || node->state != "RUNNING") {
    clearEventGates(command_run_id);
    return;
  }

  nlohmann::json output = node->output_data;
  output["correlation_id"] = event.correlation_id;
  output["result"] = event.values;
  output["result_success"] = event.success;
  if (!event.error.empty()) {
    output["result_error"] = event.error;
  }
  if (output.contains("outbox_id") && output["outbox_id"].is_string()) {
    std::optional<nlohmann::json> outbox_error;
    if (!event.success) {
      outbox_error = nlohmann::json{
          {"message", event.error.empty()
                          ? "navigation result failed"
                          : event.error}};
    }
    repository_.updateOutboxState(
        output["outbox_id"].get<std::string>(),
        event.success ? "ACKNOWLEDGED" : "FAILED",
        outbox_error);
  }

  if (!event.success) {
    // Failure-side RESULT specs (field checks / neq) still evaluate.
    evaluateAndEmitEventSpecs(
        *detail,
        node_run_id,
        command_run_id,
        event_specs,
        "RESULT",
        event.values,
        false);
    node->output_data = output;
    const auto graph_node = findGraphNode(*detail, node->node_key);
    const auto data = nodeData(graph_node);
    int retry_count = data.value("retry_count", 0);
    int retry_delay_ms = data.value("retry_delay_ms", 1000);
    if (nodeType(graph_node) == "STATION_ACTION") {
      const auto action = repository_.getStationAction(
          optionalString(data, "station_action_id"));
      if (action.has_value()) {
        retry_count = action->retry_count;
      }
    }
    clearEventGates(command_run_id);
    scheduleNodeRetry(
        *detail,
        *node,
        retry_count,
        retry_delay_ms,
        event.error.empty() ? "robot command failed" : event.error);
    return;
  }

  completeNode(*detail, node->node_key, output, true);
  evaluateAndEmitEventSpecs(
      *detail,
      node_run_id,
      command_run_id,
      event_specs,
      "RESULT",
      event.values,
      true);
  clearEventGates(command_run_id);
}

void WorkflowExecutor::scheduleNodeRetry(
    db::WorkflowRunDetail& detail,
    db::NodeRunRecord& node,
    int retry_count,
    int retry_delay_ms,
    const std::string& message) {
  const int command_attempt = node.output_data.value("command_attempt", 1);
  if (retry_count <= 0 || command_attempt > retry_count) {
    failNode(detail, node.node_key, message);
    return;
  }
  retry_delay_ms = std::clamp(retry_delay_ms, 0, 24 * 60 * 60 * 1000);
  node.output_data["command_attempt"] = command_attempt;
  node.output_data["retry_count"] = retry_count;
  node.output_data["retry_delay_ms"] = retry_delay_ms;
  node.output_data["retry_waiting"] = true;
  node.output_data["last_error"] = message;
  repository_.updateNodeRun(
      node.id,
      "RUNNING",
      node.assigned_robot_id,
      node.output_data,
      nlohmann::json());
  const auto run_id = detail.run.id;
  const auto node_run_id = node.id;
  scheduleAfter(
      std::chrono::milliseconds(retry_delay_ms),
      [this, run_id, node_run_id] {
        auto detail = repository_.getWorkflowRun(run_id);
        if (!detail.has_value() || detail->run.state != "RUNNING") {
          return;
        }
        for (auto& candidate : detail->nodes) {
          if (candidate.id != node_run_id || candidate.state != "RUNNING") {
            continue;
          }
          candidate.output_data["retry_waiting"] = false;
          candidate.output_data["command_attempt"] =
              candidate.output_data.value("command_attempt", 1) + 1;
          repository_.updateNodeRun(
              candidate.id,
              "RUNNING",
              candidate.assigned_robot_id,
              candidate.output_data,
              nlohmann::json());
          executeNode(*detail, candidate);
          return;
        }
      });
}

void WorkflowExecutor::dispatchCapabilityNode(
    db::WorkflowRunDetail& detail,
    db::NodeRunRecord& node,
    const db::RobotRecord& robot,
    const db::CapabilityTemplateRecord& capability,
    const nlohmann::json& parameters,
    int timeout_ms,
    int retry_count,
    int retry_delay_ms,
    const std::vector<EventSpec>& event_specs,
    nlohmann::json output_context) {
  try {
    if (capability.operation_kind == "SSH") {
      dispatchSshCapabilityNode(
          detail,
          node,
          robot,
          capability,
          parameters,
          timeout_ms,
          retry_count,
          retry_delay_ms,
          event_specs,
          std::move(output_context));
      return;
    }
    const auto request_payload = renderRequestTemplate(
        capability.request_template, parameters);
    const auto command = repository_.createCommandRun(
        detail.run.id,
        node.id,
        robot.id,
        capability.id,
        capability.operation_kind,
        capability.endpoint_name,
        request_payload);
    auto protocol_config = capability.protocol_config;
    protocol_config["goal_id"] = command.command_id;

    node.assigned_robot_id = robot.id;
    const int command_attempt = node.output_data.value("command_attempt", 1);
    output_context.update({
        {"capability_key", capability.capability_key},
        {"capability_definition_id", capability.id},
        {"command_id", command.command_id},
        {"command_run_id", command.id},
        {"robot_id", robot.id},
        {"parameters", parameters},
        {"request_payload", request_payload},
        {"motion_ownership", capability.motion_ownership},
        {"dispatch_state", "DISPATCHING"},
        {"command_attempt", command_attempt},
        {"retry_count", retry_count},
        {"retry_delay_ms", retry_delay_ms},
    });
    node.output_data = std::move(output_context);
    repository_.updateNodeRun(
        node.id,
        "RUNNING",
        node.assigned_robot_id,
        node.output_data,
        nlohmann::json());

    const auto dispatched = robots_.dispatchCapability(
        robot,
        capability.operation_kind,
        capability.endpoint_name,
        capability.ros_message_type,
        request_payload,
        timeout_ms > 0 ? timeout_ms : capability.timeout_ms,
        protocol_config,
        makeRosCommandHandler(
            detail.run.id,
            node.id,
            command.id,
            event_specs));
    if (!dispatched.accepted) {
      repository_.completeCommandRun(
          command.id,
          "FAILED",
          nlohmann::json::object(),
          nlohmann::json{{"message", dispatched.error}});
      scheduleNodeRetry(
          detail, node, retry_count, retry_delay_ms, dispatched.error);
      return;
    }
    repository_.markCommandRunDispatched(
        command.id, dispatched.correlation_id);

    if (capability.operation_kind == "TOPIC") {
      const nlohmann::json result{
          {"published", true},
          {"endpoint_name", capability.endpoint_name},
      };
      repository_.completeCommandRun(
          command.id, "SUCCEEDED", result, nlohmann::json());
      node.output_data["correlation_id"] = dispatched.correlation_id;
      node.output_data["result"] = result;
      node.output_data["result_success"] = true;
      evaluateAndEmitEventSpecs(
          detail,
          node.id,
          command.id,
          event_specs,
          "RESULT",
          result,
          true);
      clearEventGates(command.id);
      completeNode(detail, node.node_key, node.output_data, true);
    }
  } catch (const std::exception& ex) {
    scheduleNodeRetry(detail, node, retry_count, retry_delay_ms, ex.what());
  }
}

void WorkflowExecutor::dispatchSshCapabilityNode(
    db::WorkflowRunDetail& detail,
    db::NodeRunRecord& node,
    const db::RobotRecord& robot,
    const db::CapabilityTemplateRecord& capability,
    const nlohmann::json& parameters,
    int timeout_ms,
    int retry_count,
    int retry_delay_ms,
    const std::vector<EventSpec>& event_specs,
    nlohmann::json output_context) {
  const auto configured_robot_id =
      capability.protocol_config.value("ssh_robot_id", "");
  const auto profile_id =
      capability.protocol_config.value("ssh_profile_id", "");
  const auto profile = profile_id.empty()
      ? std::nullopt
      : repository_.getRobotStartupProfile(profile_id);
  if (configured_robot_id != robot.id || !profile.has_value() ||
      profile->robot_id != robot.id || !profile->enabled) {
    failNode(detail, node.node_key, "SSH capability profile does not match robot");
    return;
  }
  if (!robot.host.has_value() || robot.host->empty()) {
    failNode(detail, node.node_key, "SSH capability robot has no host");
    return;
  }
  const nlohmann::json request_payload{
      {"ssh_profile_id", profile->id},
      {"ssh_profile_name", profile->name},
      {"ssh_profile_version", profile->version},
      {"parameters", parameters},
  };
  const auto command = repository_.createCommandRun(
      detail.run.id,
      node.id,
      robot.id,
      capability.id,
      "SSH",
      profile->name,
      request_payload);
  const int command_attempt = node.output_data.value("command_attempt", 1);
  node.assigned_robot_id = robot.id;
  output_context.update({
      {"capability_key", capability.capability_key},
      {"capability_definition_id", capability.id},
      {"command_id", command.command_id},
      {"command_run_id", command.id},
      {"robot_id", robot.id},
      {"parameters", parameters},
      {"request_payload", request_payload},
      {"dispatch_state", "DISPATCHING"},
      {"command_attempt", command_attempt},
      {"retry_count", retry_count},
      {"retry_delay_ms", retry_delay_ms},
  });
  node.output_data = std::move(output_context);
  repository_.updateNodeRun(
      node.id, "RUNNING", node.assigned_robot_id, node.output_data,
      nlohmann::json());

  remote::ControlledSshRequest request{
      .job_id = command.command_id,
      .host = *robot.host,
      .port = profile->ssh_port,
      .username = profile->ssh_username,
      .credential_reference = profile->credential_reference,
      .known_hosts_reference = profile->known_hosts_reference,
      .steps = profile->steps,
      .readiness_checks = profile->readiness_checks,
      .timeout_ms = timeout_ms > 0 ? timeout_ms : profile->timeout_ms,
  };
  if (const auto error = remote::ControlledSshExecutor::validateRequest(request);
      error.has_value()) {
    repository_.completeCommandRun(
        command.id, "FAILED", nlohmann::json::object(),
        nlohmann::json{{"message", *error}});
    scheduleNodeRetry(detail, node, retry_count, retry_delay_ms, *error);
    return;
  }
  repository_.markCommandRunDispatched(command.id, command.command_id);
  node.output_data["dispatch_state"] = "ACTIVE";
  repository_.updateNodeRun(
      node.id, "RUNNING", node.assigned_robot_id, node.output_data,
      nlohmann::json());
  const auto gate = callback_gate_;
  const bool accepted = ssh_executor_.execute(
      std::move(request),
      [gate,
       workflow_run_id = detail.run.id,
       node_run_id = node.id,
       command_run_id = command.id,
       event_specs,
       retry_count,
       retry_delay_ms](const remote::ControlledSshResult& result) {
        std::lock_guard lock(gate->mutex);
        if (gate->owner == nullptr) {
          return;
        }
        gate->owner->handleStartupResult(
            workflow_run_id,
            node_run_id,
            command_run_id,
            result,
            event_specs,
            retry_count,
            retry_delay_ms);
      });
  if (!accepted) {
    repository_.completeCommandRun(
        command.id, "FAILED", nlohmann::json::object(),
        nlohmann::json{{"message", "SSH worker unavailable"}});
    scheduleNodeRetry(
        detail, node, retry_count, retry_delay_ms, "SSH worker unavailable");
  }
}

void WorkflowExecutor::executeNode(
    db::WorkflowRunDetail& detail, db::NodeRunRecord& node) {
  const auto graph_node = findGraphNode(detail, node.node_key);
  const auto type = nodeType(graph_node);
  const auto data = nodeData(graph_node);

  if (type == "START") {
    completeNode(detail, node.node_key, nlohmann::json{{"ok", true}});
    return;
  }
  if (type == "END") {
    completeNode(detail, node.node_key, nlohmann::json{{"ok", true}});
    return;
  }
  if (type == "DELAY") {
    const int delay_ms = data.value("delay_ms", 1000);
    if (delay_ms < 0 || delay_ms > 24 * 60 * 60 * 1000) {
      failNode(detail, node.node_key, "DELAY delay_ms must be between 0 and 86400000");
      return;
    }
    node.output_data = {
        {"delay_ms", delay_ms},
        {"waiting", true},
    };
    repository_.updateNodeRun(
        node.id, "RUNNING", node.assigned_robot_id, node.output_data,
        nlohmann::json());
    const auto run_id = detail.run.id;
    const auto node_run_id = node.id;
    scheduleAfter(std::chrono::milliseconds(delay_ms), [this, run_id, node_run_id] {
      auto detail = repository_.getWorkflowRun(run_id);
      if (!detail.has_value() || detail->run.state != "RUNNING") {
        return;
      }
      for (const auto& candidate : detail->nodes) {
        if (candidate.id == node_run_id && candidate.state == "RUNNING") {
          completeNode(
              *detail,
              candidate.node_key,
              nlohmann::json{{"delay_ms", candidate.output_data.value("delay_ms", 0)},
                             {"waiting", false}},
              true);
          return;
        }
      }
    });
    return;
  }
  if (type == "MANUAL_CONFIRM") {
    const auto prompt = data.value("prompt", "请确认是否继续执行");
    node.output_data = {
        {"waiting_manual_confirmation", true},
        {"prompt", prompt},
    };
    repository_.updateNodeRun(
        node.id,
        "PAUSED",
        node.assigned_robot_id,
        node.output_data,
        nlohmann::json());
    node.state = "PAUSED";
    return;
  }
  if (type == "ROBOT") {
    const auto robot_id = optionalString(data, "robot_id");
    detail.run.context_data["current_robot_id"] = robot_id;
    repository_.updateWorkflowRunState(
        detail.run.id, detail.run.state, detail.run.context_data);
    if (!robot_id.empty()) {
      node.assigned_robot_id = robot_id;
    }
    completeNode(
        detail,
        node.node_key,
        nlohmann::json{{"robot_id", robot_id}});
    return;
  }
  if (type == "STATION") {
    const auto station_id = optionalString(data, "station_id");
    detail.run.context_data["current_station_id"] = station_id;
    repository_.updateWorkflowRunState(
        detail.run.id, detail.run.state, detail.run.context_data);
    completeNode(
        detail,
        node.node_key,
        nlohmann::json{{"station_id", station_id}});
    return;
  }
  if (type == "EVENT_WAIT") {
    const auto event_name = optionalString(data, "event_name");
    if (event_name.empty()) {
      failNode(detail, node.node_key, "EVENT_WAIT missing event_name");
      return;
    }
    repository_.updateNodeRun(
        node.id,
        "WAITING_EVENT",
        node.assigned_robot_id,
        nlohmann::json{{"waiting_event", event_name}},
        nlohmann::json());
    node.state = "WAITING_EVENT";
    node.output_data = {{"waiting_event", event_name}};
    return;
  }
  if (type == "SUBFLOW") {
    const auto workflow_id = optionalString(data, "workflow_definition_id");
    if (workflow_id.empty()) {
      failNode(detail, node.node_key, "SUBFLOW missing workflow_definition_id");
      return;
    }
    if (workflow_id == detail.run.workflow_definition_id) {
      failNode(detail, node.node_key, "SUBFLOW cannot call its own workflow");
      return;
    }
    const int depth = detail.run.trigger_metadata.value("subflow_depth", 0);
    if (depth >= 32) {
      failNode(detail, node.node_key, "SUBFLOW nesting depth exceeds 32");
      return;
    }
    node.output_data = {
        {"waiting_subflow", true},
        {"workflow_definition_id", workflow_id},
    };
    repository_.updateNodeRun(
        node.id,
        "WAITING_EVENT",
        node.assigned_robot_id,
        node.output_data,
        nlohmann::json());
    node.state = "WAITING_EVENT";
    try {
      db::WorkflowRunLinkage linkage{
          .parent_run_id = detail.run.id,
          .root_run_id = detail.run.root_run_id.value_or(detail.run.id),
          .source_run_id = detail.run.id,
          .parent_node_run_id = node.id,
          .business_key = detail.run.business_key,
      };
      auto child = repository_.createWorkflowRun(
          workflow_id,
          "SUBFLOW",
          nlohmann::json{
              {"parent_run_id", detail.run.id},
              {"parent_node_run_id", node.id},
              {"subflow_depth", depth + 1}},
          data.value("input_data", nlohmann::json::object()),
          linkage);
      node.output_data["child_run_id"] = child.run.id;
      repository_.updateNodeRun(
          node.id,
          "WAITING_EVENT",
          node.assigned_robot_id,
          node.output_data,
          nlohmann::json());
      (void)repository_.insertWorkflowEvent(
          detail.run.id,
          node.id,
          "subflow.started",
          {{"child_run_id", child.run.id},
           {"workflow_definition_id", workflow_id},
           {"root_run_id", linkage.root_run_id.value_or("")},
           {"business_key", linkage.business_key}},
          "subflow-started:" + child.run.id);
      bootstrapFromStart(child);
    } catch (const std::exception& ex) {
      node.state = "WAITING_EVENT";
      failNode(detail, node.node_key, ex.what());
    }
    return;
  }
  if (type == "ROBOT_STARTUP" || type == "ROBOT_SSH") {
    const auto robot_id = optionalString(data, "robot_id");
    const auto profile_id = optionalString(data, "startup_profile_id");
    const auto robot = robot_id.empty()
        ? std::nullopt
        : repository_.getRobot(robot_id);
    const auto profile = profile_id.empty()
        ? std::nullopt
        : repository_.getRobotStartupProfile(profile_id);
    if (!robot.has_value() || !profile.has_value() ||
        profile->robot_id != robot_id) {
      failNode(detail, node.node_key, "ROBOT_STARTUP robot/profile not found");
      return;
    }
    if (!profile->enabled) {
      failNode(detail, node.node_key, "ROBOT_STARTUP profile is disabled");
      return;
    }
    if (!robot->host.has_value() || robot->host->empty()) {
      failNode(detail, node.node_key, "ROBOT_STARTUP robot has no host");
      return;
    }
    const nlohmann::json request_payload{
        {"startup_profile_id", profile->id},
        {"startup_profile_name", profile->name},
        {"startup_profile_version", profile->version},
        {"robot_id", robot_id},
        {"step_count", profile->steps.size()},
        {"readiness_check_count", profile->readiness_checks.size()},
    };
    const auto command = repository_.createCommandRun(
        detail.run.id,
        node.id,
        robot_id,
        std::nullopt,
        type,
        profile->name,
        request_payload);
    node.assigned_robot_id = robot_id;
    node.output_data = {
        {"command_id", command.command_id},
        {"command_run_id", command.id},
        {"robot_id", robot_id},
        {"startup_profile_id", profile->id},
        {"startup_profile_name", profile->name},
        {"startup_profile_version", profile->version},
        {"dispatch_state", "DISPATCHING"},
    };
    repository_.updateNodeRun(
        node.id,
        "RUNNING",
        node.assigned_robot_id,
        node.output_data,
        nlohmann::json());
    remote::ControlledSshRequest request{
        .job_id = command.command_id,
        .host = *robot->host,
        .port = profile->ssh_port,
        .username = profile->ssh_username,
        .credential_reference = profile->credential_reference,
        .known_hosts_reference = profile->known_hosts_reference,
        .steps = profile->steps,
        .readiness_checks = profile->readiness_checks,
        .timeout_ms = data.value("timeout_ms", profile->timeout_ms),
    };
    if (const auto error =
            remote::ControlledSshExecutor::validateRequest(request);
        error.has_value()) {
      repository_.completeCommandRun(
          command.id,
          "FAILED",
          nlohmann::json::object(),
          nlohmann::json{{"message", *error}});
      failNode(detail, node.node_key, *error);
      return;
    }
    repository_.markCommandRunDispatched(command.id, command.command_id);
    node.output_data["dispatch_state"] = "ACTIVE";
    repository_.updateNodeRun(
        node.id,
        "RUNNING",
        node.assigned_robot_id,
        node.output_data,
        nlohmann::json());
    const auto gate = callback_gate_;
    const bool accepted = ssh_executor_.execute(
        std::move(request),
        [gate,
         workflow_run_id = detail.run.id,
         node_run_id = node.id,
         command_run_id = command.id](
            const remote::ControlledSshResult& result) {
          std::lock_guard lock(gate->mutex);
          if (gate->owner != nullptr) {
            gate->owner->handleStartupResult(
                workflow_run_id, node_run_id, command_run_id, result);
          }
        });
    if (!accepted) {
      repository_.completeCommandRun(
          command.id,
          "FAILED",
          nlohmann::json::object(),
          nlohmann::json{{"message", "startup worker unavailable"}});
      failNode(detail, node.node_key, "startup worker unavailable");
      return;
    }
    return;
  }
  if (type == "NAVIGATION") {
    const auto to_station_id = optionalString(data, "to_station_id");
    const double distance_tolerance =
        data.value("distance_tolerance", 0.04);
    const double heading_tolerance =
        data.value("heading_tolerance", 0.04);
    auto robot_id = optionalString(data, "robot_id");
    if (robot_id.empty()) {
      robot_id = detail.run.context_data.value("current_robot_id", "");
    }
    if (to_station_id.empty() || robot_id.empty()) {
      failNode(
          detail,
          node.node_key,
          "NAVIGATION requires robot_id and to_station_id");
      return;
    }
    if (!std::isfinite(distance_tolerance) ||
        distance_tolerance <= 0.0 ||
        !std::isfinite(heading_tolerance) || heading_tolerance <= 0.0) {
      failNode(
          detail,
          node.node_key,
          "NAVIGATION tolerances must be positive");
      return;
    }
    const auto station = repository_.getStation(to_station_id);
    const auto robot = repository_.getRobot(robot_id);
    if (!station.has_value() || !robot.has_value()) {
      failNode(detail, node.node_key, "navigation robot/station not found");
      return;
    }
    if (!station->scene_id.empty()) {
      try {
        const auto goal = repository_.enqueueNavigationGoal(
            robot_id,
            station->scene_id,
            station->map_version_id,
            station->x,
            station->y,
            station->yaw,
            distance_tolerance,
            heading_tolerance);
        const nlohmann::json request_payload{
            {"to_station_id", to_station_id},
            {"scene_id", station->scene_id},
            {"map_version_id", station->map_version_id},
            {"x", station->x},
            {"y", station->y},
            {"yaw", station->yaw},
            {"distance_tolerance", distance_tolerance},
            {"heading_tolerance", heading_tolerance},
        };
        const auto command = repository_.createCommandRun(
            detail.run.id,
            node.id,
            robot_id,
            std::nullopt,
            "NAVIGATION",
            robot->nav_action.value_or(""),
            request_payload,
            goal.command_id);
        node.assigned_robot_id = robot_id;
        node.output_data = {
            {"command_id", command.command_id},
            {"command_run_id", command.id},
            {"outbox_id", goal.outbox_id},
            {"to_station_id", to_station_id},
            {"distance_tolerance", distance_tolerance},
            {"heading_tolerance", heading_tolerance},
            {"dispatch_state", "DISPATCHING"},
        };
        repository_.updateNodeRun(
            node.id,
            "RUNNING",
            node.assigned_robot_id,
            node.output_data,
            nlohmann::json());
        const auto dispatched = robots_.sendNavigationGoalTracked(
            *robot,
            goal,
            station->x,
            station->y,
            station->yaw,
            ros::NavigationGoalOptions{
                .distance_tolerance = distance_tolerance,
                .heading_tolerance = heading_tolerance,
            },
            makeRosCommandHandler(
                detail.run.id, node.id, command.id, {}));
        if (!dispatched.accepted) {
          repository_.completeCommandRun(
              command.id,
              "FAILED",
              nlohmann::json::object(),
              nlohmann::json{{"message", dispatched.error}});
          repository_.updateOutboxState(
              goal.outbox_id,
              "FAILED",
              nlohmann::json{{"message", dispatched.error}});
          failNode(detail, node.node_key, dispatched.error);
          return;
        }
        repository_.markCommandRunDispatched(
            command.id, dispatched.correlation_id);
      } catch (const std::exception& ex) {
        failNode(detail, node.node_key, ex.what());
      }
      return;
    }
    failNode(detail, node.node_key, "station missing scene_id");
    return;
  }
  if (type == "ROBOT_CAPABILITY") {
    auto robot_id = optionalString(data, "robot_id");
    if (robot_id.empty()) {
      robot_id = detail.run.context_data.value("current_robot_id", "");
    }
    const auto capability_id = optionalString(
        data, "capability_definition_id");
    if (robot_id.empty() || capability_id.empty()) {
      failNode(
          detail,
          node.node_key,
          "ROBOT_CAPABILITY requires robot_id and capability_definition_id");
      return;
    }
    const auto robot = repository_.getRobot(robot_id);
    if (!robot.has_value()) {
      failNode(detail, node.node_key, "capability robot not found");
      return;
    }
    const auto capability = repository_.getCapabilityTemplate(capability_id);
    if (!capability.has_value()) {
      failNode(detail, node.node_key, "capability definition not found");
      return;
    }
    const auto parameters = data.value(
        "parameters", nlohmann::json::object());
    if (!parameters.is_object()) {
      failNode(
          detail, node.node_key, "ROBOT_CAPABILITY parameters must be object");
      return;
    }
    const auto event_specs = resolveEventSpecs(
        capability->event_specs,
        nlohmann::json::array(),
        optionalString(data, "success_event_name"));
    dispatchCapabilityNode(
        detail,
        node,
        *robot,
        *capability,
        parameters,
        data.value("timeout_ms", capability->timeout_ms),
        data.value("retry_count", 0),
        data.value("retry_delay_ms", 1000),
        event_specs,
        nlohmann::json{
            {"node_kind", "ROBOT_CAPABILITY"},
            {"action_name",
             data.value("action_name", capability->capability_key)},
        });
    return;
  }
  if (type == "STATION_ACTION") {
    const auto action_id = optionalString(data, "station_action_id");
    auto action = action_id.empty()
                      ? std::nullopt
                      : repository_.getStationAction(action_id);
    if (!action.has_value()) {
      failNode(detail, node.node_key, "station_action not found");
      return;
    }
    std::string robot_id = action->robot_id.value_or("");
    if (robot_id.empty()) {
      robot_id = optionalString(data, "robot_id");
    }
    if (robot_id.empty()) {
      robot_id = detail.run.context_data.value("current_robot_id", "");
    }
    if (robot_id.empty()) {
      failNode(detail, node.node_key, "station action has no robot_id");
      return;
    }
    const auto robot = repository_.getRobot(robot_id);
    if (!robot.has_value()) {
      failNode(detail, node.node_key, "action robot not found");
      return;
    }
    if (!action->capability_definition_id.has_value()) {
      failNode(
          detail,
          node.node_key,
          "station action is not bound to a capability definition");
      return;
    }
    const auto capability = repository_.getCapabilityTemplate(
        *action->capability_definition_id);
    if (!capability.has_value()) {
      failNode(detail, node.node_key, "capability definition not found");
      return;
    }

    const auto event_specs = resolveEventSpecs(
        capability->event_specs,
        action->event_specs,
        action->success_event_name);
    dispatchCapabilityNode(
        detail,
        node,
        *robot,
        *capability,
        action->parameters,
        action->timeout_ms > 0 ? action->timeout_ms : capability->timeout_ms,
        action->retry_count,
        data.value("retry_delay_ms", 1000),
        event_specs,
        nlohmann::json{
            {"station_action_id", action->id},
            {"action_name", action->action_name},
        });
    return;
  }

  // Unknown node types pass through for forward compatibility.
  completeNode(
      detail,
      node.node_key,
      nlohmann::json{{"skipped", true}, {"type", type}});
}

nlohmann::json WorkflowExecutor::injectEvent(
    const std::string& event_name, const nlohmann::json& payload) {
  db::WorkflowSignalEnvelope envelope{
      .event_name = event_name,
      .business_key = payload.value("business_key", ""),
      .payload = payload,
  };
  if (payload.contains("source_run_id") && payload["source_run_id"].is_string()) {
    envelope.source_run_id = payload["source_run_id"].get<std::string>();
  }
  if (payload.contains("source_node_run_id") &&
      payload["source_node_run_id"].is_string()) {
    envelope.source_node_run_id =
        payload["source_node_run_id"].get<std::string>();
  }
  if (payload.contains("target_run_id") && payload["target_run_id"].is_string()) {
    envelope.target_run_id = payload["target_run_id"].get<std::string>();
  }
  if (payload.contains("target_workflow_definition_id") &&
      payload["target_workflow_definition_id"].is_string()) {
    envelope.target_workflow_definition_id =
        payload["target_workflow_definition_id"].get<std::string>();
  }
  if (payload.contains("deduplication_key") &&
      payload["deduplication_key"].is_string()) {
    envelope.deduplication_key =
        payload["deduplication_key"].get<std::string>();
  }
  return injectEvent(envelope);
}

nlohmann::json WorkflowExecutor::injectEvent(
    const db::WorkflowSignalEnvelope& envelope) {
  auto routed_envelope = envelope;
  std::optional<db::WorkflowRunDetail> source;
  if (routed_envelope.source_run_id.has_value()) {
    source = repository_.getWorkflowRun(*routed_envelope.source_run_id);
    if (source.has_value() && routed_envelope.business_key.empty()) {
      routed_envelope.business_key = source->run.business_key;
    }
  }
  nlohmann::json result{
      {"event_name", routed_envelope.event_name},
      {"woken_runs", nlohmann::json::array()},
      {"started_runs", nlohmann::json::array()},
  };
  if (routed_envelope.event_name.empty()) {
    return result;
  }
  const auto signal_id = repository_.insertWorkflowSignal(routed_envelope);
  result["signal_id"] = signal_id;

  for (const auto& waiting : repository_.listWaitingEventNodeRuns(
           routed_envelope.event_name,
           routed_envelope.target_run_id,
           routed_envelope.business_key,
           routed_envelope.target_workflow_definition_id)) {
    auto detail = repository_.getWorkflowRun(waiting.workflow_run_id);
    if (!detail.has_value() || detail->run.state != "RUNNING") {
      continue;
    }
    completeNode(
        *detail,
        waiting.node_key,
        nlohmann::json{
            {"waiting_event", routed_envelope.event_name},
            {"signal_id", signal_id},
            {"source_run_id", routed_envelope.source_run_id.value_or("")},
            {"business_key", routed_envelope.business_key},
            {"received_payload", routed_envelope.payload}},
        true);
    // Also follow event edges from the waiter predecessors is handled by complete.
    result["woken_runs"].push_back(waiting.workflow_run_id);
  }

  if (routed_envelope.target_run_id.has_value()) {
    return result;
  }
  for (const auto& published : repository_.listPublishedWorkflowsByEvent(
           routed_envelope.event_name,
           routed_envelope.target_workflow_definition_id)) {
    db::WorkflowRunLinkage linkage{
        .root_run_id = source.has_value()
            ? source->run.root_run_id.value_or(source->run.id)
            : std::optional<std::string>{},
        .source_run_id = routed_envelope.source_run_id,
        .causation_event_id = signal_id,
        .business_key = routed_envelope.business_key,
    };
    auto started = repository_.createWorkflowRun(
        published.definition_id,
        "EVENT",
        nlohmann::json{
            {"event_name", routed_envelope.event_name},
            {"signal_id", signal_id},
            {"source_run_id", routed_envelope.source_run_id.value_or("")},
            {"business_key", linkage.business_key},
            {"payload", routed_envelope.payload}},
        nlohmann::json{
            {"event_name", routed_envelope.event_name},
            {"signal_id", signal_id},
            {"payload", routed_envelope.payload}},
        linkage);
    bootstrapFromStart(started);
    result["started_runs"].push_back(started.run.id);
  }
  return result;
}

void WorkflowExecutor::handleStartupResult(
    const std::string& workflow_run_id,
    const std::string& node_run_id,
    const std::string& command_run_id,
    const remote::ControlledSshResult& result,
    const std::vector<EventSpec>& event_specs,
    int retry_count,
    int retry_delay_ms) {
  const nlohmann::json result_payload{
      {"success", result.success},
      {"timed_out", result.timed_out},
      {"exit_code", result.exit_code},
      {"steps", result.step_results},
  };
  repository_.completeCommandRun(
      command_run_id,
      result.success ? "SUCCEEDED" : (result.timed_out ? "TIMED_OUT" : "FAILED"),
      result_payload,
      result.success
          ? nlohmann::json()
          : nlohmann::json{{"message", result.error}});
  auto detail = repository_.getWorkflowRun(workflow_run_id);
  if (!detail.has_value() || detail->run.state != "RUNNING") {
    return;
  }
  auto node_it = std::find_if(
      detail->nodes.begin(),
      detail->nodes.end(),
      [&node_run_id](const db::NodeRunRecord& node) {
        return node.id == node_run_id;
      });
  if (node_it == detail->nodes.end() || node_it->state != "RUNNING") {
    return;
  }
  nlohmann::json output = node_it->output_data;
  output["result"] = result_payload;
  output["result_success"] = result.success;
  output["dispatch_state"] = result.success ? "SUCCEEDED" : "FAILED";
  node_it->output_data = output;
  if (result.success) {
    evaluateAndEmitEventSpecs(
        *detail,
        node_it->id,
        command_run_id,
        event_specs,
        "RESULT",
        result_payload,
        true);
    clearEventGates(command_run_id);
    completeNode(*detail, node_it->node_key, output, true);
    return;
  }
  repository_.updateNodeRun(
      node_it->id,
      "RUNNING",
      node_it->assigned_robot_id,
      output,
      nlohmann::json());
  clearEventGates(command_run_id);
  scheduleNodeRetry(
      *detail,
      *node_it,
      retry_count,
      retry_delay_ms,
      result.error.empty() ? "robot SSH execution failed" : result.error);
}

db::WorkflowRunDetail WorkflowExecutor::cancel(const std::string& run_id) {
  auto detail = repository_.getWorkflowRun(run_id);
  if (!detail.has_value()) {
    throw std::runtime_error("workflow run not found");
  }
  for (auto& node : detail->nodes) {
    if (node.state == "PENDING" || node.state == "RUNNING" ||
        node.state == "WAITING_EVENT" || node.state == "WAITING_RESOURCE" ||
        node.state == "PAUSED") {
      repository_.updateNodeRun(
          node.id,
          "CANCELLED",
          node.assigned_robot_id,
          node.output_data,
          nlohmann::json{{"cancelled", true}});
    }
  }
  repository_.updateWorkflowRunState(
      run_id, "CANCELLED", detail->run.context_data);
  auto refreshed = repository_.getWorkflowRun(run_id);
  if (!refreshed.has_value()) {
    throw std::runtime_error("failed to reload cancelled run");
  }
  notifyParentRun(*refreshed, false, "child workflow was cancelled");
  return *refreshed;
}

db::WorkflowRunDetail WorkflowExecutor::decideManualConfirmation(
    const std::string& run_id,
    const std::string& node_run_id,
    bool approved,
    const std::string& note,
    const nlohmann::json& audit) {
  auto detail = repository_.getWorkflowRun(run_id);
  if (!detail.has_value()) {
    throw std::runtime_error("workflow run not found");
  }
  if (detail->run.state != "RUNNING") {
    throw std::runtime_error("workflow run is not running");
  }
  auto node_it = std::find_if(
      detail->nodes.begin(),
      detail->nodes.end(),
      [&node_run_id](const db::NodeRunRecord& node) {
        return node.id == node_run_id;
      });
  if (node_it == detail->nodes.end()) {
    throw std::runtime_error("node run not found in workflow run");
  }
  if (nodeType(findGraphNode(*detail, node_it->node_key)) != "MANUAL_CONFIRM") {
    throw std::runtime_error("node is not a manual confirmation");
  }
  nlohmann::json decision{
      {"approved", approved},
      {"note", note},
      {"audit", audit},
      {"waiting_manual_confirmation", false},
  };
  const auto next_state = approved ? "SUCCEEDED" : "FAILED";
  const auto error = approved
      ? nlohmann::json()
      : nlohmann::json{{"error", "manual confirmation rejected"},
                       {"decision", decision}};
  if (!repository_.transitionNodeRun(
          node_it->id,
          "PAUSED",
          next_state,
          node_it->assigned_robot_id,
          decision,
          error)) {
    throw std::runtime_error("manual confirmation was already decided");
  }
  node_it->state = next_state;
  node_it->output_data = decision;
  node_it->error_data = error;
  (void)repository_.insertWorkflowEvent(
      run_id,
      node_run_id,
      approved ? "manual.confirmed" : "manual.rejected",
      decision,
      std::string("manual-decision:") + node_run_id);
  if (approved) {
    followEdges(*detail, node_it->node_key, "success");
    maybeFinishRun(*detail);
  } else if (followEdges(*detail, node_it->node_key, "failure") > 0) {
    maybeFinishRun(*detail);
  } else {
    for (auto& other : detail->nodes) {
      if (other.id == node_it->id) {
        continue;
      }
      if (other.state == "PENDING" || other.state == "RUNNING" ||
          other.state == "WAITING_EVENT" ||
          other.state == "WAITING_RESOURCE" || other.state == "PAUSED") {
        repository_.updateNodeRun(
            other.id,
            "CANCELLED",
            other.assigned_robot_id,
            other.output_data,
            nlohmann::json{{"cancelled_by_manual_rejection", true},
                           {"manual_confirmation_node_run_id", node_run_id}});
        other.state = "CANCELLED";
      }
    }
    repository_.updateWorkflowRunState(
        run_id, "FAILED", detail->run.context_data);
  }
  auto refreshed = repository_.getWorkflowRun(run_id);
  if (!refreshed.has_value()) {
    throw std::runtime_error("failed to reload workflow run");
  }
  return *refreshed;
}

db::WorkflowRunDetail WorkflowExecutor::advance(const std::string& run_id) {
  auto detail = repository_.getWorkflowRun(run_id);
  if (!detail.has_value()) {
    throw std::runtime_error("workflow run not found");
  }
  if (detail->run.state != "RUNNING") {
    return *detail;
  }
  for (auto& node : detail->nodes) {
    if (node.state == "PENDING") {
      // Only activate nodes that have a succeeded predecessor or are START.
      const auto graph_node = findGraphNode(*detail, node.node_key);
      if (nodeType(graph_node) == "START") {
        activateNode(*detail, node.node_key);
      }
    }
  }
  maybeFinishRun(*detail);
  auto refreshed = repository_.getWorkflowRun(run_id);
  if (!refreshed.has_value()) {
    throw std::runtime_error("failed to reload run");
  }
  return *refreshed;
}

}  // namespace dispatcher::workflow
