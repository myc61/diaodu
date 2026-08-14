#pragma once

#include "dispatcher/db/workspace_repository.hpp"
#include "dispatcher/remote/controlled_ssh_executor.hpp"
#include "dispatcher/ros/robot_runtime.hpp"
#include "dispatcher/workflow/event_spec.hpp"
#include "dispatcher/workflow/feedback_trigger.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <functional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dispatcher::workflow {

class WorkflowExecutor {
 public:
  WorkflowExecutor(
      db::WorkspaceRepository& repository,
      ros::RobotRuntime& robots,
      remote::ControlledSshExecutor& ssh_executor);
  ~WorkflowExecutor();

  [[nodiscard]] db::WorkflowRunDetail startManual(
      const std::string& workflow_definition_id,
      const nlohmann::json& input = nlohmann::json::object());

  [[nodiscard]] nlohmann::json injectEvent(
      const std::string& event_name,
      const nlohmann::json& payload = nlohmann::json::object());
  [[nodiscard]] nlohmann::json injectEvent(
      const db::WorkflowSignalEnvelope& envelope);

  [[nodiscard]] db::WorkflowRunDetail cancel(const std::string& run_id);

  [[nodiscard]] db::WorkflowRunDetail decideManualConfirmation(
      const std::string& run_id,
      const std::string& node_run_id,
      bool approved,
      const std::string& note = "",
      const nlohmann::json& audit = nlohmann::json::object());

  [[nodiscard]] db::WorkflowRunDetail advance(const std::string& run_id);

 private:
  void bootstrapFromStart(db::WorkflowRunDetail& detail);
  void activateNode(db::WorkflowRunDetail& detail, const std::string& node_key);
  void completeNode(
      db::WorkflowRunDetail& detail,
      const std::string& node_key,
      const nlohmann::json& output,
      bool follow_success_edges = true);
  void failNode(
      db::WorkflowRunDetail& detail,
      const std::string& node_key,
      const std::string& message);
  void executeNode(db::WorkflowRunDetail& detail, db::NodeRunRecord& node);
  void dispatchCapabilityNode(
      db::WorkflowRunDetail& detail,
      db::NodeRunRecord& node,
      const db::RobotRecord& robot,
      const db::CapabilityTemplateRecord& capability,
      const nlohmann::json& parameters,
      int timeout_ms,
      int retry_count,
      int retry_delay_ms,
      const std::vector<EventSpec>& event_specs,
      nlohmann::json output_context);
  void followEdges(
      db::WorkflowRunDetail& detail,
      const std::string& source_key,
      const std::string& edge_kind,
      const std::string& event_name = "");
  void maybeFinishRun(db::WorkflowRunDetail& detail);
  void emitAndPropagateEvent(
      db::WorkflowRunDetail& detail,
      const std::optional<std::string>& node_run_id,
      const std::string& event_name,
      const nlohmann::json& payload);
  void handleRosCommandEvent(
      const std::string& workflow_run_id,
      const std::string& node_run_id,
      const std::string& command_run_id,
      const std::vector<EventSpec>& event_specs,
      const ros::RosCommandEvent& event);
  void evaluateAndEmitEventSpecs(
      db::WorkflowRunDetail& detail,
      const std::string& node_run_id,
      const std::string& command_run_id,
      const std::vector<EventSpec>& event_specs,
      const std::string& source,
      const nlohmann::json& payload,
      bool ros_success);
  [[nodiscard]] ros::RosCommandHandler makeRosCommandHandler(
      const std::string& workflow_run_id,
      const std::string& node_run_id,
      const std::string& command_run_id,
      const std::vector<EventSpec>& event_specs);
  void clearEventGates(const std::string& command_run_id);
  void scheduleNodeRetry(
      db::WorkflowRunDetail& detail,
      db::NodeRunRecord& node,
      int retry_count,
      int retry_delay_ms,
      const std::string& message);
  void finishOrRepeatWorkflow(db::WorkflowRunDetail& detail);
  void notifyParentRun(
      const db::WorkflowRunDetail& child, bool succeeded,
      const std::string& error = "");
  void handleStartupResult(
      const std::string& workflow_run_id,
      const std::string& node_run_id,
      const std::string& command_run_id,
      const remote::ControlledSshResult& result,
      const std::vector<EventSpec>& event_specs = {},
      int retry_count = 0,
      int retry_delay_ms = 1000);
  void dispatchSshCapabilityNode(
      db::WorkflowRunDetail& detail,
      db::NodeRunRecord& node,
      const db::RobotRecord& robot,
      const db::CapabilityTemplateRecord& capability,
      const nlohmann::json& parameters,
      int timeout_ms,
      int retry_count,
      int retry_delay_ms,
      const std::vector<EventSpec>& event_specs,
      nlohmann::json output_context);
  void scheduleAfter(std::chrono::milliseconds delay, std::function<void()> task);
  void timerLoop();

  [[nodiscard]] static std::string nodeType(const nlohmann::json& node);
  [[nodiscard]] static nlohmann::json nodeData(const nlohmann::json& node);
  [[nodiscard]] db::NodeRunRecord* findNode(
      db::WorkflowRunDetail& detail, const std::string& node_key);
  [[nodiscard]] nlohmann::json findGraphNode(
      const db::WorkflowRunDetail& detail, const std::string& node_key) const;

  db::WorkspaceRepository& repository_;
  ros::RobotRuntime& robots_;
  remote::ControlledSshExecutor& ssh_executor_;

  struct CallbackGate {
    std::mutex mutex;
    WorkflowExecutor* owner{nullptr};
  };
  std::shared_ptr<CallbackGate> callback_gate_;
  std::unordered_map<std::string, FeedbackTriggerGate> event_gates_;

  struct ScheduledTask {
    std::chrono::steady_clock::time_point due;
    std::uint64_t sequence{0};
    std::function<void()> task;
  };
  struct ScheduledTaskLater {
    bool operator()(const ScheduledTask& left, const ScheduledTask& right) const {
      if (left.due == right.due) {
        return left.sequence > right.sequence;
      }
      return left.due > right.due;
    }
  };
  std::mutex timer_mutex_;
  std::condition_variable timer_cv_;
  std::priority_queue<ScheduledTask, std::vector<ScheduledTask>, ScheduledTaskLater>
      scheduled_tasks_;
  std::thread timer_thread_;
  bool timer_stopping_{false};
  std::uint64_t timer_sequence_{0};
};

}  // namespace dispatcher::workflow
