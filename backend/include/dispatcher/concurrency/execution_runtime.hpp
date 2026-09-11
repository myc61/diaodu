#pragma once

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dispatcher::concurrency {

struct ExecutionRuntimeOptions {
  std::size_t protocol_threads{4};
  std::size_t blocking_threads{2};
};

class ExecutionRuntime {
 public:
  using Task = std::function<void()>;
  using ErrorHandler =
      std::function<void(std::string_view task_group, std::string_view error)>;

  explicit ExecutionRuntime(
      ExecutionRuntimeOptions options = {},
      ErrorHandler error_handler = {});
  ~ExecutionRuntime();

  ExecutionRuntime(const ExecutionRuntime&) = delete;
  ExecutionRuntime& operator=(const ExecutionRuntime&) = delete;

  // Tasks for one robot are serialized; tasks for different robots may run in
  // parallel on the shared protocol pool.
  bool postRobot(std::string robot_id, Task task);
  bool postRobotAfter(
      std::string robot_id, std::chrono::milliseconds delay, Task task);
  bool postParallel(Task task);
  bool postBlocking(Task task);

  // Drains submitted work and joins all worker threads. No tasks are accepted
  // after join starts.
  void join();

  [[nodiscard]] std::size_t protocolThreadCount() const noexcept;
  [[nodiscard]] std::size_t blockingThreadCount() const noexcept;

 private:
  using ProtocolExecutor = boost::asio::thread_pool::executor_type;
  using RobotStrand = boost::asio::strand<ProtocolExecutor>;

  Task guarded(std::string task_group, Task task) const;
  std::shared_ptr<RobotStrand> strandFor(const std::string& robot_id);

  std::size_t protocol_thread_count_;
  std::size_t blocking_thread_count_;
  boost::asio::thread_pool protocol_pool_;
  boost::asio::thread_pool blocking_pool_;
  ErrorHandler error_handler_;
  std::atomic<bool> accepting_{true};
  mutable std::mutex lifecycle_mutex_;
  std::mutex strands_mutex_;
  std::unordered_map<std::string, std::shared_ptr<RobotStrand>> robot_strands_;
  std::mutex delayed_mutex_;
  std::vector<std::weak_ptr<boost::asio::steady_timer>> delayed_timers_;
};

}  // namespace dispatcher::concurrency
