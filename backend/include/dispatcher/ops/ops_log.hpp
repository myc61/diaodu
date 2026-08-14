#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace dispatcher::ops {

struct OpsLogEntry {
  std::int64_t id{0};
  std::string level;   // info | warn | error
  std::string source;  // ros | scene | point | nav | system
  std::string message;
  nlohmann::json detail = nlohmann::json::object();
  std::chrono::system_clock::time_point at{std::chrono::system_clock::now()};
};

class OpsLog {
 public:
  static OpsLog& instance();

  void info(std::string source, std::string message,
            nlohmann::json detail = nlohmann::json::object());
  void warn(std::string source, std::string message,
            nlohmann::json detail = nlohmann::json::object());
  void error(std::string source, std::string message,
             nlohmann::json detail = nlohmann::json::object());

  [[nodiscard]] std::vector<OpsLogEntry> list(
      std::size_t limit = 100, std::int64_t after_id = 0) const;

 private:
  void append(std::string level, std::string source, std::string message,
              nlohmann::json detail);

  mutable std::mutex mutex_;
  std::deque<OpsLogEntry> entries_;
  std::int64_t next_id_{1};
  static constexpr std::size_t kCapacity = 500;
};

}  // namespace dispatcher::ops
