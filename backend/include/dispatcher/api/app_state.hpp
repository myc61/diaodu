#pragma once

#include "dispatcher/concurrency/execution_runtime.hpp"
#include "dispatcher/db/pg_pool.hpp"
#include "dispatcher/db/workspace_repository.hpp"
#include "dispatcher/ros/pose_cache.hpp"
#include "dispatcher/ros/robot_runtime.hpp"
#include "dispatcher/remote/controlled_ssh_executor.hpp"
#include "dispatcher/workflow/workflow_executor.hpp"

#include <memory>
#include <string>

namespace dispatcher::api {

struct AppState {
  std::string map_root{"/var/lib/dispatcher/maps"};
  std::unique_ptr<db::PgPool> pool;
  std::unique_ptr<db::WorkspaceRepository> repository;
  std::unique_ptr<concurrency::ExecutionRuntime> execution;
  std::unique_ptr<ros::PoseCache> pose_cache;
  std::unique_ptr<ros::RobotRuntime> robot_runtime;
  std::unique_ptr<remote::ControlledSshExecutor> ssh_executor;
  std::unique_ptr<workflow::WorkflowExecutor> workflow_executor;
};

AppState& appState();
void initializeAppState();

}  // namespace dispatcher::api
