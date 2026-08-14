#include "dispatcher/concurrency/execution_runtime.hpp"

#include <boost/asio/post.hpp>

#include <algorithm>
#include <exception>
#include <utility>

namespace dispatcher::concurrency {
namespace {

std::size_t validThreadCount(std::size_t requested) {
  return std::max<std::size_t>(requested, 1);
}

}  // namespace

ExecutionRuntime::ExecutionRuntime(
    ExecutionRuntimeOptions options,
    ErrorHandler error_handler)
    : protocol_thread_count_(validThreadCount(options.protocol_threads)),
      blocking_thread_count_(validThreadCount(options.blocking_threads)),
      protocol_pool_(protocol_thread_count_),
      blocking_pool_(blocking_thread_count_),
      error_handler_(std::move(error_handler)) {}

ExecutionRuntime::~ExecutionRuntime() {
  join();
}

bool ExecutionRuntime::postRobot(std::string robot_id, Task task) {
  if (robot_id.empty() || !task) {
    return false;
  }
  std::lock_guard lock(lifecycle_mutex_);
  if (!accepting_.load(std::memory_order_acquire)) {
    return false;
  }
  const auto strand = strandFor(robot_id);
  boost::asio::post(
      *strand, guarded("robot/" + std::move(robot_id), std::move(task)));
  return true;
}

bool ExecutionRuntime::postParallel(Task task) {
  if (!task) {
    return false;
  }
  std::lock_guard lock(lifecycle_mutex_);
  if (!accepting_.load(std::memory_order_acquire)) {
    return false;
  }
  boost::asio::post(protocol_pool_, guarded("parallel", std::move(task)));
  return true;
}

bool ExecutionRuntime::postBlocking(Task task) {
  if (!task) {
    return false;
  }
  std::lock_guard lock(lifecycle_mutex_);
  if (!accepting_.load(std::memory_order_acquire)) {
    return false;
  }
  boost::asio::post(blocking_pool_, guarded("blocking", std::move(task)));
  return true;
}

void ExecutionRuntime::join() {
  {
    std::lock_guard lock(lifecycle_mutex_);
    if (!accepting_.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
  }
  protocol_pool_.join();
  blocking_pool_.join();
}

std::size_t ExecutionRuntime::protocolThreadCount() const noexcept {
  return protocol_thread_count_;
}

std::size_t ExecutionRuntime::blockingThreadCount() const noexcept {
  return blocking_thread_count_;
}

ExecutionRuntime::Task ExecutionRuntime::guarded(
    std::string task_group, Task task) const {
  const auto error_handler = error_handler_;
  return [task_group = std::move(task_group), task = std::move(task),
          error_handler]() mutable {
    try {
      task();
    } catch (const std::exception& error) {
      if (error_handler) {
        error_handler(task_group, error.what());
      }
    } catch (...) {
      if (error_handler) {
        error_handler(task_group, "unknown exception");
      }
    }
  };
}

std::shared_ptr<ExecutionRuntime::RobotStrand> ExecutionRuntime::strandFor(
    const std::string& robot_id) {
  std::lock_guard lock(strands_mutex_);
  const auto existing = robot_strands_.find(robot_id);
  if (existing != robot_strands_.end()) {
    return existing->second;
  }
  auto strand = std::make_shared<RobotStrand>(protocol_pool_.get_executor());
  robot_strands_.emplace(robot_id, strand);
  return strand;
}

}  // namespace dispatcher::concurrency
