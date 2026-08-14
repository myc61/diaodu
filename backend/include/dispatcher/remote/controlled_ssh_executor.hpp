#pragma once

#include "dispatcher/concurrency/execution_runtime.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>

namespace dispatcher::remote {

struct ControlledSshRequest {
  std::string job_id;
  std::string host;
  int port{22};
  std::string username;
  std::string credential_reference;
  std::string known_hosts_reference;
  nlohmann::json steps = nlohmann::json::array();
  nlohmann::json readiness_checks = nlohmann::json::array();
  int timeout_ms{120000};
};

struct ControlledSshResult {
  bool success{false};
  bool timed_out{false};
  int exit_code{-1};
  std::string error;
  nlohmann::json step_results = nlohmann::json::array();
};

using ControlledSshHandler = std::function<void(const ControlledSshResult&)>;

class ControlledSshExecutor {
 public:
  explicit ControlledSshExecutor(concurrency::ExecutionRuntime& execution);

  [[nodiscard]] bool execute(
      ControlledSshRequest request, ControlledSshHandler handler);

  [[nodiscard]] static std::optional<std::string> validateRequest(
      const ControlledSshRequest& request);

 private:
  [[nodiscard]] static ControlledSshResult run(
      const ControlledSshRequest& request);

  concurrency::ExecutionRuntime& execution_;
};

}  // namespace dispatcher::remote
