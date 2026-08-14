#include "dispatcher/remote/controlled_ssh_executor.hpp"

#include <sys/poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <regex>
#include <thread>
#include <utility>
#include <vector>

namespace dispatcher::remote {
namespace {

constexpr std::size_t kMaxOutputBytes = 256 * 1024;

struct ProcessResult {
  int exit_code{-1};
  bool timed_out{false};
  std::string output;
  std::string error;
};

std::string shellQuote(const std::string& value) {
  std::string quoted{"'"};
  for (const char ch : value) {
    if (ch == '\'') {
      quoted += "'\\''";
    } else {
      quoted += ch;
    }
  }
  quoted += '\'';
  return quoted;
}

bool validText(const std::string& value) {
  return value.find('\0') == std::string::npos &&
      value.find('\n') == std::string::npos &&
      value.find('\r') == std::string::npos;
}

std::vector<std::filesystem::path> configuredRoots(
    const char* environment_name,
    const std::vector<std::filesystem::path>& defaults) {
  const char* raw = std::getenv(environment_name);
  if (raw == nullptr || *raw == '\0') {
    return defaults;
  }
  std::vector<std::filesystem::path> roots;
  std::string value(raw);
  std::size_t offset = 0;
  while (offset <= value.size()) {
    const auto separator = value.find(':', offset);
    const auto item = value.substr(
        offset,
        separator == std::string::npos ? value.size() - offset
                                       : separator - offset);
    if (!item.empty()) {
      const std::filesystem::path path(item);
      if (path.is_absolute()) {
        roots.push_back(path.lexically_normal());
      }
    }
    if (separator == std::string::npos) {
      break;
    }
    offset = separator + 1;
  }
  return roots;
}

bool safeExecutablePath(const std::string& value) {
  if (value.empty() || !validText(value) || value.front() == '-') {
    return false;
  }
  const std::filesystem::path path(value);
  for (const auto& component : path) {
    if (component == "..") {
      return false;
    }
  }
  for (const char ch : value) {
    if (std::iscntrl(static_cast<unsigned char>(ch)) ||
        ch == ';' || ch == '|' || ch == '&' || ch == '>' || ch == '<' ||
        ch == '$' || ch == '`') {
      return false;
    }
  }
  return true;
}

bool forbiddenCommandInterpreter(const std::string& executable) {
  const auto name = std::filesystem::path(executable).filename().string();
  static const std::vector<std::string> forbidden{
      "sh", "bash", "dash", "zsh", "fish", "csh", "tcsh", "sudo", "su"};
  return std::find(forbidden.begin(), forbidden.end(), name) != forbidden.end();
}

std::optional<std::string> validateStep(
    const nlohmann::json& step, const std::string& field) {
  if (!step.is_object()) {
    return field + " entries must be objects";
  }
  const bool has_script = step.contains("script") &&
      step["script"].is_string() && !step["script"].get<std::string>().empty();
  const bool has_command = step.contains("command") && step["command"].is_array() &&
      !step["command"].empty();
  if (has_script == has_command) {
    return field + " must contain exactly one script or command array";
  }
  if (has_script && !safeExecutablePath(step["script"].get<std::string>())) {
    return field + " script must be a safe relative or absolute executable path";
  }
  if (has_script && forbiddenCommandInterpreter(
          step["script"].get<std::string>())) {
    return field + " script cannot invoke a shell or privilege escalator";
  }
  if (has_command) {
    for (const auto& token : step["command"]) {
      if (!token.is_string() || !validText(token.get<std::string>())) {
        return field + " command entries must be strings without line breaks";
      }
    }
    if (!safeExecutablePath(step["command"][0].get<std::string>())) {
      return field + " command executable must be a safe path";
    }
    if (forbiddenCommandInterpreter(step["command"][0].get<std::string>())) {
      return field + " command cannot invoke a shell or privilege escalator";
    }
    if (step.contains("args") && !step.value("args", nlohmann::json::array()).empty()) {
      return field + " command entries already contain their arguments";
    }
  }
  if (step.contains("args") && !step["args"].is_array()) {
    return field + " args must be an array";
  }
  for (const auto& arg : step.value("args", nlohmann::json::array())) {
    if (!arg.is_string() || !validText(arg.get<std::string>())) {
      return field + " args must contain strings without line breaks";
    }
  }
  const auto workdir = step.value("working_directory", "");
  if (!workdir.empty() && !safeExecutablePath(workdir)) {
    return field + " working_directory must be a safe path";
  }
  const auto mode = step.value("mode", "wait");
  if (mode != "wait" && mode != "detached") {
    return field + " mode must be wait or detached";
  }
  if (field == "readiness_checks" && mode == "detached") {
    return "readiness_checks cannot run in detached mode";
  }
  const auto log_path = step.value("log_path", "");
  if (mode == "detached" &&
      (log_path.empty() || !safeExecutablePath(log_path) ||
       !std::filesystem::path(log_path).is_absolute())) {
    return field + " detached steps require an absolute safe log_path";
  }
  if (step.contains("environment") && !step["environment"].is_object()) {
    return field + " environment must be an object";
  }
  static const std::regex env_key{"^[A-Za-z_][A-Za-z0-9_]*$"};
  for (const auto& [key, value] :
       step.value("environment", nlohmann::json::object()).items()) {
    if (!std::regex_match(key, env_key) || !value.is_string() ||
        !validText(value.get<std::string>())) {
      return field + " contains an invalid environment entry";
    }
  }
  const int timeout_ms = step.value("timeout_ms", 60000);
  if (timeout_ms < 100 || timeout_ms > 3600000) {
    return field + " timeout_ms must be between 100 and 3600000";
  }
  const int retries = step.value("retry_count", 0);
  if (retries < 0 || retries > 100) {
    return field + " retry_count must be between 0 and 100";
  }
  return std::nullopt;
}

std::string remoteCommand(const nlohmann::json& step) {
  std::string command{"set -e; "};
  const auto workdir = step.value("working_directory", "");
  if (!workdir.empty()) {
    command += "cd " + shellQuote(workdir) + "; ";
  }
  const bool detached = step.value("mode", "wait") == "detached";
  if (detached) {
    command += "nohup env";
  } else {
    command += "exec env";
  }
  for (const auto& [key, value] :
       step.value("environment", nlohmann::json::object()).items()) {
    command += " " + key + "=" + shellQuote(value.get<std::string>());
  }
  nlohmann::json arguments = step.value("args", nlohmann::json::array());
  if (step.contains("command")) {
    command += " " + shellQuote(step.at("command").at(0).get<std::string>());
    arguments = nlohmann::json::array();
    for (std::size_t index = 1; index < step.at("command").size(); ++index) {
      arguments.push_back(step.at("command").at(index));
    }
  } else {
    command += " " + shellQuote(step.at("script").get<std::string>());
  }
  for (const auto& arg : arguments) {
    command += " " + shellQuote(arg.get<std::string>());
  }
  if (detached) {
    command += " >> " + shellQuote(step.at("log_path").get<std::string>()) +
        " 2>&1 < /dev/null & echo $!";
  }
  return command;
}

ProcessResult runProcess(
    const std::vector<std::string>& arguments, int timeout_ms) {
  ProcessResult result;
  int output_pipe[2];
  if (::pipe(output_pipe) != 0) {
    result.error = std::string("pipe failed: ") + std::strerror(errno);
    return result;
  }

  const pid_t child = ::fork();
  if (child < 0) {
    ::close(output_pipe[0]);
    ::close(output_pipe[1]);
    result.error = std::string("fork failed: ") + std::strerror(errno);
    return result;
  }
  if (child == 0) {
    (void)::setpgid(0, 0);
    ::close(output_pipe[0]);
    (void)::dup2(output_pipe[1], STDOUT_FILENO);
    (void)::dup2(output_pipe[1], STDERR_FILENO);
    ::close(output_pipe[1]);
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execvp(argv[0], argv.data());
    _exit(127);
  }

  ::close(output_pipe[1]);
  const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(timeout_ms);
  bool exited = false;
  int status = 0;
  while (!exited) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      result.timed_out = true;
      (void)::kill(-child, SIGTERM);
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      (void)::kill(-child, SIGKILL);
    }
    const int wait_ms = result.timed_out
        ? 0
        : static_cast<int>(std::clamp<std::int64_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                  .count(),
              0,
              200));
    pollfd descriptor{.fd = output_pipe[0], .events = POLLIN, .revents = 0};
    (void)::poll(&descriptor, 1, wait_ms);
    if ((descriptor.revents & (POLLIN | POLLHUP)) != 0) {
      char buffer[4096];
      const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
      if (count > 0 && result.output.size() < kMaxOutputBytes) {
        const auto remaining = kMaxOutputBytes - result.output.size();
        result.output.append(
            buffer,
            std::min<std::size_t>(static_cast<std::size_t>(count), remaining));
      }
    }
    const pid_t wait_result = ::waitpid(child, &status, WNOHANG);
    exited = wait_result == child;
    if (result.timed_out && !exited) {
      (void)::waitpid(child, &status, 0);
      exited = true;
    }
  }
  ::close(output_pipe[0]);
  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.exit_code = 128 + WTERMSIG(status);
  }
  return result;
}

ProcessResult runSshStep(
    const ControlledSshRequest& request,
    const nlohmann::json& step,
    int timeout_ms) {
  const int connect_timeout_seconds = std::clamp(
      step.value("connect_timeout_seconds", 10), 1, 60);
  std::vector<std::string> arguments{
      "ssh",
      "-o", "BatchMode=yes",
      "-o", "StrictHostKeyChecking=yes",
      "-o", "ConnectTimeout=" + std::to_string(connect_timeout_seconds),
      "-o", "UserKnownHostsFile=" + request.known_hosts_reference,
      "-i", request.credential_reference,
      "-p", std::to_string(request.port),
      request.username + "@" + request.host,
      remoteCommand(step),
  };
  return runProcess(arguments, timeout_ms);
}

nlohmann::json processToJson(
    const nlohmann::json& step, const ProcessResult& process, int attempt) {
  return {
      {"name", step.value(
          "name",
          step.contains("script") ? step.value("script", "")
                                  : step.value("command", nlohmann::json::array()).dump())},
      {"script", step.value("script", "")},
      {"command", step.value("command", nlohmann::json::array())},
      {"mode", step.value("mode", "wait")},
      {"attempt", attempt},
      {"exit_code", process.exit_code},
      {"timed_out", process.timed_out},
      {"output", process.output},
      {"error", process.error},
      {"success", process.exit_code == 0 && !process.timed_out && process.error.empty()},
  };
}

}  // namespace

ControlledSshExecutor::ControlledSshExecutor(
    concurrency::ExecutionRuntime& execution)
    : execution_(execution) {}

bool ControlledSshExecutor::execute(
    ControlledSshRequest request, ControlledSshHandler handler) {
  if (validateRequest(request).has_value() || !handler) {
    return false;
  }
  return execution_.postBlocking(
      [request = std::move(request), handler = std::move(handler)] {
        handler(run(request));
      });
}

std::optional<std::string> ControlledSshExecutor::validateRequest(
    const ControlledSshRequest& request) {
  if (request.job_id.empty() || request.host.empty() ||
      request.username.empty()) {
    return "SSH job_id, host and username are required";
  }
  static const std::regex host_pattern{"^[A-Za-z0-9_.:%-]+$"};
  static const std::regex username_pattern{"^[A-Za-z_][A-Za-z0-9_.-]*$"};
  if (!std::regex_match(request.host, host_pattern) ||
      request.host.front() == '-' ||
      !std::regex_match(request.username, username_pattern) ||
      request.username == "root") {
    return "SSH host or username is invalid for a controlled low-privilege connection";
  }
  if (request.port < 1 || request.port > 65535) {
    return "SSH port must be between 1 and 65535";
  }
  if (!std::filesystem::path(request.credential_reference).is_absolute() ||
      !std::filesystem::path(request.known_hosts_reference).is_absolute()) {
    return "SSH credential and known_hosts references must be absolute server paths";
  }
  const auto secret_roots = configuredRoots(
      "DISPATCHER_SSH_SECRET_ROOTS",
      {"/run/secrets", "/etc/dispatcher/ssh"});
  const auto credential_path = std::filesystem::path(request.credential_reference);
  const auto known_hosts_path = std::filesystem::path(request.known_hosts_reference);
  const auto within_secret_root = [&secret_roots](const std::filesystem::path& path) {
    const auto normalized = path.lexically_normal();
    for (const auto& root : secret_roots) {
      auto path_component = normalized.begin();
      bool matches = true;
      for (const auto& root_component : root) {
        if (path_component == normalized.end() ||
            *path_component != root_component) {
          matches = false;
          break;
        }
        ++path_component;
      }
      if (matches) {
        return true;
      }
    }
    return false;
  };
  if (!within_secret_root(credential_path) ||
      !within_secret_root(known_hosts_path)) {
    return "SSH credential and known_hosts references are outside the configured secret roots";
  }
  if (!request.steps.is_array() || request.steps.empty()) {
    return "startup steps must be a non-empty array";
  }
  if (!request.readiness_checks.is_array()) {
    return "readiness_checks must be an array";
  }
  for (const auto& step : request.steps) {
    if (const auto error = validateStep(step, "steps"); error.has_value()) {
      return error;
    }
  }
  for (const auto& step : request.readiness_checks) {
    if (const auto error = validateStep(step, "readiness_checks");
        error.has_value()) {
      return error;
    }
  }
  if (request.timeout_ms < 1000 || request.timeout_ms > 3600000) {
    return "startup timeout_ms must be between 1000 and 3600000";
  }
  return std::nullopt;
}

ControlledSshResult ControlledSshExecutor::run(
    const ControlledSshRequest& request) {
  ControlledSshResult result;
  const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(request.timeout_ms);
  auto runSteps = [&](const nlohmann::json& steps, bool checks) -> bool {
    for (const auto& step : steps) {
      const int max_attempts = checks ? step.value("retry_count", 0) + 1 : 1;
      bool step_succeeded = false;
      for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          result.timed_out = true;
          result.error = "startup profile timeout";
          return false;
        }
        const auto remaining_ms = std::max<std::int64_t>(
            1,
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                .count());
        const int execution_timeout = static_cast<int>(std::min<std::int64_t>(
            step.value("timeout_ms", 60000), remaining_ms));
        auto process = runSshStep(request, step, execution_timeout);
        result.step_results.push_back(processToJson(step, process, attempt));
        result.exit_code = process.exit_code;
        if (process.exit_code == 0 && !process.timed_out && process.error.empty()) {
          step_succeeded = true;
          break;
        }
        if (process.timed_out) {
          result.timed_out = true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          result.timed_out = true;
          result.error = "startup profile timeout";
          return false;
        }
        if (attempt < max_attempts) {
          const auto retry_now = std::chrono::steady_clock::now();
          const auto retry_remaining =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  deadline - retry_now);
          std::this_thread::sleep_for(std::min(
              std::chrono::milliseconds(
                  std::clamp(step.value("retry_interval_ms", 1000), 0, 60000)),
              std::max(retry_remaining, std::chrono::milliseconds(0))));
        }
      }
      if (!step_succeeded) {
        result.error = checks ? "readiness check failed" : "startup step failed";
        return false;
      }
    }
    return true;
  };

  if (!runSteps(request.steps, false) ||
      !runSteps(request.readiness_checks, true)) {
    return result;
  }
  result.success = true;
  result.exit_code = 0;
  return result;
}

}  // namespace dispatcher::remote
