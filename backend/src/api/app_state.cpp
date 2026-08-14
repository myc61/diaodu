#include "dispatcher/api/app_state.hpp"

#include <cstdlib>

namespace dispatcher::api {
namespace {

std::string envOr(const char* key, const char* fallback) {
  const char* value = std::getenv(key);
  if (value == nullptr || value[0] == '\0') {
    return fallback;
  }
  return value;
}

AppState& stateInstance() {
  static AppState state;
  return state;
}

}  // namespace

AppState& appState() {
  return stateInstance();
}

void initializeAppState() {
  auto& state = stateInstance();
  state.map_root = envOr("DISPATCHER_MAP_ROOT", "/var/lib/dispatcher/maps");
  state.pool = std::make_unique<db::PgPool>(
      envOr(
          "DISPATCHER_DATABASE_URL",
          "postgresql://dispatcher:dispatcher_dev_password@127.0.0.1:5432/dispatcher"));
  state.repository = std::make_unique<db::WorkspaceRepository>(*state.pool);
  state.execution = std::make_unique<concurrency::ExecutionRuntime>();
  state.pose_cache = std::make_unique<ros::PoseCache>();
  state.robot_runtime = std::make_unique<ros::RobotRuntime>(
      *state.execution,
      *state.repository,
      *state.pose_cache);
  state.ssh_executor = std::make_unique<remote::ControlledSshExecutor>(
      *state.execution);
  state.workflow_executor = std::make_unique<workflow::WorkflowExecutor>(
      *state.repository,
      *state.robot_runtime,
      *state.ssh_executor);
}

}  // namespace dispatcher::api
